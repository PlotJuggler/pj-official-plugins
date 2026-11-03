#include "../csv_dictionary.hpp"

#include <gtest/gtest.h>

#include <locale>
#include <pj_base/number_parse.hpp>
#include <pj_can_dbc/can_decoder.hpp>
#include <string>
#include <vector>

namespace {

using candump_detail::arusCsvToDbc;
using candump_detail::formatDbcNumber;
using pj_can_dbc::CanDecoder;
using pj_can_dbc::DecodedSignal;
using pj_can_dbc::DecodeResult;

const DecodedSignal* find(const std::vector<DecodedSignal>& sigs, const std::string& name) {
  for (const auto& s : sigs) {
    if (s.name == name) {
      return &s;
    }
  }
  return nullptr;
}

TEST(FormatDbcNumber, RoundTripsIntegersFractionsAndExponents) {
  for (double v : {0.0, 1.0, -1.0, 0.1, -0.02, 0.0298062593145, -0.000031688042484, 3.4e+38, -3.4e+38, 1e-05}) {
    const std::string text = formatDbcNumber(v);
    const auto parsed = PJ::parseNumber<double>(text);
    ASSERT_TRUE(parsed.has_value()) << "text=" << text;
    EXPECT_EQ(*parsed, v) << "text=" << text;
  }
}

TEST(FormatDbcNumber, UsesClassicLocaleEvenUnderSpanishGlobalLocale) {
  std::locale saved;
  try {
    saved = std::locale::global(std::locale("es_ES.UTF-8"));
  } catch (const std::runtime_error&) {
    GTEST_SKIP() << "es_ES.UTF-8 locale not installed on this system";
  }
  const std::string text = formatDbcNumber(-0.0298062593145);
  std::locale::global(saved);
  EXPECT_NE(text.find('.'), std::string::npos) << text;  // decimal point, not comma
  EXPECT_EQ(text.find(','), std::string::npos) << text;
}

TEST(ArusCsvToDbc, SimpleSingleSignalMessageExactText) {
  const std::string csv = "ID,bitIn,bitFin,Signed,Power,Scale,Offset,Name\n0x100,0,1,True,1,1,0,Speed\n";
  std::string dbc_text, warnings;
  ASSERT_TRUE(arusCsvToDbc(csv, dbc_text, warnings).has_value());
  EXPECT_TRUE(warnings.empty()) << warnings;
  const std::string expected =
      "VERSION \"\"\n\nNS_ :\n\nBS_:\n\nBU_: PJ\n\nBO_ 256 MSG_100: 2 PJ\n"
      " SG_ Speed : 0|16@1- (1,0) [0|0] \"\" PJ\n\n";
  EXPECT_EQ(dbc_text, expected);
}

TEST(ArusCsvToDbc, GroupsFourHexDigitIdsByBaseAndMatchesArusWorkedExample) {
  // Real rows from https://github.com/ARUSfs/log_plotter/blob/main/can_conversions.csv
  // (fetched 2026-09-21): 0x1a31/0x1a32 share base 0x1a3.
  const std::string csv =
      "ID,bitIn,bitFin,Signed,Power,Scale,Offset,Name\n"
      "0x1a31,0,1,True,1,0.02,0,IMU_ax\n"
      "0x1a32,2,3,True,1,0.02,0,IMU_ay\n";
  std::string dbc_text, warnings;
  ASSERT_TRUE(arusCsvToDbc(csv, dbc_text, warnings).has_value());
  EXPECT_TRUE(warnings.empty()) << warnings;

  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(dbc_text).has_value()) << dbc_text;
  EXPECT_EQ(dec.messageCount(), 1u);  // one message, two signals -- not two messages

  // data "9CFF6400": bytes 0x9C,0xFF (IMU_ax, LE signed 16-bit = -100) and
  // 0x64,0x00 (IMU_ay, LE signed 16-bit = 100).
  const std::vector<std::uint8_t> data{0x9C, 0xFF, 0x64, 0x00};
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(0x1a3u, false, data, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* ax = find(sigs, "IMU_ax");
  const auto* ay = find(sigs, "IMU_ay");
  ASSERT_NE(ax, nullptr);
  ASSERT_NE(ay, nullptr);
  EXPECT_DOUBLE_EQ(ax->value, -2.0);
  EXPECT_DOUBLE_EQ(ay->value, 2.0);
}

TEST(ArusCsvToDbc, NegativeScaleRowMatchesArusWorkedExample) {
  // Real row (extensometer): negative Scale -- exercises the common/can_dbc
  // vendored regex fix end to end (a negative factor used to be silently
  // dropped by the unpatched dbc_parser_cpp).
  const std::string csv =
      "ID,bitIn,bitFin,Signed,Power,Scale,Offset,Name\n"
      "0x134,0,1,True,1,-0.000031688042484,0.476959989071,extensometer\n";
  std::string dbc_text, warnings;
  ASSERT_TRUE(arusCsvToDbc(csv, dbc_text, warnings).has_value());
  EXPECT_TRUE(warnings.empty()) << warnings;

  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(dbc_text).has_value()) << dbc_text;
  EXPECT_EQ(dec.messageCount(), 1u);

  const std::vector<std::uint8_t> data{0xE8, 0x03};  // raw 1000
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(0x134u, false, data, result);
  ASSERT_EQ(result, DecodeResult::kDecoded);
  const auto* ext = find(sigs, "extensometer");
  ASSERT_NE(ext, nullptr);
  EXPECT_NEAR(ext->value, 0.445271946587, 1e-9);
}

