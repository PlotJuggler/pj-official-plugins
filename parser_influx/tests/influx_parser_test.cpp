#include <gtest/gtest.h>

#include <cstdint>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/testing/parser_write_recorder.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"

#ifndef PJ_INFLUX_PARSER_PLUGIN_PATH
#error "PJ_INFLUX_PARSER_PLUGIN_PATH must be defined"
#endif

namespace {

using PJ::PrimitiveType;
using PJ::sdk::testing::ParserWriteRecorder;
using PJ::sdk::testing::RecordedField;
using PJ::sdk::testing::RecordedRow;

constexpr int64_t kReceiveTime = 1000;

class InfluxParserTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto lib = PJ::MessageParserLibrary::load(PJ_INFLUX_PARSER_PLUGIN_PATH);
    ASSERT_TRUE(lib) << lib.error();
    library_ = std::move(*lib);
    handle_ = library_.createHandle();
    ASSERT_TRUE(handle_.valid());
    registry_.registerService<PJ::sdk::ParserWriteHostService>(recorder_.makeHost());
    ASSERT_TRUE(handle_.bind(registry_.view()));
  }

  void parse(std::string_view text) {
    const auto* data = reinterpret_cast<const uint8_t*>(text.data());
    auto status = handle_.parse(kReceiveTime, PJ::Span<const uint8_t>(data, text.size()));
    ASSERT_TRUE(status) << status.error();
    // The recorder accepts what the real host rejects (and a live source
    // stops on): a row naming one series twice once '//' and a leading '/'
    // are normalized away.
    for (const auto& row : recorder_.rows()) {
      std::set<std::string> names;
      for (const auto& f : row.fields) {
        std::string name = std::regex_replace(f.name, std::regex("/+"), "/");
        if (name.starts_with('/')) {
          name.erase(0, 1);
        }
        EXPECT_TRUE(names.insert(name).second) << "row names " << name << " twice";
      }
    }
  }

  const std::vector<RecordedRow>& rows() const {
    return recorder_.rows();
  }

  static const RecordedField& field(const RecordedRow& row, std::string_view name) {
    const auto* f = ParserWriteRecorder::findField(row, name);
    EXPECT_NE(f, nullptr) << "no field " << name;
    static const RecordedField kMissing;
    return f != nullptr ? *f : kMissing;
  }

  PJ::MessageParserLibrary library_;
  PJ::MessageParserHandle handle_{static_cast<const PJ_message_parser_vtable_t*>(nullptr)};
  PJ::ServiceRegistryBuilder registry_;
  ParserWriteRecorder recorder_;
};

TEST_F(InfluxParserTest, NamesASeriesByMeasurementTagsAndField) {
  parse("cpu,cpu=cpu-total,host=box usage_idle=97.5 1700000000000000000");
  ASSERT_EQ(rows().size(), 1u);
  EXPECT_EQ(rows()[0].timestamp, 1700000000000000000);
  ASSERT_EQ(rows()[0].fields.size(), 1u);
  EXPECT_EQ(rows()[0].fields[0].name, "cpu/cpu=cpu-total/host=box/usage_idle");
  EXPECT_EQ(rows()[0].fields[0].type, PrimitiveType::kFloat64);
  EXPECT_DOUBLE_EQ(rows()[0].fields[0].numeric, 97.5);
}

TEST_F(InfluxParserTest, ALineWithoutTimestampTakesTheReceiveTime) {
  parse("mem used=2");
  ASSERT_EQ(rows().size(), 1u);
  EXPECT_EQ(rows()[0].timestamp, kReceiveTime);
  EXPECT_DOUBLE_EQ(field(rows()[0], "mem/used").numeric, 2.0);
}

TEST_F(InfluxParserTest, KeepsEachFieldTypeNative) {
  parse(R"(m i=-42i,u=42u,f=1.5,e=1e3,b=t,B=FALSE,T=True,s="hello" 5)");
  ASSERT_EQ(rows().size(), 1u);
  const auto& row = rows()[0];
  EXPECT_EQ(field(row, "m/i").type, PrimitiveType::kInt64);
  EXPECT_DOUBLE_EQ(field(row, "m/i").numeric, -42.0);
  EXPECT_EQ(field(row, "m/u").type, PrimitiveType::kUint64);
  EXPECT_DOUBLE_EQ(field(row, "m/u").numeric, 42.0);
  EXPECT_EQ(field(row, "m/f").type, PrimitiveType::kFloat64);
  EXPECT_DOUBLE_EQ(field(row, "m/e").numeric, 1000.0);
  EXPECT_EQ(field(row, "m/b").type, PrimitiveType::kBool);
  EXPECT_TRUE(field(row, "m/b").bool_value);
  EXPECT_FALSE(field(row, "m/B").bool_value);
  EXPECT_TRUE(field(row, "m/T").bool_value);
  EXPECT_EQ(field(row, "m/s").type, PrimitiveType::kString);
  EXPECT_EQ(field(row, "m/s").string_value, "hello");
}

TEST_F(InfluxParserTest, EachLineKeepsItsOwnTimestamp) {
  parse("m a=1 100\nm a=2 200\n");
  ASSERT_EQ(rows().size(), 2u);
  EXPECT_EQ(rows()[0].timestamp, 100);
  EXPECT_DOUBLE_EQ(field(rows()[0], "m/a").numeric, 1.0);
  EXPECT_EQ(rows()[1].timestamp, 200);
  EXPECT_DOUBLE_EQ(field(rows()[1], "m/a").numeric, 2.0);
}

