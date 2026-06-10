// pj_video_demux implementation. Indexing uses libavformat to walk packets
// (no decode); the fetch path is pure file I/O + parameter-set rewrite (no libav)
// so the compressed video is never resident — only the index + parameter sets.
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

struct ParsedParamSets {
  std::vector<uint8_t> params;  // SPS/PPS (+VPS for HEVC) as Annex-B.
  int nal_length_size = 4;
};

void appendAnnexBNal(std::vector<uint8_t>& out, const uint8_t* nal, size_t len) {
  out.insert(out.end(), kStartCode, kStartCode + 4);
  out.insert(out.end(), nal, nal + len);
}

// Parse an `avcC` (AVCDecoderConfigurationRecord) into Annex-B SPS/PPS + the
// NAL length-prefix width. MP4 H.264 extradata always starts with version=1.
Expected<ParsedParamSets> parseAvcc(Span<const uint8_t> ed) {
  const uint8_t* d = ed.data();
  const size_t n = ed.size();
  if (n < 7 || d[0] != 1) {
    return unexpected(std::string("video_demux: unrecognized H.264 extradata (not avcC)"));
  }
  ParsedParamSets out;
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

// Parse an `hvcC` (HEVCDecoderConfigurationRecord) into Annex-B VPS/SPS/PPS +
// the NAL length-prefix width. Layout: a 22-byte fixed header (version=1 at
// byte 0, lengthSizeMinusOne in the low 2 bits of byte 21), then byte 22 =
// numOfArrays, then per array: {1 type byte, uint16 numNalus, numNalus×(uint16
// len + NAL bytes)}. Only the parameter-set arrays (NAL types 32/33/34) are kept.
Expected<ParsedParamSets> parseHvcc(Span<const uint8_t> ed) {
  const uint8_t* d = ed.data();
  const size_t n = ed.size();
  if (n < 23 || d[0] != 1) {
    return unexpected(std::string("video_demux: unrecognized HEVC extradata (not hvcC)"));
  }
  ParsedParamSets out;
  out.nal_length_size = (d[21] & 0x03) + 1;
  const int num_arrays = d[22];
  size_t off = 23;
  for (int a = 0; a < num_arrays; ++a) {
    if (off + 3 > n) {
      return unexpected(std::string("video_demux: truncated hvcC (array header)"));
    }
    const int nal_type = d[off] & 0x3F;  // VPS=32, SPS=33, PPS=34
    off += 1;
    const int num_nalus = (static_cast<int>(d[off]) << 8) | d[off + 1];
    off += 2;
    const bool is_param_set = (nal_type == 32 || nal_type == 33 || nal_type == 34);
    for (int i = 0; i < num_nalus; ++i) {
      if (off + 2 > n) {
        return unexpected(std::string("video_demux: truncated hvcC (NAL length)"));
      }
      const size_t len = (static_cast<size_t>(d[off]) << 8) | d[off + 1];
      off += 2;
      if (off + len > n) {
        return unexpected(std::string("video_demux: truncated hvcC (NAL data)"));
      }
      if (is_param_set) {
        appendAnnexBNal(out.params, d + off, len);
      }
      off += len;
    }
  }
  return out;
}

// Parse an `av1C` (AV1CodecConfigurationRecord) into the raw sequence-header
// configOBUs. Layout: byte 0 = marker(1)|version(7) (= 0x81); the configOBUs —
// the sequence header, with obu_has_size_field set — start at byte 4. They are
// returned verbatim (no start codes; AV1 has none) for prepending to keyframes.
Expected<std::vector<uint8_t>> parseAv1c(Span<const uint8_t> ed) {
  const uint8_t* d = ed.data();
  const size_t n = ed.size();
  if (n <= 4 || (d[0] & 0x80) == 0) {
    return unexpected(std::string("video_demux: unrecognized AV1 extradata (not av1C)"));
  }
  return std::vector<uint8_t>(d + 4, d + n);
}

}  // namespace

