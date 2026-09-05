#include "ipc_decoder.hpp"

#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "arrow_error.hpp"

namespace pj::parser_arrow {
namespace {

enum class Scan { kContinue, kDone, kDefer };

/// nanoarrow_ipc 0.7 cannot decode DictionaryBatch messages; delete this rejection once CCI PR #30738 (nanoarrow
/// 0.9) lands and the decoder gains dictionary support.
[[nodiscard]] std::string dictionaryError() {
  return parserError(
      "a dictionary-encoded column is not supported (nanoarrow_ipc 0.7 cannot decode DictionaryBatch messages); "
      "producers must decode dictionaries before encoding");
}

/// Add producer guidance for the two known nanoarrow_ipc 0.7 schema-decoder limits.
[[nodiscard]] std::string explainNanoarrow07SchemaLimit(int result, const char* message) {
  const std::string_view text = message != nullptr ? std::string_view(message) : std::string_view{};
  if (text.find("DictionaryEncoding") != std::string_view::npos) {
    return dictionaryError();
  }
  std::string diagnostic = nanoarrowError(result, message);
  if (text.find("Unrecognized Field type") != std::string_view::npos) {
    diagnostic +=
        " (string_view/binary_view and other IPC field types unsupported by nanoarrow_ipc 0.7; cast to "
        "utf8/binary before encoding)";
  }
  return diagnostic;
}

/// Inspect one IPC header, leaving malformed input for the real reader and never reading its body.
[[nodiscard]] PJ::Expected<Scan> scanIpcMessage(ArrowIpcDecoder* decoder, ArrowBufferView remaining) {
  ArrowError error{};
  int32_t prefix_size = 0;
  const int peek_result = ArrowIpcDecoderPeekHeader(decoder, remaining, &prefix_size, &error);
  if (peek_result == ENODATA) {
    return Scan::kDone;
  }
  if (peek_result != NANOARROW_OK) {
    return Scan::kDefer;
  }

  const int verify_result = ArrowIpcDecoderVerifyHeader(decoder, remaining, &error);
  if (verify_result != NANOARROW_OK) {
    return Scan::kDefer;
  }
  const int decode_result = ArrowIpcDecoderDecodeHeader(decoder, remaining, &error);
  if (decode_result != NANOARROW_OK) {
    return Scan::kDefer;
  }

  if (decoder->message_type == NANOARROW_IPC_MESSAGE_TYPE_SCHEMA) {
    PJ::sdk::ArrowSchemaHolder schema;
    const int schema_result = ArrowIpcDecoderDecodeSchema(decoder, schema.out(), &error);
    if (schema_result != NANOARROW_OK) {
      return PJ::unexpected(explainNanoarrow07SchemaLimit(schema_result, error.message));
    }
    if (ArrowIpcDecoderSetSchema(decoder, schema.get(), &error) != NANOARROW_OK) {
      return Scan::kDefer;
    }
  }

  if (decoder->message_type == NANOARROW_IPC_MESSAGE_TYPE_DICTIONARY_BATCH) {
    return Scan::kDefer;
  }

  if (decoder->message_type == NANOARROW_IPC_MESSAGE_TYPE_RECORD_BATCH &&
      decoder->codec == NANOARROW_IPC_COMPRESSION_TYPE_LZ4_FRAME) {
    // CCI PR #30738 tracks the missing lz4 decoder in nanoarrow_ipc 0.7.
    return PJ::unexpected(parserError("lz4-compressed Arrow IPC bodies are not supported"));
  }

  const int64_t header_size = decoder->header_size_bytes;
  const int64_t body_size = decoder->body_size_bytes;
  if (header_size <= 0 || body_size < 0 || header_size > remaining.size_bytes ||
      body_size > remaining.size_bytes - header_size) {
    return Scan::kDefer;
  }
  return Scan::kContinue;
}

/// Preflight every IPC header without consuming record-batch bodies.
[[nodiscard]] PJ::Status rejectLz4AndDictionaryIpc(PJ::Span<const uint8_t> bytes) {
  ArrowIpcDecoder decoder{};
  const int init_result = ArrowIpcDecoderInit(&decoder);
  if (init_result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(init_result, nullptr));
  }
  const auto reset_decoder = [](ArrowIpcDecoder* value) { ArrowIpcDecoderReset(value); };
  const std::unique_ptr<ArrowIpcDecoder, decltype(reset_decoder)> decoder_owner(&decoder, reset_decoder);

