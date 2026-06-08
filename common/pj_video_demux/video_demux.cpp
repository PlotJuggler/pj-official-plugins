// pj_video_demux implementation. Indexing uses libavformat to walk packets
// (no decode); the fetch path is pure file I/O + AVCC→Annex-B rewrite (no libav)
// so the compressed video is never resident — only the index + SPS/PPS are.
#include "pj_video_demux/video_demux.hpp"

#include <ios>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace PJ {
namespace video_demux {

namespace {

constexpr uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

struct ParsedAvcc {
  std::vector<uint8_t> params;  // SPS+PPS as Annex-B
  int nal_length_size = 4;
};

void appendAnnexBNal(std::vector<uint8_t>& out, const uint8_t* nal, size_t len) {
  out.insert(out.end(), kStartCode, kStartCode + 4);
  out.insert(out.end(), nal, nal + len);
}

// Parse an `avcC` (AVCDecoderConfigurationRecord) into Annex-B SPS/PPS + the
// NAL length-prefix width. MP4 H.264 extradata always starts with version=1.
Expected<ParsedAvcc> parseAvcc(Span<const uint8_t> ed) {
  const uint8_t* d = ed.data();
  const size_t n = ed.size();
  if (n < 7 || d[0] != 1) {
    return unexpected(std::string("video_demux: unrecognized H.264 extradata (not avcC)"));
  }
  ParsedAvcc out;
  out.nal_length_size = (d[4] & 0x03) + 1;

  size_t off = 5;
  const int num_sps = d[off] & 0x1F;
  off += 1;
  for (int i = 0; i < num_sps; ++i) {
    if (off + 2 > n) {
      return unexpected(std::string("video_demux: truncated avcC (SPS length)"));
    }
    const size_t len = (static_cast<size_t>(d[off]) << 8) | d[off + 1];
    off += 2;
    if (off + len > n) {
      return unexpected(std::string("video_demux: truncated avcC (SPS data)"));
    }
    appendAnnexBNal(out.params, d + off, len);
    off += len;
  }
  if (off + 1 > n) {
    return unexpected(std::string("video_demux: truncated avcC (PPS count)"));
  }
  const int num_pps = d[off];
  off += 1;
  for (int i = 0; i < num_pps; ++i) {
    if (off + 2 > n) {
      return unexpected(std::string("video_demux: truncated avcC (PPS length)"));
    }
    const size_t len = (static_cast<size_t>(d[off]) << 8) | d[off + 1];
    off += 2;
    if (off + len > n) {
      return unexpected(std::string("video_demux: truncated avcC (PPS data)"));
    }
    appendAnnexBNal(out.params, d + off, len);
    off += len;
  }
  return out;
}

}  // namespace

std::vector<uint8_t> avccToAnnexB(
    Span<const uint8_t> avcc, Span<const uint8_t> annexb_params, bool keyframe, int nal_length_size) {
  std::vector<uint8_t> out;
  out.reserve(avcc.size() + (keyframe ? annexb_params.size() : 0) + 16);
  if (keyframe && annexb_params.size() > 0) {
    out.insert(out.end(), annexb_params.data(), annexb_params.data() + annexb_params.size());
  }
  if (nal_length_size < 1 || nal_length_size > 4) {
    nal_length_size = 4;
  }
  const uint8_t* d = avcc.data();
  const size_t n = avcc.size();
  size_t off = 0;
  while (off + static_cast<size_t>(nal_length_size) <= n) {
    size_t len = 0;
    for (int i = 0; i < nal_length_size; ++i) {
      len = (len << 8) | d[off + static_cast<size_t>(i)];
    }
    off += static_cast<size_t>(nal_length_size);
    if (len == 0 || off + len > n) {
      break;  // truncated / malformed — stop at the last clean NAL
    }
    appendAnnexBNal(out, d + off, len);
    off += len;
  }
  return out;
}

Expected<VideoIndex> indexFile(const std::string& path) {
  AVFormatContext* ctx = nullptr;
  if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0) {
    return unexpected("video_demux: cannot open " + path);
  }
  struct CtxCloser {
    AVFormatContext** c;
    ~CtxCloser() {
      if (*c != nullptr) {
        avformat_close_input(c);
      }
    }
  } ctx_closer{&ctx};

  if (avformat_find_stream_info(ctx, nullptr) < 0) {
    return unexpected("video_demux: cannot read stream info: " + path);
  }

