// Differential cases for src/codex/quota.ts recorded from Bun 1.4.0 (the TS tests
// do not cover percent formatting edges or withCodexQuotaHeaders).
#include "uos/codex/quota.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace uos;
using codex::quota::buildCodexQuotaHeaders;
using codex::quota::ClientQuotaSnapshot;
using codex::quota::withCodexQuotaHeaders;
using Entries = std::vector<rt::Headers::Entry>;

TEST("used_percent is clamped to 0..100 and rounded to six decimals") {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const std::vector<std::pair<double, std::string>> cases = {
    {0, "0"},
    {50, "50"},
    {95, "95"},
    {100, "100"},
    {33.333333333, "33.333333"},
    {66.6666666666, "66.666667"},
    {-5, "0"},
    {150, "100"},
    {nan, "NaN"},
    {inf, "100"},
    {-inf, "0"},
    {0.0000004, "0"},
    {0.0000005, "0.000001"},
    {0.0000015, "0.000002"},
    {99.9999995, "100"},
    {12.3456785, "12.345679"},
    {1e-7, "0"},
    {-0.0, "0"},
    {42.0000004999, "42"},
    {2.5e-7, "0"},
    {0.1 + 0.2, "0.3"},
  };
  for (const auto& [percent, expected] : cases) {
    const rt::Headers headers = buildCodexQuotaHeaders(rt::Headers{}, ClientQuotaSnapshot{.used_percent = percent});
    EXPECT_EQ(headers.get("x-codex-primary-used-percent"), std::optional<std::string>(expected));
    EXPECT_EQ(headers.get("x-codex-limit-name"), std::optional<std::string>("Metered balance"));
  }
}

TEST("only x- families with a known suffix and the shared Codex headers are stripped") {
  const rt::Headers headers = buildCodexQuotaHeaders(
    rt::Headers{
      {"X-Foo-Primary-Used-Percent", "1"},
      {"x-a", "2"},
      {"x-limit-name", "z"},
      {"y-codex-primary-used-percent", "keep"},
      {"x-codex-credits-balance", "1"},
      {"x-codex-primary-over-secondary-limit-percent", "3"},
      {"set-cookie", "a=1"},
      {"x--limit-name", "q"},
      {"x-limit-names", "keep2"},
    },
    ClientQuotaSnapshot{.used_percent = std::nullopt});
  EXPECT_EQ(headers.entries(), (Entries{{"x-a", "2"}, {"x-limit-names", "keep2"}, {"y-codex-primary-used-percent", "keep"}, {"set-cookie", "a=1"}}));

  const rt::Headers replaced = buildCodexQuotaHeaders(rt::Headers{{"x-codex-limit-name", "old"}}, ClientQuotaSnapshot{.used_percent = 7.25});
  EXPECT_EQ(replaced.entries(), (Entries{{"x-codex-limit-name", "Metered balance"}, {"x-codex-primary-used-percent", "7.25"}}));

  const rt::Headers stripped = buildCodexQuotaHeaders(rt::Headers{{"x-codex-limit-name", "old"}}, std::nullopt);
  EXPECT_EQ(stripped.entries(), Entries{});
}

ASYNC_TEST("withCodexQuotaHeaders keeps status, statusText and body and rewrites the headers") {
  const rt::Response original(rt::BodyInit(std::string("body text")),
                              rt::ResponseInit{.status = 429, .statusText = "Too Many", .headers = rt::Headers{{"content-type", "text/plain"}, {"x-codex-primary-used-percent", "9"}, {"x-other", "v"}}});
  const rt::Response wrapped = withCodexQuotaHeaders(original, ClientQuotaSnapshot{.used_percent = 12.5});
  EXPECT_EQ(wrapped.status(), 429);
  EXPECT_EQ(wrapped.statusText(), "Too Many");
  EXPECT_EQ(wrapped.headers().entries(),
            (Entries{{"content-type", "text/plain"}, {"x-codex-limit-name", "Metered balance"}, {"x-codex-primary-used-percent", "12.5"}, {"x-other", "v"}}));
  // Bun already reports the source used here (it locked a buffered body's stream); rt marks it
  // used once the stream is read (docs/rt/web.md, Limits), so only the state after the read is pinned.
  EXPECT_EQ(wrapped.bodyUsed(), false);
  const std::string text = co_await wrapped.text();
  EXPECT_EQ(text, "body text");
  EXPECT_EQ(original.bodyUsed(), true);

  rt::ResponseInit noContent;
  noContent.status = 204;
  const rt::Response empty = withCodexQuotaHeaders(rt::Response(nullptr, noContent), std::nullopt);
  EXPECT_EQ(empty.status(), 204);
  EXPECT_EQ(empty.statusText(), "");
  EXPECT_EQ(empty.headers().entries(), Entries{});
  EXPECT_EQ(empty.body().has_value(), false);

  rt::ResponseInit sourceInit;
  sourceInit.status = 200;
  sourceInit.headers = rt::Headers{{"x-codex-primary-used-percent", "1"}};
  const rt::Response source(rt::BodyInit(std::string("x")), sourceInit);
  const rt::Response decorated = withCodexQuotaHeaders(source, ClientQuotaSnapshot{.used_percent = 1});
  EXPECT_EQ(source.headers().entries(), (Entries{{"x-codex-primary-used-percent", "1"}}));
  EXPECT_EQ(decorated.headers().entries(), (Entries{{"x-codex-limit-name", "Metered balance"}, {"x-codex-primary-used-percent", "1"}}));
}