  const int64_t total_size = static_cast<int64_t>(bytes.size());
  int64_t offset = 0;
  while (offset < total_size) {
    ArrowBufferView remaining{};
    remaining.data.data = bytes.data() + static_cast<std::size_t>(offset);
    remaining.size_bytes = total_size - offset;

    auto scan = scanIpcMessage(&decoder, remaining);
    if (!scan) {
      return PJ::unexpected(std::move(scan).error());
    }
    if (*scan != Scan::kContinue) {
      return PJ::okStatus();
    }
    offset += decoder.header_size_bytes + decoder.body_size_bytes;
  }

  return PJ::okStatus();
}

/// Do nothing when nanoarrow releases a buffer that borrows caller-owned payload storage.
void releaseBorrowedBuffer(ArrowBufferAllocator*, uint8_t*, int64_t) {}

/// Share reader construction between production preflight and the test-only bypass.
[[nodiscard]] PJ::Expected<PJ::sdk::ArrowStreamHolder> decodeIpcStreamImpl(
    PJ::Span<const uint8_t> bytes, bool run_preflight) {
  if (bytes.empty()) {
    return PJ::unexpected(parserError("empty Arrow IPC payload"));
  }
  if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int64_t>::max())) {
    return PJ::unexpected(parserError("Arrow IPC payload is too large"));
  }

  if (run_preflight) {
    if (auto status = rejectLz4AndDictionaryIpc(bytes); !status) {
      return PJ::unexpected(std::move(status).error());
    }
  }

  ArrowBuffer input_buffer{};
  ArrowBufferInit(&input_buffer);
  const int allocator_result =
      ArrowBufferSetAllocator(&input_buffer, ArrowBufferDeallocator(&releaseBorrowedBuffer, nullptr));
  if (allocator_result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(allocator_result, nullptr));
  }
  input_buffer.data = const_cast<uint8_t*>(bytes.data());
  input_buffer.size_bytes = static_cast<int64_t>(bytes.size());
  input_buffer.capacity_bytes = input_buffer.size_bytes;

  ArrowIpcInputStream input_stream{};
  const int input_result = ArrowIpcInputStreamInitBuffer(&input_stream, &input_buffer);
  if (input_result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(input_result, nullptr));
  }

  ArrowArrayStream raw_stream{};
  const int reader_result = ArrowIpcArrayStreamReaderInit(&raw_stream, &input_stream, nullptr);
  if (reader_result != NANOARROW_OK) {
    if (input_stream.release != nullptr) {
      input_stream.release(&input_stream);
    }
    return PJ::unexpected(nanoarrowError(reader_result, nullptr));
  }
  PJ::sdk::ArrowStreamHolder stream(raw_stream);

  // nanoarrow initializes the reader lazily. Validate the schema now so empty,
  // truncated, and garbage payloads fail decodeIpcStream() instead of get_next().
  PJ::sdk::ArrowSchemaHolder schema;
  const int schema_result = stream.get()->get_schema(stream.get(), schema.out());
  if (schema_result != NANOARROW_OK) {
    return PJ::unexpected(explainNanoarrow07SchemaLimit(schema_result, ArrowArrayStreamGetLastError(stream.get())));
  }

  return stream;
}

}  // namespace

PJ::Expected<PJ::sdk::ArrowStreamHolder> decodeIpcStream(PJ::Span<const uint8_t> bytes) {
  return decodeIpcStreamImpl(bytes, true);
}

PJ::Expected<PJ::sdk::ArrowStreamHolder> decodeIpcStream(PJ::Span<const uint8_t> bytes, IpcPreflight) {
  return decodeIpcStreamImpl(bytes, false);
}

}  // namespace pj::parser_arrow
