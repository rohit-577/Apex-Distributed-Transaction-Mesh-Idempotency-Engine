// DependencyChecker tests: the async TCP probes used by GET /ready.
// Deterministic by construction — the "dependencies" are real local sockets
// owned by the test, a guaranteed-closed port, and a TEST-NET-1 address that
// is unroutable by definition (RFC 5737).

#include <chrono>
#include <future>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <gtest/gtest.h>

#include "common/test_helpers.hpp"
#include "execution/DependencyChecker.hpp"

namespace apex::execution {
namespace {

using namespace std::chrono_literals;

DependencyStatus run_check(Endpoint postgres, Endpoint redis, std::chrono::milliseconds timeout) {
  namespace asio = boost::asio;
  asio::io_context ioc;
  std::promise<DependencyStatus> done;
  auto result = done.get_future();
  DependencyChecker::async_check(ioc.get_executor(), std::move(postgres), std::move(redis),
                                 timeout,
                                 [&done](DependencyStatus status) { done.set_value(status); });
  ioc.run();  // Returns when both probes finish; the checker always finishes.
  return result.get();
}

TEST(DependencyCheckTest, BothReachableWhenSomethingListens) {
  namespace asio = boost::asio;
  asio::io_context ioc;
  asio::ip::tcp::acceptor first(ioc);
  first.open(asio::ip::tcp::v4());
  first.bind({asio::ip::tcp::v4(), 0});
  first.listen();
  asio::ip::tcp::acceptor second(ioc);
  second.open(asio::ip::tcp::v4());
  second.bind({asio::ip::tcp::v4(), 0});
  second.listen();

  const DependencyStatus status =
      run_check({"127.0.0.1", first.local_endpoint().port()},
                {"127.0.0.1", second.local_endpoint().port()}, 5000ms);
  EXPECT_TRUE(status.postgres_reachable);
  EXPECT_TRUE(status.redis_reachable);
  EXPECT_TRUE(status.ready());
}

TEST(DependencyCheckTest, BothUnreachableWhenNothingListens) {
  const DependencyStatus status =
      run_check({"127.0.0.1", test::acquire_closed_port()},
                {"127.0.0.1", test::acquire_closed_port()}, 5000ms);
  EXPECT_FALSE(status.postgres_reachable);
  EXPECT_FALSE(status.redis_reachable);
  EXPECT_FALSE(status.ready());
}

TEST(DependencyCheckTest, MixedReachabilityIsReportedPerDependency) {
  namespace asio = boost::asio;
  asio::io_context ioc;
  asio::ip::tcp::acceptor only_pg(ioc);
  only_pg.open(asio::ip::tcp::v4());
  only_pg.bind({asio::ip::tcp::v4(), 0});
  only_pg.listen();

  const DependencyStatus status = run_check({"127.0.0.1", only_pg.local_endpoint().port()},
                                            {"127.0.0.1", test::acquire_closed_port()}, 5000ms);
  EXPECT_TRUE(status.postgres_reachable);
  EXPECT_FALSE(status.redis_reachable);
  EXPECT_FALSE(status.ready());
}

TEST(DependencyCheckTest, SilentNetworkHitsDeadlineInsteadOfHanging) {
  // 192.0.2.1 is TEST-NET-1: guaranteed unroutable, so no host can answer.
  // The probe must return at the deadline, not hang.
  const auto start = std::chrono::steady_clock::now();
  const DependencyStatus status =
      run_check({"192.0.2.1", 5432}, {"192.0.2.1", 6379}, 500ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_FALSE(status.ready());
  EXPECT_LT(elapsed, 10s) << "probe ignored its deadline";
}

}  // namespace
}  // namespace apex::execution
