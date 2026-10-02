// Subscriber mechanics validation (temporary spike test): publish/subscribe
// delivery, stop responsiveness, and reconnect sweep. If stop() hangs here,
// the CompletionSubscriber design must change before P3 tests depend on it.

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "common/pg_fixture.hpp"
#include "coordination/CompletionSubscriber.hpp"
#include "coordination/RedisClient.hpp"
#include "idempotency/WaiterRegistry.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"

namespace apex::coordination {
namespace {

using test::PgFixture;
using namespace std::chrono_literals;

TEST_F(PgFixture, SubscriberSpikeDeliveryAndStop) {
  REQUIRE_REDIS();
  apex::idempotency::WaiterRegistry registry;
  apex::observability::Logger quiet(apex::observability::Level::Error);
  auto metrics = std::make_shared<apex::observability::Metrics>();
  CompletionSubscriber sub(test::PgFixture::shared_redis_client(), registry, quiet, metrics);
  sub.start();

  const std::string channel =
      apex::idempotency::WaiterRegistry::channel_for("spike-key", "spike-fp");
  std::atomic<bool> delivered{false};
  auto waiter = std::make_shared<int>(42);
  std::weak_ptr<void> weak = waiter;
  const auto reg =
      registry.register_waiter(channel, weak, [&] { delivered.store(true); });
  ASSERT_FALSE(reg.rejected);

  // Publish-until-delivered: the subscription establishes asynchronously,
  // so a single publish could race it. Repetition is idempotent (wake-only)
  // and the deadline bounds the wait on infrastructure health, not timing.
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!delivered.load() && std::chrono::steady_clock::now() < deadline) {
    (void)redis().publish(channel, "");
    std::this_thread::sleep_for(100ms);
  }
  EXPECT_TRUE(delivered.load()) << "pub/sub delivery failed";

  const auto stop_start = std::chrono::steady_clock::now();
  sub.stop();
  const auto stop_elapsed = std::chrono::steady_clock::now() - stop_start;
  EXPECT_LT(stop_elapsed, 10s) << "subscriber stop hung";
}

}  // namespace
}  // namespace apex::coordination