// Telegraf sends every metric of one collection with the same timestamp.
TEST_F(InfluxParserTest, LinesSharingATimestampShareARow) {
  parse("cpu a=1 100\nmem b=2 100\ndisk c=3 300");
  ASSERT_EQ(rows().size(), 2u);
  EXPECT_EQ(rows()[0].timestamp, 100);
  EXPECT_EQ(rows()[0].fields.size(), 2u);
  EXPECT_DOUBLE_EQ(field(rows()[0], "cpu/a").numeric, 1.0);
  EXPECT_DOUBLE_EQ(field(rows()[0], "mem/b").numeric, 2.0);
  EXPECT_EQ(rows()[1].timestamp, 300);
}

TEST_F(InfluxParserTest, ARepeatedSeriesAtTheSameTimestampStartsANewRow) {
  parse("m a=1 100\nm a=2 100");
  ASSERT_EQ(rows().size(), 2u);
  EXPECT_DOUBLE_EQ(field(rows()[0], "m/a").numeric, 1.0);
  EXPECT_DOUBLE_EQ(field(rows()[1], "m/a").numeric, 2.0);
}

// The host rejects a row naming a series twice, and a UDP source stops on that error.
TEST_F(InfluxParserTest, AFieldRepeatedInALineKeepsItsLastValue) {
  parse("m a=1,b=2,a=3 5");
  ASSERT_EQ(rows().size(), 1u);
  ASSERT_EQ(rows()[0].fields.size(), 2u);
  EXPECT_DOUBLE_EQ(field(rows()[0], "m/a").numeric, 3.0);
  EXPECT_DOUBLE_EQ(field(rows()[0], "m/b").numeric, 2.0);
}

TEST_F(InfluxParserTest, NamesEqualOnlyAfterSlashNormalizationAreOneSeries) {
  parse("m /a=1,a=2 5\n/m b=1 5\nm b=2 5");
  ASSERT_EQ(rows().size(), 2u);
  ASSERT_EQ(rows()[0].fields.size(), 2u);
  EXPECT_DOUBLE_EQ(field(rows()[0], "m/a").numeric, 2.0);
  EXPECT_DOUBLE_EQ(field(rows()[0], "m/b").numeric, 1.0);
  EXPECT_DOUBLE_EQ(field(rows()[1], "m/b").numeric, 2.0);
}

TEST_F(InfluxParserTest, AQuoteOutsideAStringValueIsLiteral) {
  parse("m a\"b=1,c=x\"y,d=2 5");
  ASSERT_EQ(rows().size(), 1u);
  EXPECT_EQ(rows()[0].timestamp, 5);
  ASSERT_EQ(rows()[0].fields.size(), 2u);
  EXPECT_DOUBLE_EQ(field(rows()[0], "m/a\"b").numeric, 1.0);
  EXPECT_DOUBLE_EQ(field(rows()[0], "m/d").numeric, 2.0);
}

TEST_F(InfluxParserTest, StringsFromMergedLinesSurviveUntilTheRowIsWritten) {
  parse("c z=\"s1\" 5\nd w=\"s2\" 5");
  ASSERT_EQ(rows().size(), 1u);
  EXPECT_EQ(field(rows()[0], "c/z").string_value, "s1");
  EXPECT_EQ(field(rows()[0], "d/w").string_value, "s2");
}

TEST_F(InfluxParserTest, AQuotedStringMayHoldSpacesCommasAndEscapedQuotes) {
  parse(R"(log msg="disk full, \"sda\" = 99% \\ retry",level=3i 7)");
  ASSERT_EQ(rows().size(), 1u);
  EXPECT_EQ(rows()[0].timestamp, 7);
  EXPECT_EQ(field(rows()[0], "log/msg").string_value, R"(disk full, "sda" = 99% \ retry)");
  EXPECT_DOUBLE_EQ(field(rows()[0], "log/level").numeric, 3.0);
}

TEST_F(InfluxParserTest, UnescapesMeasurementTagsAndFieldKeys) {
  parse(R"(my\ meas,tag\ k=v\,1\=x fi\ eld=1 5)");
  ASSERT_EQ(rows().size(), 1u);
  ASSERT_EQ(rows()[0].fields.size(), 1u);
  EXPECT_EQ(rows()[0].fields[0].name, "my meas/tag k=v,1=x/fi eld");
}

TEST_F(InfluxParserTest, SkipsCommentsBlankLinesAndCarriageReturns) {
  parse("# a comment\n\n  \nm a=1 5\r\n");
  ASSERT_EQ(rows().size(), 1u);
  EXPECT_EQ(rows()[0].timestamp, 5);
  EXPECT_DOUBLE_EQ(field(rows()[0], "m/a").numeric, 1.0);
}

TEST_F(InfluxParserTest, SkipsMalformedLinesAndFieldsButKeepsTheRest) {
  parse(
      "garbage\n"
      "m a=1 5 extra\n"
      "m a=1 notatime\n"
      "m novalue,=9,b=,c=abc,d=2 6\n");
  ASSERT_EQ(rows().size(), 1u);
  EXPECT_EQ(rows()[0].timestamp, 6);
  ASSERT_EQ(rows()[0].fields.size(), 1u);
  EXPECT_DOUBLE_EQ(field(rows()[0], "m/d").numeric, 2.0);
}

TEST_F(InfluxParserTest, AnEmptyPayloadWritesNothing) {
  parse("");
  EXPECT_TRUE(rows().empty());
}

}  // namespace
