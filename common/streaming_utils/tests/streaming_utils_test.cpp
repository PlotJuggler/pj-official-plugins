#include <gtest/gtest.h>

#include <pj_streaming/delegated_ingest.hpp>
#include <pj_streaming/dialog_utils.hpp>
#include <pj_streaming/drain_queue.hpp>
#include <pj_streaming/endpoint.hpp>
#include <pj_streaming/latest_value_slot.hpp>
#include <set>
#include <string>
#include <vector>

namespace {

TEST(EndpointTest, ParsesOnlyCompletePortsInRange) {
  EXPECT_EQ(pj::streaming::parsePort("1883"), 1883);
  EXPECT_FALSE(pj::streaming::parsePort(""));
  EXPECT_FALSE(pj::streaming::parsePort("0"));
  EXPECT_FALSE(pj::streaming::parsePort("65536"));
  EXPECT_FALSE(pj::streaming::parsePort("1883x"));
}

TEST(EndpointTest, FormatsIpv4DnsAndIpv6Authorities) {
  EXPECT_EQ(pj::streaming::composeEndpoint("ws", "localhost", uint16_t{8765}), "ws://localhost:8765");
  EXPECT_EQ(pj::streaming::composeEndpoint("http://", "127.0.0.1", "8889", "camera"), "http://127.0.0.1:8889/camera");
  EXPECT_EQ(pj::streaming::composeEndpoint("ws", "::1", uint16_t{8765}), "ws://[::1]:8765");
  EXPECT_EQ(pj::streaming::composeEndpoint("ws", "[::1]", uint16_t{8765}), "ws://[::1]:8765");
}

TEST(LatestValueSlotTest, CoalescesAndDrains) {
  pj::streaming::LatestValueSlot<std::set<std::string>> slot;
  EXPECT_FALSE(slot.take());
  slot.set({"/first"});
  slot.set({"/last"});
  const auto value = slot.take();
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, (std::set<std::string>{"/last"}));
  EXPECT_FALSE(slot.take());
}

TEST(DrainQueueTest, PreservesFifoAndEmptiesSharedQueue) {
  pj::streaming::DrainQueue<std::string> queue;
  queue.push("first");
  queue.push("second");
  auto batch = queue.drain();
  ASSERT_EQ(batch.size(), 2U);
  EXPECT_EQ(batch.front(), "first");
  batch.pop();
  EXPECT_EQ(batch.front(), "second");
  EXPECT_TRUE(queue.drain().empty());
}

TEST(DialogUtilsTest, MergesOnlyTheVisibleSelection) {
  const std::vector<std::string> previous{"hidden", "visible-old"};
  const std::vector<std::string> reported{"visible-new"};
  const auto merged = pj::streaming::mergeVisibleSelection(
      previous, reported, [](const std::string& value) { return value != "hidden"; }, [](const auto&) { return true; });
  EXPECT_EQ(merged, (std::vector<std::string>{"hidden", "visible-new"}));
}

TEST(DelegatedIngestTest, ReadsOnlyStringParserOverrides) {
  EXPECT_EQ(pj::streaming::parserConfigOverride(R"({"_parser_config":"schema"})"), "schema");
  EXPECT_TRUE(pj::streaming::parserConfigOverride(R"({"_parser_config":42})").empty());
  EXPECT_TRUE(pj::streaming::parserConfigOverride("not json").empty());
}

}  // namespace
