#include "idempotency/CorrelationId.hpp"

#include <stdexcept>

#include <openssl/rand.h>

namespace apex::idempotency {

std::optional<std::string> validate_correlation_id(std::string_view value) {
  if (value.empty() || value.size() > 64) {
    return std::nullopt;
  }
  for (char c : value) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) {
      return std::nullopt;
    }
  }
  return std::string(value);
}

std::string new_correlation_id() {
  unsigned char bytes[8]{};
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
    throw std::runtime_error("CSPRNG failure while minting correlation ID");
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(16);
  for (unsigned char byte : bytes) {
    out += kHex[(byte >> 4) & 0xF];
    out += kHex[byte & 0xF];
  }
  return out;
}

}  // namespace apex::idempotency
