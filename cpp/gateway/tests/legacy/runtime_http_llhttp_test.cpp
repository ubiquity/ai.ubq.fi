// Links: src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
//        src/runtime/http_upgrade.cpp src/runtime/websocket.cpp
#include "src/runtime/http.cpp"
#include "uos/rt/test.hpp"

#include <tuple>

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

TEST("llhttp rejects malformed request lines and header values") {
  for (const std::string_view wire : {
         "GET  / HTTP/1.1\r\nHost: local\r\n\r\n",
         "GET /  HTTP/1.1\r\nHost: local\r\n\r\n",
         "GET /caf\xc3\xa9 HTTP/1.1\r\nHost: local\r\n\r\n",
         "GET / HTTP/1.1\r\nHost: local\r\nX-Value: a\x7f" "b\r\n\r\n",
       }) {
    Fixture fixture;
    fixture.append(wire);
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

TEST("llhttp bounds the complete trailer block across fields") {
  Fixture fixture;
  auto& connection = *fixture.connection;
  fixture.append("POST / HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n");
  ASSERT(connection.parseHead());
  rt::ByteChunk chunk;
  const auto finalChunk = connection.decodeChunked(chunk);
  ASSERT(finalChunk && *finalChunk);

  fixture.append("X-First: " + std::string(8300, 'x') + "\r\n");
  const auto partial = connection.decodeChunked(chunk);
  EXPECT(partial && !*partial);
  fixture.append("X-Second: " + std::string(8300, 'x') + "\r\n\r\n");
  EXPECT(!connection.decodeChunked(chunk));
  EXPECT(!connection.bodyComplete);
}

TEST("client header bytes and field counts admit the exact ceiling") {
  using namespace uos::runtime::http::node_http;
  for (const std::size_t extra : {0u, 1u}) {
    std::string head = "GET / HTTP/1.1\r\nHost: local\r\nX-Fill: ";
    head.resize(MAX_HEAD_BYTES - 4 + extra, 'a');
    head += "\r\n\r\n";
    RequestParser parser;
    std::size_t consumed = 0;
    EXPECT(parser.parseHead(head, consumed) == (extra == 0 ? RequestParser::Head::complete : RequestParser::Head::overflow));

    head = "GET / HTTP/1.1\r\nHost: local\r\n";
    for (std::size_t i = 1; i < MAX_HEADER_FIELDS + extra; ++i) head += "X: a\r\n";
    head += "\r\n";
    RequestParser fields;
    EXPECT(fields.parseHead(head, consumed) == (extra == 0 ? RequestParser::Head::complete : RequestParser::Head::overflow));

    Fixture trail;
    trail.append("POST / HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n");
    ASSERT(trail.connection->parseHead());
    rt::ByteChunk payload;
    ASSERT(trail.connection->decodeChunked(payload));
    for (std::size_t i = 0; i < MAX_HEADER_FIELDS + extra; ++i) trail.append("X: a\r\n");
    trail.append("\r\n");
    EXPECT_EQ(trail.connection->decodeChunked(payload).has_value(), extra == 0);
  }
}

TEST("fixed and chunked request bodies keep the HTTP and image route ceilings") {
  using namespace std::string_literals;
  constexpr auto ordinary = rt::limits::kHttpBodyBytes;
  constexpr auto imageJson = rt::limits::kImageEditJsonBytes;
  constexpr auto imageMultipart = rt::limits::kImageEditMultipartBytes;
  for (const std::size_t extra : {0u, 1u}) {
    for (const auto& [target, type, limit] : std::array{
           std::tuple{"/v1/responses", "application/json", ordinary},
           std::tuple{"/v1/images/edits/?q=1", "application/json", imageJson},
           std::tuple{"/v1/images/edits", "Multipart/Form-Data; boundary=demo", imageMultipart}}) {
      Fixture fixture;
      fixture.append("POST "s + target + " HTTP/1.1\r\nHost: local\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(limit + extra) + "\r\n\r\n");
      EXPECT_EQ(fixture.connection->parseHead(), extra == 0);
    }

    Fixture chunked;
    chunked.append("POST /v1/responses HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n");
    ASSERT(chunked.connection->parseHead());
    // One 16 MiB chunk spends the complete budget as soon as its size is known.
    chunked.append(extra == 0 ? "1000000\r\n" : "1000001\r\n");
    rt::ByteChunk chunk;
    EXPECT_EQ(chunked.connection->decodeChunked(chunk).has_value(), extra == 0);
    EXPECT(chunk.empty());
    EXPECT_EQ(chunked.connection->parser.bodyTooLarge, extra != 0);
  }
}

TEST("client query parameter limits reject at the HTTP boundary") {
  std::string target = "/?q";
  for (std::size_t i = 1; i < rt::limits::kUrlQueryPairs; ++i) target += "&q";
  for (const std::size_t extra : {0u, 1u}) {
    Fixture fixture;
    if (extra) target += "&q";
    fixture.append("GET " + target + " HTTP/1.1\r\nHost: local\r\n\r\n");
    EXPECT_EQ(fixture.connection->parseHead(), extra == 0);
  }
}
