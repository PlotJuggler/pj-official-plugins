#include <pj_base/builtin/asset_video.hpp>
#include <pj_base/builtin/asset_video_codec.hpp>
#include <pj_base/builtin/video_frame.hpp>
#include <pj_base/builtin/video_frame_codec.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_base/sdk/media_metadata.hpp>
#include <pj_base/sdk/platform.hpp>
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
#include <stdexcept>
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

/// Generic MP4 loader with two emission modes (chosen by the `emit_videoframe`
/// config flag, default false):
///
///  - **AssetVideo** (default): registers ONE sdk::AssetVideo entry pointing at
///    the file; the host opens FileVideoSource and decodes from the file.
///
///  - **VideoFrame** (emit_videoframe=true): demux-indexes the container (no
///    decode) and pushes one LAZY sdk::VideoFrame per access unit through a
///    PJ.VideoFrame parser binding. Each entry's bitstream is read from the file
///    on demand (pj_video_demux::LazyAccessUnitReader), so the whole video never
///    lands on the heap. Both modes coexist so the same file can be loaded
///    either way for side-by-side comparison.
///
/// VideoFrame-path codecs: H.264 / H.265 / AV1 (matching the host streaming
/// decoder); any other codec surfaces a clear error from indexFile().
class Mp4Source : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    // Advertise both: the AssetVideo path writes objects directly, the
    // VideoFrame path delegates to a parser binding. The active mode is chosen
    // per-import by `emit_videoframe_`.
    return PJ::kCapabilityDirectIngest | PJ::kCapabilityDelegatedIngest;
  }

  std::string saveConfig() const override {
    return nlohmann::json{{"filepath", filepath_}, {"emit_videoframe", emit_videoframe_}}.dump();
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
    emit_videoframe_ = cfg.value("emit_videoframe", false);
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    // Benchmark/dev override: PJ_MP4_EMIT_VIDEOFRAME=1 forces the lazy VideoFrame
    // path without editing saved config, so the same file can be A/B-loaded.
    const bool emit_vf = emit_videoframe_ || PJ::sdk::getEnv("PJ_MP4_EMIT_VIDEOFRAME").has_value();
    return emit_vf ? importVideoFrames() : importAssetVideo();
  }

 private:
  /// Default path: one file-reference AssetVideo entry (unchanged behavior).
  PJ::Status importAssetVideo() {
    auto meta_or = readMp4Metadata(filepath_);
    if (!meta_or) {
      return PJ::unexpected(meta_or.error());
    }
    const Mp4Metadata& meta = *meta_or;

    const PJ::sdk::SourceObjectWriteHostView* obj = objectWriteHost();
    if (obj == nullptr) {
      return PJ::unexpected(std::string("MP4 plugin: objectWriteHost not bound"));
    }

    const std::string topic_meta = PJ::sdk::MediaMetadataBuilder()
                                       .extraString("builtin_object_type", "kAssetVideo")
                                       .schema(PJ::kSchemaAssetVideo)
                                       .build();
    auto topic = obj->registerTopic("video", topic_meta);
    if (!topic) {
      return PJ::unexpected(topic.error());
    }

    PJ::sdk::AssetVideo asset;
    asset.file_path = filepath_;
    if (meta.creation_time_ns.has_value()) {
      asset.time_origin_ns = PJ::Timestamp{*meta.creation_time_ns};
    }
    // start_ns / end_ns left absent → whole file is playable (single-clip MP4).
    // media_type, width, height, frame_rate: defaults → PJ4 probes via FFmpeg.

    // ObjectStore timestamp equals time_origin_ns per AssetVideo contract;
    // when the MP4 lacks a creation_time tag, fall back to 0 (file-relative).
    const int64_t entry_ts = meta.creation_time_ns.value_or(0);
    const std::vector<uint8_t> bytes = PJ::serializeAssetVideo(asset);
    auto pushed = obj->pushOwned(*topic, PJ::Timestamp{entry_ts}, PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
    if (!pushed) {
      return PJ::unexpected(pushed.error());
    }

    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kInfo,
        "MP4: imported " + filepath_ +
            (meta.creation_time_ns.has_value() ? " (wall-clock anchored)" : " (file-relative; no creation_time tag)"));
    return PJ::okStatus();
  }

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
          binding, PJ::Timestamp{host_ts}, [reader, au, fmt, frame_id, pts_ts]() -> PJ::sdk::PayloadView {
            auto bytes_or = reader->readUnit(au);
            if (!bytes_or) {
              // Surface the read error through the fetcher ABI (a thrown exception
              // becomes a failed pull) instead of recording a successful zero-byte
              // frame.
              throw std::runtime_error("MP4 video read failed: " + bytes_or.error());
            }
            PJ::sdk::VideoFrame vf;
            vf.timestamp_ns = pts_ts;
            vf.frame_id = frame_id;
            vf.format = fmt;
            vf.data = PJ::Span<const uint8_t>(bytes_or->data(), bytes_or->size());
            auto serialized = std::make_shared<std::vector<uint8_t>>(PJ::serializeVideoFrame(vf));
            return PJ::sdk::PayloadView{serialized};
          });
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
  bool emit_videoframe_ = false;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(Mp4Source, kMp4Manifest)
