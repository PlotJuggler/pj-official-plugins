#pragma once

#include <cstdint>

#include "pj_base/expected.hpp"
#include "pj_base/sdk/arrow.hpp"
#include "pj_base/span.hpp"

namespace pj::parser_arrow {

/// Decode one Arrow IPC stream into a lazily consumed Arrow C Data Interface stream.
[[nodiscard]] PJ::Expected<PJ::sdk::ArrowStreamHolder> decodeIpcStream(PJ::Span<const uint8_t> bytes);

}  // namespace pj::parser_arrow
