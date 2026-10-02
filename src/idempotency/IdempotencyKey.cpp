#include "idempotency/IdempotencyKey.hpp"

namespace apex::idempotency {

namespace {

constexpr std::size_t kMaxKeyLength = 255;

bool is_allowed(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '_' || c == '.' || c == ':';
}

}  // namespace

KeyCheck validate_key(std::string_view key) {
  if (key.empty()) {
    return {false, KeyProblem::Empty};
  }
  if (key.size() > kMaxKeyLength) {
    return {false, KeyProblem::TooLong};
  }
  for (char c : key) {
    if (!is_allowed(c)) {
      return {false, KeyProblem::BadCharacters};
    }
  }
  return {true, KeyProblem::None};
}

const char* key_problem_reason(KeyProblem problem) {
  switch (problem) {
    case KeyProblem::None:
      return "ok";
    case KeyProblem::Empty:
      return "empty";
    case KeyProblem::TooLong:
      return "too_long";
    case KeyProblem::BadCharacters:
      return "invalid_characters";
  }
  return "unknown";
}

}  // namespace apex::idempotency
