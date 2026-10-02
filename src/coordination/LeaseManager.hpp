#pragma once

// Lease-based ownership over a single Redis instance (NOT Redlock: one
// instance, no quorum, documented as such in docs/lease-and-fencing.md).
//
// Concepts:
// - Lease key: `apex:lease:<idempotency-key>`. The fixed prefix namespaces
//   coordination keys away from anything else; the idempotency key is
//   embedded EXACTLY (never hashed, never normalized — the same no-collision
//   policy as the key itself). The key charset is Redis-safe by construction.
// - Owner token: 128-bit cryptographic randomness (OpenSSL RAND_bytes),
//   hex-encoded, minted FRESH on every acquisition attempt. It names one
//   specific lease holder. PID/thread/time/counters are all forgeable or
//   reusable across restarts — randomness is the only property that makes
//   "is this MY lease" answerable after expiry and recovery.
// - Acquisition: SET key token NX PX ttl — atomic by Redis decree. Won or
//   held, no middle state.
// - Release: Lua compare-and-delete (see RedisClient). Never blind DEL.
// - No renewal in Phase 2: the ownership window is one TTL. An owner that
//   overruns does NOT lose correctness — fencing epochs (not the lease)
//   decide whose terminal write counts. Renewal would add a background
//   system for no correctness gain; it arrives only if a later execution
//   model genuinely needs it.
//
// Threading: blocking, worker threads only (INV-01). LeaseManager holds no
// connections itself (RedisClient does) and no idempotency state.

#include <chrono>
#include <memory>
#include <string>

namespace apex::observability {
class Logger;
}

namespace apex::coordination {

class RedisClient;
class RedisError;

struct LeaseAttempt {
  enum class Result {
    Acquired,        // This caller now holds the lease (token valid).
    HeldByOther,     // A value is stored; someone else owns it.
    RedisUnavailable,  // Fail-closed: Redis could not be reached.
  };

  Result result{Result::RedisUnavailable};
  std::string token;  // Meaningful only when Acquired.
};

class LeaseManager {
 public:
  LeaseManager(std::shared_ptr<RedisClient> redis, std::chrono::milliseconds ttl,
               observability::Logger& logger);

  // Attempts one atomic acquisition with a FRESH owner token. Never throws:
  // Redis failures map to RedisUnavailable (fail-closed — the caller must
  // NOT proceed as owner).
  [[nodiscard]] LeaseAttempt try_acquire(const std::string& idempotency_key);

  // True iff a lease value is currently stored. Liveness hint only (INV-09):
  // present does not prove the holder is alive, absent does not prove the
  // operation is ownerless — it only gates the recovery path. Throws
  // RedisError when Redis is unreachable (caller decides: fail closed).
  [[nodiscard]] bool is_held(const std::string& idempotency_key);

  // Token-guarded release. Returns true iff OUR token was stored (lease
  // released); false means it expired, was recovered, or never ours — all
  // safe, all logged. Never throws (failures are logged, not fatal: the TTL
  // cleans up regardless).
  bool release(const std::string& idempotency_key, const std::string& token);

  // Deterministic namespace mapping. Public so tests can assert on it and
  // operators can inspect keys with redis-cli.
  [[nodiscard]] static std::string lease_key_for(const std::string& idempotency_key);

  // 128-bit cryptographic owner token, hex-encoded (32 chars). A fresh token
  // per attempt — even immediate retries — so no two holders share identity.
  [[nodiscard]] static std::string new_token();

 private:
  std::shared_ptr<RedisClient> redis_;
  std::chrono::milliseconds ttl_;
  observability::Logger& logger_;
};

}  // namespace apex::coordination
