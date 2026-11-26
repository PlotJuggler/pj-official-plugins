#include <charconv>
#include <cstddef>
#include <pj_can_dbc/signal_row.hpp>
#include <string>
#include <string_view>

namespace pj_can_dbc {

PJ::Span<const PJ::sdk::NamedFieldValue> SignalRowBuilder::build(const std::vector<DecodedSignal>& signals) {
  // Resized before any string_view below is taken, so it never moves a buffer already
  // referenced by a field written earlier in this call.
  if (fallback_buffers_.size() < signals.size()) {
    fallback_buffers_.resize(signals.size());
  }
  const std::size_t max_fields = signals.size() * 2;
  if (fields_.size() < max_fields) {
    fields_.resize(max_fields);
  }

  std::size_t fallback_used = 0;
  std::size_t field_count = 0;
  for (const auto& sig : signals) {
    fields_[field_count].name.assign(sig.name);
    fields_[field_count].value = sig.value;
    ++field_count;
    if (!sig.raw.has_value()) {
      continue;
    }
    std::string_view label_text;
    if (sig.label.has_value()) {
      label_text = *sig.label;
    } else {
      // No table entry matched: fall back to the number as text, in a buffer that outlives this
      // build().
      auto& buf = fallback_buffers_[fallback_used++];
      const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), *sig.raw);
      label_text = std::string_view(buf.data(), static_cast<std::size_t>(res.ptr - buf.data()));
    }
    fields_[field_count].name.assign(sig.label_name);
    fields_[field_count].value = label_text;
    ++field_count;
  }
  return PJ::Span<const PJ::sdk::NamedFieldValue>(fields_.data(), field_count);
}

}  // namespace pj_can_dbc
