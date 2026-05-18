#include "video_ingest.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <cstddef>
#include <cstdint>
#include <string>

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
struct CodecCtx {
  AVCodecContext* ctx = nullptr;
  ~CodecCtx() {
    if (ctx != nullptr) {
      avcodec_free_context(&ctx);
    }
  }
};
struct SwsCtx {
  SwsContext* ctx = nullptr;
  ~SwsCtx() {
    if (ctx != nullptr) {
      sws_freeContext(ctx);
    }
  }
};
struct FramePtr {
  AVFrame* f = av_frame_alloc();
  ~FramePtr() {
    if (f != nullptr) {
      av_frame_free(&f);
    }
  }
};
struct PacketPtr {
  AVPacket* pkt = av_packet_alloc();
  ~PacketPtr() {
    if (pkt != nullptr) {
      av_packet_free(&pkt);
    }
  }
};

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
  AVCodecParameters* par = fmt.ctx->streams[video_idx]->codecpar;

  // FFmpeg's native "av1" decoder is hardware-only (fails on headless boxes
  // with "Failed to get pixel format"); the software AV1 decoder is the
  // separate libdav1d. Pick it by name so we don't get the HW stub.
  const AVCodec* dec = nullptr;
  if (par->codec_id == AV_CODEC_ID_AV1) {
    dec = avcodec_find_decoder_by_name("libdav1d");
  }
  if (dec == nullptr) {
    dec = avcodec_find_decoder(par->codec_id);
  }
  if (dec == nullptr) {
    return PJ::unexpected(
        std::string("no decoder for codec '") + avcodec_get_name(par->codec_id) + "' in " + mp4_path);
  }
  CodecCtx dctx;
  dctx.ctx = avcodec_alloc_context3(dec);
  if (dctx.ctx == nullptr || avcodec_parameters_to_context(dctx.ctx, par) < 0 ||
      avcodec_open2(dctx.ctx, dec, nullptr) < 0) {
    return PJ::unexpected("cannot open decoder for " + mp4_path);
  }

  const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  if (enc == nullptr) {
    return PJ::unexpected("MJPEG encoder unavailable");
  }

  CodecCtx ectx;   // opened lazily once the first frame's geometry is known
  SwsCtx sws;
  FramePtr dframe;
  FramePtr jframe;
  PacketPtr pkt;
  PacketPtr jpkt;
  if (dframe.f == nullptr || jframe.f == nullptr || pkt.pkt == nullptr || jpkt.pkt == nullptr) {
    return PJ::unexpected("FFmpeg alloc failed");
  }

  const auto last_ts = frame_ts_ns.back();
  std::size_t ordinal = 0;
  bool init_done = false;
  PJ::Status fail = PJ::okStatus();

  auto initEncoderAndScaler = [&](int w, int h, AVPixelFormat src_fmt) -> bool {
    ectx.ctx = avcodec_alloc_context3(enc);
    if (ectx.ctx == nullptr) {
      return false;
    }
    ectx.ctx->width = w;
    ectx.ctx->height = h;
    ectx.ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    ectx.ctx->color_range = AVCOL_RANGE_JPEG;
    ectx.ctx->time_base = AVRational{1, 25};
    ectx.ctx->flags |= AV_CODEC_FLAG_QSCALE;
    ectx.ctx->global_quality = FF_QP2LAMBDA * 3;  // 1=best … 31=worst (high quality)
    if (avcodec_open2(ectx.ctx, enc, nullptr) < 0) {
      return false;
    }
    sws.ctx = sws_getContext(
        w, h, src_fmt, w, h, AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (sws.ctx == nullptr) {
      return false;
    }
    jframe.f->format = AV_PIX_FMT_YUVJ420P;
    jframe.f->width = w;
    jframe.f->height = h;
    return av_frame_get_buffer(jframe.f, 32) == 0;
  };

  auto encodeAndPush = [&](AVFrame* frame) -> PJ::Status {
    if (avcodec_send_frame(ectx.ctx, frame) < 0) {
      return PJ::unexpected("JPEG encode (send) failed: " + mp4_path);
    }
    while (true) {
      const int r = avcodec_receive_packet(ectx.ctx, jpkt.pkt);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
        break;
      }
      if (r < 0) {
        return PJ::unexpected("JPEG encode (receive) failed: " + mp4_path);
      }
      const int64_t ts = ordinal < frame_ts_ns.size() ? frame_ts_ns[ordinal] : last_ts;
      auto st = host.pushOwned(
          topic, PJ::Timestamp{ts},
          PJ::Span<const uint8_t>(jpkt.pkt->data, static_cast<std::size_t>(jpkt.pkt->size)));
      av_packet_unref(jpkt.pkt);
      ++ordinal;
      if (!st) {
        return st;
      }
    }
    return PJ::okStatus();
  };

  auto drainDecoder = [&]() -> PJ::Status {
    while (true) {
      const int r = avcodec_receive_frame(dctx.ctx, dframe.f);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
        break;
      }
      if (r < 0) {
        return PJ::unexpected("decode (receive) failed: " + mp4_path);
      }
      if (!init_done) {
        if (!initEncoderAndScaler(
                dframe.f->width, dframe.f->height, static_cast<AVPixelFormat>(dframe.f->format))) {
          return PJ::unexpected("encoder/scaler init failed: " + mp4_path);
        }
        init_done = true;
      }
      sws_scale(
          sws.ctx, dframe.f->data, dframe.f->linesize, 0, dframe.f->height, jframe.f->data,
          jframe.f->linesize);
      jframe.f->pts = static_cast<int64_t>(ordinal);
      jframe.f->quality = ectx.ctx->global_quality;
      auto st = encodeAndPush(jframe.f);
      if (!st) {
        return st;
      }
    }
    return PJ::okStatus();
  };

  while (av_read_frame(fmt.ctx, pkt.pkt) >= 0) {
    const bool is_video = pkt.pkt->stream_index == video_idx;
    if (is_video && avcodec_send_packet(dctx.ctx, pkt.pkt) >= 0) {
      fail = drainDecoder();
    }
    av_packet_unref(pkt.pkt);
    if (!fail) {
      return fail;
    }
  }

  // Flush the decoder, then the encoder.
  avcodec_send_packet(dctx.ctx, nullptr);
  fail = drainDecoder();
  if (!fail) {
    return fail;
  }
  if (init_done) {
    fail = encodeAndPush(nullptr);
    if (!fail) {
      return fail;
    }
  }
  return PJ::okStatus();
}

}  // namespace lerobot
