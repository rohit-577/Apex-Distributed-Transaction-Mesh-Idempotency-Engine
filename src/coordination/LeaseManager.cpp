#include "coordination/LeaseManager.hpp"

#include <stdexcept>

#include <openssl/rand.h>

#include "coordination/RedisClient.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"

namespace apex::coordination {

namespace {

constexpr const char* kLeasePrefix = "apex:lease:";

std::string to_hex(const unsigned char* bytes, std::size_t count) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(count * 2);
  for (std::size_t i = 0; i < count; ++i) {
    out += kHex[(bytes[i] >> 4) & 0xF];
    out += kHex[bytes[i] & 0xF];
  }
  return out;
}

}  // namespace

LeaseManager::LeaseManager(std::shared_ptr<RedisClient> redis, std::chrono::milliseconds ttl,
                           observability::Logger& logger,
                           std::shared_ptr<observability::Metrics> metrics)
    : redis_(std::move(redis)), ttl_(ttl), logger_(logger), metrics_(std::move(metrics)) {}

std::string LeaseManager::lease_key_for(const std::string& idempotency_key) {
  return std::string(kLeasePrefix) + idempotency_key;
}

std::string LeaseManager::new_token() {
  // 128 bits from the OS CSPRNG via OpenSSL. Unpredictability is the point:
  // after a lease expires, NOBODY — not the previous holder, not a new
  // contender — may mistake one generation for another.
  unsigned char bytes[16]{};
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
    // Practically unreachable (RAND_bytes fails only when the CSPRNG is
    // broken, in which case proceeding with a weak token would be worse
    // than failing). Fail closed, like everything else here.
    throw RedisError("CSPRNG failure while minting lease owner token");
  }
  return to_hex(bytes, sizeof(bytes));
}

LeaseAttempt LeaseManager::try_acquire(const std::string& idempotency_key) {
  const std::string key = lease_key_for(idempotency_key);
  const std::string logged = observability::safe_key(idempotency_key);
  std::string token;
  try {
    token = new_token();
  } catch (const std::exception& e) {
    logger_.error("lease key=" + logged + " token minting failed: " + e.what());
    return {LeaseAttempt::Result::RedisUnavailable, ""};
  }
  try {
    if (redis_->set_if_absent(key, token, ttl_)) {
      logger_.info("lease key=" + logged + " acquired ttl_ms=" + std::to_string(ttl_.count()));
      metrics_->increment_lease_acquired();
      return {LeaseAttempt::Result::Acquired, token};
    }
    logger_.info("lease key=" + logged + " held-by-other");
    metrics_->increment_lease_held();
    return {LeaseAttempt::Result::HeldByOther, ""};
  } catch (const RedisError& e) {
    logger_.error("lease key=" + logged + " acquire failed (Redis unavailable): " + e.what());
    metrics_->increment_redis_failures();
    return {LeaseAttempt::Result::RedisUnavailable, ""};
  }
}

bool LeaseManager::is_held(const std::string& idempotency_key) {
  try {
    return redis_->get(lease_key_for(idempotency_key)).has_value();
  } catch (const RedisError&) {
    metrics_->increment_redis_failures();
    throw;
  }
}

bool LeaseManager::release(const std::string& idempotency_key, const std::string& token) {
  const std::string logged = observability::safe_key(idempotency_key);
  if (token.empty()) {
    return false;  // Never attempt an unguarded delete.
  }
  try {
    const bool released =
        redis_->compare_and_delete(lease_key_for(idempotency_key), token) == 1;
    if (released) {
      logger_.info("lease key=" + logged + " released");
      metrics_->increment_lease_releases();
    } else {
      // Expired, recovered by a newer owner, or never ours: the TTL owns
      // cleanup from here. This is EXPECTED on the stale-owner path, not an
      // error — hence info, and hence no retry.
      logger_.info("lease key=" + logged + " release rejected (token mismatch)");
      metrics_->increment_lease_release_rejected();
    }
    return released;
  } catch (const RedisError& e) {
    logger_.error("lease key=" + logged + " release failed (Redis unavailable): " + e.what());
    metrics_->increment_redis_failures();
    return false;
  } catch (const std::exception& e) {
    // Belt-and-braces: release() runs inside ~LeaseReleaser (implicitly
    // noexcept), so NOTHING may escape — not even a non-Redis failure.
    logger_.error("lease key=" + logged + " release failed unexpectedly: " + e.what());
    return false;
  } catch (...) {
    logger_.error("lease key=" + logged + " release failed with unknown error");
    return false;
  }
}

}  // namespace apex::coordination
