#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_base/sdk/text_utils.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <pj_can_dbc/can_topic.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "candump_dialog.hpp"
#include "candump_manifest.hpp"
#include "candump_parser.hpp"
#include "csv_dictionary.hpp"

namespace {

bool endsWithCsv(const std::string& path) {
  return PJ::sdk::lowerAscii(std::filesystem::path(path).extension().string()) == ".csv";
}

/// Per-interface decode/topic state, resolved once per bus-name CHANGE
/// rather than on every frame (a capture typically stays on the same
/// interface for long runs). Keyed internally by (id << 1) | extended
/// instead of a per-frame string concatenation.
struct InterfaceTopics {
  pj_can_dbc::CanDecoder* decoder = nullptr;  // nullptr if no dictionary assigned
  std::unordered_map<std::uint64_t, PJ::sdk::TopicHandle> decoded;
  std::unordered_map<std::uint64_t, PJ::sdk::TopicHandle> raw;
};

std::uint64_t frameKey(std::uint32_t can_id, bool extended) {
  return (static_cast<std::uint64_t>(can_id) << 1) | (extended ? 1u : 0u);
}

/// Imports candump `-l` log files and interactive screen captures. Frames are
/// decoded per-interface via a DBC (or an ARUS-style CSV, translated
/// in-memory to DBC text) and grouped as one topic per (interface, message);
/// undecodable/unassigned frames optionally become one raw topic per
/// (interface, id) with byte0..N fields.
class CandumpSource : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest | PJ::kCapabilityHasDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    // A config-only/headless restore (no dialog ever shown): the summary/
    // interface-table a full prescan would produce is never read, so don't
    // pay for it here -- CandumpDialog runs it lazily instead, on the first
    // widget_data() call, if/when the dialog is actually opened.
    if (!dialog_.loadConfigDeferringScan(config_json)) {
      return PJ::unexpected(std::string("invalid config JSON"));
    }
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    const std::string& filepath = dialog_.filePath();
    if (filepath.empty()) {
      return PJ::unexpected(std::string("no filepath configured"));
    }

    std::error_code ec;
    const std::uintmax_t file_size = std::filesystem::file_size(filepath, ec);
    const std::uint64_t total_bytes = ec ? 0 : static_cast<std::uint64_t>(file_size);
    (void)runtimeHost().progressStart("Importing candump", total_bytes, true);

