#include "navmap_msg_decoder.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace navmap_parser {

namespace {

// Nested messages deeper than this are rejected (NavMap needs 3).
constexpr int kMaxDepth = 32;

std::string_view trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1);
  }
  return text;
}

std::vector<std::string_view> splitWhitespace(std::string_view text) {
  std::vector<std::string_view> tokens;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
      ++i;
    }
    const size_t start = i;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) == 0) {
      ++i;
    }
    if (i > start) {
      tokens.push_back(text.substr(start, i - start));
    }
  }
  return tokens;
}

bool isSectionSeparator(std::string_view line) {
  return line.size() >= 3 && std::all_of(line.begin(), line.end(), [](char c) { return c == '='; });
}

const std::unordered_set<std::string_view>& primitiveTypes() {
  static const std::unordered_set<std::string_view> kTypes = {
      "bool",  "byte",   "char",  "int8",   "uint8",   "int16",   "uint16",
      "int32", "uint32", "int64", "uint64", "float32", "float64", "string",
  };
  return kTypes;
}

std::string packageOf(std::string_view type_name) {
  const auto slash = type_name.find('/');
  return slash == std::string_view::npos ? std::string() : std::string(type_name.substr(0, slash));
}

// Resolves a field's base type in the scope of `package`: primitives stay,
// bounded strings drop their bound, "Header" is std_msgs, an unqualified name
// is the current package.
PJ::Expected<std::string> resolveBaseType(std::string_view base, const std::string& package) {
  if (base.starts_with("string<=")) {
    return std::string("string");
  }
  if (base == "wstring" || base.starts_with("wstring<=")) {
    return PJ::unexpected(std::string("wstring fields are not supported"));
  }
  if (base == "time" || base == "duration") {
    return PJ::unexpected(std::string("ROS 1 time/duration fields are not supported"));
  }
  if (primitiveTypes().contains(base)) {
    return std::string(base);
  }
  if (base == "Header") {
    return std::string("std_msgs/Header");
  }
  if (base.find('/') == std::string_view::npos) {
    if (package.empty()) {
      return PJ::unexpected("cannot resolve unqualified type " + std::string(base));
    }
    return package + "/" + std::string(base);
  }
  return normalizeRosTypeName(base);
}

// Parses one definition body into fields; constants and comments are skipped.
PJ::Expected<std::vector<MsgField>> parseDefinition(std::string_view body, const std::string& package) {
  std::vector<MsgField> fields;
  size_t start = 0;
  while (start <= body.size()) {
    size_t end = body.find('\n', start);
    if (end == std::string_view::npos) {
      end = body.size();
    }
    std::string_view line = body.substr(start, end - start);
    start = end + 1;
    if (const auto hash = line.find('#'); hash != std::string_view::npos) {
      line = line.substr(0, hash);
    }
    line = trim(line);
    if (line.empty()) {
      continue;
    }
    const auto tokens = splitWhitespace(line);
    if (tokens.size() < 2) {
      return PJ::unexpected("malformed field line: " + std::string(line));
    }
    // Constants: "uint8 U8 = 0" or "uint8 U8=0".
    if (tokens[1].find('=') != std::string_view::npos || (tokens.size() > 2 && tokens[2].starts_with('='))) {
      continue;
    }

    MsgField field;
    field.name = std::string(tokens[1]);
    std::string_view type = tokens[0];
    if (const auto bracket = type.find('['); bracket != std::string_view::npos) {
      if (!type.ends_with(']')) {
        return PJ::unexpected("malformed array type: " + std::string(type));
      }
      field.is_array = true;
      const std::string_view size_text = type.substr(bracket + 1, type.size() - bracket - 2);
      if (!size_text.empty() && !size_text.starts_with("<=")) {
        uint32_t fixed = 0;
        const auto [ptr, ec] = std::from_chars(size_text.data(), size_text.data() + size_text.size(), fixed);
        if (ec != std::errc() || ptr != size_text.data() + size_text.size() || fixed == 0) {
          return PJ::unexpected("malformed array size: " + std::string(type));
        }
        field.fixed_size = fixed;
      }
      type = type.substr(0, bracket);
    }
    auto resolved = resolveBaseType(type, package);
    if (!resolved) {
      return PJ::unexpected(resolved.error());
    }
    field.type = std::move(*resolved);
    fields.push_back(std::move(field));
  }
  return fields;
}

template <typename T>
void readNumberArray(CdrReader& reader, const MsgField& field, bool keep, Value& value) {
  const uint32_t count = field.fixed_size != 0 ? field.fixed_size : reader.readSequenceLength(sizeof(T));
  value.kind = Value::Kind::kNumberArray;
  value.count = count;
  std::vector<T> elements;
  if (keep) {
    elements.reserve(count);
  }
  reader.readArray<T>(count, keep ? &elements : nullptr);
  value.numbers = std::move(elements);
}

