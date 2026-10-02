#include "api/Router.hpp"

#include <boost/beast/http/status.hpp>
#include <nlohmann/json.hpp>

namespace apex::api {
namespace {

namespace http = boost::beast::http;

std::string_view path_only(std::string_view target) {
  const std::size_t q = target.find('?');
  return q == std::string_view::npos ? target : target.substr(0, q);
}

std::string not_implemented_body() {
  const nlohmann::json body = {
      {"error", "not_implemented"},
      {"message",
       "The idempotency engine is Phase 1 work. This Phase 0 baseline only proves the HTTP "
       "gateway, configuration, and infrastructure wiring."},
      {"contract", "POST /v1/operations with Idempotency-Key header (see docs/architecture.md)"},
  };
  return body.dump();
}

}  // namespace

Router::Router(std::string version) : version_(std::move(version)) {}

RouteResult Router::route(http::verb method, std::string_view target) const {
  const std::string_view path = path_only(target);

  if (path == "/health") {
    if (method == http::verb::get) {
      const nlohmann::json body = {
          {"status", "ok"}, {"service", "apex"}, {"version", version_}, {"phase", "phase-0"}};
      return {200, body.dump(), ""};
    }
    return {405, R"({"error":"method_not_allowed"})", "GET"};
  }

  if (path == "/v1/operations") {
    if (method == http::verb::post) {
      return {501, not_implemented_body(), ""};
    }
    return {405, R"({"error":"method_not_allowed"})", "POST"};
  }

  const nlohmann::json body = {{"error", "not_found"}, {"path", std::string(path)}};
  return {404, body.dump(), ""};
}

}  // namespace apex::api
