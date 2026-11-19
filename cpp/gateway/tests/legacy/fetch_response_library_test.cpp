// Links: tests/legacy/fetch_response_library_test.cpp
#include "src/rt/fetch_transport_internal.hpp"
#include "src/rt/websocket_internal.hpp"
#include "uos/rt/test.hpp"

namespace {
  namespace fetch = uos::rt::detail::fetch;
  namespace ws = uos::rt::ws;
  using namespace std::string_view_literals;

  ws::detail::BytesView wireBytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
  }
}

TEST("llhttp collects split status and field spans in wire order") {
  constexpr auto wire = "HTTP/1.1 200 Caf\xe9\r\nX-Name: first  \t\r\nx-name: second\r\nX-Empty:\r\nContent-Length: 3\r\n\r\nabc"sv;
  fetch::ResponseParser parser;
  std::size_t offset = 0;
  auto status = HPE_OK;
  while (offset < wire.size() && status == HPE_OK) {
    std::size_t consumed = 0;
    status = parser.readHead(wire.substr(offset, 1), consumed);
    offset += consumed;
  }
  ASSERT_EQ(status, HPE_PAUSED);
  EXPECT_EQ(wire.substr(offset), "abc"sv);
  const auto head = parser.takeHead();
  EXPECT_EQ(head.status, 200);
  EXPECT_EQ(head.reason, "Caf\xe9");
  ASSERT_EQ(head.fields.size(), 4U);
  EXPECT_EQ(head.fields[0].first, "X-Name");
  EXPECT_EQ(head.fields[1].first, "x-name");
  EXPECT_EQ(head.fields[2].second, "");
  EXPECT_EQ(head.field("x-name"), std::optional<std::string>("first, second"));
}

TEST("llhttp reports a fixed-length boundary without consuming the next bytes") {
  fetch::ResponseParser parser;
  std::size_t consumed = 0;
  ASSERT_EQ(parser.readHead("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", consumed), HPE_PAUSED);
  const auto head = parser.takeHead();
  EXPECT_EQ(fetch::responseFraming("GET", head).length, 5U);
  EXPECT_EQ(fetch::responseFraming("HEAD", head).framing, fetch::Framing::empty);
  parser.beginBody();
  std::string body;
  EXPECT_EQ(parser.readBody("ab", consumed, body), HPE_OK);
  EXPECT_EQ(consumed, 2U);
  EXPECT_EQ(parser.readBody("cdeextra", consumed, body), HPE_PAUSED);
  EXPECT_EQ(consumed, 3U);
  EXPECT_EQ(body, "abcde");
}

TEST("llhttp distinguishes EOF-delimited success from a truncated length") {
  fetch::ResponseParser close;
  std::size_t consumed = 0;
  ASSERT_EQ(close.readHead("HTTP/1.0 200 OK\r\n\r\n", consumed), HPE_PAUSED);
  EXPECT_EQ(fetch::responseFraming("GET", close.takeHead()).framing, fetch::Framing::close);
  close.beginBody();
  std::string body;
  EXPECT_EQ(close.readBody("abc", consumed, body), HPE_OK);
  EXPECT_EQ(close.finish(), HPE_PAUSED);
  EXPECT_EQ(body, "abc");

  fetch::ResponseParser length;
  ASSERT_EQ(length.readHead("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n", consumed), HPE_PAUSED);
  length.beginBody();
  body.clear();
  EXPECT_EQ(length.readBody("abc", consumed, body), HPE_OK);
  EXPECT_EQ(length.finish(), HPE_INVALID_EOF_STATE);
}

TEST("llhttp consumes chunk extensions and trailers through arbitrary splits") {
  constexpr auto chunks = "3;name=\"v;x\"\r\nabc\r\n2\r\nde\r\n0\r\nX-Trailer: discard\r\n\r\nextra"sv;
  fetch::ChunkedDecoder decoder;
  std::string body;
  std::size_t offset = 0;
  auto status = fetch::ChunkedDecoder::Result::more;
  for (std::size_t end = 1; end <= chunks.size() && status == fetch::ChunkedDecoder::Result::more; ++end) {
    status = decoder.decode(chunks.substr(0, end), offset, body);
  }
  EXPECT_EQ(status, fetch::ChunkedDecoder::Result::done);
  EXPECT_EQ(chunks.substr(offset), "extra"sv);
  EXPECT_EQ(body, "abcde");
}

