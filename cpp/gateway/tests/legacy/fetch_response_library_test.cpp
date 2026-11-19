// Links: tests/legacy/fetch_response_library_test.cpp
#include "src/rt/fetch_transport_internal.hpp"
#include "src/rt/decompression_budget.hpp"
#include "src/rt/tls_internal.hpp"
#include "src/rt/websocket_internal.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/test.hpp"

#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
#include <zstd.h>
#endif

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

    head = "HTTP/1.1 200 OK\r\nX-Fill: ";
    head.resize(limits::kFetchResponseHeaderBlockBytes - 4 + extra, 'a');
    head += "\r\n\r\n";
    fetch::ResponseParser compatibility(limits::kHttpHeaderFields, limits::kFetchResponseHeaderBlockBytes);
    EXPECT_EQ(compatibility.readHead(head, consumed), extra == 0 ? HPE_PAUSED : HPE_USER);

    head = "HTTP/1.1 200 OK\r\n";
    for (std::size_t i = 0; i < limits::kHttpHeaderFields + extra; ++i) head += "X: a\r\n";
    head += "\r\n";
    EXPECT_EQ(fetch::parseResponseHead(head).status, extra == 0 ? fetch::HeadStatus::complete : fetch::HeadStatus::malformed);
  }
}

TEST("Fetch request heads enforce the same aggregate wire limits") {
  namespace rt = uos::rt;
  const rt::URL url("http://localhost/");
  const auto serialize = [&](const rt::Headers& headers) {
    return fetch::requestHead("GET", url, headers, fetch::RequestFraming::none, 0, "fixture", "identity");
  };
  rt::Headers headers;
  headers.set("X-Fill", "a");
  const auto overhead = serialize(headers).size() - 1;
  headers.set("X-Fill", std::string(rt::limits::kHttpHeaderBlockBytes - overhead, 'a'));
  EXPECT_EQ(serialize(headers).size(), rt::limits::kHttpHeaderBlockBytes);
  headers.set("X-Fill", std::string(rt::limits::kHttpHeaderBlockBytes - overhead + 1, 'a'));
  EXPECT_THROWS_MATCHING(serialize(headers), "RequestHeadersTooLarge");

  rt::Headers fields;
  for (std::size_t i = 0; i < rt::limits::kHttpHeaderFields - 5; ++i) fields.set("x-" + std::to_string(i), "a");
  EXPECT(!serialize(fields).empty());
  fields.set("x-overflow", "a");
  EXPECT_THROWS_MATCHING(serialize(fields), "RequestHeadersTooLarge");
}

