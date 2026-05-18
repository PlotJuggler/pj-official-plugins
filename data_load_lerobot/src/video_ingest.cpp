#include "video_ingest.hpp"

#include <pj_base/sdk/media_metadata.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <cstddef>
#include <cstdint>

namespace lerobot {
namespace {

// RAII for the FFmpeg objects so every error path frees cleanly (no
// exceptions cross the ABI; the SDK trampoline catches anyway).
struct FormatCtx {
  AVFormatContext* ctx = nullptr;
  ~FormatCtx() {
    if (ctx != nullptr) {
      avformat_close_input(&ctx);
    }
  }
};
struct BsfCtx {
  AVBSFContext* ctx = nullptr;
  ~BsfCtx() {
    if (ctx != nullptr) {
      av_bsf_free(&ctx);
    }
  }
};
struct PacketPtr {
  AVPacket* pkt = nullptr;
  explicit PacketPtr() : pkt(av_packet_alloc()) {}
  ~PacketPtr() {
    if (pkt != nullptr) {
      av_packet_free(&pkt);
    }
  }
};

const char* bsfNameForCodec(AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_H264: return "h264_mp4toannexb";
    case AV_CODEC_ID_HEVC: return "hevc_mp4toannexb";
    default: return nullptr;  // mp4v/av1/… → push packets as-is
  }
}

}  // namespace

PJ::Status ingestEpisodeVideo(
    const PJ::sdk::SourceObjectWriteHostView& host,
    PJ::sdk::ObjectTopicHandle topic,
    const std::string& mp4_path,
    const std::string& /*codec*/,
    const std::vector<int64_t>& frame_ts_ns) {
  if (frame_ts_ns.empty()) {
    return PJ::okStatus();  // nothing to align against; skip silently
  }

  FormatCtx fmt;
  if (avformat_open_input(&fmt.ctx, mp4_path.c_str(), nullptr, nullptr) < 0) {
    return PJ::unexpected("cannot open video: " + mp4_path);
  }
  if (avformat_find_stream_info(fmt.ctx, nullptr) < 0) {
    return PJ::unexpected("cannot read stream info: " + mp4_path);
  }

  int video_idx = -1;
  for (unsigned i = 0; i < fmt.ctx->nb_streams; ++i) {
    if (fmt.ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_idx = static_cast<int>(i);
      break;
    }
  }
  if (video_idx < 0) {
    return PJ::unexpected("no video stream in " + mp4_path);
  }
  AVStream* stream = fmt.ctx->streams[video_idx];

  // Optional annex-B bitstream filter for H.264/H.265.
  BsfCtx bsf;
  const char* bsf_name = bsfNameForCodec(stream->codecpar->codec_id);
  if (bsf_name != nullptr) {
    const AVBitStreamFilter* filter = av_bsf_get_by_name(bsf_name);
    if (filter == nullptr || av_bsf_alloc(filter, &bsf.ctx) < 0 ||
        avcodec_parameters_copy(bsf.ctx->par_in, stream->codecpar) < 0) {
      return PJ::unexpected(std::string("bitstream filter init failed: ") + bsf_name);
    }
    bsf.ctx->time_base_in = stream->time_base;
    if (av_bsf_init(bsf.ctx) < 0) {
      return PJ::unexpected(std::string("bitstream filter init failed: ") + bsf_name);
    }
  }

  PacketPtr pkt;
  PacketPtr out;
  if (pkt.pkt == nullptr || out.pkt == nullptr) {
    return PJ::unexpected("av_packet_alloc failed");
  }

  const auto last_ts = frame_ts_ns.back();
  std::size_t ordinal = 0;

  auto pushOne = [&](const uint8_t* data, int size) -> PJ::Status {
    const int64_t ts = ordinal < frame_ts_ns.size()
                            ? frame_ts_ns[ordinal]
                            : last_ts;  // clamp: never assume packet_count==length
    auto st = host.pushOwned(
        topic, PJ::Timestamp{ts},
        PJ::Span<const uint8_t>(data, static_cast<std::size_t>(size)));
    ++ordinal;
    return st;
  };

  while (av_read_frame(fmt.ctx, pkt.pkt) >= 0) {
    if (pkt.pkt->stream_index != video_idx) {
      av_packet_unref(pkt.pkt);
      continue;
    }
    if (bsf.ctx != nullptr) {
      if (av_bsf_send_packet(bsf.ctx, pkt.pkt) >= 0) {
        while (av_bsf_receive_packet(bsf.ctx, out.pkt) >= 0) {
          auto st = pushOne(out.pkt->data, out.pkt->size);
          av_packet_unref(out.pkt);
          if (!st) {
            av_packet_unref(pkt.pkt);
            return st;
          }
        }
      }
    } else {
      auto st = pushOne(pkt.pkt->data, pkt.pkt->size);
      if (!st) {
        av_packet_unref(pkt.pkt);
        return st;
      }
    }
    av_packet_unref(pkt.pkt);
  }
  return PJ::okStatus();
}

}  // namespace lerobot
