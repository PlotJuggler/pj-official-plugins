#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace ros2_trace_model {

// Identity of a traced object. In a single-host trace the raw pointer value is
// unique; for multi-host/distributed traces (pid, host) disambiguate pointers
// that collide across processes. Designed in from the start so multi-host is
// not a future rewrite.
struct EntityKey {
  std::uint64_t ptr{};
  std::uint32_t pid{};
  std::uint32_t host{};

  friend bool operator==(const EntityKey&, const EntityKey&) = default;
};

struct EntityKeyHash {
  std::size_t operator()(const EntityKey& k) const noexcept {
    std::size_t h = std::hash<std::uint64_t>{}(k.ptr);
    h = h * 1000003U ^ std::hash<std::uint32_t>{}(k.pid);
    h = h * 1000003U ^ std::hash<std::uint32_t>{}(k.host);
    return h;
  }
};

}  // namespace ros2_trace_model
