// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

/// @file bind_scoped_registry.hpp
/// @brief A service registry valid only until close(), like a PlotJuggler 4.0 host's, which
///  lives only for the bind() call. A lookup after close() is not served, and fails the test
///  when the registry is destroyed: a plugin must never reach the registry after bind().

#include <gtest/gtest.h>

#include <pj_base/sdk/service_registry.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>

namespace toolbox_testing {

class BindScopedRegistry {
 public:
  BindScopedRegistry() : vtable_(*builder.view().vtable) {
    vtable_.get_service = &getService;
  }
  ~BindScopedRegistry() {
    EXPECT_EQ(late_lookups_, 0) << "the plugin reached the registry after bind()";
  }
  BindScopedRegistry(const BindScopedRegistry&) = delete;
  BindScopedRegistry& operator=(const BindScopedRegistry&) = delete;

  /// What the plugin's bind() receives; register services on `builder` first.
  [[nodiscard]] PJ::sdk::ServiceRegistry registry() {
    return PJ::sdk::ServiceRegistry(PJ_service_registry_t{this, &vtable_});
  }

  /// Ends the bind() call: later lookups fail and are counted.
  void close() {
    open_ = false;
  }

  PJ::ServiceRegistryBuilder builder;

 private:
  static bool getService(
      void* ctx, PJ_string_view_t name, uint32_t min_version, PJ_service_t* out_service,
      PJ_error_t* out_error) noexcept {
    auto* self = static_cast<BindScopedRegistry*>(ctx);
    if (!self->open_) {
      ++self->late_lookups_;
      return false;
    }
    const auto inner = self->builder.view();
    return inner.vtable->get_service(inner.ctx, name, min_version, out_service, out_error);
  }

  PJ_service_registry_vtable_t vtable_;
  bool open_ = true;
  int late_lookups_ = 0;
};

}  // namespace toolbox_testing
