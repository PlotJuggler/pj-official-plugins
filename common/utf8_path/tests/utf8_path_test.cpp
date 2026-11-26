// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <pj_utf8_path/utf8_path.hpp>
#include <string>
#include <system_error>

namespace {

using PJ::utf8_path::pathFromUtf8;
using PJ::utf8_path::pathToUtf8;
using PJ::utf8_path::utf8String;

// Cyrillic, CJK and a Latin accent, as \u escapes so the source file stays ASCII (MSVC reads a source file
// without a BOM in the ANSI code page).
constexpr std::u8string_view kFileName = u8"utf8_путь_飛行_übung.txt";

TEST(Utf8Path, RoundTripKeepsEveryByte) {
  const std::string utf8 = utf8String(kFileName);
  EXPECT_EQ(pathToUtf8(pathFromUtf8(utf8)), utf8);
  EXPECT_EQ(pathToUtf8(pathFromUtf8("")), "");
  EXPECT_EQ(pathToUtf8(pathFromUtf8("plain/ascii.txt")), "plain/ascii.txt");
}

TEST(Utf8Path, PathHoldsTheCodePointsNotTheBytes) {
  EXPECT_EQ(pathFromUtf8(utf8String(kFileName)).u8string(), std::u8string(kFileName));
}

TEST(Utf8Path, OpensAFileInsideANonAsciiFolder) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / pathFromUtf8(utf8String(kFileName));
  std::filesystem::create_directories(dir);
  const std::string utf8_file = pathToUtf8(dir / pathFromUtf8(utf8String(kFileName)));
  {
    std::ofstream out(pathFromUtf8(utf8_file), std::ios::binary);
    ASSERT_TRUE(out.is_open());
    out << "ok";
  }
  std::ifstream in(pathFromUtf8(utf8_file), std::ios::binary);
  EXPECT_TRUE(in.is_open());
  EXPECT_TRUE(std::filesystem::exists(pathFromUtf8(utf8_file)));
  in.close();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
