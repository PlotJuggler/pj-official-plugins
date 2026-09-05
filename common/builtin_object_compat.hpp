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
// The holder type is a deduced template parameter so the requires-expression
// stays dependent: probing a member on the concrete std::any of SDK <= 0.28
// would otherwise be a hard error rather than a false requirement.
template <typename T, typename Object>
[[nodiscard]] const T* getBuiltinObjectFrom(const Object& object) {
  if constexpr (requires { object.template get<T>(); }) {
    return object.template get<T>();
  } else {
    return std::any_cast<T>(&object);
  }
}

template <typename T>
[[nodiscard]] const T* getBuiltinObject(const PJ::sdk::BuiltinObject& object) {
  return getBuiltinObjectFrom<T>(object);
}

}  // namespace pj_compat
