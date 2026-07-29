#pragma once

// Keep Arrow's canonical C ABI definitions ahead of the PJ SDK wrapper. The SDK
// vendors the same frozen structs for plugins that do not otherwise use Arrow.
#include <arrow/c/abi.h>

#include <optional>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>

#include "export_core.hpp"

// The decode functions consume both C Data Interface structs on every path.
// Their release callbacks are transferred to Arrow C++ or invoked during a
// failed import; callers must not release the structs afterward.
// Guarantee: the returned series has non-decreasing `t` (the export_core
// precondition) even when the host delivers rows out of time order;
// duplicate-timestamp rows keep delivery order.
std::optional<NumericSeriesData> decodeNumericSeries(
    struct ArrowSchema* schema, struct ArrowArray* array, std::string name);
std::optional<StringSeriesData> decodeStringSeries(
    struct ArrowSchema* schema, struct ArrowArray* array, std::string name);

std::optional<NumericSeriesData> readNumericSeries(
    PJ::sdk::ToolboxHostView& host, PJ::sdk::FieldHandle field, std::string name);
std::optional<StringSeriesData> readStringSeries(
    PJ::sdk::ToolboxHostView& host, PJ::sdk::FieldHandle field, std::string name);
