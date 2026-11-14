/**
 * @file navmap_msg_decoder.hpp
 * @brief Schema-driven decoding of ROS 2 messages into a small value tree.
 *
 * The NavMap messages changed layout across releases (layers gained metadata,
 * int16 data and sparse indices; the map gained geometry_id and links). Rather
 * than hardcode one revision, the parser compiles the `.msg` bundle that ships
 * with each topic (MCAP schema / ROS 2 type description) and decodes by it, so
 * every revision decodes and unknown future fields are skipped by type.
 *
 * Only what a NavMap needs is supported: primitives, strings, nested messages,
 * fixed arrays and (bounded) sequences. wstring and ROS 1 time/duration are
 * rejected at bind time.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <pj_base/expected.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "navmap_cdr_reader.hpp"

namespace navmap_parser {

/// One field of a compiled message definition.
struct MsgField {
  /// Primitive name ("uint32", "string", ...) or a resolved "pkg/Type".
  std::string type;
  std::string name;
  bool is_array = false;
  /// Fixed array length; 0 for a (possibly bounded) sequence.
  uint32_t fixed_size = 0;
};

/// A compiled `.msg` bundle: every message type reachable from the root.
struct MsgSchema {
  std::string root;  ///< "pkg/Type"
  std::unordered_map<std::string, std::vector<MsgField>> types;
  /// Fewest payload bytes one message of each type takes, alignment padding
  /// excluded (saturating; at least 1, since a field-less message is one
  /// uint8). Bounds the element count of an array of that type by the
  /// remaining payload, so a hostile length cannot allocate far beyond it.
  std::unordered_map<std::string, size_t> min_wire_size;
};

/// Normalizes "pkg/msg/Type" and "pkg/Type" to "pkg/Type".
[[nodiscard]] std::string normalizeRosTypeName(std::string_view type_name);

/// Compiles a ROS 2 concatenated `.msg` bundle (root definition first, then
/// "MSG: pkg/Type" sections). Fails when a referenced type is undefined, a
/// field uses an unsupported type, or a type contains itself other than
/// through a sequence.
[[nodiscard]] PJ::Expected<MsgSchema> compileMsgBundle(std::string_view root_type, std::string_view bundle);

/// A decoded primitive array, kept in its wire type.
using NumberArray = std::variant<
    std::vector<uint8_t>, std::vector<int8_t>, std::vector<int16_t>, std::vector<uint16_t>, std::vector<int32_t>,
    std::vector<uint32_t>, std::vector<int64_t>, std::vector<uint64_t>, std::vector<float>, std::vector<double>>;

struct Value;

/// A decoded message: its fields in declaration order.
struct Node {
  std::vector<std::pair<std::string, Value>> fields;

  [[nodiscard]] const Value* find(std::string_view name) const;
  [[nodiscard]] Value* find(std::string_view name);
};

/// One decoded field. Exactly the member matching `kind` is meaningful.
struct Value {
  enum class Kind : uint8_t { kNumber, kString, kNumberArray, kStringArray, kNode, kNodeArray };
  Kind kind = Kind::kNumber;
  double number = 0.0;
  uint64_t unsigned_bits = 0;  ///< Exact value of an unsigned integer field (uint64 does not fit a double).
  std::string text;
  NumberArray numbers;
  /// Element count of an array, also set when the elements were skipped.
  uint32_t count = 0;
  std::vector<std::string> strings;
  std::vector<Node> nodes;  ///< One element for kNode, N for kNodeArray.
};

/// Decodes one message of `schema.root`. With keep_primitive_arrays false,
/// primitive arrays are skipped (only `count` is set) for a cheap scalar pass.
/// Throws CdrError on malformed data.
[[nodiscard]] Node decodeMessage(const MsgSchema& schema, CdrReader& reader, bool keep_primitive_arrays);

/// The number field @p name of @p node; nullopt when it is missing or not a number.
[[nodiscard]] std::optional<double> numberOf(const Node& node, std::string_view name);

}  // namespace navmap_parser
