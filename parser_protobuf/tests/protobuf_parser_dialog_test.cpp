#include "../protobuf_parser_dialog.hpp"

#include <google/protobuf/descriptor.pb.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <pj_utf8_path/utf8_path.hpp>
#include <string>

namespace {

std::string encodeBase64(const std::string& input) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve(((input.size() + 2) / 3) * 4);
  for (size_t index = 0; index < input.size(); index += 3) {
    uint32_t value = static_cast<uint32_t>(static_cast<uint8_t>(input[index])) << 16;
    if (index + 1 < input.size()) {
      value |= static_cast<uint32_t>(static_cast<uint8_t>(input[index + 1])) << 8;
    }
    if (index + 2 < input.size()) {
      value |= static_cast<uint32_t>(static_cast<uint8_t>(input[index + 2]));
    }
    output.push_back(kAlphabet[(value >> 18) & 0x3F]);
    output.push_back(kAlphabet[(value >> 12) & 0x3F]);
    output.push_back(index + 1 < input.size() ? kAlphabet[(value >> 6) & 0x3F] : '=');
    output.push_back(index + 2 < input.size() ? kAlphabet[value & 0x3F] : '=');
  }
  return output;
}

std::string descriptorSetWithMessages() {
  google::protobuf::FileDescriptorSet set;
  auto* file = set.add_file();
  file->set_name("portable.proto");
  file->set_package("example.telemetry");
  file->add_message_type()->set_name("Sample");
  file->add_message_type()->set_name("Status");
  return set.SerializeAsString();
}

TEST(ProtobufParserDialogTest, RestoresEmbeddedDescriptorWhenProtoPathIsUnavailable) {
  nlohmann::json config;
  config["proto_file_path"] = "/path/that/no/longer/exists/portable.proto";
  config["message_type"] = "example.telemetry.Status";
  config["compiled_schema_base64"] = encodeBase64(descriptorSetWithMessages());

  ProtobufParserDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(config.dump()));

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(saved["message_type"], "example.telemetry.Status");
  EXPECT_EQ(saved["proto_file_path"], config["proto_file_path"]);
  EXPECT_EQ(saved["compiled_schema_base64"], config["compiled_schema_base64"]);
  EXPECT_NE(dialog.widget_data().find("SOURCE NOTICE"), std::string::npos);
}

TEST(ProtobufParserDialogTest, DefaultsInvalidSavedTypeToFirstEmbeddedMessage) {
  nlohmann::json config;
  config["proto_file_path"] = "/missing/portable.proto";
  config["message_type"] = "example.telemetry.Removed";
  config["compiled_schema_base64"] = encodeBase64(descriptorSetWithMessages());

  ProtobufParserDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(config.dump()));

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(saved["message_type"], "example.telemetry.Sample");
}

TEST(ProtobufParserDialogTest, KeepsEmbeddedDescriptorWhenPresentSourceDoesNotCompile) {
  const auto path = std::filesystem::temp_directory_path() / "pj_invalid_portable.proto";
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  } cleanup{path};
  {
    std::ofstream source(path);
    ASSERT_TRUE(source.is_open());
    source << "this is not valid protobuf syntax";
  }

  nlohmann::json config;
  config["proto_file_path"] = path.string();
  config["message_type"] = "example.telemetry.Status";
  config["compiled_schema_base64"] = encodeBase64(descriptorSetWithMessages());

  ProtobufParserDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(config.dump()));

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(saved["message_type"], "example.telemetry.Status");
  EXPECT_EQ(saved["compiled_schema_base64"], config["compiled_schema_base64"]);
  EXPECT_NE(dialog.widget_data().find("SOURCE COMPILE ERROR (USING SAVED DESCRIPTOR)"), std::string::npos);
}

// The host hands over the .proto path as UTF-8; on Windows a narrow open reads it in the ANSI code page.
TEST(ProtobufParserDialogTest, CompilesProtoFileUnderNonAsciiFolder) {
  const auto dir = std::filesystem::temp_directory_path() /
                   std::filesystem::path(u8"pj_proto_utf8_\u043f\u0443\u0442\u044c_\u98db\u884c");
  std::filesystem::create_directories(dir);
  struct Cleanup {
    std::filesystem::path dir;
    ~Cleanup() {
      std::error_code ec;
      std::filesystem::remove_all(dir, ec);
    }
  } cleanup{dir};
  const auto path = dir / std::filesystem::path(u8"\u0441\u0445\u0435\u043c\u0430_\u30b9\u30ad\u30fc\u30de.proto");
  {
    std::ofstream source(path);
    ASSERT_TRUE(source.is_open());
    source << "syntax = \"proto3\";\npackage example.utf8;\nmessage Hello { int32 value = 1; }\n";
  }

  nlohmann::json config;
  config["proto_file_path"] = PJ::utf8_path::pathToUtf8(path);
  ProtobufParserDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(config.dump()));

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(saved["message_type"], "example.utf8.Hello");
  EXPECT_TRUE(saved.contains("compiled_schema_base64"));
  EXPECT_EQ(saved["proto_file_path"], config["proto_file_path"]);
}

}  // namespace
