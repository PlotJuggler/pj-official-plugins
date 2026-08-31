#include "ipc_decoder.hpp"

#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace pj::parser_arrow {
namespace {

/// Prefix a nanoarrow diagnostic for the parser-facing error contract.
[[nodiscard]] std::string parserError(std::string_view message) {
  return "parser_arrow: " + std::string(message);
}

/// Convert an errno-style nanoarrow result into a parser-facing diagnostic.
[[nodiscard]] std::string nanoarrowError(int result, const char* message) {
  if (message != nullptr && message[0] != '\0') {
    return parserError(message);
  }
  return parserError(std::error_code(result, std::generic_category()).message());
}

/// Inspect IPC message metadata without consuming record-batch bodies.
PJ::Status rejectUnsupportedCompression(PJ::Span<const uint8_t> bytes) {
  ArrowIpcDecoder decoder{};
  const int init_result = ArrowIpcDecoderInit(&decoder);
  if (init_result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(init_result, nullptr));
  }
  const auto reset_decoder = [](ArrowIpcDecoder* value) { ArrowIpcDecoderReset(value); };
  const std::unique_ptr<ArrowIpcDecoder, decltype(reset_decoder)> decoder_owner(&decoder, reset_decoder);

  ArrowError error{};
  const int64_t total_size = static_cast<int64_t>(bytes.size());
  int64_t offset = 0;
  while (offset < total_size) {
    ArrowBufferView remaining{};
    remaining.data.data = bytes.data() + static_cast<std::size_t>(offset);
    remaining.size_bytes = total_size - offset;

    int32_t prefix_size = 0;
    const int peek_result = ArrowIpcDecoderPeekHeader(&decoder, remaining, &prefix_size, &error);
    if (peek_result != NANOARROW_OK) {
      // EOS and malformed later messages are handled by the actual stream reader.
      break;
    }

    const int verify_result = ArrowIpcDecoderVerifyHeader(&decoder, remaining, &error);
    if (verify_result != NANOARROW_OK) {
      break;
    }
    const int decode_result = ArrowIpcDecoderDecodeHeader(&decoder, remaining, &error);
    if (decode_result != NANOARROW_OK) {
      break;
    }

    if (decoder.message_type == NANOARROW_IPC_MESSAGE_TYPE_SCHEMA) {
      PJ::sdk::ArrowSchemaHolder schema;
      const int schema_result = ArrowIpcDecoderDecodeSchema(&decoder, schema.out(), &error);
      if (schema_result != NANOARROW_OK || ArrowIpcDecoderSetSchema(&decoder, schema.get(), &error) != NANOARROW_OK) {
        break;
      }
    }

    if (decoder.message_type == NANOARROW_IPC_MESSAGE_TYPE_RECORD_BATCH &&
        decoder.codec == NANOARROW_IPC_COMPRESSION_TYPE_LZ4_FRAME) {
      return PJ::unexpected("parser_arrow: lz4-compressed Arrow IPC bodies are not supported");
    }

    const int64_t header_size = decoder.header_size_bytes;
    const int64_t body_size = decoder.body_size_bytes;
    const int64_t remaining_size = total_size - offset;
    if (header_size <= 0 || body_size < 0 || header_size > remaining_size || body_size > remaining_size - header_size) {
      break;
    }
    offset += header_size + body_size;
  }

  return PJ::okStatus();
}

}  // namespace

PJ::Expected<PJ::sdk::ArrowStreamHolder> decodeIpcStream(PJ::Span<const uint8_t> bytes) {
  if (bytes.empty()) {
    return PJ::unexpected("parser_arrow: empty Arrow IPC payload");
  }
  if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int64_t>::max())) {
    return PJ::unexpected("parser_arrow: Arrow IPC payload is too large");
  }

  if (auto status = rejectUnsupportedCompression(bytes); !status) {
    return PJ::unexpected(std::move(status).error());
  }

  ArrowBuffer input_buffer;
  ArrowBufferInit(&input_buffer);
  const auto reset_buffer = [](ArrowBuffer* value) { ArrowBufferReset(value); };
  const std::unique_ptr<ArrowBuffer, decltype(reset_buffer)> buffer_owner(&input_buffer, reset_buffer);
  const int append_result = ArrowBufferAppend(&input_buffer, bytes.data(), static_cast<int64_t>(bytes.size()));
  if (append_result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(append_result, nullptr));
  }

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
    return PJ::unexpected(nanoarrowError(schema_result, ArrowArrayStreamGetLastError(stream.get())));
  }

  return stream;
}

}  // namespace pj::parser_arrow