TEST("strict HTTP client rejects ambiguous framing and obsolete head grammar") {
  constexpr std::string_view malformed[] = {
    "HTTP/1.1  200 OK\r\n\r\n",
    "HTTP/1.2 200 OK\r\n\r\n",
    "HTTP/1.1 200 OK\nX: y\n\n",
    "HTTP/1.1 200 OK\r\nX: y\r\n folded\r\n\r\n",
    "HTTP/1.1 200 A\x7f" "B\r\n\r\n",
    "\r\nHTTP/1.1 200 OK\r\n\r\n",
    "RTSP/1.0 200 OK\r\n\r\n",
    "ICE/1.0 200 OK\r\n\r\n",
    "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
    "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\nContent-Length: 0\r\n\r\n",
  };
  for (const auto input : malformed) {
    EXPECT_EQ(fetch::parseResponseHead(input).status, fetch::HeadStatus::malformed);
  }
  constexpr std::string_view ambiguous[] = {
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip\r\n\r\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, chunked\r\n\r\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: gzip\r\n\r\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked,\r\n\r\n",
  };
  for (const auto input : ambiguous) {
    const auto parsed = fetch::parseResponseHead(input);
    ASSERT_EQ(parsed.status, fetch::HeadStatus::complete);
    EXPECT(fetch::responseFraming("GET", parsed.head).error);
  }
}

TEST("llhttp rejects invalid chunk size, terminators and trailer fields") {
  for (const auto chunk : {"2\nab\n0\n\n"sv, "2 \r\nab\r\n0\r\n\r\n"sv, "2\r\nab!\r\n0\r\n\r\n"sv,
                           "2\r\nab\r\n0\r\ninvalid-trailer\r\n\r\n"sv, "10000000000000000\r\n"sv}) {
    fetch::ChunkedDecoder decoder;
    std::size_t consumed = 0;
    std::string body;
    EXPECT_EQ(decoder.decode(chunk, consumed, body), fetch::ChunkedDecoder::Result::invalid);
  }
}

TEST("WebSocket head parsing preserves Latin-1 fields and coalesced frame bytes") {
  constexpr auto head = "HTTP/1.1 101 Switching\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nX-V: caf\xe9\r\n\r\n"sv;
  const std::string frame("\x81\x01\x00", 3);
  const std::string wire = std::string(head) + frame;
  EXPECT(!ws::detail::parseResponseHead(wireBytes(head.substr(0, head.size() - 1))));
  const auto parsed = ws::detail::parseResponseHead(wireBytes(wire));
  ASSERT(parsed);
  EXPECT_EQ(parsed->length, head.size());
  EXPECT_EQ(parsed->response.statusCode, 101);
  EXPECT_EQ(parsed->response.header("x-v"), std::optional<std::string>("caf\xc3\xa9"));
  EXPECT_EQ(wire.substr(parsed->length), frame);
}

TEST("response head limits stop at the header boundary") {
  constexpr auto head = "HTTP/1.1 200 OK\r\nX: a\r\nY: b\r\n\r\n"sv;
  EXPECT_EQ(fetch::parseResponseHead(head, head.size(), 2).status, fetch::HeadStatus::complete);
  EXPECT_EQ(fetch::parseResponseHead(head, head.size() - 1, 2).status, fetch::HeadStatus::incomplete);
  EXPECT_EQ(fetch::parseResponseHead(head, head.size(), 1).status, fetch::HeadStatus::malformed);

  const std::string tooLarge = "HTTP/1.1 101 Switching\r\nX-Large: " + std::string(16384, 'a') + "\r\n\r\n";
  EXPECT_THROWS(ws::detail::parseResponseHead(wireBytes(tooLarge)), uos::rt::JsError);
  const std::string largeFrame = std::string(head) + std::string(16384, 'a');
  const auto parsed = ws::detail::parseResponseHead(wireBytes(largeFrame));
  ASSERT(parsed);
  EXPECT_EQ(parsed->length, head.size());
}

