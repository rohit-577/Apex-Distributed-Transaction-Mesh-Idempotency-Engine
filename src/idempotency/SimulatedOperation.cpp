#include "idempotency/SimulatedOperation.hpp"

#include <nlohmann/json.hpp>

namespace apex::idempotency {

SimulatedResult run_simulated(const std::string& canonical_body) {
  const nlohmann::json body =
      nlohmann::json::parse(canonical_body, /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (body.is_discarded()) {
    // Cannot happen for fingerprints produced by fingerprint_for (the input
    // is already canonical), but robustness beats assertion here: a corrupt
    // canonical body must fail the operation, never crash the worker.
    SimulatedResult result;
    result.error_code = "invalid_canonical_body";
    result.error_message = "Stored canonical body is not valid JSON.";
    result.body = R"({"error":"operation_failed","reason":"invalid_canonical_body"})";
    return result;
  }

  if (body.is_object()) {
    const auto fail = body.find("fail");
    if (fail != body.end() && fail->is_boolean() && fail->get<bool>()) {
      SimulatedResult result;
      result.success = false;
      result.http_status = 500;
      result.error_code = "simulated_failure";
      result.error_message = "Simulated operation was asked to fail (\"fail\": true).";
      result.body = nlohmann::json({{"error", "operation_failed"},
                                    {"reason", "simulated_failure"}})
                        .dump();
      return result;
    }
  }

  SimulatedResult result;
  result.success = true;
  result.http_status = 200;
  result.body = nlohmann::json({{"result", "ok"}, {"request", body}}).dump();
  return result;
}

}  // namespace apex::idempotency
