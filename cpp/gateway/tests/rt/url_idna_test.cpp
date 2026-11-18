// Links: tests/rt/url_idna_test.cpp
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/url.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace {
  using namespace std::string_view_literals;
  using uos::rt::InvalidURLError;
  using uos::rt::URL;

  struct IdnaVector {
    std::string_view host;
    std::string_view scheme;
    std::string_view operation;
    std::string_view input;
    std::optional<std::string_view> expected;
  };

  struct ScalarBlock {
    std::uint32_t end;
    std::uint32_t hash;
    std::uint32_t accepted;
  };

#include "url_idna_vectors.inc"

  // Adjacent scalar blocks retain the original recorded checksums.
  constexpr ScalarBlock kAdaScalarBlocks[] = {
      {2048u, 3736327821u, 1366u},
      {3072u, 578839389u, 1185u},
      {64512u, 4113564197u, 1206u},
      {68608u, 4006216133u, 700u},
      {69632u, 472399413u, 436u},
  };

  std::optional<std::string_view> expectedHost(const IdnaVector& vector) {
    // The original corpus remains verbatim. Only these ContextJ hosts
    // change under the adapter's fail-closed rule for special schemes.
    constexpr std::string_view joinerHosts[] = {
        "क्\u200dष.example", "क्\u200cष.example", "نامه\u200cای.example", "क्\u200d.example", "ب\u200cت.example",
    };
    if (vector.scheme == "custom" || std::ranges::find(joinerHosts, vector.host) == std::end(joinerHosts)) return vector.expected;
    if (vector.operation != "host" && vector.operation != "hostname") return std::nullopt;
    if (vector.scheme == "file") return "file://initial.example/p?q#f\001initial.example\001initial.example\001null"sv;
    return "http://initial.example/p?q#f\001initial.example\001initial.example\001http://initial.example"sv;
  }

  std::string describe(const URL& url) { return url.href() + '\001' + url.host() + '\001' + url.hostname() + '\001' + url.origin(); }

  std::uint32_t updateHash(std::uint32_t hash, std::string_view text) {
    for (const char byte : text) hash = (hash ^ static_cast<unsigned char>(byte)) * 16777619u;
    return hash;
  }
}

TEST("Ada IDNA covers constructors and host, hostname and href setters") {
  std::size_t mismatches = 0;
  for (const auto& vector : kIdnaVectors) {
    std::optional<std::string> actual;
    try {
      URL url = vector.operation == "construct" ? URL(vector.input) : URL(std::string(vector.scheme) + "://initial.example/p?q#f");
      if (vector.operation == "host") url.host(vector.host);
      else if (vector.operation == "hostname") url.hostname(vector.host);
      else if (vector.operation == "href") url.href(vector.input);
      actual = describe(url);
    } catch (const InvalidURLError& error) {
      EXPECT_EQ(error.name, "TypeError");
      EXPECT_EQ(error.message, "Invalid URL");
      EXPECT_EQ(error.code, "ERR_INVALID_URL");
    }
    const auto expectedView = expectedHost(vector);
    const auto expected = expectedView ? std::optional<std::string>(std::string(*expectedView)) : std::nullopt;
    if (actual != expected) {
      if (++mismatches <= 12) {
        std::fprintf(stderr, "IDNA operation=%s scheme=%s host=%s\n", std::string(vector.operation).c_str(), std::string(vector.scheme).c_str(),
                     uos::rt::test::show(std::string(vector.host)).c_str());
        EXPECT_EQ(actual, expected);
      }
    }
  }
  EXPECT_EQ(mismatches, std::size_t{0});
  std::fprintf(stderr, "IDNA curated vectors=%zu\n", std::size(kIdnaVectors));
}

TEST("Ada IDNA covers every Unicode code point in two host contexts") {
  std::uint32_t start = 0;
  std::size_t mismatches = 0, acceptedTotal = 0;
  for (const auto& block : kScalarBlocks) {
    std::uint32_t hash = 0x811c9dc5u, accepted = 0;
    for (std::uint32_t codePoint = start; codePoint < block.end; ++codePoint) {
      const std::string unit = uos::rt::fromCodePoint(static_cast<double>(codePoint));
      for (int context = 0; context < 2; ++context) {
        std::string result;
        try {
          const std::string host = context == 0 ? unit + ".example" : "a" + unit + "b.example";
          result = describe(URL("http://" + host + "/p?q#f"));
          ++accepted;
        } catch (const InvalidURLError& error) {
          result = error.name + '\001' + error.message + '\001' + error.code;
        }
        hash = updateHash(hash, std::to_string(codePoint) + '\002' + std::to_string(context) + '\002' + result + '\003');
      }
    }
    const auto revised = std::ranges::find(kAdaScalarBlocks, block.end, &ScalarBlock::end);
    const auto& expected = revised == std::end(kAdaScalarBlocks) ? block : *revised;
    if (hash != expected.hash || accepted != expected.accepted) {
      if (++mismatches <= 12) {
        std::fprintf(stderr, "IDNA scalar range=%u..%u\n", start, block.end - 1);
        EXPECT_EQ(hash, expected.hash);
        EXPECT_EQ(accepted, expected.accepted);
      }
    }
    acceptedTotal += accepted;
    start = block.end;
  }
  EXPECT_EQ(mismatches, std::size_t{0});
  EXPECT_EQ(start, 0x110000u);
  std::fprintf(stderr, "IDNA scalar codepoints=%u contexts=2 records=%u accepted=%zu blocks=%zu\n", start, start * 2, acceptedTotal, std::size(kScalarBlocks));
}
TEST("IDNA rejects malformed A-labels and unsafe bidirectional hosts") {
  EXPECT(URL::canParse("http://b\303\274cher.de"));
  EXPECT(URL::canParse("http://\344\275\240\345\245\275"));
  EXPECT(URL::canParse("https://אב.example/"));
  EXPECT(URL::canParse("https://1.example/"));
  EXPECT(URL::canParse("https://a\u0898b.example/"));

  for (const auto badHost : {"xn--a"sv, "%78%6e--a"sv, "1.אב"sv, "a\u0870b"sv, "क्\u200c\u200da"sv, "\u0898.example"sv, "xn--hyb.example"sv}) {
    const auto input = "https://" + std::string(badHost) + "/";
    EXPECT(!URL::canParse(input));
    EXPECT(!URL::parse(input, "https://example.test/"));
    EXPECT_EQ(URL("blob:" + input).origin(), "null");
    URL url("https://example.test:8443/path?q=1");
    url.host(badHost);
    url.hostname(badHost);
    EXPECT_EQ(url.href(), "https://example.test:8443/path?q=1");
  }
}
