#pragma once

// Idempotency-Key validation rules (part of the POST /v1/operations contract,
// documented in docs/architecture.md):
//
// - Missing header  -> caller reports `missing_idempotency_key` (HTTP 400).
// - Empty value     -> `invalid_idempotency_key`, reason "empty" (HTTP 400).
// - Longer than 255 -> `invalid_idempotency_key`, reason "too_long" (HTTP 400).
//   Keys are NEVER truncated: silent truncation could merge two distinct
//   client keys into one record.
// - Other violations -> `invalid_idempotency_key` with a reason (HTTP 400).
//
// Allowed characters: ASCII letters, digits, and `-` `_` `.` `:`.
// The set is deliberately conservative (header- and URL-safe) and the key is
// stored EXACTLY as received: no case folding, no trimming, no other
// normalization, because any normalization risks colliding two distinct
// client keys. Clients that need case-insensitivity must normalize before
// sending.

#include <string>
#include <string_view>

namespace apex::idempotency {

enum class KeyProblem {
  None,
  Empty,
  TooLong,
  BadCharacters,
};

struct KeyCheck {
  bool ok{false};
  KeyProblem problem{KeyProblem::Empty};
};

// Validates an Idempotency-Key header VALUE. An absent header is detected by
// the caller (missing != empty) and is not representable here.
[[nodiscard]] KeyCheck validate_key(std::string_view key);

[[nodiscard]] const char* key_problem_reason(KeyProblem problem);

}  // namespace apex::idempotency
