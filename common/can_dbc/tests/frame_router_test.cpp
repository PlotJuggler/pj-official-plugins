#include <gtest/gtest.h>

#include <cstdint>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <pj_can_dbc/dictionary_loader.hpp>
#include <pj_can_dbc/frame_router.hpp>
#include <string>
#include <vector>

#include "source_import_harness.hpp"

namespace {

using pj_can_dbc::CanDecoder;
using pj_can_dbc::CanFrameRouter;
using pj_can_dbc::testing::ScratchDir;
using pj_can_dbc::testing::detail::CaptureHost;

constexpr std::uint32_t kEngineId = 256;  // writeEngineDbc(): BO_ 256 Engine, 16-bit Speed
constexpr int kEngineDlc = 8;

/// A CAN decoder holding the shared one-message test DBC (id 256 "Engine").
class FrameRouterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto dbc = scratch_.root() / "vehicle.dbc";
    pj_can_dbc::testing::writeEngineDbc(dbc, "Engine", kEngineDlc);
    std::string warnings;
    const auto status = pj_can_dbc::loadDictionary(decoder_, pj_can_dbc::testing::generic(dbc), warnings);
    ASSERT_TRUE(status) << status.error();
  }

  void route(CanFrameRouter& router, std::string_view bus, std::uint32_t id, std::size_t size) {
    router.route(host_view_, bus, id, false, std::vector<std::uint8_t>(size, 0x11), 1000);
  }

  ScratchDir scratch_;
  CanDecoder decoder_;
  CaptureHost host_;
  PJ::sdk::SourceWriteHostView host_view_{host_.writeHost()};
};

TEST_F(FrameRouterTest, RawFallbackCoversCanFdPayload) {
  CanFrameRouter router(true);
  router.setDecoder("can0", nullptr);
  route(router, "can0", 0x123, 64);

  EXPECT_EQ(host_.rows["CAN/can0/0x123"], 1u);
  EXPECT_EQ(host_.last_field_count["CAN/can0/0x123"], 64u);  // byte0..byte63
  EXPECT_EQ(router.counts().no_dictionary, 1u);
  EXPECT_EQ(router.counts().decoded, 0u);
}

TEST_F(FrameRouterTest, RawFallbackTruncatesAtKMaxRawBytes) {
  CanFrameRouter router(true);
  router.setDecoder("can0", nullptr);
  route(router, "can0", 0x123, CanFrameRouter::kMaxRawBytes + 1);

  EXPECT_EQ(host_.rows["CAN/can0/0x123"], 1u);
  EXPECT_EQ(host_.last_field_count["CAN/can0/0x123"], CanFrameRouter::kMaxRawBytes);
}

TEST_F(FrameRouterTest, CountsDecodedUnmatchedAndUndecodable) {
  CanFrameRouter router(true);
  router.setDecoder("can0", &decoder_);

  route(router, "can0", kEngineId, kEngineDlc);
  EXPECT_EQ(router.counts().decoded, 1u);
  EXPECT_EQ(router.counts().decoded_topics, 1u);
  EXPECT_EQ(host_.rows["CAN/can0/Engine"], 1u);

  route(router, "can0", 0x7FF, 8);
  EXPECT_EQ(router.counts().unmatched, 1u);
  EXPECT_EQ(host_.rows["CAN/can0/0x7FF"], 1u);

  route(router, "can0", kEngineId, 1);  // 16-bit Speed does not fit in one byte
  EXPECT_EQ(router.counts().undecodable, 1u);
  EXPECT_EQ(host_.rows["CAN/can0/0x100"], 1u);

  EXPECT_EQ(router.counts().decoded, 1u);
  EXPECT_EQ(router.counts().no_dictionary, 0u);
}

TEST_F(FrameRouterTest, RawOffPublishesNothingForUnmatched) {
  CanFrameRouter router(false);
  router.setDecoder("can0", &decoder_);
  route(router, "can0", 0x7FF, 8);

  EXPECT_EQ(router.counts().unmatched, 1u);
  EXPECT_TRUE(host_.topic_names.empty());
}

TEST_F(FrameRouterTest, DecoderIsResolvedPerBusWhenBusChanges) {
  CanFrameRouter router(true);
  router.setDecoder("can0", &decoder_);
  router.setDecoder("can1", nullptr);

  // Alternating buses: the cached bucket must follow the bus on every change.
  route(router, "can0", kEngineId, kEngineDlc);
  route(router, "can1", kEngineId, kEngineDlc);
  route(router, "can0", kEngineId, kEngineDlc);
  route(router, "can2", kEngineId, kEngineDlc);  // never configured: no dictionary

  EXPECT_EQ(router.counts().decoded, 2u);
  EXPECT_EQ(router.counts().decoded_topics, 1u);
  EXPECT_EQ(router.counts().no_dictionary, 2u);
  EXPECT_EQ(host_.rows["CAN/can0/Engine"], 2u);
  EXPECT_EQ(host_.rows["CAN/can1/0x100"], 1u);
  EXPECT_EQ(host_.rows["CAN/can2/0x100"], 1u);
}

}  // namespace
