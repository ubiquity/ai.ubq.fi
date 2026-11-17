// Links: tests/rt/fetch_connection_test.cpp (every src/rt/*.cpp links automatically).
// C++ ownership invariants behind the Bun-recorded response-abort integration cases.
#include "src/rt/fetch_transport_internal.hpp"
#include "uos/rt/test_async.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <memory>
#include <utility>

namespace asio = boost::asio;
namespace rt = uos::rt;
namespace fetch = uos::rt::detail::fetch;

ASYNC_TEST("a pending fetch read retains its connection until cancellation completes") {
  asio::ip::tcp::acceptor acceptor(rt::executor(), asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  asio::ip::tcp::socket client(rt::executor());
  client.connect(acceptor.local_endpoint());
  auto server = acceptor.accept();
  auto connection = std::make_shared<fetch::Connection>(std::move(client), "loopback", false);
  const std::weak_ptr<fetch::Connection> weak = connection;
  rt::Promise<fetch::ReadStatus> pending(connection->fill());
  connection->close();
  connection.reset();
  EXPECT(!weak.expired());
  const auto status = co_await pending.wait();
  EXPECT(status == fetch::ReadStatus::aborted);
  EXPECT(weak.expired());
}

ASYNC_TEST("a deferred fetch read owns its connection from the call until its task ends") {
  asio::ip::tcp::socket socket(rt::executor());
  auto connection = std::make_shared<fetch::Connection>(std::move(socket), "unopened", false);
  const std::weak_ptr<fetch::Connection> weak = connection;
  auto pending = connection->fill();
  connection->close();
  connection.reset();
  EXPECT(!weak.expired());
  const auto status = co_await std::move(pending);
  EXPECT(status == fetch::ReadStatus::aborted);
  EXPECT(weak.expired());
}
