// Lease contract unit tests: namespace mapping, owner-token properties,
// and the shared log-safety policy. Pure CPU, no infrastructure.

#include <string>
#include <unordered_set>

#include <gtest/gtest.h>

#include "coordination/LeaseManager.hpp"
#include "observability/Logger.hpp"

namespace apex::coordination {
namespace {

TEST(LeaseContractTest, KeyNamespaceIsDeterministicAndPrefixed) {
  EXPECT_EQ(LeaseManager::lease_key_for("order-123"), "apex:lease:order-123");
  EXPECT_EQ(LeaseManager::lease_key_for(""), "apex:lease:");
  // The idempotency key is embedded exactly: no hashing, no normalization,
  // so distinct client keys can never collide after mapping.
  EXPECT_EQ(LeaseManager::lease_key_for("a:b"), "apex:lease:a:b");
}

TEST(LeaseContractTest, OwnerTokensAreHexAndUnique) {
  const std::string first = LeaseManager::new_token();
  EXPECT_EQ(first.size(), 32u) << "128 bits hex-encoded";
  EXPECT_TRUE(first.find_first_not_of("0123456789abcdef") == std::string::npos);

  // Fresh token per attempt: 1000 mints, zero collisions (would require
  // defeating 128-bit randomness, not luck).
  std::unordered_set<std::string> seen;
  for (int i = 0; i < 1000; ++i) {
    EXPECT_TRUE(seen.insert(LeaseManager::new_token()).second) << "token collision at " << i;
  }
}

TEST(LeaseContractTest, SafeKeyTruncatesLongKeys) {
  using apex::observability::safe_key;
  EXPECT_EQ(safe_key("short"), "short");
  EXPECT_EQ(safe_key(std::string(16, 'k')), std::string(16, 'k'));
  const std::string truncated = safe_key(std::string(255, 'k'));
  EXPECT_EQ(truncated, std::string(16, 'k') + "...(len=255)");
}

}  // namespace
}  // namespace apex::coordination