TEST("shared TLS context bounds peer certificate messages before ASN.1 decoding") {
  const std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(uos::rt::detail::tls::newContext(), SSL_CTX_free);
  EXPECT_EQ(SSL_CTX_get_max_cert_list(context.get()), static_cast<long>(uos::rt::limits::kCertificateBytes));
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

TEST("Fetch bounds chunk metadata cumulatively") {
  namespace limits = uos::rt::limits;
  constexpr auto fullLines = (limits::kHttpChunkMetadataBytes / limits::kHttpChunkLineBytes) - 1;
  const std::string line = "1;x=" + std::string(limits::kHttpChunkLineBytes - 6, 'a') + "\r\na\r\n";
  for (const std::size_t extra : {0u, 1u}) {
    fetch::ChunkedDecoder decoder;
    std::string data;
    for (std::size_t i = 0; i < fullLines; ++i) {
      std::size_t offset = 0;
      ASSERT_EQ(decoder.decode(line, offset, data), fetch::ChunkedDecoder::Result::more);
    }
    std::string last = "1;x=" + std::string(limits::kHttpChunkLineBytes - 11 + extra, 'a') + "\r\na\r\n0\r\n\r\n";
    std::size_t offset = 0;
    EXPECT_EQ(decoder.decode(last, offset, data), extra == 0 ? fetch::ChunkedDecoder::Result::done : fetch::ChunkedDecoder::Result::invalid);
  }
}

TEST("decompression budgets admit exact input, output and cumulative ratio") {
  namespace limits = uos::rt::limits;
  using uos::rt::detail::DecompressionBudget;
  constexpr auto max = limits::kHttpBodyBytes;
  DecompressionBudget bytes;
  EXPECT(bytes.feed(max));
  EXPECT(!bytes.feed(1));
  EXPECT(bytes.advance(max, max));
  EXPECT(!bytes.advance(0, 1));

  DecompressionBudget ratio;
  EXPECT(ratio.feed(2));
  EXPECT(ratio.advance(1, limits::kDecompressionRatioSlackBytes));
  EXPECT(ratio.advance(1, 2 * limits::kDecompressionRatio));
  EXPECT(!ratio.advance(0, 1));
}

TEST("Fetch gzip bounds decoded bytes independently of wire size") {
  namespace limits = uos::rt::limits;
  for (const std::size_t extra : {0u, 1u}) {
    const auto compressed = uos::rt::gzip::gzipSync(uos::rt::gzip::Bytes(limits::kHttpBodyBytes + extra, 'a'));
    const std::string_view wire(reinterpret_cast<const char*>(compressed.data()), compressed.size());
    auto decoder = fetch::contentDecoder("gzip", "http://localhost");
    std::string out;
    if (extra == 0) {
      decoder->write(wire, out);
      decoder->finish(out);
      EXPECT_EQ(out.size(), limits::kHttpBodyBytes);
    } else {
      EXPECT_THROWS_MATCHING(decoder->write(wire, out), "ResponseBodyTooLarge");
    }
  }
}

TEST("Fetch Brotli rejects a valid stream past the expansion budget") {
  // Brotli 1.1.0 (-q 5) encoding of 1,048,576 and 1,100,000 ASCII 'a' bytes.
  constexpr auto within = "\x5f\xff\xff\x0f\x40\x22\x2c\x1e\x0b\x24\xf7\xfe\x01"sv;
  constexpr auto beyond = "\x9f\xdf\xc8\x10\x00\x24\xc2\xe2\xb1\x40\x32\x81\x21\x00"sv;
  auto accepted = fetch::contentDecoder("br", "http://localhost");
  std::string out;
  accepted->write(within, out);
  accepted->finish(out);
  EXPECT_EQ(out.size(), uos::rt::limits::kDecompressionRatioSlackBytes);

  auto rejected = fetch::contentDecoder("br", "http://localhost");
  out.clear();
  EXPECT_THROWS_MATCHING(rejected->write(beyond, out), "ResponseBodyTooLarge");
}

#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
TEST("Fetch Zstd caps both decoded bytes and the advertised frame window") {
  namespace limits = uos::rt::limits;
  const std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> compressor(ZSTD_createCCtx(), ZSTD_freeCCtx);
  ASSERT(compressor);
  for (const int extra : {0, 1}) {
    const std::size_t decodedSize = limits::kHttpBodyBytes + static_cast<std::size_t>(extra);
    std::string input(decodedSize, '\0');
    std::uint32_t state = 1;
    constexpr std::size_t blockBytes = 65536;
    for (std::size_t i = 0; i < blockBytes; ++i) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      input[i] = static_cast<char>(state & 255);
    }
    for (std::size_t i = blockBytes; i < input.size(); ++i) input[i] = input[i % blockBytes];
    std::string compressed(ZSTD_compressBound(input.size()), '\0');
    const auto length = ZSTD_compress2(compressor.get(), compressed.data(), compressed.size(), input.data(), input.size());
    ASSERT(!ZSTD_isError(length));
    auto decoder = fetch::contentDecoder("zstd", "http://localhost");
    const std::string_view wire(compressed.data(), length);
    std::string out;
    if (extra == 0) {
      decoder->write(wire, out);
      decoder->finish(out);
      EXPECT(out == input);
    } else EXPECT_THROWS_MATCHING(decoder->write(wire, out), "ResponseBodyTooLarge");

    ASSERT(!ZSTD_isError(ZSTD_CCtx_reset(compressor.get(), ZSTD_reset_session_and_parameters)));
    ASSERT(!ZSTD_isError(ZSTD_CCtx_setParameter(compressor.get(), ZSTD_c_windowLog, limits::kZstdWindowLog + extra)));
    ASSERT(!ZSTD_isError(ZSTD_CCtx_setParameter(compressor.get(), ZSTD_c_contentSizeFlag, 0)));
    ZSTD_inBuffer inputChunk{input.data(), blockBytes, 0};
    ZSTD_outBuffer frame{compressed.data(), compressed.size(), 0};
    ASSERT_EQ(ZSTD_compressStream2(compressor.get(), &frame, &inputChunk, ZSTD_e_flush), 0u);
    ASSERT_EQ(ZSTD_compressStream2(compressor.get(), &frame, &inputChunk, ZSTD_e_end), 0u);
    auto window = fetch::contentDecoder("zstd", "http://localhost");
    const std::string_view windowWire(compressed.data(), frame.pos);
    out.clear();
    if (extra == 0) {
      window->write(windowWire, out);
      window->finish(out);
      EXPECT_EQ(out.size(), blockBytes);
    } else EXPECT_THROWS_MATCHING(window->write(windowWire, out), "ResponseBodyTooLarge");
    ASSERT(!ZSTD_isError(ZSTD_CCtx_reset(compressor.get(), ZSTD_reset_session_and_parameters)));
  }
}
#endif
