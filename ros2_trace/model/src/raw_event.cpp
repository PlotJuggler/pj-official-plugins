#include "ros2_trace_model/raw_event.hpp"

namespace ros2_trace_model {

const FieldValue* RawEvent::find(std::string_view name) const {
  for (const auto& f : fields_) {
    if (f.name == name) {
      return &f.value;
    }
  }
  return nullptr;
}

std::optional<std::uint64_t> RawEvent::handle(std::string_view name) const {
  if (const FieldValue* v = find(name)) {
    if (const auto* p = std::get_if<std::uint64_t>(v)) {
      return *p;
    }
  }
  return std::nullopt;
}

std::optional<std::int64_t> RawEvent::i64(std::string_view name) const {
  if (const FieldValue* v = find(name)) {
    if (const auto* p = std::get_if<std::int64_t>(v)) {
      return *p;
    }
  }
  return std::nullopt;
}

std::optional<bool> RawEvent::boolean(std::string_view name) const {
  if (const FieldValue* v = find(name)) {
    if (const auto* p = std::get_if<bool>(v)) {
      return *p;
    }
  }
  return std::nullopt;
}

std::optional<std::string_view> RawEvent::str(std::string_view name) const {
  if (const FieldValue* v = find(name)) {
    if (const auto* p = std::get_if<std::string>(v)) {
      return std::string_view(*p);
    }
  }
  return std::nullopt;
}

}  // namespace ros2_trace_model