template <typename T>
void readNumber(CdrReader& reader, Value& value) {
  const T raw = reader.read<T>();
  value.kind = Value::Kind::kNumber;
  value.number = static_cast<double>(raw);
  if constexpr (std::is_unsigned_v<T>) {
    value.unsigned_bits = raw;
  }
}

// Dispatches one primitive type name to F<T>; returns false for "string".
template <typename Visitor>
bool visitPrimitive(std::string_view type, Visitor&& visit) {
  if (type == "bool" || type == "byte" || type == "uint8" || type == "char") {
    visit(uint8_t{});
  } else if (type == "int8") {
    visit(int8_t{});
  } else if (type == "int16") {
    visit(int16_t{});
  } else if (type == "uint16") {
    visit(uint16_t{});
  } else if (type == "int32") {
    visit(int32_t{});
  } else if (type == "uint32") {
    visit(uint32_t{});
  } else if (type == "int64") {
    visit(int64_t{});
  } else if (type == "uint64") {
    visit(uint64_t{});
  } else if (type == "float32") {
    visit(float{});
  } else if (type == "float64") {
    visit(double{});
  } else {
    return false;
  }
  return true;
}

Node decodeNode(const MsgSchema& schema, const std::string& type, CdrReader& reader, bool keep, int depth);

Value decodeField(const MsgSchema& schema, const MsgField& field, CdrReader& reader, bool keep, int depth) {
  Value value;
  if (field.type == "string") {
    if (!field.is_array) {
      value.kind = Value::Kind::kString;
      value.text = reader.readString();
      return value;
    }
    const uint32_t count = field.fixed_size != 0 ? field.fixed_size : reader.readSequenceLength(4);
    value.kind = Value::Kind::kStringArray;
    value.count = count;
    for (uint32_t i = 0; i < count; ++i) {
      value.strings.push_back(reader.readString());
    }
    return value;
  }
  const bool primitive = visitPrimitive(field.type, [&](auto tag) {
    using T = decltype(tag);
    if (field.is_array) {
      readNumberArray<T>(reader, field, keep, value);
    } else {
      readNumber<T>(reader, value);
    }
  });
  if (primitive) {
    return value;
  }
  if (!field.is_array) {
    value.kind = Value::Kind::kNode;
    value.nodes.push_back(decodeNode(schema, field.type, reader, keep, depth + 1));
    return value;
  }
  // Every element takes at least its type's minimum wire size, which bounds
  // the count (fixed or not) by the remaining payload.
  const size_t element_size = schema.min_wire_size.at(field.type);
  const uint32_t count = field.fixed_size != 0 ? reader.requireElements(field.fixed_size, element_size)
                                               : reader.readSequenceLength(element_size);
  value.kind = Value::Kind::kNodeArray;
  value.count = count;
  value.nodes.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    value.nodes.push_back(decodeNode(schema, field.type, reader, keep, depth + 1));
  }
  return value;
}

Node decodeNode(const MsgSchema& schema, const std::string& type, CdrReader& reader, bool keep, int depth) {
  if (depth > kMaxDepth) {
    throw CdrError("message nesting is too deep");
  }
  const auto it = schema.types.find(type);
  if (it == schema.types.end()) {
    throw CdrError("type is missing from the schema: " + type);
  }
  Node node;
  if (it->second.empty()) {
    (void)reader.read<uint8_t>();  // rosidl puts an empty message on the wire as one uint8
    return node;
  }
  node.fields.reserve(it->second.size());
  for (const MsgField& field : it->second) {
    node.fields.emplace_back(field.name, decodeField(schema, field, reader, keep, depth));
  }
  return node;
}

// Types every ROS 2 bundle may reference without defining.
void addImplicitTypes(MsgSchema& schema) {
  for (const char* time_type : {"builtin_interfaces/Time", "builtin_interfaces/Duration"}) {
    schema.types.try_emplace(
        time_type, std::vector<MsgField>{{.type = "int32", .name = "sec"}, {.type = "uint32", .name = "nanosec"}});
  }
  schema.types.try_emplace(
      "std_msgs/Header",
      std::vector<MsgField>{
          {.type = "builtin_interfaces/Time", .name = "stamp"}, {.type = "string", .name = "frame_id"}});
}

size_t saturatingAdd(size_t a, size_t b) {
  return a > std::numeric_limits<size_t>::max() - b ? std::numeric_limits<size_t>::max() : a + b;
}

size_t saturatingMul(size_t a, size_t b) {
  return b != 0 && a > std::numeric_limits<size_t>::max() / b ? std::numeric_limits<size_t>::max() : a * b;
}

