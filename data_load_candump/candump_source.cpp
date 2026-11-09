#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <pj_can_dbc/dictionary_loader.hpp>
#include <pj_can_dbc/frame_router.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "candump_dialog.hpp"
#include "candump_manifest.hpp"
#include "candump_parser.hpp"

namespace {

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
        std::string warnings;
        const PJ::Status status = pj_can_dbc::loadDictionary(decoder, path, warnings);
        if (!warnings.empty()) {
          runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, warnings);
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

    // One decoder per interface; the router resolves it (and the topic maps) once per bus-name change.
    pj_can_dbc::CanFrameRouter router(dialog_.rawUnassigned());
    for (const auto& [iface, decoder] : decoders) {
      router.setDecoder(iface, &decoder);
    }

    std::uint64_t seen = 0;
    std::uint64_t line_no = 0;
    std::uint64_t bytes_read = 0;
    candump_detail::RecognizedCounters counters;
    std::uint64_t malformed_count = 0;
    std::vector<std::uint64_t> first_malformed_lines;

    candump_detail::TimelineClock clock(mode);

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
      // Fed every line, before the kind switch (see TimelineClock).
      const std::optional<std::int64_t> timeline_ns = clock.advance(parsed);
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

      // A kData line always has a numeric timestamp (parseCommonPrefix only
      // continues into the body when it does).
      const std::int64_t ts_ns = *timeline_ns;

      router.route(writeHost(), parsed.interface, parsed.can_id, parsed.extended, parsed.data, ts_ns);
    }

    if (cancelled || runtimeHost().isStopRequested()) {
      return PJ::unexpected(std::string("import cancelled"));
    }
    (void)runtimeHost().progressUpdate(total_bytes);

    const pj_can_dbc::RouterCounts& routed = router.counts();
    std::string summary = "Decoded " + std::to_string(routed.decoded) + " CAN frame(s) into " +
                          std::to_string(routed.decoded_topics) + " message topic(s)";
    if (routed.no_dictionary > 0) {
      summary += "; " + std::to_string(routed.no_dictionary) + " frame(s) on interfaces with no dictionary";
    }
    if (routed.unmatched > 0) {
      summary += "; " + std::to_string(routed.unmatched) + " frame(s) had no dictionary match";
    }
    if (routed.undecodable > 0) {
      summary += "; " + std::to_string(routed.undecodable) + " matched frame(s) could not be decoded (truncated frame)";
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
