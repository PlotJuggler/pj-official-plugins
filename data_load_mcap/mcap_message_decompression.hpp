#pragma once

// Per-message payload compression as rosbag2 writes it with `compression_mode: message`: every MCAP Message record
// of a CDR channel holds one zstd frame of the serialized message, while the chunks around it may be uncompressed.
// Nothing else in the file marks it reliably (`mcap filter` drops the `rosbag2` Metadata record), so payloads are
// recognised by the zstd magic number: a CDR payload starts with its encapsulation header, whose first byte is 0.

#include <zstd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <pj_base/buffer_anchor.hpp>
#include <stdexcept>
#include <string>

namespace PJ::McapHelpers {

inline bool isZstdFrame(const void* data, size_t size) {
  static constexpr unsigned char kMagic[] = {0x28, 0xB5, 0x2F, 0xFD};  // ZSTD_MAGICNUMBER, little endian
  return size >= sizeof(kMagic) && std::memcmp(data, kMagic, sizeof(kMagic)) == 0;
}

/// Decodes one zstd frame as rosbag2 writes it: a single frame that declares its decompressed size. Anything else is
/// treated as damaged. A frame declaring more than `max_decoded_bytes` is refused before anything is allocated.
/// Throws std::runtime_error with the reason.
inline sdk::PayloadView decodeZstdMessage(PJ::Span<const uint8_t> frame, uint64_t max_decoded_bytes) {
  const auto fail = [](const std::string& reason) {
    return std::runtime_error("zstd decompression failed: " + reason);
  };
  const unsigned long long declared = ZSTD_getFrameContentSize(frame.data(), frame.size());
  if (declared == ZSTD_CONTENTSIZE_ERROR) {
    throw fail("not a zstd frame");
  }
  if (declared == ZSTD_CONTENTSIZE_UNKNOWN) {
    throw fail("the frame does not declare its decompressed size");
  }
  if (declared > max_decoded_bytes) {
    throw fail(
        "declared decompressed size " + std::to_string(declared) + " exceeds the limit of " +
        std::to_string(max_decoded_bytes) + " bytes");
  }
  const size_t frame_size = ZSTD_findFrameCompressedSize(frame.data(), frame.size());
  if (ZSTD_isError(frame_size) != 0) {
    throw fail(std::string("truncated or corrupt frame (") + ZSTD_getErrorName(frame_size) + ")");
  }
  if (frame_size != frame.size()) {
    throw fail(std::to_string(frame.size() - frame_size) + " unexpected byte(s) after the frame");
  }
  // One context per fetching thread (creating one per message allocates ~100 KiB each time); a failed creation is
  // retried on the next message instead of disabling decoding on this thread.
  struct Context {
    ZSTD_DCtx* ctx = nullptr;
    ~Context() {
      ZSTD_freeDCtx(ctx);
    }
  };
  thread_local Context context;
  if (context.ctx == nullptr && (context.ctx = ZSTD_createDCtx()) == nullptr) {
    throw fail("cannot allocate a decompression context");
  }
  // Left uninitialised: a damaged size field then commits only the pages zstd writes before it detects the mismatch.
  const size_t size = static_cast<size_t>(declared);
  std::shared_ptr<uint8_t[]> decoded(new uint8_t[size]);
  const size_t written = ZSTD_decompressDCtx(context.ctx, decoded.get(), size, frame.data(), frame.size());
  if (ZSTD_isError(written) != 0) {
    throw fail(std::string("corrupt frame (") + ZSTD_getErrorName(written) + ")");
  }
  if (written != size) {
    throw fail(
        "frame decoded to " + std::to_string(written) + " bytes instead of the declared " + std::to_string(size));
  }
  return sdk::PayloadView{PJ::Span<const uint8_t>(decoded.get(), size), sdk::BufferAnchor(decoded)};
}

}  // namespace PJ::McapHelpers
