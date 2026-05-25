#include <pj_base/builtin/asset_video.hpp>
#include <pj_base/builtin/asset_video_codec.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_base/sdk/media_metadata.hpp>

#include "mp4_iso8601.hpp"
#include "mp4_manifest.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace {

/// Parsed metadata of one MP4 file. The plugin opens the container, reads
/// these fields, and closes — no frames are decoded.
struct Mp4Metadata {
  std::optional<int64_t> creation_time_ns;  // epoch ns; nullopt if absent / unparseable
  int64_t duration_ns = 0;                  // 0 if unknown
  std::string codec;                        // e.g. "h264", "av1"; empty if unknown
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

  // AVFormatContext::duration is in AV_TIME_BASE units (microseconds).
  if (ctx->duration > 0) {
    meta.duration_ns = ctx->duration * 1000;
  }

  for (unsigned i = 0; i < ctx->nb_streams; ++i) {
    AVStream* st = ctx->streams[i];
    if (st->codecpar != nullptr && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
      if (codec != nullptr && codec->name != nullptr) {
        meta.codec = codec->name;
      }
      break;
    }
  }

  avformat_close_input(&ctx);
  return meta;
}

/// Generic MP4 loader. For each .mp4 the user opens, reads container metadata
/// (creation_time, duration, codec) via libavformat without decoding any
/// frames, then registers ONE sdk::AssetVideo ObjectStore entry pointing at
/// the file. pj_app's Media2DDockWidget deserializes the entry, opens
/// FileVideoSource on file_path, applies time_origin_ns as wall-clock anchor
/// (when the MP4 carried a creation_time tag), and drives the decoder from
/// the global tracker (PJ4 ARCH §4.5).
class Mp4Source : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest;
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
    if (meta.duration_ns > 0) {
      asset.duration_ns = meta.duration_ns;
    }
    // media_type, width, height, frame_rate: defaults → PJ4 probes via FFmpeg.

    // ObjectStore timestamp equals time_origin_ns per AssetVideo contract;
    // when the MP4 lacks a creation_time tag, fall back to 0 (file-relative).
    const int64_t entry_ts = meta.creation_time_ns.value_or(0);
    const std::vector<uint8_t> bytes = PJ::serializeAssetVideo(asset);
    auto pushed = obj->pushOwned(*topic, PJ::Timestamp{entry_ts}, PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
    if (!pushed) {
      return PJ::unexpected(pushed.error());
    }
    return PJ::okStatus();
  }

 private:
  std::string filepath_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(Mp4Source, kMp4Manifest)
