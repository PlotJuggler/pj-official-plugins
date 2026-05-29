#include "pj_config_utils/config_utils.hpp"

#include <gtest/gtest.h>

namespace {

using pj::config::parseLenient;
using pj::config::parseStrict;

// --- parseStrict (data-source tier: malformed is a hard error) -------------

TEST(ParseStrict, EmptyInputIsEmptyObject) {
  auto result = parseStrict("");
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->is_object());
  EXPECT_TRUE(result->empty());
}

TEST(ParseStrict, ValidObjectParses) {
  auto result = parseStrict(R"({"filepath":"/tmp/a.csv","n":3})");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->value("filepath", std::string{}), "/tmp/a.csv");
  EXPECT_EQ(result->value("n", 0), 3);
}

TEST(ParseStrict, MalformedIsHardError) {
  auto result = parseStrict("{not valid json");
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("invalid config JSON"), std::string::npos);
}

TEST(ParseStrict, ContextAppearsInErrorMessage) {
  auto result = parseStrict("}{", "CSV config");
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("invalid CSV config JSON"), std::string::npos);
}

// --- parseLenient (parser tier: malformed keeps defaults, never fails) -----

TEST(ParseLenient, EmptyInputIsEmptyObjectNotMalformed) {
  bool malformed = true;
  auto cfg = parseLenient("", &malformed);
  EXPECT_TRUE(cfg.is_object());
  EXPECT_TRUE(cfg.empty());
  EXPECT_FALSE(malformed);
}

TEST(ParseLenient, ValidObjectParses) {
  bool malformed = true;
  auto cfg = parseLenient(R"({"max_array_size":500})", &malformed);
  EXPECT_EQ(cfg.value("max_array_size", 0), 500);
  EXPECT_FALSE(malformed);
}

TEST(ParseLenient, MalformedYieldsEmptyObjectAndFlag) {
  bool malformed = false;
  auto cfg = parseLenient("{garbage", &malformed);
  EXPECT_TRUE(cfg.is_object());
  EXPECT_TRUE(cfg.empty());
  EXPECT_TRUE(malformed);
  // Defaults still resolve cleanly off the empty object.
  EXPECT_EQ(cfg.value("max_array_size", 42), 42);
}

TEST(ParseLenient, NullFlagPointerIsAccepted) {
  auto cfg = parseLenient("also bad", nullptr);
  EXPECT_TRUE(cfg.is_object());
  EXPECT_TRUE(cfg.empty());
}

}  // namespace
