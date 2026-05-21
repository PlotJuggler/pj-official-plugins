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

namespace {

/// Parsed metadata of one MP4 file. The plugin opens the container, reads
/// these fields, and closes — no frames are decoded.
struct Mp4Metadata {
  std::optional<int64_t> creation_time_ns;  // epoch ns; nullopt if absent / unparseable
  int64_t duration_ns = 0;                  // 0 if unknown
  std::string codec;                        // e.g. "h264", "av1"; empty if unknown
  std::string creation_time_iso;            // raw tag value when present, kept for display
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
    meta.creation_time_iso = tag->value;
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
/// frames, then registers a metadata-only ObjectStore topic carrying
/// `video_file_path` (so the host renders via FileVideoSource) and, when the
/// MP4 carries a `creation_time` tag, `media_start_ns` so the host can anchor
/// playback on a wall-clock timeline.
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

    PJ::sdk::MediaMetadataBuilder builder;
    builder.mediaClass("video").encoding(meta.codec).extraString("video_file_path", filepath_);
    if (meta.creation_time_ns.has_value()) {
      builder.extra("media_start_ns", std::to_string(*meta.creation_time_ns));
      builder.extraString("creation_time", meta.creation_time_iso);
    }
    if (meta.duration_ns > 0) {
      builder.extra("media_duration_ns", std::to_string(meta.duration_ns));
    }

    auto topic = obj->registerTopic("video", builder.build());
    if (!topic) {
      return PJ::unexpected(topic.error());
    }
    return PJ::okStatus();
  }

 private:
  std::string filepath_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(Mp4Source, kMp4Manifest)
