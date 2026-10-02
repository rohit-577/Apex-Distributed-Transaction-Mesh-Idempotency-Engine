#include "execution/DependencyChecker.hpp"

#include <string>
#include <utility>

#include <boost/asio.hpp>

namespace apex::execution {

void DependencyChecker::async_check(boost::asio::any_io_executor executor, Endpoint postgres,
                                    Endpoint redis, std::chrono::milliseconds timeout,
                                    Handler handler) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;

  // Shared probe state. All mutations happen on `strand_` (every socket,
  // timer, and resolver is constructed on it), so no mutex is needed. The
  // final handler is posted back to the caller's executor.
  struct Shared : public std::enable_shared_from_this<Shared> {
    asio::strand<asio::any_io_executor> strand_;
    asio::any_io_executor out_;
    tcp::resolver resolver_;
    tcp::socket socket_pg_;
    tcp::socket socket_redis_;
    asio::steady_timer timer_pg_;
    asio::steady_timer timer_redis_;
    Endpoint postgres_;
    Endpoint redis_;
    std::chrono::milliseconds timeout_;
    Handler handler_;
    DependencyStatus status_;
    bool done_pg_{false};
    bool done_redis_{false};

    Shared(asio::any_io_executor ex, Endpoint postgres, Endpoint redis,
           std::chrono::milliseconds timeout, Handler handler)
        : strand_(asio::make_strand(ex)),
          out_(ex),
          resolver_(strand_),
          socket_pg_(strand_),
          socket_redis_(strand_),
          timer_pg_(strand_),
          timer_redis_(strand_),
          postgres_(std::move(postgres)),
          redis_(std::move(redis)),
          timeout_(timeout),
          handler_(std::move(handler)) {}

    void start() {
      probe(/*is_postgres=*/true);
      probe(/*is_postgres=*/false);
    }

    void probe(bool is_postgres) {
      auto self = shared_from_this();
      asio::steady_timer& timer = is_postgres ? timer_pg_ : timer_redis_;
      timer.expires_after(timeout_);
      // Deadline: give up on a silent network. Cancelling the socket makes
      // the pending connect complete with operation_aborted, which finish()
      // records as unreachable.
      timer.async_wait([self, is_postgres](const boost::system::error_code& ec) {
        if (ec) {
          return;  // Cancelled because the probe already finished.
        }
        boost::system::error_code ignored;
        (is_postgres ? self->socket_pg_ : self->socket_redis_).cancel(ignored);
      });

      const Endpoint& endpoint = is_postgres ? postgres_ : redis_;
      resolver_.async_resolve(
          endpoint.host, std::to_string(endpoint.port),
          [self, is_postgres](const boost::system::error_code& ec,
                               tcp::resolver::results_type results) {
            if (ec) {
              self->finish(is_postgres, /*reachable=*/false);
              return;
            }
            tcp::socket& socket = is_postgres ? self->socket_pg_ : self->socket_redis_;
            asio::async_connect(
                socket, results,
                [self, is_postgres](const boost::system::error_code& connect_ec,
                                     const tcp::endpoint& /*endpoint*/) {
                  self->finish(is_postgres, /*reachable=*/!connect_ec);
                });
          });
    }

    void finish(bool is_postgres, bool reachable) {
      bool& done = is_postgres ? done_pg_ : done_redis_;
      if (done) {
        return;  // Late completion after the deadline already fired.
      }
      done = true;
      if (is_postgres) {
        status_.postgres_reachable = reachable;
      } else {
        status_.redis_reachable = reachable;
      }
      (is_postgres ? timer_pg_ : timer_redis_).cancel();
      if (done_pg_ && done_redis_) {
        asio::post(out_, [handler = handler_, status = status_]() mutable { handler(status); });
      }
    }
  };

  std::make_shared<Shared>(executor, std::move(postgres), std::move(redis), timeout,
                           std::move(handler))
      ->start();
}

}  // namespace apex::execution
