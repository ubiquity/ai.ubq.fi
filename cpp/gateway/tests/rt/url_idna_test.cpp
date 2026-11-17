// Links: tests/rt/url_idna_test.cpp
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/url.hpp"

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

#ifdef UOS_GATEWAY_NATIVE_LIBRARIES
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

  std::string describe(const URL& url) { return url.href() + '\001' + url.host() + '\001' + url.hostname() + '\001' + url.origin(); }

  std::uint32_t updateHash(std::uint32_t hash, std::string_view text) {
    for (const char byte : text) hash = (hash ^ static_cast<unsigned char>(byte)) * 16777619u;
    return hash;
  }
#endif
}

#ifdef UOS_GATEWAY_NATIVE_LIBRARIES
TEST("ICU UTS46 matches Bun constructors and host hostname href setters") {
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
    const auto expected = vector.expected ? std::optional<std::string>(std::string(*vector.expected)) : std::nullopt;
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

TEST("ICU UTS46 matches Bun for every Unicode code point in two host contexts") {
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
    if (hash != block.hash || accepted != block.accepted) {
      if (++mismatches <= 12) {
        std::fprintf(stderr, "IDNA scalar range=%u..%u\n", start, block.end - 1);
        EXPECT_EQ(hash, block.hash);
        EXPECT_EQ(accepted, block.accepted);
      }
    }
    acceptedTotal += accepted;
    start = block.end;
  }
  EXPECT_EQ(mismatches, std::size_t{0});
  EXPECT_EQ(start, 0x110000u);
  std::fprintf(stderr, "IDNA scalar codepoints=%u contexts=2 records=%u accepted=%zu blocks=%zu\n", start, start * 2, acceptedTotal, std::size(kScalarBlocks));
}
#else
TEST("SDK comparison explicitly retains the documented Latin1 IDNA boundary") {
  std::fprintf(stderr, "Full ICU oracle is native-profile acceptance; SDK comparison checks its retained Latin1 boundary.\n");
  EXPECT(URL::canParse("http://b\303\274cher.de"));
  EXPECT(!URL::canParse("http://\344\275\240\345\245\275"));
}
#endif
