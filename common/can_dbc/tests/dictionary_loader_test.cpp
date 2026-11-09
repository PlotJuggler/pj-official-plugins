#include <gtest/gtest.h>

#include <fstream>
#include <pj_can_dbc/can_decoder.hpp>
#include <pj_can_dbc/dictionary_loader.hpp>
#include <string>

#include "dictionary_test_files.hpp"

namespace {

using pj_can_dbc::CanDecoder;
using pj_can_dbc::loadDictionary;
using pj_can_dbc::testing::generic;
using pj_can_dbc::testing::ScratchDir;

TEST(DictionaryLoader, CsvSuffixIsCaseInsensitiveAndRowWarningsCarryThePath) {
  ScratchDir scratch;
  const auto csv = scratch.root() / "dict.CSV";
  std::ofstream(csv) << "ID,bitIn,bitFin,Signed,Power,Scale,Offset,Name\n"
                        "ZZZ,0,1,True,1,1,0,BadId\n"
                        "0x104,0,0,True,1,1,0,Good\n";

  CanDecoder decoder;
  std::string warnings;
  const auto status = loadDictionary(decoder, generic(csv), warnings);
  ASSERT_TRUE(status) << status.error();
  EXPECT_EQ(decoder.messageCount(), 1u);
  EXPECT_EQ(warnings.rfind(generic(csv) + ": ", 0), 0u) << warnings;
  EXPECT_NE(warnings.find("ZZZ"), std::string::npos) << warnings;
}

TEST(DictionaryLoader, OtherSuffixesLoadAsDbcWithoutWarnings) {
  ScratchDir scratch;
  const auto dbc = scratch.root() / "vehicle.dbc";
  pj_can_dbc::testing::writeEngineDbc(dbc, "Engine", 8);

  CanDecoder decoder;
  std::string warnings;
  ASSERT_TRUE(loadDictionary(decoder, generic(dbc), warnings));
  EXPECT_EQ(decoder.messageCount(), 1u);
  EXPECT_TRUE(warnings.empty());
}

TEST(DictionaryLoader, MissingFilesReportTheirKind) {
  ScratchDir scratch;
  CanDecoder decoder;
  std::string warnings;

  const auto csv = generic(scratch.root() / "gone.csv");
  const auto csv_status = loadDictionary(decoder, csv, warnings);
  ASSERT_FALSE(csv_status);
  EXPECT_EQ(csv_status.error(), "dictionary file not found: " + csv);

  const auto dbc = generic(scratch.root() / "gone.dbc");
  const auto dbc_status = loadDictionary(decoder, dbc, warnings);
  ASSERT_FALSE(dbc_status);
  EXPECT_EQ(dbc_status.error(), "DBC file not found: " + dbc);
  EXPECT_TRUE(warnings.empty());
}

}  // namespace
