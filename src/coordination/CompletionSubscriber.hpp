#pragma once

// Cross-node wake-up via Redis Pub/Sub: an OPTIMIZATION, never correctness
// (INV-MUX-04/05). The owner publishes after its durable terminal commit;
// any gateway whose subscriber is alive wakes its local waiters, which then
// re-read PostgreSQL and decide from the row — never from the message.
//
// Topic namespace: WaiterRegistry::channel_for (`apex:w:<32 hex>`) with an
// always-empty payload (wake-only). Rationale — bounded length, no raw user
// keys, nothing sensitive, collision-harmless — lives with channel_for,
// the single definition all sides share.
//
// Subscriber model: ONE dedicated thread per process running the blocking
// psubscribe loop (a Redis Pub/Sub connection cannot be shared with
// commands). Reconnects are explicit: on connection loss the loop backs
// off, resubscribes, and sweeps the registry (prod_all) so notifications
// missed during the outage become immediate durable re-checks instead of
// hangs — this is what makes INV-MUX-05 structural rather than hopeful.
// Stop is prompt (bounded by the socket read timeout) and joinable:
// shutdown never hangs on Pub/Sub (P3-18 proves it).

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace apex::observability {
class Logger;
}

namespace apex::idempotency {
class WaiterRegistry;
}

namespace apex::coordination {

class RedisClient;

class CompletionSubscriber {
 public:
  // `pattern` defaults to the wildcard over the waiter namespace. The
  // subscriber shares the process RedisClient (dedicated connection taken
  // internally); it never issues commands on pooled connections.
  CompletionSubscriber(std::shared_ptr<RedisClient> redis,
                       idempotency::WaiterRegistry& registry, observability::Logger& logger);

  CompletionSubscriber(const CompletionSubscriber&) = delete;
  CompletionSubscriber& operator=(const CompletionSubscriber&) = delete;

  ~CompletionSubscriber();

  // Starts the subscriber thread (idempotent). The thread runs until stop().
  void start();
  // Signals stop and joins the thread. Idempotent, never throws, bounded by
  // the socket read timeout. Must precede destruction of the registry.
  void stop();

 private:
  void run();

  std::shared_ptr<RedisClient> redis_;
  idempotency::WaiterRegistry& registry_;
  observability::Logger& logger_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> started_{false};
};

}  // namespace apex::coordination
