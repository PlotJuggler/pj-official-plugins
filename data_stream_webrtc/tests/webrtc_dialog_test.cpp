#include "webrtc_dialog.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

namespace {

bool okEnabled(webrtc_dialog_detail::WebrtcDialog& dialog) {
  const auto widget_data = nlohmann::json::parse(dialog.widget_data());
  return widget_data.at("buttonBox").at("ok_enabled").get<bool>();
}

TEST(WebrtcDialogTest, RetainsApiPortWhileHostIsTemporarilyEmpty) {
  webrtc_dialog_detail::WebrtcDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(R"({
    "server_url": "http://camera:8889",
    "api_url": "http://control:1234",
    "api_url_edited": true
  })"));

  EXPECT_TRUE(dialog.onTextChanged("lineEditApiAddress", ""));
  EXPECT_TRUE(dialog.onTextChanged("lineEditApiAddress", "new-control"));

  const auto config = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(config.at("api_url"), "http://new-control:1234");
}

TEST(WebrtcDialogTest, DerivesApiUrlFromIpv6ServerAuthority) {
  webrtc_dialog_detail::WebrtcDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(R"({"server_url":"http://[::1]:8889"})"));

  const auto config = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(config.at("api_url"), "http://[::1]:9997");
}

TEST(WebrtcDialogTest, PortlessUrlStaysPortlessAfterEditingAnotherField) {
  webrtc_dialog_detail::WebrtcDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(R"({
    "server_url": "http://camera",
    "api_url": "",
    "api_url_edited": true
  })"));

  EXPECT_TRUE(dialog.onTextChanged("lineEditPath", "base"));

  const auto config = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(config.at("server_url"), "http://camera/base");
}

TEST(WebrtcDialogTest, ExplicitDefaultPortIsPreserved) {
  webrtc_dialog_detail::WebrtcDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(R"({"server_url":"http://camera:80"})"));

  EXPECT_TRUE(dialog.onTextChanged("lineEditPath", "base"));

  const auto config = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(config.at("server_url"), "http://camera:80/base");
}

TEST(WebrtcDialogTest, PreservesHttpsSchemeAcrossEditsAndApiDerivation) {
  webrtc_dialog_detail::WebrtcDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(R"({"server_url":"https://camera:8889"})"));

  EXPECT_TRUE(dialog.onTextChanged("lineEditAddress", "new-camera"));

  const auto config = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(config.at("server_url"), "https://new-camera:8889");
  EXPECT_EQ(config.at("api_url"), "https://new-camera:9997");
}

TEST(WebrtcDialogTest, InvalidPortsDisableAcceptanceUntilCorrected) {
  webrtc_dialog_detail::WebrtcDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(R"({
    "server_url": "http://camera:8889",
    "selected": ["stream"]
  })"));
  EXPECT_TRUE(okEnabled(dialog));

  EXPECT_TRUE(dialog.onTextChanged("lineEditPort", "8889junk"));
  EXPECT_FALSE(okEnabled(dialog));
  EXPECT_TRUE(dialog.onTextChanged("lineEditPort", ""));
  EXPECT_TRUE(okEnabled(dialog));

  EXPECT_TRUE(dialog.onTextChanged("lineEditApiPort", "65536"));
  EXPECT_FALSE(okEnabled(dialog));
  EXPECT_TRUE(dialog.onTextChanged("lineEditApiPort", "9997"));
  EXPECT_TRUE(okEnabled(dialog));
}

}  // namespace
