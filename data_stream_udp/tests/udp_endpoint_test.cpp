/**
 * @file udp_endpoint_test.cpp
 * @brief SharedUdpEndpoint::acquire failure paths. A failed open (invalid address, port held by another socket)
 *  must come back as an error, not freeze the calling thread, and must leave the registry usable.
 *
 * The plugin translation unit is included so its anonymous-namespace classes are reachable.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../udp_source.cpp"  // NOLINT(bugprone-suspicious-include)

namespace {

using Acquired = PJ::Expected<std::shared_ptr<SharedUdpEndpoint>>;

/// Runs acquire() on its own thread so a self-deadlock fails the test instead of hanging it. A timed-out call
/// stays blocked forever, so its future is parked in a leaked list: a std::future from std::async would join in
/// its destructor.
std::optional<Acquired> acquireWithTimeout(const std::string& address, uint16_t port) {
  static auto* stuck = new std::vector<std::future<Acquired>>();
  auto call = std::async(std::launch::async, [address, port] { return SharedUdpEndpoint::acquire(address, port); });
  if (call.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    stuck->push_back(std::move(call));
    ADD_FAILURE() << "acquire(" << address << ":" << port << ") did not return within 3 s (deadlocked)";
    return std::nullopt;
  }
  return call.get();
}

/// A loopback UDP port nobody is bound to right now.
uint16_t freeUdpPort() {
  asio::io_context io;
  asio::ip::udp::socket probe(io, asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  return probe.local_endpoint().port();
}

/// A later acquire must work, and must share the endpoint with a concurrent acquire of the same key.
void expectAcquireStillWorks(uint16_t port) {
  auto first = acquireWithTimeout("127.0.0.1", port);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(*first) << first->error();
  auto second = acquireWithTimeout("127.0.0.1", port);
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(*second) << second->error();
  EXPECT_EQ(first->value().get(), second->value().get());
}

TEST(SharedUdpEndpoint, InvalidAddressIsAnErrorAndRegistryStaysUsable) {
  auto result = acquireWithTimeout("not-an-address", 9870);
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(*result);
  EXPECT_NE(result->error().find("invalid address"), std::string::npos) << result->error();

  expectAcquireStillWorks(freeUdpPort());
}

#ifdef _WIN32

TEST(SharedUdpEndpoint, PortHeldByAnotherSocketIsAnErrorAndRegistryStaysUsable) {
  GTEST_SKIP() << "Windows lets a second socket bind a port held without SO_EXCLUSIVEADDRUSE";
}

#else

TEST(SharedUdpEndpoint, PortHeldByAnotherSocketIsAnErrorAndRegistryStaysUsable) {
  asio::io_context io;
  // No SO_REUSEADDR: the plugin's own reuse_address(true) cannot share this port.
  asio::ip::udp::socket holder(io, asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  const uint16_t port = holder.local_endpoint().port();

  auto result = acquireWithTimeout("127.0.0.1", port);
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(*result);
  EXPECT_NE(result->error().find("UDP bind failed"), std::string::npos) << result->error();

  expectAcquireStillWorks(freeUdpPort());

  holder.close();
  expectAcquireStillWorks(port);
}

#endif

}  // namespace
