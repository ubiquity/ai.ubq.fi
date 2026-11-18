// Links: src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
//        src/runtime/http_upgrade.cpp src/runtime/websocket.cpp
#include "src/runtime/http.cpp"
#include "uos/rt/test.hpp"

namespace {
  namespace rt = uos::rt;
  using uos::runtime::http::node_http::ConnectionState;
  using Socket = boost::asio::generic::stream_protocol::socket;

  struct Fixture {
    rt::Loop loop;
    rt::LoopScope scope{loop};
    std::shared_ptr<ConnectionState> connection = std::make_shared<ConnectionState>(Socket(loop.context()));

    ~Fixture() {
      connection->destroy(nullptr);
      loop.run();
    }

    void append(std::string_view bytes) { connection->input.insert(connection->input.end(), bytes.begin(), bytes.end()); }
  };
}

TEST("llhttp retains ordered Latin-1 headers and unread bytes across a split head") {
  Fixture fixture;
  auto& connection = *fixture.connection;
  const std::string_view head =
    "POST /caf%C3%A9 HTTP/1.1\r\nHost: local\r\nX-Mixed: \tcaf\xe9 \t\r\nx-mixed: second\r\nEmpty:\r\nContent-Length: 3\r\n\r";
  for (const char byte : head) {
    fixture.append(std::string_view(&byte, 1));
    EXPECT(!connection.parseHead());
  }
  fixture.append("\nabc");
  ASSERT(connection.parseHead());
  EXPECT_EQ(connection.request->method, "POST");
  EXPECT_EQ(connection.request->url, "/caf%C3%A9");
  const std::vector<std::string> headers{"Host", "local", "X-Mixed", "caf\xc3\xa9", "x-mixed", "second", "Empty", "", "Content-Length", "3"};
  EXPECT_EQ(connection.request->rawHeaders, headers);
  EXPECT_EQ(connection.remaining, 3u);
  EXPECT_EQ(std::string(connection.input.begin(), connection.input.end()), "abc");
}

TEST("llhttp decodes split chunk metadata and stops before the next head") {
  Fixture fixture;
  auto& connection = *fixture.connection;
  fixture.append("POST / HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n");
  ASSERT(connection.parseHead());
  EXPECT(connection.framing == ConnectionState::Framing::chunked);

  std::string body;
  const auto drain = [&] {
    while (!connection.input.empty() && !connection.bodyComplete) {
      rt::ByteChunk chunk;
      const auto progress = connection.decodeChunked(chunk);
      ASSERT(progress.has_value());
      body.append(chunk.begin(), chunk.end());
      if (!*progress) break;
    }
  };
  const std::string_view chunks = "3;name=value\r\nabc\r\n2\r\nde\r\n0\r\nX-Trailer: done\r\n\r";
  for (const char byte : chunks) {
    fixture.append(std::string_view(&byte, 1));
    drain();
    EXPECT(!connection.bodyComplete);
  }
  fixture.append("\nGET /next HTTP/1.1\r\nHost: local\r\n\r\n");
  drain();
  EXPECT(connection.bodyComplete);
  EXPECT_EQ(body, "abcde");
  EXPECT_EQ(std::string(connection.input.begin(), connection.input.end()), "GET /next HTTP/1.1\r\nHost: local\r\n\r\n");
  ASSERT(connection.parseHead());
  EXPECT_EQ(connection.request->url, "/next");
}

TEST("llhttp refuses ambiguous request framing before dispatch") {
  for (const std::string_view fields : {
         "Content-Length: 3\r\nContent-Length: 3\r\n",
         "Content-Length: 3\r\nTransfer-Encoding: chunked\r\n",
         "Transfer-Encoding: chunked\r\nContent-Length: 3\r\n",
         "Transfer-Encoding: identity\r\n",
         "Transfer-Encoding: chunked\r\nTransfer-Encoding: gzip\r\n",
       }) {
    Fixture fixture;
    fixture.append("POST / HTTP/1.1\r\nHost: local\r\n");
    fixture.append(fields);
    fixture.append("\r\nabc");
    EXPECT(!fixture.connection->parseHead());
    EXPECT(!fixture.connection->request);
    EXPECT(fixture.connection->phase == ConnectionState::Phase::lingering);
  }
}

TEST("llhttp delivers payload before reporting a malformed chunk delimiter") {
  Fixture fixture;
  auto& connection = *fixture.connection;
  fixture.append("POST / HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n");
  ASSERT(connection.parseHead());
  fixture.append("3\r\nabcX\n");

  rt::ByteChunk chunk;
  auto progress = connection.decodeChunked(chunk);
  ASSERT(progress && *progress);
  EXPECT(chunk.empty());
  progress = connection.decodeChunked(chunk);
  ASSERT(progress && *progress);
  EXPECT_EQ(std::string(chunk.begin(), chunk.end()), "abc");
  EXPECT(!connection.bodyComplete);
  chunk.clear();
  EXPECT(!connection.decodeChunked(chunk));
  EXPECT(chunk.empty());
}

TEST("llhttp bounds a fragmented header and chunk extension") {
  Fixture head;
  head.append("GET / HTTP/1.1\r\nHost: local\r\nX-Fill: ");
  head.append(std::string(8000, 'x'));
  EXPECT(!head.connection->parseHead());
  EXPECT(head.connection->phase == ConnectionState::Phase::head);
  head.append(std::string(9000, 'x'));
  EXPECT(!head.connection->parseHead());
  EXPECT(head.connection->phase == ConnectionState::Phase::lingering);

  Fixture chunked;
  chunked.append("POST / HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n");
  ASSERT(chunked.connection->parseHead());
  chunked.append("1;name=");
  chunked.append(std::string(2000, 'x'));
  rt::ByteChunk chunk;
  auto progress = chunked.connection->decodeChunked(chunk);
  EXPECT(progress && !*progress);
  chunked.append(std::string(2100, 'x'));
  progress = chunked.connection->decodeChunked(chunk);
  EXPECT(!progress);
}
