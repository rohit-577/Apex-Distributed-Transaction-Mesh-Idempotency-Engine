// Request-contract unit tests: key rules, canonical JSON, fingerprinting,
// and the simulated operation. Pure CPU, no I/O, hermetic — every case can
// fail for a real reason.

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "idempotency/Fingerprint.hpp"
#include "idempotency/IdempotencyKey.hpp"
#include "idempotency/SimulatedOperation.hpp"
#include "persistence/IdempotencyRepository.hpp"
#include "persistence/PgConnection.hpp"

namespace apex::idempotency {
namespace {

TEST(KeyValidationTest, TypicalKeysAreAccepted) {
  EXPECT_TRUE(validate_key("order-123").ok);
  EXPECT_TRUE(validate_key("550e8400-e29b-41d4-a716-446655440000").ok);
  EXPECT_TRUE(validate_key("a").ok);
  EXPECT_TRUE(validate_key("key_with.dots:and:colons-123_ABC").ok);
  EXPECT_TRUE(validate_key(std::string(255, 'k')).ok);
}

TEST(KeyValidationTest, EmptyIsDistinctFromMissing) {
  // An absent header is detected by Session (missing_idempotency_key); an
  // empty value reaches the validator and fails here with reason "empty".
  const KeyCheck check = validate_key("");
  EXPECT_FALSE(check.ok);
  EXPECT_EQ(check.problem, KeyProblem::Empty);
}

TEST(KeyValidationTest, OverlongKeysAreRejectedNeverTruncated) {
  const KeyCheck check = validate_key(std::string(256, 'k'));
  EXPECT_FALSE(check.ok);
  EXPECT_EQ(check.problem, KeyProblem::TooLong);
}

TEST(KeyValidationTest, BadCharactersAreRejected) {
  for (const std::string bad : {"has space", "tab\there", "at@sign", "sla/sh",
                                 "quote\"x", "uni→code", "semi;colon"}) {
    const KeyCheck check = validate_key(bad);
    EXPECT_FALSE(check.ok) << bad;
    EXPECT_EQ(check.problem, KeyProblem::BadCharacters) << bad;
  }
}

TEST(FingerprintTest, SameInputGivesSameFingerprint) {
  const Fingerprint first = fingerprint_for("POST", "/v1/operations", R"({"a":1})");
  const Fingerprint second = fingerprint_for("POST", "/v1/operations", R"({"a":1})");
  EXPECT_TRUE(first.ok);
  EXPECT_EQ(first.hex.size(), 64u);
  EXPECT_EQ(first.hex, second.hex);
  EXPECT_EQ(first.canonical_body, second.canonical_body);
}

TEST(FingerprintTest, KeyOrderAndWhitespaceDoNotMatter) {
  const Fingerprint a = fingerprint_for("POST", "/v1/operations", R"({"a":1,"b":[1,2]})");
  const Fingerprint b = fingerprint_for("POST", "/v1/operations", " { \"b\" : [1, 2] , \"a\" : 1 } ");
  EXPECT_TRUE(a.ok);
  EXPECT_TRUE(b.ok);
  EXPECT_EQ(a.hex, b.hex);
  EXPECT_EQ(a.canonical_body, R"({"a":1,"b":[1,2]})");
}

TEST(FingerprintTest, NestedObjectsCanonicalizeRecursively) {
  const Fingerprint fp = fingerprint_for("POST", "/v1/operations",
                                         R"({"z":{"y":1,"x":0},"a":null,"m":[true,false]})");
  EXPECT_TRUE(fp.ok);
  EXPECT_EQ(fp.canonical_body, R"({"a":null,"m":[true,false],"z":{"x":0,"y":1}})");
}

TEST(FingerprintTest, MethodRouteAndBodyAreAllLoadBearing) {
  const std::string base = fingerprint_for("POST", "/v1/operations", R"({"a":1})").hex;
  EXPECT_NE(fingerprint_for("PUT", "/v1/operations", R"({"a":1})").hex, base);
  EXPECT_NE(fingerprint_for("POST", "/v1/other", R"({"a":1})").hex, base);
  EXPECT_NE(fingerprint_for("POST", "/v1/operations", R"({"a":2})").hex, base);
}

TEST(FingerprintTest, InvalidJsonIsRejected) {
  EXPECT_FALSE(fingerprint_for("POST", "/v1/operations", "not json").ok);
  EXPECT_FALSE(fingerprint_for("POST", "/v1/operations", "").ok);
  EXPECT_FALSE(fingerprint_for("POST", "/v1/operations", R"({"a":})").ok);
}

TEST(FingerprintTest, Sha256MatchesKnownVector) {
  // "abc" is the canonical FIPS 180-4 test vector.
  EXPECT_EQ(sha256_hex("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(FingerprintTest, ScalarBodiesAreFingerprintedToo) {
  // Phase 1 accepts any JSON body shape; the fingerprint covers all of them.
  EXPECT_TRUE(fingerprint_for("POST", "/v1/operations", "[1,2]").ok);
  EXPECT_TRUE(fingerprint_for("POST", "/v1/operations", "\"str\"").ok);
  EXPECT_TRUE(fingerprint_for("POST", "/v1/operations", "42").ok);
}

TEST(SimulatedOperationTest, SuccessEchoesTheCanonicalRequest) {
  const SimulatedResult result = run_simulated(R"({"a":1})");
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.http_status, 200);
  EXPECT_EQ(result.content_type, "application/json");
  const nlohmann::json body = nlohmann::json::parse(result.body);
  EXPECT_EQ(body.at("result").get<std::string>(), "ok");
  EXPECT_EQ(body.at("request"), nlohmann::json::parse(R"({"a":1})"));
}

TEST(SimulatedOperationTest, FailFlagFailsDeterministically) {
  const SimulatedResult first = run_simulated(R"({"fail":true})");
  const SimulatedResult second = run_simulated(R"({"fail" : true, "x" : 1})");
  EXPECT_FALSE(first.success);
  EXPECT_EQ(first.http_status, 500);
  EXPECT_EQ(first.error_code, "simulated_failure");
  EXPECT_EQ(first.body, second.body) << "same logical request must fail identically";
}

TEST(SimulatedOperationTest, FailFlagMustBeBooleanTrue) {
  EXPECT_TRUE(run_simulated(R"({"fail":false})").success);
  EXPECT_TRUE(run_simulated(R"({"fail":"true"})").success);
  EXPECT_TRUE(run_simulated(R"([{"fail":true}])").success);
}

TEST(RecordStatusTest, RoundTripsAllStatesAndRejectsGarbage) {
  using apex::persistence::RecordStatus;
  EXPECT_EQ(apex::persistence::status_from_string("PROCESSING"), RecordStatus::Processing);
  EXPECT_EQ(apex::persistence::status_from_string("COMPLETED"), RecordStatus::Completed);
  EXPECT_EQ(apex::persistence::status_from_string("FAILED"), RecordStatus::Failed);
  EXPECT_STREQ(apex::persistence::to_string(RecordStatus::Processing), "PROCESSING");
  // EXPECT_THROW cannot take the nodiscard call directly (macro argument
  // splitting), so the throwing statements are wrapped in void lambdas.
  const auto parse_pending = [] { (void)apex::persistence::status_from_string("PENDING"); };
  const auto parse_empty = [] { (void)apex::persistence::status_from_string(""); };
  EXPECT_THROW(parse_pending(), apex::persistence::PgError);
  EXPECT_THROW(parse_empty(), apex::persistence::PgError);
}

}  // namespace
}  // namespace apex::idempotency
