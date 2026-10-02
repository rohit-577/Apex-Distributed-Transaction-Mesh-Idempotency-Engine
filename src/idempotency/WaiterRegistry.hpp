#pragma once

// Process-local waiter registry: efficient same-process coordination for
// duplicate requests. An OPTIMIZATION, never an authority (INV-MUX-11):
// every waiter verdict still comes from re-reading PostgreSQL, so losing
// this entire registry (process restart) cannot corrupt correctness.
//
// Model: waiter-topic -> slot shared by all waiters of one logical
// operation (see channel_for). A topic identifies (idempotency key,
// fingerprint) without exposing either.
//
// Completion-before-subscription (§8) is closed by PROTOCOL, not by
// registry memory: every registration is immediately followed by a durable
// re-check, so a terminal write landing anywhere before that re-check's
// read is observed. The registry stores no completed flags, remembers no
// history, and needs no eviction policy — erase-on-notify is safe because
// no waiter ever waits without a fresh re-check first.
// - register(): creates or joins the slot. Returns already_completed=true
//   when the slot was notified before this registration landed — the
//   completion-before-subscription race (§8) closes here: the caller must
//   re-check durable state immediately instead of waiting.
// - notify(): marks the slot completed, detaches all waiters, erases the
//   slot. Callbacks run OUTSIDE all locks (they only asio::post).
// - prod(): wakes current waiters WITHOUT completing (subscriber-reconnect
//   sweep, shutdown): they re-check durable state and re-register if still
//   PROCESSING.
// - unregister(): removes one waiter (settle path). Empty non-completed
//   slots linger until notify/prod/cleanup; they hold no resources beyond
//   the map entry and are bounded by key cardinality + waiter timeout.
// - shutdown(): rejects new registrations, wakes everything once.
//   Waiters settling during shutdown answer 202 and never re-register.
//
// Threading: all methods are thread-safe. Sharded mutexes (fixed count by
// key hash) keep unrelated keys from contending; callbacks never run under
// a lock, so no lock ordering exists to deadlock.
// Lifetimes: waiters are (weak owner, wake callback) pairs. Expired owners
// are pruned on notify/prod/cleanup and never invoked (no use-after-free,
// no leaks: the registry never holds a strong session reference).
// DoS bound: max_waiters_per_key rejections keep one key from growing the
// registry without bound (202 + log, never an exception).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace apex::idempotency {

struct WaiterOptions {
  // Maximum time one HTTP request waits for completion. On expiry the
  // request answers 202; the durable record is untouched (INV-MUX-06).
  long long timeout_ms{30000};
  // Fallback durable re-check interval (missed-notification recovery).
  // Pub/sub wake-ups arrive sooner when delivery works; this bound only
  // fires when they don't. Never a busy loop: one Asio timer per waiter.
  long long recheck_ms{1000};
  // Per-key waiter cap (DoS bound, §29). Rejections answer 202 immediately.
  std::size_t max_waiters_per_key{1024};
};

class WaiterRegistry {
 public:
  using WakeCallback = std::function<void()>;

  struct Registration {
    std::uint64_t waiter_id{0};
    bool rejected{false};  // shutdown or per-key cap: caller must not wait.
  };

  // Waiter topic for one logical operation (key + fingerprint):
  // `apex:w:<32 hex>` = first 128 bits of SHA-256 over `key + "\n" + fp`.
  // Bounded (fixed length), opaque (no raw user keys, nothing sensitive),
  // collision-harmless (a spurious wake only triggers a durable re-check
  // that finds PROCESSING). Shared by publishers, subscribers, sessions,
  // and tests so all sides agree without shared state.
  [[nodiscard]] static std::string channel_for(const std::string& idempotency_key,
                                               const std::string& fingerprint);
  [[nodiscard]] static const char* channel_pattern();

  explicit WaiterRegistry(WaiterOptions options = {});
  ~WaiterRegistry();

  WaiterRegistry(const WaiterRegistry&) = delete;
  WaiterRegistry& operator=(const WaiterRegistry&) = delete;

  [[nodiscard]] Registration register_waiter(const std::string& key,
                                             std::weak_ptr<void> owner,
                                             WakeCallback on_wake);
  void unregister(const std::string& key, std::uint64_t waiter_id);

  // Terminal event for `key`: completes the slot, wakes members, erases.
  // Returns the number of live waiters woken.
  std::size_t notify(const std::string& key);

  // Transient prod: wakes current members WITHOUT completing (reconnect
  // sweep). They re-check PostgreSQL and re-register as needed.
  std::size_t prod_all();

  void shutdown();
  [[nodiscard]] bool is_shutdown() const;

  // Test/ops hooks (read-only snapshots, no decisions made from them).
  [[nodiscard]] std::size_t waiter_count(const std::string& key) const;
  [[nodiscard]] WaiterOptions options() const { return options_; }

 private:
  struct Slot;
  struct Shard;
  static constexpr std::size_t kShards = 16;

  Shard& shard_for(const std::string& key);
  const Shard& shard_for(const std::string& key) const;

  WaiterOptions options_;
  std::vector<std::unique_ptr<Shard>> shards_;
  mutable std::atomic<bool> shutdown_{false};
};

}  // namespace apex::idempotency
