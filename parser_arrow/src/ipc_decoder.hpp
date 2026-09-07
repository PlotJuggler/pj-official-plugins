#pragma once

#include <cstdint>
#include <memory>

#include "pj_base/expected.hpp"
#include "pj_base/sdk/arrow.hpp"
#include "pj_base/span.hpp"

namespace arrow {
class Table;
}

namespace pj::parser_arrow {

/// Decode one Arrow IPC stream into a lazily consumed Arrow C Data Interface stream.
///
/// The returned stream borrows `bytes`. Callers must fully drain and release the stream before the payload storage
/// dies. The parser host contract satisfies this by consuming appendArrowStream() synchronously inside parse().
[[nodiscard]] PJ::Expected<PJ::sdk::ArrowStreamHolder> decodeIpcStream(PJ::Span<const uint8_t> bytes);

/// Materialize canonical-object messages through the same decoder and normalization.
[[nodiscard]] PJ::Expected<std::shared_ptr<arrow::Table>> decodeIpcTable(PJ::Span<const uint8_t> bytes);

}  // namespace pj::parser_arrow