// Memoizes min_wire_size for `type` and every type it embeds. `visiting` holds
// the types on the current path: reaching one again means the type would embed
// itself, which no finite payload can hold.
PJ::Expected<size_t> computeMinWireSize(
    MsgSchema& schema, const std::string& type, std::unordered_set<std::string>& visiting) {
  if (const auto known = schema.min_wire_size.find(type); known != schema.min_wire_size.end()) {
    return known->second;
  }
  if (!visiting.insert(type).second) {
    return PJ::unexpected(type + " contains itself");
  }
  size_t total = 0;
  for (const MsgField& field : schema.types.at(type)) {
    size_t element = 0;
    if (field.is_array && field.fixed_size == 0) {
      element = sizeof(uint32_t);  // the sequence length; the elements may be absent
    } else if (field.type == "string") {
      element = sizeof(uint32_t) + 1;  // the length and the terminating NUL
    } else if (!visitPrimitive(field.type, [&](auto tag) { element = sizeof(tag); })) {
      auto nested = computeMinWireSize(schema, field.type, visiting);
      if (!nested) {
        return nested;
      }
      element = *nested;
    }
    total = saturatingAdd(total, field.fixed_size != 0 ? saturatingMul(element, field.fixed_size) : element);
  }
  visiting.erase(type);
  const size_t size = std::max<size_t>(total, 1);
  schema.min_wire_size.emplace(type, size);
  return size;
}

}  // namespace

std::string normalizeRosTypeName(std::string_view type_name) {
  const auto first = type_name.find('/');
  if (first == std::string_view::npos) {
    return std::string(type_name);
  }
  const auto second = type_name.find('/', first + 1);
  if (second == std::string_view::npos) {
    return std::string(type_name);
  }
  return std::string(type_name.substr(0, first)) + std::string(type_name.substr(second));
}

PJ::Expected<MsgSchema> compileMsgBundle(std::string_view root_type, std::string_view bundle) {
  MsgSchema schema;
  schema.root = normalizeRosTypeName(root_type);

  // Split into (type, body) sections.
  std::string current_type = schema.root;
  std::string current_body;
  std::vector<std::pair<std::string, std::string>> sections;
  size_t start = 0;
  while (start <= bundle.size()) {
    size_t end = bundle.find('\n', start);
    if (end == std::string_view::npos) {
      end = bundle.size();
    }
    const std::string_view line = bundle.substr(start, end - start);
    start = end + 1;
    const std::string_view trimmed = trim(line);
    if (isSectionSeparator(trimmed)) {
      sections.emplace_back(std::move(current_type), std::move(current_body));
      current_type.clear();
      current_body.clear();
      continue;
    }
    if (current_type.empty() && trimmed.starts_with("MSG:")) {
      current_type = normalizeRosTypeName(trim(trimmed.substr(4)));
      continue;
    }
    current_body.append(line);
    current_body.push_back('\n');
  }
  sections.emplace_back(std::move(current_type), std::move(current_body));

  for (const auto& [type, body] : sections) {
    if (type.empty()) {
      if (trim(body).empty()) {
        continue;
      }
      return PJ::unexpected(std::string("schema section without an MSG: header"));
    }
    auto fields = parseDefinition(body, packageOf(type));
    if (!fields) {
      return PJ::unexpected(type + ": " + fields.error());
    }
    schema.types.insert_or_assign(type, std::move(*fields));
  }
  addImplicitTypes(schema);

  // Every referenced message type must be defined.
  for (const auto& [type, fields] : schema.types) {
    for (const MsgField& field : fields) {
      if (!primitiveTypes().contains(field.type) && !schema.types.contains(field.type)) {
        return PJ::unexpected(type + ": field " + field.name + " uses undefined type " + field.type);
      }
    }
  }
  if (!schema.types.contains(schema.root)) {
    return PJ::unexpected("schema does not define " + schema.root);
  }
  std::unordered_set<std::string> visiting;
  for (const auto& entry : schema.types) {
    if (auto size = computeMinWireSize(schema, entry.first, visiting); !size) {
      return PJ::unexpected(size.error());
    }
  }
  return schema;
}

const Value* Node::find(std::string_view name) const {
  for (const auto& [field_name, value] : fields) {
    if (field_name == name) {
      return &value;
    }
  }
  return nullptr;
}

Value* Node::find(std::string_view name) {
  return const_cast<Value*>(std::as_const(*this).find(name));
}

Node decodeMessage(const MsgSchema& schema, CdrReader& reader, bool keep_primitive_arrays) {
  return decodeNode(schema, schema.root, reader, keep_primitive_arrays, 0);
}

std::optional<double> numberOf(const Node& node, std::string_view name) {
  const Value* value = node.find(name);
  return value != nullptr && value->kind == Value::Kind::kNumber ? std::optional<double>(value->number) : std::nullopt;
}

}  // namespace navmap_parser
