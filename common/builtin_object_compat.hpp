// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <any>
#include <pj_base/builtin/builtin_object.hpp>

namespace pj_compat {

/// Extract the concrete value from a BuiltinObject on any SDK version:
/// SDK <= 0.28 defines BuiltinObject as std::any (RTTI-based any_cast),
/// SDK >= 0.29 as the tagged holder (enum-guarded get<T>()). Detecting the
/// member at compile time lets the SDK bump land without touching call
/// sites; delete this shim and call get<T>() directly once the repo floor
/// is >= 0.29.
template <typename T>
[[nodiscard]] const T* getBuiltinObject(const PJ::sdk::BuiltinObject& object) {
  if constexpr (requires { object.template get<T>(); }) {
    return object.template get<T>();
  } else {
    return std::any_cast<T>(&object);
  }
}

}  // namespace pj_compat