TEST("Fetch budgets split response heads by bytes and fields") {
  namespace limits = uos::rt::limits;
  for (const std::size_t extra : {0u, 1u}) {
    std::string head = "HTTP/1.1 200 OK\r\nX-Fill: ";
    head.resize(limits::kHttpHeaderBlockBytes - 4 + extra, 'a');
    head += "\r\n\r\n";
    fetch::ResponseParser parser;
    std::size_t consumed = 0;
    ASSERT_EQ(parser.readHead(std::string_view(head).substr(0, 101), consumed), HPE_OK);
    EXPECT_EQ(parser.readHead(std::string_view(head).substr(consumed), consumed), extra == 0 ? HPE_PAUSED : HPE_USER);

    head = "HTTP/1.1 200 OK\r\n";
    for (std::size_t i = 0; i < limits::kHttpHeaderFields + extra; ++i) head += "X: a\r\n";
    head += "\r\n";
    EXPECT_EQ(fetch::parseResponseHead(head).status, extra == 0 ? fetch::HeadStatus::complete : fetch::HeadStatus::malformed);
  }
}

TEST("Fetch bounds declared, chunked and EOF-delimited response bodies") {
  namespace limits = uos::rt::limits;
  for (const std::size_t extra : {0u, 1u}) {
    fetch::ResponseParser fixed;
    std::size_t consumed = 0;
    const auto head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(limits::kHttpBodyBytes + extra) + "\r\n\r\n";
    ASSERT_EQ(fixed.readHead(head, consumed), HPE_PAUSED);
    EXPECT_EQ(fixed.beginBody(), extra == 0 ? HPE_OK : HPE_USER);

    fetch::ChunkedDecoder chunked;
    std::string body;
    consumed = 0;
    EXPECT_EQ(chunked.decode(extra == 0 ? "1000000\r\n" : "1000001\r\n", consumed, body),
              extra == 0 ? fetch::ChunkedDecoder::Result::more : fetch::ChunkedDecoder::Result::invalid);
    EXPECT(body.empty());

    fetch::ResponseParser close;
    ASSERT_EQ(close.readHead("HTTP/1.0 200 OK\r\n\r\n", consumed), HPE_PAUSED);
    ASSERT_EQ(close.beginBody(), HPE_OK);
    EXPECT_EQ(close.readBody(std::string(limits::kHttpBodyBytes + extra, 'a'), consumed), extra == 0 ? HPE_OK : HPE_USER);
    EXPECT_EQ(consumed, limits::kHttpBodyBytes);
  }
}

TEST("Fetch bounds each chunk line and the trailer block") {
  namespace limits = uos::rt::limits;
  for (const std::size_t extra : {0u, 1u}) {
    std::string line = "1;name=";
    line.resize(limits::kHttpChunkLineBytes - 2 + extra, 'a');
    line += "\r\na\r\n0\r\n\r\n";
    fetch::ChunkedDecoder chunked;
    std::size_t offset = 0;
    std::string body;
    EXPECT_EQ(chunked.decode(line, offset, body), extra == 0 ? fetch::ChunkedDecoder::Result::done : fetch::ChunkedDecoder::Result::invalid);

    std::string trailers = "X-Fill: ";
    trailers.resize(limits::kHttpHeaderBlockBytes - 4 + extra, 'a');
    trailers += "\r\n\r\n";
    fetch::ChunkedDecoder trailerParser;
    offset = 0;
    EXPECT_EQ(trailerParser.decode("0\r\n" + trailers, offset, body), extra == 0 ? fetch::ChunkedDecoder::Result::done : fetch::ChunkedDecoder::Result::invalid);
  }
}
