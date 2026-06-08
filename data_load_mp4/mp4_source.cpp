#include <pj_base/builtin/video_frame_codec.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_video_demux/video_demux.hpp>

#include "mp4_iso8601.hpp"
#include "mp4_manifest.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace {

/// Parsed metadata of one MP4 file. The plugin opens the container, reads
/// these fields, and closes — no frames are decoded.
struct Mp4Metadata {
  std::optional<int64_t> creation_time_ns;  // epoch ns; nullopt if absent / unparseable
};

[[nodiscard]] PJ::Expected<Mp4Metadata> readMp4Metadata(const std::string& path) {
  AVFormatContext* ctx = nullptr;
  if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0) {
    return PJ::unexpected("cannot open MP4: " + path);
  }
  if (avformat_find_stream_info(ctx, nullptr) < 0) {
    avformat_close_input(&ctx);
    return PJ::unexpected("cannot read stream info: " + path);
  }
  Mp4Metadata meta;

  AVDictionaryEntry* tag = av_dict_get(ctx->metadata, "creation_time", nullptr, 0);
  if (tag != nullptr && tag->value != nullptr) {
    meta.creation_time_ns = pj_mp4::parseIso8601ToEpochNs(tag->value);
  }

  avformat_close_input(&ctx);
  return meta;
}

/// Generic MP4 loader: demux-indexes the container (no decode) and pushes one
/// LAZY sdk::VideoFrame per access unit through a PJ.VideoFrame parser binding.
/// Each entry's bitstream is read from the file on demand
/// (pj_video_demux::LazyAccessUnitReader), so the whole video never lands on the
/// heap. Codecs: H.264 / H.265 / AV1 (matching the host streaming decoder); any
/// other codec surfaces a clear error from indexFile().
class Mp4Source : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    // Lazy VideoFrame entries are pushed through a parser binding (delegated).
    return PJ::kCapabilityDelegatedIngest;
  }

  std::string saveConfig() const override {
    return nlohmann::json{{"filepath", filepath_}}.dump();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected(std::string("invalid MP4 config JSON"));
    }
    filepath_ = cfg.value("filepath", std::string{});
    if (filepath_.empty()) {
      return PJ::unexpected(std::string("MP4 config missing required `filepath` field"));
    }
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    return importVideoFrames();
  }

 private:
  /// Lazy per-frame PJ.VideoFrame entries over the original file.
  PJ::Status importVideoFrames() {
    auto idx_or = PJ::video_demux::indexFile(filepath_);
    if (!idx_or) {
      return PJ::unexpected(idx_or.error());
    }
    const PJ::video_demux::VideoIndex& idx = *idx_or;
    if (idx.units.empty()) {
      return PJ::unexpected("MP4: no video access units in " + filepath_);
    }

    auto meta_or = readMp4Metadata(filepath_);
    if (!meta_or) {
      return PJ::unexpected(meta_or.error());
    }
    const std::optional<int64_t> creation_time_ns = meta_or->creation_time_ns;
    const int64_t origin_ns = creation_time_ns.value_or(0);
    const int64_t base_dts_ns = idx.units.front().dts_ns;

    // Bind the protobuf parser for PJ.VideoFrame (descriptor-free canonical
    // fast path). The host unwraps each pushed entry via deserializeVideoFrameView.
    auto binding_or = runtimeHost().ensureParserBinding({
        .topic_name = "video",
        .parser_encoding = "protobuf",
        .type_name = PJ::kSchemaVideoFrame,
        .schema = {},
        .parser_config_json = {},
    });
    if (!binding_or) {
      return PJ::unexpected("MP4: ensureParserBinding(PJ.VideoFrame) failed: " + binding_or.error());
    }
    const PJ::ParserBindingHandle binding = *binding_or;

    // Shared, lazily-opened reader: each fetch reads exactly one access unit
    // from the file. Captured by shared_ptr so it outlives this call.
    auto reader =
        PJ::video_demux::LazyAccessUnitReader::create(filepath_, idx.format, idx.param_sets, idx.nal_length_size);
    const std::string frame_id = "camera";
    const std::string fmt = idx.format;

    for (const PJ::video_demux::AccessUnit& au : idx.units) {
      // ObjectStore key is DTS-based (monotonic decode order); the embedded
      // VideoFrame.timestamp is PTS-based (presentation). Both rebased to the
      // creation_time anchor (or to 0 when the file has no creation_time).
      const int64_t host_ts = origin_ns + (au.dts_ns - base_dts_ns);
      const int64_t pts_ts = origin_ns + (au.pts_ns - base_dts_ns);

      auto status = runtimeHost().pushMessage(
          binding, PJ::Timestamp{host_ts}, PJ::video_demux::makeVideoFrameFetcher(reader, au, fmt, frame_id, pts_ts));
      if (!status) {
        return PJ::unexpected("MP4: pushMessage failed: " + status.error());
      }
    }

    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kInfo,
        "MP4: imported " + std::to_string(idx.units.size()) + " lazy VideoFrame entries from " + filepath_ +
            (creation_time_ns.has_value() ? " (wall-clock anchored)" : " (file-relative)"));
    return PJ::okStatus();
  }

  std::string filepath_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(Mp4Source, kMp4Manifest)
