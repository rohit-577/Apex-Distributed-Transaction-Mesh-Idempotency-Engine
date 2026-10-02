#include "common/test_helpers.hpp"

#include <cstdlib>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace apex::test {

EnvGuard::EnvGuard(const char* name, const char* value) : name_(name) {
#if defined(_WIN32)
  char* buffer = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&buffer, &length, name) == 0 && buffer != nullptr) {
    old_value_ = buffer;
    had_old_ = true;
    std::free(buffer);
  }
  _putenv_s(name, value);
#else
  if (const char* old = std::getenv(name); old != nullptr) {
    old_value_ = old;
    had_old_ = true;
  }
  ::setenv(name, value, /*overwrite=*/1);
#endif
}

EnvGuard::~EnvGuard() {
#if defined(_WIN32)
  if (had_old_) {
    _putenv_s(name_.c_str(), old_value_.c_str());
  } else {
    _putenv_s(name_.c_str(), "");
  }
#else
  if (had_old_) {
    ::setenv(name_.c_str(), old_value_.c_str(), /*overwrite=*/1);
  } else {
    ::unsetenv(name_.c_str());
  }
#endif
}

std::uint16_t acquire_closed_port() {
  namespace asio = boost::asio;
  asio::io_context ioc;
  asio::ip::tcp::acceptor acceptor(ioc);
  acceptor.open(asio::ip::tcp::v4());
  acceptor.bind({asio::ip::tcp::v4(), 0});
  const auto port = acceptor.local_endpoint().port();
  boost::system::error_code ec;
  acceptor.close(ec);
  return port;
}

TestServer::TestServer(config::Config config) : server_(ioc_, config, "test-version") {
  server_.start();
  thread_ = std::thread([this] { ioc_.run(); });
}

TestServer::~TestServer() {
  server_.stop();
  ioc_.stop();
  if (thread_.joinable()) {
    thread_.join();
  }
}

HttpResult http_send(const std::string& host, std::uint16_t port,
                     boost::beast::http::verb method, const std::string& target,
                     const std::string& body) {
  namespace asio = boost::asio;
  namespace beast = boost::beast;
  namespace http = beast::http;

  asio::io_context ioc;
  asio::ip::tcp::resolver resolver(ioc);
  beast::tcp_stream stream(ioc);
  const auto endpoints = resolver.resolve(host, std::to_string(port));
  stream.connect(endpoints);

  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, host);
  req.set(http::field::user_agent, "apex-tests/phase0");
  req.body() = body;
  req.prepare_payload();
  http::write(stream, req);

  beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(stream, buffer, res);

  beast::error_code ec;
  stream.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);

  return {static_cast<int>(res.result_int()), res.body()};
}

}  // namespace apex::test
