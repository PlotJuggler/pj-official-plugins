#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <nanoarrow/nanoarrow.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../src/ipc_decoder.hpp"
#include "pj_base/sdk/arrow.hpp"
#include "pj_base/span.hpp"

namespace pj::parser_arrow::test {

/// Return the absolute path of one checked-in Arrow IPC fixture.
[[nodiscard]] inline std::filesystem::path fixturePath(std::string_view filename) {
  return std::filesystem::path(PJ_ARROW_TEST_DATA_DIR) / filename;
}

/// Read a complete binary fixture or throw when the path cannot be read.
[[nodiscard]] inline std::vector<uint8_t> readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    throw std::runtime_error("Failed to open test fixture: " + path.string());
  }

  const auto end_position = input.tellg();
  if (end_position < 0) {
    throw std::runtime_error("Failed to determine test fixture size: " + path.string());
  }

  std::vector<uint8_t> bytes(static_cast<std::size_t>(end_position));
  input.seekg(0, std::ios::beg);
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  if (!input) {
    throw std::runtime_error("Failed to read test fixture: " + path.string());
  }
  return bytes;
}

/// Return process-lifetime fixture bytes suitable for streams that borrow their input payload.
[[nodiscard]] inline const std::vector<uint8_t>& fixtureBytes(std::string_view filename) {
  static std::map<std::string, std::vector<uint8_t>> fixtures;
  auto [found, inserted] = fixtures.try_emplace(std::string(filename));
  if (inserted) {
    found->second = readFile(fixturePath(filename));
  }
  return found->second;
}

/// Decode one checked-in IPC fixture whose cached payload outlives the returned borrowing stream.
[[nodiscard]] inline PJ::sdk::ArrowStreamHolder decodeFixture(std::string_view filename) {
  const auto& bytes = fixtureBytes(filename);
  auto decoded = decodeIpcStream(PJ::Span<const uint8_t>(bytes.data(), bytes.size()));
  if (!decoded) {
    throw std::runtime_error(decoded.error());
  }
  return std::move(*decoded);
}

/// Read a stream schema or throw its stream diagnostic.
[[nodiscard]] inline PJ::sdk::ArrowSchemaHolder readSchema(PJ::sdk::ArrowStreamHolder& stream) {
  PJ::sdk::ArrowSchemaHolder schema;
  const int result = stream.get()->get_schema(stream.get(), schema.out());
  if (result != NANOARROW_OK) {
    throw std::runtime_error(ArrowArrayStreamGetLastError(stream.get()));
  }
  return schema;
}

/// Pull the next batch or throw its stream diagnostic or an unexpected-EOS error.
[[nodiscard]] inline PJ::sdk::ArrowArrayHolder readBatch(PJ::sdk::ArrowStreamHolder& stream) {
  PJ::sdk::ArrowArrayHolder batch;
  const int result = stream.get()->get_next(stream.get(), batch.out());
  if (result != NANOARROW_OK) {
    throw std::runtime_error(ArrowArrayStreamGetLastError(stream.get()));
  }
  if (!batch.valid()) {
    throw std::runtime_error("unexpected end of stream");
  }
  return batch;
}

/// Initialize and bind an RAII array view or throw nanoarrow's diagnostic.
[[nodiscard]] inline nanoarrow::UniqueArrayView bindArrayView(const ArrowSchema* schema, const ArrowArray* array) {
  nanoarrow::UniqueArrayView view;
  ArrowError error{};
  int result = ArrowArrayViewInitFromSchema(view.get(), schema, &error);
  if (result == NANOARROW_OK) {
    result = ArrowArrayViewSetArray(view.get(), array, &error);
  }
  if (result != NANOARROW_OK) {
    throw std::runtime_error(error.message);
  }
  return view;
}

}  // namespace pj::parser_arrow::test
