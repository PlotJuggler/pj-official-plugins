#include "webrtc_dialog.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

namespace {

TEST(WebrtcDialogTest, RetainsApiPortWhileHostIsTemporarilyEmpty) {
  webrtc_dialog_detail::WebrtcDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(R"({
    "server_url": "http://camera:8889",
    "api_url": "http://control:1234",
    "api_url_edited": true
  })"));

  EXPECT_FALSE(dialog.onTextChanged("lineEditApiAddress", ""));
  EXPECT_FALSE(dialog.onTextChanged("lineEditApiAddress", "new-control"));

  const auto config = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(config.at("api_url"), "http://new-control:1234");
}

TEST(WebrtcDialogTest, DerivesApiUrlFromIpv6ServerAuthority) {
  webrtc_dialog_detail::WebrtcDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(R"({"server_url":"http://[::1]:8889"})"));

  const auto config = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(config.at("api_url"), "http://[::1]:9997");
}

}  // namespace
