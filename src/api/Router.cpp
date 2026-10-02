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

}  // namespace

Router::Router(std::string version, std::string phase)
    : version_(std::move(version)), phase_(std::move(phase)) {}

RouteResult Router::route(http::verb method, std::string_view target) const {
  const std::string_view path = path_only(target);

  if (path == "/health") {
    if (method == http::verb::get) {
      const nlohmann::json body = {
          {"status", "ok"}, {"service", "apex"}, {"version", version_}, {"phase", phase_}};
      return {200, body.dump(), ""};
    }
    return {405, R"({"error":"method_not_allowed"})", "GET"};
  }

  // NOTE: POST /v1/operations is intentionally absent here. It needs the
  // durable service (I/O + state), so Session dispatches it before the pure
  // Router ever sees it — the same split as GET /ready.
  const nlohmann::json body = {{"error", "not_found"}, {"path", std::string(path)}};
  return {404, body.dump(), ""};
}

}  // namespace apex::api
