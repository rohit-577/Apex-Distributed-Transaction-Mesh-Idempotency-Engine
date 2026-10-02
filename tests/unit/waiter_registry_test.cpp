// WaiterRegistry unit tests: registration, notification, completion-before-
// subscription race, transient prod, per-key cap, shutdown, channel mapping.
// Hermetic (no infrastructure): the registry is pure coordination state.

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "idempotency/WaiterRegistry.hpp"

namespace apex::idempotency {
namespace {

struct Probe {
  std::shared_ptr<int> owner = std::make_shared<int>(7);
  std::atomic<int> wakes{0};
  WaiterRegistry::WakeCallback callback() {
    return [this] { ++wakes; };
  }
  std::weak_ptr<void> weak() { return owner; }
};

TEST(WaiterRegistryTest, ChannelMappingIsFixedLengthAndPrefixed) {
  const std::string channel = WaiterRegistry::channel_for("order-123", "fp-abc");
  EXPECT_EQ(channel.substr(0, 7), "apex:w:");
  EXPECT_EQ(channel.size(), 7u + 32u);
  // Deterministic: same input, same channel. Different key, different channel.
  EXPECT_EQ(WaiterRegistry::channel_for("order-123", "fp-abc"), channel);
  EXPECT_NE(WaiterRegistry::channel_for("order-124", "fp-abc"), channel);
  EXPECT_NE(WaiterRegistry::channel_for("order-123", "fp-abd"), channel);
  EXPECT_EQ(WaiterRegistry::channel_for("order-123", "fp-abc").find("order-123"),
            std::string::npos)
      << "no raw user key in channel names";
}

TEST(WaiterRegistryTest, NotifyWakesRegisteredWaiters) {
  WaiterRegistry registry;
  Probe a;
  Probe b;
  const auto ra = registry.register_waiter("ch-1", a.weak(), a.callback());
  const auto rb = registry.register_waiter("ch-1", b.weak(), b.callback());
  ASSERT_FALSE(ra.rejected);
  ASSERT_FALSE(rb.rejected);
  EXPECT_EQ(registry.waiter_count("ch-1"), 2u);

  EXPECT_EQ(registry.notify("ch-1"), 2u);
  EXPECT_EQ(a.wakes.load(), 1);
  EXPECT_EQ(b.wakes.load(), 1);
  EXPECT_EQ(registry.waiter_count("ch-1"), 0u) << "slot erased on notify";
}

TEST(WaiterRegistryTest, NotifyWithNoWaitersIsHarmless) {
  WaiterRegistry registry;
  EXPECT_EQ(registry.notify("nothing-here"), 0u);
}

TEST(WaiterRegistryTest, CompletionBeforeRegistrationNeedsNoMemory) {
  // The §8 race is closed by PROTOCOL (register-then-always-recheck), not by
  // registry history: notify with nobody registered is a no-op, and a later
  // registration yields a normal live slot. The session's mandatory
  // immediate re-check then observes the terminal row — no hang, no flags,
  // no eviction policy needed.
  WaiterRegistry registry;
  EXPECT_EQ(registry.notify("ch-race"), 0u);
  Probe late;
  const auto receipt = registry.register_waiter("ch-race", late.weak(), late.callback());
  EXPECT_FALSE(receipt.rejected);
  EXPECT_NE(receipt.waiter_id, 0u);
  EXPECT_EQ(registry.waiter_count("ch-race"), 1u);
  // And a subsequent completion still wakes normally.
  EXPECT_EQ(registry.notify("ch-race"), 1u);
  EXPECT_EQ(late.wakes.load(), 1);
}

TEST(WaiterRegistryTest, UnregisterRemovesSingleWaiter) {
  WaiterRegistry registry;
  Probe a;
  Probe b;
  const auto ra = registry.register_waiter("ch-u", a.weak(), a.callback());
  const auto rb = registry.register_waiter("ch-u", b.weak(), b.callback());
  registry.unregister("ch-u", ra.waiter_id);
  EXPECT_EQ(registry.waiter_count("ch-u"), 1u);
  EXPECT_EQ(registry.notify("ch-u"), 1u);
  EXPECT_EQ(a.wakes.load(), 0);
  EXPECT_EQ(b.wakes.load(), 1);
  registry.unregister("ch-u", rb.waiter_id);  // No-op after erase; must not crash.
}

TEST(WaiterRegistryTest, ExpiredOwnersAreNeverWoken) {
  WaiterRegistry registry;
  int local_wakes = 0;
  {
    auto temporary = std::make_shared<int>(1);
    std::weak_ptr<void> weak = temporary;
    const auto receipt =
        registry.register_waiter("ch-exp", weak, [&] { ++local_wakes; });
    ASSERT_FALSE(receipt.rejected);
    EXPECT_EQ(registry.waiter_count("ch-exp"), 1u);
  }  // owner dies here.
  EXPECT_EQ(registry.waiter_count("ch-exp"), 0u);
  EXPECT_EQ(registry.notify("ch-exp"), 0u);
  EXPECT_EQ(local_wakes, 0) << "dead waiter must never be invoked";
}

TEST(WaiterRegistryTest, ProdWakesWithoutCompleting) {
  // Reconnect sweep semantics: waiters are woken but the slot persists and
  // a subsequent registration is NOT already_completed.
  WaiterRegistry registry;
  Probe a;
  const auto ra = registry.register_waiter("ch-p", a.weak(), a.callback());
  ASSERT_FALSE(ra.rejected);
  EXPECT_EQ(registry.prod_all(), 1u);
  EXPECT_EQ(a.wakes.load(), 1);
  Probe b;
  const auto rb = registry.register_waiter("ch-p", b.weak(), b.callback());
  EXPECT_FALSE(rb.rejected);
  EXPECT_NE(rb.waiter_id, 0u);
  EXPECT_EQ(registry.waiter_count("ch-p"), 2u);
}

TEST(WaiterRegistryTest, PerKeyCapRejectsExcessWaiters) {
  WaiterOptions options;
  options.max_waiters_per_key = 3;
  WaiterRegistry registry(options);
  std::vector<Probe> probes(5);
  int accepted = 0;
  int rejected = 0;
  for (auto& probe : probes) {
    const auto receipt = registry.register_waiter("ch-cap", probe.weak(), probe.callback());
    accepted += !receipt.rejected ? 1 : 0;
    rejected += receipt.rejected ? 1 : 0;
  }
  EXPECT_EQ(accepted, 3);
  EXPECT_EQ(rejected, 2);
  EXPECT_EQ(registry.waiter_count("ch-cap"), 3u);
  // A different key is unaffected by the cap on this one.
  Probe other;
  EXPECT_FALSE(registry.register_waiter("ch-other", other.weak(), other.callback()).rejected);
}

TEST(WaiterRegistryTest, ShutdownRejectsAndWakes) {
  WaiterRegistry registry;
  Probe a;
  const auto ra = registry.register_waiter("ch-s", a.weak(), a.callback());
  ASSERT_FALSE(ra.rejected);
  registry.shutdown();
  EXPECT_TRUE(registry.is_shutdown());
  EXPECT_EQ(a.wakes.load(), 1) << "shutdown wakes via prod_all";
  Probe b;
  EXPECT_TRUE(registry.register_waiter("ch-s", b.weak(), b.callback()).rejected);
  EXPECT_TRUE(registry.register_waiter("ch-new", b.weak(), b.callback()).rejected);
}

TEST(WaiterRegistryTest, UnrelatedKeysDoNotInterfere) {
  WaiterRegistry registry;
  Probe a;
  Probe b;
  ASSERT_FALSE(registry.register_waiter("ch-x", a.weak(), a.callback()).rejected);
  ASSERT_FALSE(registry.register_waiter("ch-y", b.weak(), b.callback()).rejected);
  EXPECT_EQ(registry.notify("ch-x"), 1u);
  EXPECT_EQ(a.wakes.load(), 1);
  EXPECT_EQ(b.wakes.load(), 0);
  EXPECT_EQ(registry.waiter_count("ch-y"), 1u);
}

}  // namespace
}  // namespace apex::idempotency
