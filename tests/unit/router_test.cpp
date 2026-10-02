// Router tests: pure function, no I/O. Every documented route and every
// documented error code gets a case.

#include <gtest/gtest.h>

#include "api/Router.hpp"

namespace apex::api {
namespace {

namespace http = boost::beast::http;

TEST(RouterTest, HealthReturnsOkWithServiceMetadata) {
  const Router router("test-version");
  const RouteResult result = router.route(http::verb::get, "/health");
  EXPECT_EQ(result.status, 200);
  EXPECT_NE(result.body.find("\"status\":\"ok\""), std::string::npos);
  EXPECT_NE(result.body.find("test-version"), std::string::npos);
}

TEST(RouterTest, HealthIgnoresQueryString) {
  const Router router("test-version");
  const RouteResult result = router.route(http::verb::get, "/health?verbose=true");
  EXPECT_EQ(result.status, 200);
}

TEST(RouterTest, HealthIsStrictAboutTrailingSlash) {
  const Router router("test-version");
  EXPECT_EQ(router.route(http::verb::get, "/health/").status, 404);
}

TEST(RouterTest, WrongMethodOnHealthIs405WithAllowHeader) {
  const Router router("test-version");
  const RouteResult result = router.route(http::verb::post, "/health");
  EXPECT_EQ(result.status, 405);
  EXPECT_EQ(result.allow, "GET");
}

TEST(RouterTest, OperationsStubIsExplicitlyNotImplemented) {
  const Router router("test-version");
  const RouteResult result = router.route(http::verb::post, "/v1/operations");
  EXPECT_EQ(result.status, 501);
  EXPECT_NE(result.body.find("not_implemented"), std::string::npos);
}

TEST(RouterTest, WrongMethodOnOperationsIs405) {
  const Router router("test-version");
  const RouteResult result = router.route(http::verb::get, "/v1/operations");
  EXPECT_EQ(result.status, 405);
  EXPECT_EQ(result.allow, "POST");
}

TEST(RouterTest, UnknownPathsAre404) {
  const Router router("test-version");
  EXPECT_EQ(router.route(http::verb::get, "/nope").status, 404);
  EXPECT_EQ(router.route(http::verb::get, "/").status, 404);
  EXPECT_EQ(router.route(http::verb::delete_, "/v1/operations/123").status, 404);
}

}  // namespace
}  // namespace apex::api
