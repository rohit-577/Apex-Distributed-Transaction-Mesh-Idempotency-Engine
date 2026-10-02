#pragma once

// Deterministic simulated business operation (Phase 1 stand-in).
//
// The durable idempotency layer — not the operation itself — is what Phase 1
// must prove, so the "work" behind a key is a pure function of the canonical
// request body instead of a real side effect:
//
// - Any valid canonical body without `"fail": true` succeeds with HTTP 200
//   and a JSON echo of the request. Same input always yields the same output,
//   which is exactly the property replay tests need.
// - A body whose top-level object contains `"fail": true` fails
//   deterministically with error_code `simulated_failure`. This exercises the
//   FAILED path (repository transition, terminal policy, 409-on-retry)
//   without any real-world side effects.
//
// A later phase replaces run_simulated with real work behind the same
// repository interface; the idempotency semantics do not change.

#include <string>

namespace apex::idempotency {

struct SimulatedResult {
  bool success{false};
  int http_status{500};
  std::string body;
  std::string content_type{"application/json"};
  std::string error_code;
  std::string error_message;
};

[[nodiscard]] SimulatedResult run_simulated(const std::string& canonical_body);

}  // namespace apex::idempotency
