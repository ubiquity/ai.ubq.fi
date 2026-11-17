// Differential cases for src/auth/relay.ts: every expected value was recorded by
// running parseTrustedAuthRelayOrigin under Bun 1.4.0. Non-string inputs (null,
// undefined, 42 in the TS run) are std::nullopt here.
#include "uos/auth/relay.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>

namespace {
  using uos::auth::relay::parseTrustedAuthRelayOrigin;

  struct RelayVector {
    std::optional<std::string> input;
    std::optional<std::string> output;
  };
}

TEST("differential: parseTrustedAuthRelayOrigin matches Bun") {
  const RelayVector vectors[] = {
    {std::nullopt, std::nullopt},
    {"", std::nullopt},
    {"   ", std::nullopt},
    {"https://ai.ubq.fi", "https://ai.ubq.fi"},
    {" https://ai.ubq.fi ", "https://ai.ubq.fi"},
    {"https://ai.ubq.fi/", "https://ai.ubq.fi"},
    {"https://ai.ubq.fi///", "https://ai.ubq.fi"},
    {"https://ai.ubq.fi/x", std::nullopt},
    {"https://AI.UBQ.FI", std::nullopt},
    {"HTTPS://ai.ubq.fi", std::nullopt},
    {"https://ai.ubq.fi:443", std::nullopt},
    {"https://ai.ubq.fi:444", std::nullopt},
    {"https://ai.ubq.fi?x", std::nullopt},
    {"https://ai.ubq.fi#x", std::nullopt},
    {"https://user@ai.ubq.fi", std::nullopt},
    {"https://ai.ubq.fi.", std::nullopt},
    {"https://ai-ubq-fi.deno.dev", "https://ai-ubq-fi.deno.dev"},
    {"https://ai-ubq-fi-abcdefghijkl.deno.dev", "https://ai-ubq-fi-abcdefghijkl.deno.dev"},
    {"https://ai-ubq-fi-abcdefghijk.deno.dev", std::nullopt},
    {"https://ai-ubq-fi-abcdefghijklm.deno.dev", std::nullopt},
    {"https://ai-ubq-fi-ABCDEFGHIJKL.deno.dev", std::nullopt},
    {"https://p-ai-ubq-fi-0123456789ab.ubiquity-dao.deno.net", "https://p-ai-ubq-fi-0123456789ab.ubiquity-dao.deno.net"},
    {"https://p-ai-ubq-fi-0123456789ab.ubiquity-dao.deno.net/", "https://p-ai-ubq-fi-0123456789ab.ubiquity-dao.deno.net"},
    {"https://telegram-daily-exporter.0x4007.deno.net", "https://telegram-daily-exporter.0x4007.deno.net"},
    {"https://agent-worker-zzzzzzzzzzzz.ubiquity-os.deno.net", "https://agent-worker-zzzzzzzzzzzz.ubiquity-os.deno.net"},
    {"https://agent-worker-zzzzzzzzzzzz.ubiquity-os.deno.net.", std::nullopt},
    {"https://.ubiquity-os.deno.net", std::nullopt},
    {"https://ai-ubq-fi.ubiquity-dao.deno.net:443", std::nullopt},
    {"https://ai-ubq-fi.UBIQUITY-DAO.deno.net", std::nullopt},
    {"wss://ai.ubq.fi", std::nullopt},
    {"https://ai.ubq.fi\t", "https://ai.ubq.fi"},
    {" https://ai.ubq.fi", "https://ai.ubq.fi"},
    {"https://xn--ai-ubq-fi.deno.dev", std::nullopt},
    {"https:ai.ubq.fi", std::nullopt},
    {"https:/ai.ubq.fi", std::nullopt},
    {"https://ai.ubq.fi:", std::nullopt},
    {"https://[::1]", std::nullopt},
    {"https://ai-ubq-fi-abcdefghijkl.deno.dev.evil", std::nullopt},
  };
  for (const RelayVector& vector : vectors) EXPECT_EQ(parseTrustedAuthRelayOrigin(vector.input), vector.output);
}
