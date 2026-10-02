#include "idempotency/Fingerprint.hpp"

#include <algorithm>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

namespace apex::idempotency {

namespace {

// Canonical serialization: objects sorted by key, arrays in order, compact
// separators, strings via operator<< equivalent escaping (json::dump on the
// scalar). Deterministic across runs and platforms for the same value.
void dump_canonical(const nlohmann::json& value, std::string& out) {
  switch (value.type()) {
    case nlohmann::json::value_t::object: {
      out += '{';
      std::vector<std::string> keys;
      keys.reserve(value.size());
      for (const auto& [key, _] : value.items()) {
        keys.push_back(key);
      }
      std::sort(keys.begin(), keys.end());
      bool first = true;
      for (const std::string& key : keys) {
        if (!first) {
          out += ',';
        }
        first = false;
        out += nlohmann::json(key).dump();
        out += ':';
        dump_canonical(value.at(key), out);
      }
      out += '}';
      return;
    }
    case nlohmann::json::value_t::array: {
      out += '[';
      bool first = true;
      for (const nlohmann::json& item : value) {
        if (!first) {
          out += ',';
        }
        first = false;
        dump_canonical(item, out);
      }
      out += ']';
      return;
    }
    default:
      out += value.dump();
      return;
  }
}

}  // namespace

std::optional<std::string> canonical_json(const std::string& raw) {
  // No-throw parse: returns a discarded value instead of raising.
  const nlohmann::json parsed = nlohmann::json::parse(raw, /*cb=*/nullptr,
                                                      /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return std::nullopt;
  }
  std::string out;
  dump_canonical(parsed, out);
  return out;
}

std::string sha256_hex(const std::string& bytes) {
  unsigned char digest[SHA256_DIGEST_LENGTH]{};
  SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(SHA256_DIGEST_LENGTH * 2);
  for (unsigned char byte : digest) {
    hex += kHex[(byte >> 4) & 0xF];
    hex += kHex[byte & 0xF];
  }
  return hex;
}

Fingerprint fingerprint_for(const std::string& method, const std::string& route,
                            const std::string& raw_body) {
  const std::optional<std::string> canonical = canonical_json(raw_body);
  if (!canonical) {
    return {false, "", ""};
  }
  const std::string payload = method + "\n" + route + "\n" + *canonical;
  return {true, sha256_hex(payload), *canonical};
}

}  // namespace apex::idempotency
