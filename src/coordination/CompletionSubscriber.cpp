#include "coordination/CompletionSubscriber.hpp"

#include <chrono>
#include <utility>

#include "coordination/RedisClient.hpp"
#include "idempotency/WaiterRegistry.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"

namespace apex::coordination {

namespace {

// Reconnect backoff: fixed, modest, on the subscriber thread only (test and
// shutdown paths never wait on it: stop() only waits for the in-flight read
// timeout, and tests gate reconnect assertions on state, not time).
constexpr std::chrono::milliseconds kReconnectBackoff{500};

}  // namespace

CompletionSubscriber::CompletionSubscriber(std::shared_ptr<RedisClient> redis,
                                           idempotency::WaiterRegistry& registry,
                                           observability::Logger& logger,
                                           std::shared_ptr<observability::Metrics> metrics)
    : redis_(std::move(redis)), registry_(registry), logger_(logger), metrics_(std::move(metrics)) {}

CompletionSubscriber::~CompletionSubscriber() { stop(); }

void CompletionSubscriber::start() {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) {
    return;
  }
  stop_.store(false);
  thread_ = std::thread([this] { run(); });
}

void CompletionSubscriber::stop() {
  stop_.store(true);
  if (thread_.joinable()) {
    thread_.join();
  }
}

void CompletionSubscriber::run() {
  // Reconnect loop: any RedisError (refused, dropped, timed-out server)
  // ends the blocking psubscribe_loop, and we come back here. Before
  // resubscribing, sweep every local waiter to re-check durable state:
  // anything that completed — or published — while we were deaf converges
  // without waiting for the next message (INV-MUX-05, structural).
  bool first = true;
  while (!stop_.load()) {
    if (!first) {
      ++reconnects_;  // Only genuine re-subscriptions count, not the initial one.
      metrics_->increment_subscriber_reconnects();
      const auto swept = registry_.prod_all();
      logger_.warning("subscriber reconnected; swept " + std::to_string(swept) +
                      " local waiters to re-check durable state");
      std::this_thread::sleep_for(kReconnectBackoff);
      if (stop_.load()) {
        return;
      }
    }
    first = false;
    try {
      redis_->psubscribe_loop(
          idempotency::WaiterRegistry::channel_pattern(),
          [this](const std::string& /*pattern*/, const std::string& channel,
                 const std::string& /*message*/) {
            // Wake-only: the channel names the logical operation; the row
            // decides. Never parse the (empty) payload for meaning.
            const std::size_t woken = registry_.notify(channel);
            logger_.debug("subscriber wake channel=" + channel + " waiters=" +
                          std::to_string(woken));
          },
          stop_);
    } catch (const RedisError& e) {
      if (stop_.load()) {
        return;
      }
      logger_.warning(std::string("subscriber connection lost: ") + e.what());
      // Loop around: backoff, resubscribe, sweep.
    }
  }
}

}  // namespace apex::coordination
