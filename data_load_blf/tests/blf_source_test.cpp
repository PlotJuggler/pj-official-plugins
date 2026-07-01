#include <gtest/gtest.h>

#include <blf_reader.hh>
#include <filesystem>
#include <string>

// Phase 2 scaffold check: lblf links and reads the bundled sample.blf (proving
// the CPM integration and the zlib-compressed LogContainer path work in a real
// build). Frame-level assertions land with the adapter in Phase 3.
TEST(BlfScaffold, LblfReadsBundledFixture) {
  const std::string path = (std::filesystem::path(BLF_TEST_DATA_DIR) / "sample.blf").string();
  lblf::blf_reader reader(path);
  int objects = 0;
  while (reader.next()) {
    (void)reader.data();
    ++objects;
  }
  EXPECT_GT(objects, 0);
}
