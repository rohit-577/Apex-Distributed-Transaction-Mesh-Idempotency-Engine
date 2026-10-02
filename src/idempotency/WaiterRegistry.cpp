#include "idempotency/WaiterRegistry.hpp"

#include <mutex>
#include <unordered_map>
#include <utility>

#include <openssl/sha.h>

namespace apex::idempotency {

namespace {

constexpr const char* kChannelPrefix = "apex:w:";
constexpr const char* kChannelPattern = "apex:w:*";

struct Waiter {
  std::uint64_t id{0};
  std::weak_ptr<void> owner;
  WaiterRegistry::WakeCallback on_wake;
};

}  // namespace

const char* WaiterRegistry::channel_pattern() { return kChannelPattern; }

std::string WaiterRegistry::channel_for(const std::string& idempotency_key,
                                         const std::string& fingerprint) {
  const std::string input = idempotency_key + "\n" + fingerprint;
  unsigned char digest[SHA256_DIGEST_LENGTH]{};
  SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(32);
  for (int i = 0; i < 16; ++i) {
    hex += kHex[(digest[i] >> 4) & 0xF];
    hex += kHex[digest[i] & 0xF];
  }
  return std::string(kChannelPrefix) + hex;
}

struct WaiterRegistry::Slot {
  std::uint64_t next_id{1};
  std::unordered_map<std::uint64_t, Waiter> waiters;
};

struct WaiterRegistry::Shard {
  mutable std::mutex mutex;
  std::unordered_map<std::string, Slot> slots;
};

WaiterRegistry::WaiterRegistry(WaiterOptions options) : options_(options) {
  shards_.reserve(kShards);
  for (std::size_t i = 0; i < kShards; ++i) {
    shards_.push_back(std::make_unique<Shard>());
  }
}

WaiterRegistry::~WaiterRegistry() = default;

WaiterRegistry::Shard& WaiterRegistry::shard_for(const std::string& key) {
  static constexpr std::hash<std::string> kHash;
  return *shards_[kHash(key) % kShards];
}

const WaiterRegistry::Shard& WaiterRegistry::shard_for(const std::string& key) const {
  static constexpr std::hash<std::string> kHash;
  return *shards_[kHash(key) % kShards];
}

WaiterRegistry::Registration WaiterRegistry::register_waiter(const std::string& key,
                                                             std::weak_ptr<void> owner,
                                                             WakeCallback on_wake) {
  Registration receipt;
  if (shutdown_.load()) {
    receipt.rejected = true;
    return receipt;
  }
  Shard& shard = shard_for(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  Slot& slot = shard.slots[key];  // Creates on first waiter; erased on notify.
  if (slot.waiters.size() >= options_.max_waiters_per_key) {
    receipt.rejected = true;
    return receipt;
  }
  const std::uint64_t id = slot.next_id++;
  slot.waiters.emplace(id, Waiter{id, std::move(owner), std::move(on_wake)});
  receipt.waiter_id = id;
  return receipt;
}

void WaiterRegistry::unregister(const std::string& key, std::uint64_t waiter_id) {
  if (waiter_id == 0) {
    return;
  }
  Shard& shard = shard_for(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto slot = shard.slots.find(key);
  if (slot == shard.slots.end()) {
    return;  // Already notified/erased: nothing to remove.
  }
  slot->second.waiters.erase(waiter_id);
}

std::size_t WaiterRegistry::notify(const std::string& key) {
  std::vector<WakeCallback> live;
  {
    Shard& shard = shard_for(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    const auto slot = shard.slots.find(key);
    if (slot == shard.slots.end()) {
      return 0;  // Nobody waiting (or already notified): nothing to do.
    }
    for (auto& [id, waiter] : slot->second.waiters) {
      if (!waiter.owner.expired()) {
        live.push_back(std::move(waiter.on_wake));
      }
    }
    shard.slots.erase(slot);
  }
  // Outside the lock: callbacks only asio::post back to session strands.
  for (auto& wake : live) {
    wake();
  }
  return live.size();
}

std::size_t WaiterRegistry::prod_all() {
  std::vector<WakeCallback> live;
  for (auto& shard_ptr : shards_) {
    std::lock_guard<std::mutex> lock(shard_ptr->mutex);
    for (auto& [key, slot] : shard_ptr->slots) {
      for (auto& [id, waiter] : slot.waiters) {
        if (!waiter.owner.expired()) {
          live.push_back(waiter.on_wake);  // Copy: slot persists.
        }
      }
    }
  }
  for (auto& wake : live) {
    wake();
  }
  return live.size();
}

void WaiterRegistry::shutdown() {
  shutdown_.store(true);
  prod_all();  // Wake everything once; settlers answer 202 via is_shutdown().
}

bool WaiterRegistry::is_shutdown() const { return shutdown_.load(); }

std::size_t WaiterRegistry::waiter_count(const std::string& key) const {
  const Shard& shard = shard_for(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto slot = shard.slots.find(key);
  if (slot == shard.slots.end()) {
    return 0;
  }
  std::size_t live = 0;
  for (const auto& [id, waiter] : slot->second.waiters) {
    live += waiter.owner.expired() ? 0 : 1;
  }
  return live;
}

}  // namespace apex::idempotency
