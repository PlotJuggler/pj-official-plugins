#include <gtest/gtest.h>
#include <mdf/mdfreader.h>

#include <string>

// Phase 1 scaffold check: mdflib links into the test executable and its reader
// reports a bad path cleanly. Real reader/value/can_decoder unit tests (with
// synthetic mdflib-writer fixtures) land in Phases 2-4.
TEST(Mf4Scaffold, MdflibLinksAndReportsBadFile) {
  // Explicit std::string: a bare literal is ambiguous between MdfReader's
  // std::string and std::string_view overloads.
  mdf::MdfReader reader(std::string("/nonexistent/path/does-not-exist.mf4"));
  EXPECT_FALSE(reader.IsOk());
}
