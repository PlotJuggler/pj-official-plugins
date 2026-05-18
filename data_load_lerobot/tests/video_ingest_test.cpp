// Headless F3 test: feed a real mp4 through ingestEpisodeVideo() and assert,
// via a MOCK SourceObjectWriteHostView, that every frame is pushOwned() at the
// expected synthesized timestamp with a non-empty payload. The decode side is
// pj_scene2D's concern (tested upstream) and is intentionally NOT exercised
// here — pj_scene2D/pj_datastore are not buildable inside pj-official-plugins.
//
// Provide a fixture via the LEROBOT_TEST_MP4 env var (e.g. an episode mp4 from
// `huggingface-cli download lerobot/pusht`). Without it the test SKIPs.
#include "video_ingest.hpp"

#include <pj_base/plugin_data_api.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct Recorder {
  std::vector<int64_t> timestamps;
  std::vector<std::size_t> sizes;
  bool any_empty = false;
};

bool mockPushOwned(
    void* ctx, PJ_object_topic_handle_t /*topic*/, int64_t ts, const uint8_t* data, std::size_t size,
    PJ_error_t* /*err*/) noexcept {
  auto* rec = static_cast<Recorder*>(ctx);
  rec->timestamps.push_back(ts);
  rec->sizes.push_back(size);
  if (data == nullptr || size == 0) {
    rec->any_empty = true;
  }
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

}  // namespace

TEST(VideoIngest, PushesEveryFramePayloadAtSyntheticTimestamps) {
  const std::string mp4 = fixtureMp4();
  if (mp4.empty() || !std::filesystem::is_regular_file(mp4)) {
    GTEST_SKIP() << "set LEROBOT_TEST_MP4 to an episode mp4 to run this test";
  }

  Recorder rec;
  auto host = makeMockHost(rec);
  const PJ::sdk::ObjectTopicHandle topic{1};

  // Plenty of monotonically increasing timestamps (no clamp): 1 ms apart.
  std::vector<int64_t> frame_ts;
  frame_ts.reserve(500000);
  for (int64_t i = 0; i < 500000; ++i) {
    frame_ts.push_back(1'000'000'000LL + i * 1'000'000LL);
  }

  auto st = lerobot::ingestEpisodeVideo(host, topic, mp4, "h264", frame_ts);

  ASSERT_TRUE(st.has_value()) << (st.has_value() ? "" : st.error());
  ASSERT_FALSE(rec.timestamps.empty()) << "no frames were pushed";
  EXPECT_FALSE(rec.any_empty) << "an empty payload was pushed";
  for (std::size_t i = 0; i < rec.timestamps.size(); ++i) {
    EXPECT_EQ(rec.timestamps[i], frame_ts[i]) << "frame " << i << " ts mismatch";
  }
}

TEST(VideoIngest, ClampsTimestampWhenFewerEntriesThanPackets) {
  const std::string mp4 = fixtureMp4();
  if (mp4.empty() || !std::filesystem::is_regular_file(mp4)) {
    GTEST_SKIP() << "set LEROBOT_TEST_MP4 to an episode mp4 to run this test";
  }

  Recorder rec;
  auto host = makeMockHost(rec);
  const PJ::sdk::ObjectTopicHandle topic{1};

  // Only two timestamps: every packet past the second clamps to the last.
  const std::vector<int64_t> frame_ts = {10, 20};

  auto st = lerobot::ingestEpisodeVideo(host, topic, mp4, "h264", frame_ts);

  ASSERT_TRUE(st.has_value()) << (st.has_value() ? "" : st.error());
  ASSERT_GE(rec.timestamps.size(), 1u);
  EXPECT_EQ(rec.timestamps.front(), 10);
  EXPECT_EQ(rec.timestamps.back(), 20);  // clamped to frame_ts.back()
}

TEST(VideoIngest, EmptyTimestampVectorIsNoOp) {
  Recorder rec;
  auto host = makeMockHost(rec);
  auto st = lerobot::ingestEpisodeVideo(host, PJ::sdk::ObjectTopicHandle{1}, "/no/such.mp4", "h264", {});
  EXPECT_TRUE(st.has_value());            // empty ts ⇒ skip before opening file
  EXPECT_TRUE(rec.timestamps.empty());
}
