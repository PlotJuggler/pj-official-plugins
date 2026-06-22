#pragma once
// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT
//
// Small descriptor-reflection helpers shared by the hand-rolled foxglove codecs.
// The decoders resolve their protobuf field NUMBERS by NAME from the schema
// embedded in the .mcap, so a self-describing file that renumbered a foxglove
// schema still decodes (see foxglove_object_codecs.hpp for the full rationale).

#include <google/protobuf/descriptor.h>

namespace pj_protobuf {

/// Look up a field's wire number by NAME in an embedded message descriptor,
/// returning `fallback` when the descriptor is null or has no field by that
/// name. A complete foxglove descriptor defines every field, so the resolved
/// numbers form a permutation of distinct values; partial descriptors (which the
/// well-known schemas never are) could alias a resolved number onto a default.
[[nodiscard]] inline int fieldNumberOr(const google::protobuf::Descriptor* descriptor, const char* name, int fallback) {
  if (descriptor != nullptr) {
    if (const auto* field = descriptor->FindFieldByName(name)) {
      return field->number();
    }
  }
  return fallback;
}

/// The descriptor of the message type referenced by message-typed field `name`,
/// or nullptr if `descriptor` is null or `name` is absent / not a message field.
/// Used to descend into nested foxglove messages (SceneEntity, PackedElement
/// Field) whose own field numbers also need resolving.
[[nodiscard]] inline const google::protobuf::Descriptor* nestedDescriptor(
    const google::protobuf::Descriptor* descriptor, const char* name) {
  if (descriptor != nullptr) {
    if (const auto* field = descriptor->FindFieldByName(name)) {
      return field->message_type();  // nullptr for non-message fields
    }
  }
  return nullptr;
}

}  // namespace pj_protobuf
