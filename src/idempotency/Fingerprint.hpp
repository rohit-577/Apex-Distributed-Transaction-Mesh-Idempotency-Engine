#pragma once

// Request fingerprinting for POST /v1/operations.
//
// The fingerprint identifies the LOGICAL request being protected, so that:
//   same key + same fingerprint     -> same operation (replay is safe), and
//   same key + different fingerprint -> client error (409, never executed).
//
// Fingerprint input: HTTP method, normalized route, canonical request body.
// Fingerprint value: lowercase hex SHA-256 over
//   method + "\n" + route + "\n" + canonical_body.
//
// Canonical JSON: the body is parsed and re-serialized with object keys
// sorted byte-wise, no insignificant whitespace, and standard string
// escaping. Semantically identical bodies with different key order or
// whitespace therefore share a fingerprint. Raw bytes are deliberately NOT
// hashed: two byte streams can encode the same JSON value.
//
// Documented limitations of this Phase 1 canonicalization (it is
// deterministic, not universal):
// - `1` and `1.0` are different fingerprints (numeric lexical form matters).
// - Duplicate object keys resolve parser-last-wins before canonicalization.
// - Non-JSON bodies are rejected (ok == false); only JSON operations exist
//   in Phase 1.
// - Route normalization is minimal today (query stripped, exact match):
//   richer normalization arrives with more routes.
//
// SHA-256 comes from OpenSSL (not hand-rolled: never roll your own crypto,
// even for non-security hashing where a collision would corrupt dedup).

#include <optional>
#include <string>

namespace apex::idempotency {

// Parses `raw` as JSON and returns its canonical form, or nullopt when the
// body is not valid JSON.
[[nodiscard]] std::optional<std::string> canonical_json(const std::string& raw);

// Lowercase hex SHA-256 of `bytes`.
[[nodiscard]] std::string sha256_hex(const std::string& bytes);

struct Fingerprint {
  bool ok{false};
  std::string hex;             // 64 lowercase hex chars when ok.
  std::string canonical_body;  // Canonical JSON that was hashed.
};

// Builds the operation fingerprint. `route` must already be normalized
// (query stripped). ok == false means the body was not valid JSON.
[[nodiscard]] Fingerprint fingerprint_for(const std::string& method, const std::string& route,
                                          const std::string& raw_body);

}  // namespace apex::idempotency
