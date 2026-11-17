// Port of tests/auth-relay.test.ts: the assertions on src/auth/relay.ts
// (parseTrustedServerAuthRelayOrigin). The assertions on static/auth-relay.js
// (isTrustedAuthRelayClientOrigin, the client parseTrustedAuthRelayOrigin,
// parseAuthRelayAction) test browser code served as a static file, which is not
// part of the C++ port; the two cases made only of them are not in this file:
//   "auth relay identifies approved Deno relay client origins"
//   "auth relay only accepts the passkey login action"
#include "uos/auth/relay.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
  namespace rt = uos::rt;
  using uos::auth::relay::parseTrustedAuthRelayOrigin;

  std::optional<std::string> parseTrustedServerAuthRelayOrigin(const std::optional<std::string>& value) { return parseTrustedAuthRelayOrigin(value); }

  /**
   * The same origin downgraded to clear text. Rejecting a clear-text scheme on an
   * otherwise trusted host is the subject of the assertions below, so the
   * insecure variant is derived from the trusted origin instead of duplicated.
   */
  std::string clearTextOrigin(std::string_view origin) {
    static const rt::Regex HTTPS_SCHEME("^https:");
    return rt::replace(origin, HTTPS_SCHEME, "http:");
  }
}

TEST("auth relay rejects local development origins") {
  for (const std::string hostname : {"localhost", "127.0.0.1", "[::1]"}) {
    EXPECT_EQ(parseTrustedServerAuthRelayOrigin("http://" + hostname + ":8000"), std::nullopt);
    EXPECT_EQ(parseTrustedServerAuthRelayOrigin("https://" + hostname), std::nullopt);
  }
}

TEST("auth relay accepts canonical and owned Deno organization origins") {
  EXPECT_EQ(parseTrustedServerAuthRelayOrigin("https://ai.ubq.fi"), std::optional<std::string>("https://ai.ubq.fi"));
  EXPECT_EQ(parseTrustedServerAuthRelayOrigin("https://ai-ubq-fi.deno.dev"), std::optional<std::string>("https://ai-ubq-fi.deno.dev"));
  for (const std::string origin : {
         "https://ai-ubq-fi.ubiquity-dao.deno.net",
         "https://ai-ubq-fi-cv5fc93pzb5a.ubiquity-dao.deno.net",
         "https://p-ai-ubq-fi.ubiquity-dao.deno.net",
         "https://p-ai-ubq-fi-cv5fc93pzb5a.ubiquity-dao.deno.net",
         "https://telegram-daily-exporter.0x4007.deno.net",
         "https://telegram-daily-exporter-4d2p9cx7m1ab.0x4007.deno.net",
         "https://agent-worker.ubiquity-os.deno.net",
         "https://agent-worker-4d2p9cx7m1ab.ubiquity-os.deno.net",
         "https://ai-ubq-fi-ejc9p6zmdjxt.deno.dev",
       }) {
    EXPECT_EQ(parseTrustedServerAuthRelayOrigin(origin), std::optional<std::string>(origin));
  }
}

TEST("auth relay rejects untrusted or malformed origins") {
  for (const std::string& origin : {
         std::string("https://example.com"),
         std::string("https://app.other-org.deno.net"),
         std::string("https://untrusted-app.0x4007.deno.net"),
         std::string("https://untrusted-app.ubiquity-os.deno.net"),
         std::string("https://checkout-ubq-fi.ubiquity-dao.deno.net"),
         std::string("https://app.0x4007.deno.net.evil.example"),
         std::string("https://nested.app.0x4007.deno.net"),
         std::string("https://unowned-app.deno.dev"),
         std::string("https://ai-ubq-fi-evil.deno.dev"),
         std::string("https://ai-ubq-fi-cv5fc93pzb5a.evil.example"),
         clearTextOrigin("https://ai-ubq-fi-cv5fc93pzb5a.ubiquity-dao.deno.net"),
         std::string("https://app.0x4007.deno.net:8443"),
         std::string("https://app.0x4007.deno.net/admin"),
         std::string("https://ai.ubq.fi:8443"),
         std::string("https://ai.ubq.fi/admin"),
         std::string("javascript:alert(1)"),
       }) {
    EXPECT_EQ(parseTrustedServerAuthRelayOrigin(origin), std::nullopt);
  }
}
