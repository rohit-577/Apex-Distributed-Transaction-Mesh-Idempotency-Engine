#include "persistence/ConnectionPool.hpp"

#include "persistence/PgConnection.hpp"

namespace apex::persistence {

class PoolState {
 public:
  std::mutex mutex;
  std::condition_variable available;
  std::vector<std::unique_ptr<PgConnection>> idle;
  std::size_t total{0};  // idle.size() + checked-out count.
  bool closed{false};
};

ConnectionPool::Guard::Guard(std::shared_ptr<PoolState> state, std::unique_ptr<PgConnection> conn)
    : state_(std::move(state)), conn_(std::move(conn)) {}

ConnectionPool::Guard::Guard(Guard&&) noexcept = default;
ConnectionPool::Guard& ConnectionPool::Guard::operator=(Guard&&) noexcept = default;

ConnectionPool::Guard::~Guard() {
  if (conn_ == nullptr || state_ == nullptr) {
    return;
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  // A dead connection or a closed pool means destroy, not recycle: the next
  // acquirer will lazily create a fresh one. Either way the slot budget is
  // preserved (recycle) or released (destroy).
  if (!state_->closed && conn_->alive()) {
    state_->idle.push_back(std::move(conn_));
  } else {
    conn_.reset();
    --state_->total;
  }
  lock.unlock();
  state_->available.notify_one();
}

PgConnection& ConnectionPool::Guard::connection() { return *conn_; }

ConnectionPool::ConnectionPool(std::string conninfo, std::size_t max_size)
    : state_(std::make_shared<PoolState>()),
      conninfo_(std::move(conninfo)),
      max_size_(max_size < 1 ? 1 : max_size) {}

ConnectionPool::~ConnectionPool() {
  close();
  // Idle connections die with the vector. Any Guard still outstanding holds
  // its own shared_ptr<PoolState>, so its destructor still runs safely and
  // simply destroys its connection (the pool is closed by now).
}

ConnectionPool::Guard ConnectionPool::acquire() {
  for (;;) {
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (state_->closed) {
      throw PoolError("connection pool is closed");
    }
    if (!state_->idle.empty()) {
      std::unique_ptr<PgConnection> conn = std::move(state_->idle.back());
      state_->idle.pop_back();
      return Guard(state_, std::move(conn));
    }
    if (state_->total >= max_size_) {
      // At budget: wait for a Guard to come back. close() notifies, so this
      // cannot sleep through shutdown.
      state_->available.wait(lock, [this] {
        return state_->closed || !state_->idle.empty() || state_->total < max_size_;
      });
      continue;
    }
    // Under budget: reserve a slot, then connect WITHOUT holding the mutex
    // (PQconnectdb blocks on the network; holding the lock would serialize
    // all connection attempts and risk deadlock with close()).
    ++state_->total;
    lock.unlock();
    try {
      auto conn = std::make_unique<PgConnection>(conninfo_);
      return Guard(state_, std::move(conn));
    } catch (...) {
      lock.lock();
      --state_->total;
      lock.unlock();
      state_->available.notify_one();
      throw;
    }
  }
}

void ConnectionPool::close() {
  std::unique_lock<std::mutex> lock(state_->mutex);
  if (state_->closed) {
    return;
  }
  state_->closed = true;
  state_->idle.clear();  // Closes idle connections now.
  lock.unlock();
  state_->available.notify_all();
}

}  // namespace apex::persistence
