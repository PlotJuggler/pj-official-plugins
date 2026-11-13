/**
 * @file navmap_cdr_reader.hpp
 * @brief Bounds-checked reads over the vendored nanocdr decoder.
 *
 * nanocdr (contrib/nanocdr) does the decoding; this wrapper checks every read
 * against the remaining payload BEFORE handing it to nanocdr, whose decoder
 * trims alignment padding without a bounds check, resizes vectors to the wire
 * length unchecked and reads the header unchecked (contrib/nanocdr/README.md).
 * Never call nanocdr::Decoder directly on a payload.
 *
 * Failure is a thrown CdrError, caught at the SchemaHandler boundary and never
 * let across the ABI.
 */
#pragma once

#include <cstdint>
#include <nanocdr/nanocdr.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace navmap_parser {

/// Malformed, truncated or unsupported CDR payload.
class CdrError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Reads one CDR-encapsulated (plain XCDR1, little-endian) message.
class CdrReader {
 public:
  CdrReader(const uint8_t* data, size_t size) : size_(size) {
    if (data == nullptr || size < kHeaderSize) {
      throw CdrError("truncated CDR encapsulation header");
    }
    if (data[0] != 0 || (data[1] != kCdrBigEndian && data[1] != kCdrLittleEndian)) {
      throw CdrError("unsupported CDR representation; plain XCDR1 is required");
    }
    // nanocdr decodes every payload as little-endian (see contrib/nanocdr/README.md).
    if (data[1] == kCdrBigEndian) {
      throw CdrError("big-endian CDR is not supported");
    }
    try {
      decoder_.emplace(nanocdr::ConstBuffer(data, size));
    } catch (const std::exception& e) {
      throw CdrError(e.what());
    }
  }

  template <typename T>
  T read() {
    static_assert(std::is_arithmetic_v<T>);
    ensure(sizeof(T), 1);
    T value{};
    decoder_->decode(value);
    return value;
  }

  std::string readString() {
    ensure(sizeof(uint32_t), 1);  // the length prefix; nanocdr checks the characters
    std::string out;
    try {
      decoder_->decode(out);
    } catch (const std::exception& e) {
      throw CdrError(e.what());
    }
    return out;
  }

  /// Reads a sequence length and checks that @p element_size bytes per element fit.
  uint32_t readSequenceLength(size_t element_size) {
    const uint32_t count = read<uint32_t>();
    if (element_size != 0 && count > remaining() / element_size) {
      throw CdrError("CDR sequence length exceeds the remaining payload");
    }
    return count;
  }

  /// Appends @p count primitives of type T to @p out, or skips them when out is null.
  template <typename T>
  void readArray(uint32_t count, std::vector<T>* out) {
    static_assert(std::is_arithmetic_v<T>);
    if (count == 0) {
      return;
    }
    ensure(sizeof(T), count);
    if (out == nullptr) {
      decoder_->jump(padding(sizeof(T)) + static_cast<size_t>(count) * sizeof(T));
      return;
    }
    const size_t offset = out->size();
    out->resize(offset + count);
    for (size_t i = offset; i < out->size(); ++i) {
      decoder_->decode((*out)[i]);
    }
  }

  [[nodiscard]] size_t remaining() const noexcept {
    return decoder_->currentBuffer().size();
  }

 private:
  static constexpr size_t kHeaderSize = 4;
  static constexpr uint8_t kCdrBigEndian = 0x00;
  static constexpr uint8_t kCdrLittleEndian = 0x01;

  // Padding nanocdr will skip before an element of @p size bytes. Plain XCDR1
  // aligns every primitive, 8-byte ones included, to its own size, relative to
  // the byte after the encapsulation header.
  [[nodiscard]] size_t padding(size_t size) const noexcept {
    const size_t consumed = size_ - kHeaderSize - remaining();
    return (size - consumed % size) % size;
  }

  // Throws unless padding plus @p count elements of @p size bytes fit.
  void ensure(size_t size, size_t count) const {
    const size_t pad = padding(size);
    const size_t left = remaining();
    if (pad > left || (left - pad) / size < count) {
      throw CdrError("CDR payload is truncated");
    }
  }

  size_t size_;
  std::optional<nanocdr::Decoder> decoder_;
};

}  // namespace navmap_parser
