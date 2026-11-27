#pragma once

// Per-message payload compression as rosbag2 writes it with `compression_mode: message`: every MCAP Message record
// holds one compressed frame of the serialized message, while the MCAP chunks around it may be uncompressed. Payloads
// are decoded one by one, on whichever thread fetches them, so the codecs keep no shared state.

#include <zstd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace PJ::McapHelpers {

/// Decodes one complete compressed payload. Throws std::runtime_error with the reason on any failure. A payload that
/// declares more than `max_decoded_bytes` is refused before anything is allocated.
using MessageDecodeFn = std::vector<uint8_t> (*)(std::span<const uint8_t> frame, uint64_t max_decoded_bytes);

struct MessageCodec {
  std::string_view format;  ///< rosbag2 `compression_format` value
  MessageDecodeFn decode = nullptr;
};

/// rosbag2 writes every frame with its content size and as a single frame per message, so a frame without a declared
/// size, or with bytes after it, is treated as damaged rather than streamed.
inline std::vector<uint8_t> decodeZstdMessage(std::span<const uint8_t> frame, uint64_t max_decoded_bytes) {
  const unsigned long long declared = ZSTD_getFrameContentSize(frame.data(), frame.size());
  if (declared == ZSTD_CONTENTSIZE_ERROR) {
    throw std::runtime_error("not a zstd frame");
  }
  if (declared == ZSTD_CONTENTSIZE_UNKNOWN) {
    throw std::runtime_error("the frame does not declare its decompressed size");
  }
  if (declared > max_decoded_bytes) {
    throw std::runtime_error(
        "declared decompressed size " + std::to_string(declared) + " exceeds the limit of " +
        std::to_string(max_decoded_bytes) + " bytes");
  }
  const size_t frame_size = ZSTD_findFrameCompressedSize(frame.data(), frame.size());
  if (ZSTD_isError(frame_size) != 0) {
    throw std::runtime_error(std::string("truncated or corrupt frame (") + ZSTD_getErrorName(frame_size) + ")");
  }
  if (frame_size != frame.size()) {
    throw std::runtime_error(std::to_string(frame.size() - frame_size) + " unexpected byte(s) after the frame");
  }
  // One context per fetching thread: creating one per message would allocate and free ~100 KiB each time.
  thread_local const std::unique_ptr<ZSTD_DCtx, size_t (*)(ZSTD_DCtx*)> context(ZSTD_createDCtx(), &ZSTD_freeDCtx);
  if (!context) {
    throw std::runtime_error("cannot allocate a zstd decompression context");
  }
  std::vector<uint8_t> decoded(static_cast<size_t>(declared));
  const size_t written = ZSTD_decompressDCtx(context.get(), decoded.data(), decoded.size(), frame.data(), frame.size());
  if (ZSTD_isError(written) != 0) {
    throw std::runtime_error(std::string("corrupt frame (") + ZSTD_getErrorName(written) + ")");
  }
  if (written != decoded.size()) {
    throw std::runtime_error(
        "frame decoded to " + std::to_string(written) + " bytes instead of the declared " + std::to_string(declared));
  }
  return decoded;
}

/// Supported rosbag2 `compression_format` values.
inline constexpr MessageCodec kMessageCodecs[] = {
    {"zstd", &decodeZstdMessage},
};

/// The table entry for `format`, or nullptr.
inline const MessageCodec* findMessageCodec(std::string_view format) {
  const auto* it = std::find_if(std::begin(kMessageCodecs), std::end(kMessageCodecs), [&](const MessageCodec& codec) {
    return codec.format == format;
  });
  return it == std::end(kMessageCodecs) ? nullptr : it;
}

/// Decodes one message payload; the error names the format. The topic is not repeated here: the host prefixes every
/// failed push with it.
inline std::vector<uint8_t> decodeMessagePayload(
    const MessageCodec& codec, std::span<const uint8_t> frame, uint64_t max_decoded_bytes) {
  try {
    return codec.decode(frame, max_decoded_bytes);
  } catch (const std::exception& e) {
    throw std::runtime_error("per-message " + std::string(codec.format) + " decompression failed: " + e.what());
  }
}

/// How the message payloads of one file must be read, from its rosbag2 compression keys.
struct PayloadCompression {
  const MessageCodec* codec = nullptr;            ///< set when every payload must be decoded
  std::optional<std::string> unsupported_format;  ///< set when payloads are compressed with a format not in the table
};

/// `mode` as written in the metadata, any case. Only `message` compresses payloads: `file` compresses the whole file,
/// which is rejected before reading (see resolveRecordingFiles), and `none` or an empty mode leaves payloads as
/// recorded.
inline PayloadCompression resolvePayloadCompression(
    const std::optional<std::string>& mode, const std::optional<std::string>& format) {
  PayloadCompression out;
  const auto is_message_mode = [](std::string_view value) {
    return value.size() == 7 && std::equal(value.begin(), value.end(), "message", [](char a, char b) {
             return ((a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a) == b;
           });
  };
  if (!mode || !is_message_mode(*mode)) {
    return out;
  }
  const std::string name = format.value_or(std::string{});
  out.codec = findMessageCodec(name);
  if (!out.codec) {
    out.unsupported_format = name;
  }
  return out;
}

}  // namespace PJ::McapHelpers