  int vstream = -1;
  for (unsigned i = 0; i < ctx->nb_streams; ++i) {
    if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      vstream = static_cast<int>(i);
      break;
    }
  }
  if (vstream < 0) {
    return unexpected("video_demux: no video stream in " + path);
  }

  AVStream* st = ctx->streams[vstream];
  AVCodecParameters* par = st->codecpar;
  if (par->codec_id != AV_CODEC_ID_H264) {
    return unexpected(
        "video_demux: only H.264 is supported in the spike (codec_id=" +
        std::to_string(static_cast<int>(par->codec_id)) + ")");
  }
  if (par->extradata == nullptr || par->extradata_size < 7) {
    return unexpected("video_demux: missing/short H.264 avcC extradata in " + path);
  }
  auto parsed = parseAvcc(Span<const uint8_t>(par->extradata, static_cast<size_t>(par->extradata_size)));
  if (!parsed) {
    return unexpected(parsed.error());
  }

  VideoIndex index;
  index.format = "h264";
  index.width = par->width;
  index.height = par->height;
  index.annexb_params = std::move(parsed->params);
  index.nal_length_size = parsed->nal_length_size;

  const AVRational tb = st->time_base;
  const AVRational ns = {1, 1000000000};

  AVPacket* pkt = av_packet_alloc();
  if (pkt == nullptr) {
    return unexpected(std::string("video_demux: av_packet_alloc failed"));
  }
  struct PktFreer {
    AVPacket** p;
    ~PktFreer() {
      av_packet_free(p);
    }
  } pkt_freer{&pkt};

  while (av_read_frame(ctx, pkt) >= 0) {
    if (pkt->stream_index == vstream) {
      if (pkt->pos < 0) {
        return unexpected(
            std::string("video_demux: packet has no byte offset (pos<0); container not locator-addressable"));
      }
      const int64_t raw_dts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
      const int64_t raw_pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
      AccessUnit au;
      au.dts_ns = (raw_dts != AV_NOPTS_VALUE) ? av_rescale_q(raw_dts, tb, ns) : 0;
      au.pts_ns = (raw_pts != AV_NOPTS_VALUE) ? av_rescale_q(raw_pts, tb, ns) : au.dts_ns;
      au.file_offset = pkt->pos;
      au.size = pkt->size;
      au.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
      index.units.push_back(au);
    }
    av_packet_unref(pkt);
  }

  if (index.units.empty()) {
    return unexpected("video_demux: no video packets in " + path);
  }
  return index;
}

std::shared_ptr<LazyAnnexBReader> LazyAnnexBReader::create(
    std::string path, std::vector<uint8_t> annexb_params, int nal_length_size) {
  return std::shared_ptr<LazyAnnexBReader>(
      new LazyAnnexBReader(std::move(path), std::move(annexb_params), nal_length_size));
}

LazyAnnexBReader::LazyAnnexBReader(std::string path, std::vector<uint8_t> annexb_params, int nal_length_size)
    : path_(std::move(path)), annexb_params_(std::move(annexb_params)), nal_length_size_(nal_length_size) {}

Expected<std::vector<uint8_t>> LazyAnnexBReader::readUnit(const AccessUnit& unit) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_attempted_) {
    open_attempted_ = true;
    stream_.open(path_, std::ios::binary);
    open_failed_ = !stream_.is_open();
  }
  if (open_failed_) {
    return unexpected("video_demux: cannot open " + path_);
  }
  if (unit.size <= 0) {
    return unexpected(std::string("video_demux: invalid access-unit size"));
  }

  std::vector<uint8_t> avcc(static_cast<size_t>(unit.size));
  stream_.clear();  // drop any EOF/fail state from a prior read
  stream_.seekg(unit.file_offset, std::ios::beg);
  stream_.read(reinterpret_cast<char*>(avcc.data()), unit.size);
  if (stream_.gcount() != static_cast<std::streamsize>(unit.size)) {
    return unexpected("video_demux: short read at offset " + std::to_string(unit.file_offset));
  }
  return avccToAnnexB(
      Span<const uint8_t>(avcc.data(), avcc.size()), Span<const uint8_t>(annexb_params_.data(), annexb_params_.size()),
      unit.keyframe, nal_length_size_);
}

}  // namespace video_demux
}  // namespace PJ