    candump_detail::TimeMode mode = candump_detail::TimeMode::kAbsolute;
    {
      std::ifstream prescan(filepath);
      if (!prescan.is_open()) {
        return PJ::unexpected(std::string("cannot open file: ") + filepath);
      }
      // Same bounded scan (and the same kScanCap) as CandumpDialog's own
      // preview -- only the TimeMode half of the result is needed here.
      const auto scan = candump_detail::prescanCandump(prescan, candump_detail::kScanCap);
      const auto override_mode = dialog_.timeModeOverride();
      if (!scan.time_mode.saw_numeric_timestamp && !override_mode.has_value()) {
        return PJ::unexpected(
            std::string(
                "no timestamps found in this file -- record with `candump -l` (or `-t z`/`-t d`); "
                "wall-clock `-t a` timestamps are not supported in this version"));
      }
      mode = override_mode.value_or(scan.time_mode.mode);
      if (mode == candump_detail::TimeMode::kRelativeDelta) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning,
            "timestamps look like `-t d` (delta since previous frame) and do not carry wall-clock "
            "information; align this dataset against others in the Source Timeline manually");
      } else if (mode == candump_detail::TimeMode::kRelativeMonotonic) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning,
            "timestamps look relative (`-t z`, seconds since capture start) and do not carry wall-clock "
            "information; align this dataset against others in the Source Timeline manually");
      }
    }

    std::unordered_map<std::string, pj_can_dbc::CanDecoder> decoders;
    for (const auto& [iface, paths] : dialog_.interfaceDicts()) {
      auto& decoder = decoders[iface];
      for (const auto& path : paths) {
        PJ::Status status;
        if (endsWithCsv(path)) {
          std::ifstream csv_file(path);
          if (!csv_file.is_open()) {
            status = PJ::unexpected(std::string("cannot open dictionary: ") + path);
          } else {
            const std::string csv_text((std::istreambuf_iterator<char>(csv_file)), std::istreambuf_iterator<char>());
            std::string dbc_text;
            std::string warnings;
            status = candump_detail::arusCsvToDbc(csv_text, dbc_text, warnings);
            if (status) {
              status = decoder.loadDbcString(dbc_text);
            }
            if (!warnings.empty()) {
              runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, path + ": " + warnings);
            }
          }
        } else {
          status = decoder.loadDbcFile(path);
        }
        if (!status) {
          runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, status.error());
        }
      }
    }
    if (decoders.empty()) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, "no dictionary assigned to any interface; nothing will be decoded");
    }

    std::ifstream file(filepath);
    if (!file.is_open()) {
      return PJ::unexpected(std::string("cannot open file: ") + filepath);
    }

    // Precomputed "byte0".."byte7" names for the raw-bytes fallback topics
    // (classic CAN payload is at most 8 bytes).
    std::array<std::string, 8> byte_field_names;
    for (std::size_t i = 0; i < byte_field_names.size(); ++i) {
      byte_field_names[i] = "byte" + std::to_string(i);
    }

    // Per-interface decode/topic state, resolved once per bus-name change
    // (see InterfaceTopics) instead of a per-frame decoders/topic lookup.
    std::unordered_map<std::string, InterfaceTopics> topics_by_interface;
    std::string last_interface;
    InterfaceTopics* current_topics = nullptr;
    std::size_t decoded_topic_count = 0;
    std::vector<PJ::sdk::NamedFieldValue> row_fields;

    std::uint64_t seen = 0;
    std::uint64_t line_no = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t decoded_frames = 0;
    std::uint64_t unmatched = 0;
    std::uint64_t undecodable = 0;
    std::uint64_t no_dictionary = 0;
    candump_detail::RecognizedCounters counters;
    std::uint64_t malformed_count = 0;
    std::vector<std::uint64_t> first_malformed_lines;
    const bool raw_unassigned = dialog_.rawUnassigned();

    std::int64_t cumulative_ns = 0;

    std::string line;
    constexpr std::uint64_t kCancelPollMask = 4095;
    bool cancelled = false;
    while (std::getline(file, line)) {
      ++line_no;
      bytes_read += line.size() + 1;
      if (line.empty()) {
        continue;
      }
      if ((++seen & kCancelPollMask) == 0) {
        if (runtimeHost().isStopRequested()) {
          cancelled = true;
          break;
        }
        if (total_bytes > 0) {
          if (!runtimeHost().progressUpdate(std::min<std::uint64_t>(bytes_read, total_bytes))) {
            cancelled = true;
            break;
          }
        }
      }

      const candump_detail::ParsedLine parsed = candump_detail::parseLine(line);
      switch (parsed.kind) {
        case candump_detail::LineKind::kMalformed:
          ++malformed_count;
          if (first_malformed_lines.size() < 3) {
            first_malformed_lines.push_back(line_no);
          }
          continue;
        case candump_detail::LineKind::kWallClockTs:
          ++counters.wall_clock;
          continue;
        case candump_detail::LineKind::kDropCount:
          ++counters.dropcount;
          counters.dropped_frames_total += parsed.dropped_count;
          continue;
        case candump_detail::LineKind::kErrorDetail:
          ++counters.error_detail;  // `-e` TAB-continuation detail of the PRECEDING error frame
          continue;
        case candump_detail::LineKind::kRtr:
          ++counters.rtr;
          continue;
        case candump_detail::LineKind::kFd:
          ++counters.fd;
          continue;
        case candump_detail::LineKind::kXl:
          ++counters.xl;
          continue;
        case candump_detail::LineKind::kError:
          ++counters.error;
          continue;
        case candump_detail::LineKind::kData:
          break;
      }

      std::int64_t ts_ns;
      if (mode == candump_detail::TimeMode::kRelativeDelta) {
        // "-t d" prints the delta since the PREVIOUS frame (including the
        // first, conventionally the delta since candump started) -- the
        // relative timeline is the running sum of every delta seen so far.
        cumulative_ns += candump_detail::rawTimestampNs(parsed);
        ts_ns = cumulative_ns;
      } else {
        ts_ns = candump_detail::rawTimestampNs(parsed);
      }

      // Resolve the interface's decoder/topic-map bucket once per bus-name
      // CHANGE, not once per frame -- a capture typically stays on the same
      // interface for long runs.
      if (current_topics == nullptr || parsed.interface != last_interface) {
        last_interface = parsed.interface;
        auto [bucket_it, inserted] = topics_by_interface.try_emplace(last_interface);
        current_topics = &bucket_it->second;
        if (inserted) {
          const auto decoder_it = decoders.find(last_interface);
          current_topics->decoder = decoder_it != decoders.end() ? &decoder_it->second : nullptr;
        }
      }
      // Built once per frame, reused by whichever of decoded/raw applies.
      const std::uint64_t key = frameKey(parsed.can_id, parsed.extended);

      pj_can_dbc::DecodeResult result = pj_can_dbc::DecodeResult::kNoMatch;
      std::vector<pj_can_dbc::DecodedSignal> signals;
      if (current_topics->decoder != nullptr) {
        signals = current_topics->decoder->decode(parsed.can_id, parsed.extended, parsed.data, result);
      }

      if (result == pj_can_dbc::DecodeResult::kDecoded && !signals.empty()) {
        ++decoded_frames;
        auto it = current_topics->decoded.find(key);
        if (it == current_topics->decoded.end()) {
          const std::string topic_name = pj_can_dbc::canTopicName(
              std::string_view(parsed.interface), current_topics->decoder->messageName(parsed.can_id, parsed.extended),
              parsed.can_id);
          auto topic = writeHost().ensureTopic(topic_name);
          if (!topic) {
            continue;  // best-effort within the loop
          }
          it = current_topics->decoded.emplace(key, *topic).first;
          ++decoded_topic_count;
        }
        row_fields.clear();
        row_fields.reserve(signals.size());
        for (const auto& sig : signals) {
          row_fields.push_back({.name = sig.name, .value = sig.value});
        }
        (void)writeHost().appendRecord(
            it->second, PJ::Timestamp{ts_ns},
            PJ::Span<const PJ::sdk::NamedFieldValue>(row_fields.data(), row_fields.size()));
        continue;
      }

      if (current_topics->decoder == nullptr) {
        ++no_dictionary;
      } else if (result == pj_can_dbc::DecodeResult::kUndecodable) {
        ++undecodable;
      } else {
        ++unmatched;
      }

      if (!raw_unassigned) {
        continue;
      }
      auto it = current_topics->raw.find(key);
      if (it == current_topics->raw.end()) {
        const std::string topic_name =
            pj_can_dbc::canTopicName(std::string_view(parsed.interface), std::string(), parsed.can_id);
        auto topic = writeHost().ensureTopic(topic_name);
        if (!topic) {
          continue;
        }
        it = current_topics->raw.emplace(key, *topic).first;
      }
      row_fields.clear();
      row_fields.reserve(parsed.data.size());
      for (std::size_t i = 0; i < parsed.data.size(); ++i) {
        row_fields.push_back({.name = byte_field_names[i], .value = static_cast<std::int64_t>(parsed.data[i])});
      }
      (void)writeHost().appendRecord(
          it->second, PJ::Timestamp{ts_ns},
          PJ::Span<const PJ::sdk::NamedFieldValue>(row_fields.data(), row_fields.size()));
    }

    if (cancelled || runtimeHost().isStopRequested()) {
      return PJ::unexpected(std::string("import cancelled"));
    }
    (void)runtimeHost().progressUpdate(total_bytes);

    std::string summary = "Decoded " + std::to_string(decoded_frames) + " CAN frame(s) into " +
                          std::to_string(decoded_topic_count) + " message topic(s)";
    if (no_dictionary > 0) {
      summary += "; " + std::to_string(no_dictionary) + " frame(s) on interfaces with no dictionary";
    }
    if (unmatched > 0) {
      summary += "; " + std::to_string(unmatched) + " frame(s) had no dictionary match";
    }
    if (undecodable > 0) {
      summary += "; " + std::to_string(undecodable) + " matched frame(s) could not be decoded (truncated frame)";
    }
    if (counters.rtr > 0) {
      summary += "; " + std::to_string(counters.rtr) + " RTR frame(s) (recognized, not decoded)";
    }
    if (counters.fd > 0) {
      summary += "; " + std::to_string(counters.fd) + " CAN FD frame(s) (recognized, not decoded)";
    }
    if (counters.xl > 0) {
      summary += "; " + std::to_string(counters.xl) + " CAN XL frame(s) (recognized, not decoded)";
    }
    if (counters.error > 0) {
      summary += "; " + std::to_string(counters.error) + " error frame(s) (recognized, not decoded)";
    }
    // Shared verbatim with CandumpDialog's own preview summary.
    counters.appendErrorDetail(summary);
    counters.appendDropCount(summary);
    counters.appendWallClock(summary);
    if (malformed_count > 0) {
      summary += "; " + std::to_string(malformed_count) + " malformed line(s) (first: ";
      for (std::size_t i = 0; i < first_malformed_lines.size(); ++i) {
        if (i > 0) {
          summary += ", ";
        }
        summary += std::to_string(first_malformed_lines[i]);
      }
      summary += ")";
    }
    runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo, summary);
    return PJ::okStatus();
  }

 private:
  candump_detail::CandumpDialog dialog_;
};

}  // namespace

PJ_DIALOG_PLUGIN(candump_detail::CandumpDialog)
PJ_DATA_SOURCE_PLUGIN(CandumpSource, kCandumpManifest)
