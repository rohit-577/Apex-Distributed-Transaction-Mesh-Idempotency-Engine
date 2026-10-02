#pragma once

// Deterministic operation hook for multiplexing tests (§17). Counts every
// execution and can gate the owner mid-flight so tests control the exact
// interleaving (owner stays PROCESSING while waiters register), using
// test-side synchronization only — production code paths are untouched.
//
// Lifecycle discipline: every test that closes the gate MUST open it before
// teardown (otherwise pool threads block forever and fixture teardown
// hangs). Prefer open-by-default with explicit close windows, or a scope
// guard. The counter is atomic; the gate is a mutex+condition_variable pair
// living ENTIRELY in test code.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "idempotency/OperationExecutor.hpp"

namespace apex::test {

class GatedExecutor : public idempotency::OperationExecutor {
 public:
  explicit GatedExecutor(bool start_open = true) : open_(start_open) {}

  idempotency::ExecutionResult execute(const std::string& canonical_body) override {
    ++executions_;
    std::unique_lock<std::mutex> lock(mutex_);
    gate_.wait(lock, [this] { return open_; });
    // Deterministic simulated result (same contract as production).
    lock.unlock();
    return simulated_.execute(canonical_body);
  }

  // Blocks the next (and current) executions inside execute().
  void close_gate() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
  }

  // Releases all blocked executions. Idempotent; safe to call twice.
  void open_gate() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
    gate_.notify_all();
  }

  [[nodiscard]] std::uint64_t executions() const { return executions_.load(); }

 private:
  std::atomic<std::uint64_t> executions_{0};
  std::mutex mutex_;
  std::condition_variable gate_;
  bool open_;
  idempotency::SimulatedExecutor simulated_;
};

// Scope guard: opens the gate on destruction so a failing test cannot hang
// fixture teardown with a blocked owner thread.
class GateOpener {
 public:
  explicit GateOpener(GatedExecutor& executor) : executor_(executor) {}
  ~GateOpener() { executor_.open_gate(); }

  GateOpener(const GateOpener&) = delete;
  GateOpener& operator=(const GateOpener&) = delete;

 private:
  GatedExecutor& executor_;
};

}  // namespace apex::test
