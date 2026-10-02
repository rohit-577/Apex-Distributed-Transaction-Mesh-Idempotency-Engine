#pragma once

// Synchronous request router. Pure function of (method, target) -> result,
// which is what makes it unit-testable without sockets.
//
// Phase 0 contract:
//   GET  /health           -> 200 {"status":"ok", ...}
//   POST /v1/operations    -> 501 (idempotency engine is Phase 1 work; the
//                              stub says so explicitly in the response body)
//   GET  /ready            -> handled asynchronously by Session because it
//                              performs non-blocking dependency checks. The
//                              router never sees it.
//   anything else          -> 404, wrong method on a known path -> 405.
//
// Routing is strict: trailing slashes do not match. Query strings are
// ignored for matching but do not cause errors.

#include <string>
#include <string_view>

#include <boost/beast/http/verb.hpp>

namespace apex::api {

struct RouteResult {
  int status{404};
  std::string body;
  std::string allow;  // Set only for 405 responses (value of the Allow header).
};

class Router {
 public:
  explicit Router(std::string version);

  [[nodiscard]] RouteResult route(boost::beast::http::verb method,
                                  std::string_view target) const;

 private:
  std::string version_;
};

}  // namespace apex::api
