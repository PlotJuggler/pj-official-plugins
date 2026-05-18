// Headless F3/R4 test: feed a real mp4 through ingestEpisodeVideo() and assert,
// via a MOCK SourceObjectWriteHostView, that every entry is a VALID JPEG (the
// exact byte shape PlotJuggler's built-in kImage→JPEG pipeline decodes) at the
// expected synthesized timestamp, and that the first entry actually decodes to
// real dimensions. This proves the AV1/H.264 → JPEG conversion end-to-end
// without needing the GUI.
//
// Provide a fixture via LEROBOT_TEST_MP4 (e.g. an episode mp4 from the
// lerobot/pusht v2.1 slice). Without it the data-driven tests SKIP.
#include "video_ingest.hpp"

#include <pj_base/plugin_data_api.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct Recorder {
  std::vector<int64_t> timestamps;
  std::vector<std::vector<uint8_t>> payloads;
};

bool mockPushOwned(
    void* ctx, PJ_object_topic_handle_t /*topic*/, int64_t ts, const uint8_t* data, std::size_t size,
    PJ_error_t* /*err*/) noexcept {
  auto* rec = static_cast<Recorder*>(ctx);
  rec->timestamps.push_back(ts);
  rec->payloads.emplace_back(data, data + size);
  return true;
}

bool mockRegisterTopic(
    void*, PJ_string_view_t, PJ_string_view_t, PJ_object_topic_handle_t* out, PJ_error_t*) noexcept {
  if (out != nullptr) {
    out->id = 1;
  }
  return true;
}

PJ::sdk::SourceObjectWriteHostView makeMockHost(Recorder& rec) {
  static PJ_object_write_host_vtable_t vt{};
  vt.abi_version = 4;
  vt.struct_size = sizeof(PJ_object_write_host_vtable_t);
  vt.register_topic = &mockRegisterTopic;
  vt.push_owned = &mockPushOwned;
  vt.push_lazy = nullptr;
  vt.set_retention_budget = nullptr;
  PJ_object_write_host_t host{&rec, &vt};
  return PJ::sdk::SourceObjectWriteHostView(host);
}

std::string fixtureMp4() {
  const char* env = std::getenv("LEROBOT_TEST_MP4");
  return env != nullptr ? std::string(env) : std::string{};
}

bool isJpeg(const std::vector<uint8_t>& b) {
  return b.size() >= 4 && b[0] == 0xFF && b[1] == 0xD8 && b[b.size() - 2] == 0xFF && b[b.size() - 1] == 0xD9;
}

// Decode a JPEG buffer with FFmpeg's mjpeg decoder; returns {w,h} or {0,0}.
std::pair<int, int> jpegDims(const std::vector<uint8_t>& jpeg) {
  const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
  if (dec == nullptr) {
    return {0, 0};
  }
  AVCodecContext* c = avcodec_alloc_context3(dec);
  AVPacket* pkt = av_packet_alloc();
  AVFrame* fr = av_frame_alloc();
  std::vector<uint8_t> buf = jpeg;  // non-const for AVPacket::data (no const_cast)
  std::pair<int, int> dims{0, 0};
  if (c != nullptr && pkt != nullptr && fr != nullptr && avcodec_open2(c, dec, nullptr) >= 0) {
    pkt->data = buf.data();
    pkt->size = static_cast<int>(buf.size());
    if (avcodec_send_packet(c, pkt) >= 0 && avcodec_receive_frame(c, fr) >= 0) {
      dims = {fr->width, fr->height};
    }
  }
  av_frame_free(&fr);
  av_packet_free(&pkt);
  avcodec_free_context(&c);
  return dims;
}

}  // namespace

TEST(VideoIngest, EmitsValidJpegPerFrameAtSyntheticTimestamps) {
  const std::string mp4 = fixtureMp4();
  if (mp4.empty() || !std::filesystem::is_regular_file(mp4)) {
    GTEST_SKIP() << "set LEROBOT_TEST_MP4 to an episode mp4 to run this test";
  }

  Recorder rec;
  auto host = makeMockHost(rec);
  const PJ::sdk::ObjectTopicHandle topic{1};

  std::vector<int64_t> frame_ts;
  frame_ts.reserve(500000);
  for (int64_t i = 0; i < 500000; ++i) {
    frame_ts.push_back(1'000'000'000LL + i * 1'000'000LL);
  }

  auto st = lerobot::ingestEpisodeVideo(host, topic, mp4, "av1", frame_ts);

  ASSERT_TRUE(st.has_value()) << (st.has_value() ? "" : st.error());
  ASSERT_FALSE(rec.payloads.empty()) << "no frames were pushed";
  for (std::size_t i = 0; i < rec.payloads.size(); ++i) {
    EXPECT_TRUE(isJpeg(rec.payloads[i])) << "entry " << i << " is not a valid JPEG";
    EXPECT_EQ(rec.timestamps[i], frame_ts[i]) << "entry " << i << " ts mismatch";
  }
  const auto [w, h] = jpegDims(rec.payloads.front());
  EXPECT_GT(w, 0) << "first JPEG did not decode to a real width";
  EXPECT_GT(h, 0) << "first JPEG did not decode to a real height";
}

TEST(VideoIngest, ClampsTimestampWhenFewerEntriesThanFrames) {
  const std::string mp4 = fixtureMp4();
  if (mp4.empty() || !std::filesystem::is_regular_file(mp4)) {
    GTEST_SKIP() << "set LEROBOT_TEST_MP4 to an episode mp4 to run this test";
  }

  Recorder rec;
  auto host = makeMockHost(rec);
  const std::vector<int64_t> frame_ts = {10, 20};

  auto st = lerobot::ingestEpisodeVideo(host, PJ::sdk::ObjectTopicHandle{1}, mp4, "av1", frame_ts);

  ASSERT_TRUE(st.has_value()) << (st.has_value() ? "" : st.error());
  ASSERT_GE(rec.timestamps.size(), 1u);
  EXPECT_EQ(rec.timestamps.front(), 10);
  EXPECT_EQ(rec.timestamps.back(), 20);  // clamped to frame_ts.back()
}

TEST(VideoIngest, EmptyTimestampVectorIsNoOp) {
  Recorder rec;
  auto host = makeMockHost(rec);
  auto st = lerobot::ingestEpisodeVideo(host, PJ::sdk::ObjectTopicHandle{1}, "/no/such.mp4", "av1", {});
  EXPECT_TRUE(st.has_value());
  EXPECT_TRUE(rec.payloads.empty());
}
