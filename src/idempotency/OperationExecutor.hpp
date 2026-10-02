#pragma once

// Business-operation seam: what "executing" means behind an idempotency key.
//
// Phase 3 multiplexing must prove "N duplicates => exactly 1 execution", and
// proving that needs a deterministic execution counter/hook. Rather than
// special-casing production code for tests, the operation is a dependency:
// production wires SimulatedExecutor (identical behavior to the former free
// function), tests wire counting/gated executors. A later phase replaces
// the simulated implementation with real work behind this same interface;
// idempotency semantics do not change.

#include <string>

namespace apex::idempotency {

struct ExecutionResult {
  bool success{false};
  int http_status{500};
  std::string body;
  std::string content_type{"application/json"};
  std::string error_code;
  std::string error_message;
};

class OperationExecutor {
 public:
  virtual ~OperationExecutor() = default;

  // Runs the operation for an already-canonical request body. Deterministic
  // implementations return identical results for identical input (required
  // for replay tests). Called on database worker threads only.
  [[nodiscard]] virtual ExecutionResult execute(const std::string& canonical_body) = 0;
};

// Deterministic stand-in for real work:
// - Any canonical body without top-level `"fail": true` succeeds with HTTP
//   200 and a JSON echo of the request.
// - `"fail": true` fails deterministically with `simulated_failure`.
// Same input always yields the same output — the property replay needs.
class SimulatedExecutor : public OperationExecutor {
 public:
  [[nodiscard]] ExecutionResult execute(const std::string& canonical_body) override;
};

}  // namespace apex::idempotency