TEST(ArusCsvToDbc, ExtendedMessageWhenBaseAboveStandardRange) {
  const std::string csv = "ID,bitIn,bitFin,Signed,Power,Scale,Offset,Name\n0x9001,0,0,False,1,1,0,Foo\n";
  std::string dbc_text, warnings;
  ASSERT_TRUE(arusCsvToDbc(csv, dbc_text, warnings).has_value());
  EXPECT_TRUE(warnings.empty()) << warnings;
  // base = 0x900 (2304) > 0x7FF -> extended: BO_ id = 0x900 | 0x80000000 = 2147485952.
  EXPECT_NE(dbc_text.find("BO_ 2147485952 "), std::string::npos) << dbc_text;

  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(dbc_text).has_value());
  DecodeResult result = DecodeResult::kNoMatch;
  const auto sigs = dec.decode(0x900u, /*extended=*/true, std::vector<std::uint8_t>{5}, result);
  EXPECT_EQ(result, DecodeResult::kDecoded);
  ASSERT_FALSE(sigs.empty());
  EXPECT_DOUBLE_EQ(sigs[0].value, 5.0);
}

TEST(ArusCsvToDbc, SkipsInvalidRowsWithWarningButKeepsGoodOnes) {
  const std::string csv =
      "ID,bitIn,bitFin,Signed,Power,Scale,Offset,Name\n"
      "ZZZ,0,1,True,1,1,0,BadId\n"         // non-hex id
      "0x101,1,0,True,1,1,0,BadRange\n"    // bitIn > bitFin
      "0x102,0,8,True,1,1,0,TooWide\n"     // bitFin > 7
      "0x103,0,0,maybe,1,1,0,BadSigned\n"  // invalid Signed
      "0x104,0,0,True,1,1,0,Good\n";
  std::string dbc_text, warnings;
  ASSERT_TRUE(arusCsvToDbc(csv, dbc_text, warnings).has_value());
  EXPECT_FALSE(warnings.empty());

  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(dbc_text).has_value()) << dbc_text;
  EXPECT_EQ(dec.messageCount(), 1u);  // only "Good" survives
  EXPECT_NE(dbc_text.find("Good"), std::string::npos);
  EXPECT_EQ(dbc_text.find("BadId"), std::string::npos);
  EXPECT_EQ(dbc_text.find("BadRange"), std::string::npos);
  EXPECT_EQ(dbc_text.find("TooWide"), std::string::npos);
  EXPECT_EQ(dbc_text.find("BadSigned"), std::string::npos);
}

TEST(ArusCsvToDbc, HandlesBomAndCrlfAndWhitespace) {
  const std::string bom = "\xEF\xBB\xBF";
  const std::string csv =
      bom + "ID,bitIn,bitFin,Signed,Power,Scale,Offset,Name\r\n0x100, 0 , 1 ,True,1,1,0, Speed \r\n";
  std::string dbc_text, warnings;
  ASSERT_TRUE(arusCsvToDbc(csv, dbc_text, warnings).has_value());
  EXPECT_TRUE(warnings.empty()) << warnings;
  CanDecoder dec;
  ASSERT_TRUE(dec.loadDbcString(dbc_text).has_value()) << dbc_text;
  EXPECT_EQ(dec.messageCount(), 1u);
}

TEST(ArusCsvToDbc, CaseInsensitiveHeaderColumns) {
  const std::string csv = "id,bitin,bitfin,signed,power,scale,offset,name\n0x100,0,0,true,1,1,0,S\n";
  std::string dbc_text, warnings;
  ASSERT_TRUE(arusCsvToDbc(csv, dbc_text, warnings).has_value());
  EXPECT_TRUE(warnings.empty()) << warnings;
}

TEST(ArusCsvToDbc, SanitizesAndDedupesSignalNames) {
  const std::string csv =
      "ID,bitIn,bitFin,Signed,Power,Scale,Offset,Name\n"
      "0x1a01,0,0,True,1,1,0,1bad name!\n"
      "0x1a02,1,1,True,1,1,0,1bad name!\n";
  std::string dbc_text, warnings;
  ASSERT_TRUE(arusCsvToDbc(csv, dbc_text, warnings).has_value());
  EXPECT_NE(dbc_text.find("_1bad_name_"), std::string::npos) << dbc_text;
  EXPECT_NE(dbc_text.find("_1bad_name__2"), std::string::npos) << dbc_text;
}

TEST(ArusCsvToDbc, MissingRequiredColumnFails) {
  const std::string csv = "ID,bitIn,bitFin\n0x100,0,0\n";
  std::string dbc_text, warnings;
  EXPECT_FALSE(arusCsvToDbc(csv, dbc_text, warnings).has_value());
}

TEST(ArusCsvToDbc, EmptyInputFails) {
  std::string dbc_text, warnings;
  EXPECT_FALSE(arusCsvToDbc("", dbc_text, warnings).has_value());
}

TEST(ArusCsvToDbc, NoValidRowsFails) {
  const std::string csv = "ID,bitIn,bitFin,Signed,Power,Scale,Offset,Name\nZZ,0,0,True,1,1,0,Bad\n";
  std::string dbc_text, warnings;
  EXPECT_FALSE(arusCsvToDbc(csv, dbc_text, warnings).has_value());
}

}  // namespace
