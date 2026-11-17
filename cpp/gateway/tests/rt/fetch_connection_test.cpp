// Links: tests/rt/fetch_connection_test.cpp (every src/rt/*.cpp links automatically).
// C++ ownership invariants behind the Bun-recorded response-abort integration cases.
#include "src/rt/fetch_transport_internal.hpp"
#include "uos/rt/test_async.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <memory>
#include <utility>

#if defined(__APPLE__)
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include <boost/asio/local/stream_protocol.hpp>
#include <sys/socket.h>
#endif

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

#if defined(__APPLE__)
TEST("Darwin Asio TCP and Unix sockets suppress SIGPIPE and preserve half-close") {
  rt::Loop loop;
  rt::LoopScope scope(loop);
  const auto check = [](auto& client, auto& server) {
    for (const int fd : {client.native_handle(), server.native_handle()}) {
      int enabled = 0;
      socklen_t size = sizeof enabled;
      ASSERT_EQ(::getsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, &size), 0);
      EXPECT_EQ(enabled, 1);
    }
    server.shutdown(asio::socket_base::shutdown_send);
    char byte = 0;
    boost::system::error_code error;
    EXPECT_EQ(client.read_some(asio::buffer(&byte, 1), error), std::size_t(0));
    EXPECT_EQ(error, asio::error::eof);
    EXPECT_EQ(client.write_some(asio::buffer("x", 1)), std::size_t(1));
    EXPECT_EQ(server.read_some(asio::buffer(&byte, 1)), std::size_t(1));
    EXPECT_EQ(byte, 'x');
    client.shutdown(asio::socket_base::shutdown_send);
    EXPECT_EQ(client.write_some(asio::buffer("x", 1), error), std::size_t(0));
    EXPECT_EQ(error, asio::error::broken_pipe);
  };
  asio::ip::tcp::acceptor tcp(rt::executor(), asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  asio::ip::tcp::socket client(rt::executor());
  client.connect(tcp.local_endpoint());
  auto server = tcp.accept();
  check(client, server);
  const std::string path = rt::os::tmpdir() + "/half-close.sock";
  asio::local::stream_protocol::acceptor local(rt::executor(), asio::local::stream_protocol::endpoint(path));
  asio::local::stream_protocol::socket unixClient(rt::executor());
  unixClient.connect(local.local_endpoint());
  auto unixServer = local.accept();
  check(unixClient, unixServer);
  local.close();
  rt::fs::rmSync(path);
}
#endif