std::vector<uint8_t> avccToAnnexB(
    Span<const uint8_t> sample, Span<const uint8_t> param_sets, bool keyframe, int nal_length_size) {
  std::vector<uint8_t> out;
  out.reserve(sample.size() + (keyframe ? param_sets.size() : 0) + 16);
  if (keyframe && param_sets.size() > 0) {
    out.insert(out.end(), param_sets.data(), param_sets.data() + param_sets.size());
  }
  if (nal_length_size < 1 || nal_length_size > 4) {
    nal_length_size = 4;
  }
  const uint8_t* d = sample.data();
  const size_t n = sample.size();
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

std::vector<uint8_t> prependAv1SeqHeader(Span<const uint8_t> obu_sample, Span<const uint8_t> seq_obus, bool keyframe) {
  std::vector<uint8_t> out;
  out.reserve(obu_sample.size() + (keyframe ? seq_obus.size() : 0));
  if (keyframe && seq_obus.size() > 0) {
    out.insert(out.end(), seq_obus.data(), seq_obus.data() + seq_obus.size());
  }
  out.insert(out.end(), obu_sample.data(), obu_sample.data() + obu_sample.size());
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
  const Span<const uint8_t> extradata(
      par->extradata, par->extradata == nullptr ? 0u : static_cast<size_t>(par->extradata_size));

  VideoIndex index;
  if (par->codec_id == AV_CODEC_ID_H264) {
    index.format = "h264";
    auto parsed = parseAvcc(extradata);
    if (!parsed) {
      return unexpected(parsed.error());
    }
    index.param_sets = std::move(parsed->params);
    index.nal_length_size = parsed->nal_length_size;
  } else if (par->codec_id == AV_CODEC_ID_HEVC) {
    index.format = "h265";
    auto parsed = parseHvcc(extradata);
    if (!parsed) {
      return unexpected(parsed.error());
    }
    index.param_sets = std::move(parsed->params);
    index.nal_length_size = parsed->nal_length_size;
  } else if (par->codec_id == AV_CODEC_ID_AV1) {
    index.format = "av1";
    auto seq = parseAv1c(extradata);
    if (!seq) {
      return unexpected(seq.error());
    }
    index.param_sets = std::move(*seq);
    index.nal_length_size = 0;  // AV1 has no length-prefixed NALs.
  } else {
    return unexpected(
        "video_demux: unsupported codec (only h264/h265/av1; codec_id=" +
        std::to_string(static_cast<int>(par->codec_id)) + ") in " + path);
  }
  // H.264/H.265 keyframes are only self-decodable with their SPS/PPS(/VPS)
  // prepended; an empty param set (malformed/stripped config box, or an hvcC
  // carrying no parameter-set arrays) would silently emit undecodable keyframes.
  // (AV1 always yields a non-empty seq header — parseAv1c rejects a 4-byte box.)
  if (index.format != "av1" && index.param_sets.empty()) {
    return unexpected("video_demux: " + index.format + " config box carried no parameter sets in " + path);
  }
  index.width = par->width;
  index.height = par->height;

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

std::shared_ptr<LazyAccessUnitReader> LazyAccessUnitReader::create(
    std::string path, std::string format, std::vector<uint8_t> param_sets, int nal_length_size) {
  return std::shared_ptr<LazyAccessUnitReader>(
      new LazyAccessUnitReader(std::move(path), std::move(format), std::move(param_sets), nal_length_size));
}

LazyAccessUnitReader::LazyAccessUnitReader(
    std::string path, std::string format, std::vector<uint8_t> param_sets, int nal_length_size)
    : path_(std::move(path)),
      format_(std::move(format)),
      param_sets_(std::move(param_sets)),
      nal_length_size_(nal_length_size) {}

Expected<std::vector<uint8_t>> LazyAccessUnitReader::readUnit(const AccessUnit& unit) {
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

  std::vector<uint8_t> raw(static_cast<size_t>(unit.size));
  stream_.clear();  // drop any EOF/fail state from a prior read
  stream_.seekg(unit.file_offset, std::ios::beg);
  stream_.read(reinterpret_cast<char*>(raw.data()), unit.size);
  if (stream_.gcount() != static_cast<std::streamsize>(unit.size)) {
    return unexpected("video_demux: short read at offset " + std::to_string(unit.file_offset));
  }

  const Span<const uint8_t> sample(raw.data(), raw.size());
  const Span<const uint8_t> params(param_sets_.data(), param_sets_.size());
  if (format_ == "av1") {
    return prependAv1SeqHeader(sample, params, unit.keyframe);
  }
  return avccToAnnexB(sample, params, unit.keyframe, nal_length_size_);
}

}  // namespace video_demux
}  // namespace PJ
