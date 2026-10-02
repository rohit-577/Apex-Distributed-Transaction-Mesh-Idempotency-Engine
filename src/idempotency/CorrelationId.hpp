#pragma once

// Request correlation IDs (Phase 5): traceability WITHOUT identity.
//
// An idempotency key answers "which logical operation is this?" — it must be
// stable across retries by definition. A correlation ID answers "which HTTP
// attempt was this?" — it must be FRESH per attempt, or debugging concurrent
// duplicates becomes impossible (every waiter would share one ID).
//
// Rules:
// - Client may send `X-Request-ID`. Accepted iff 1–64 chars of
//   [A-Za-z0-9-_]; anything else is ignored (never a 400 — correlation is
//   metadata, not contract).
// - Otherwise the gateway mints 16 hex chars from the OS CSPRNG.
// - The ID travels in OperationRequest, appears in every log line for the
//   attempt (`cid=`), and is echoed back as the `X-Request-ID` response
//   header so clients can correlate both directions.
// - The ID NEVER enters the fingerprint: same key + same body + different
//   correlation IDs is the same logical operation (tested). Confusing the
//   two would break dedup; the distinction is documented in operations.md.

#include <optional>
#include <string>
#include <string_view>

namespace apex::idempotency {

// Validates a client-supplied correlation ID. nullopt = absent or invalid
// (caller mints a fresh one either way).
[[nodiscard]] std::optional<std::string> validate_correlation_id(std::string_view value);

// Mints a fresh 16-hex-char correlation ID. Throws std::runtime_error only
// on CSPRNG failure (fail closed, like owner tokens).
[[nodiscard]] std::string new_correlation_id();

}  // namespace apex::idempotency
