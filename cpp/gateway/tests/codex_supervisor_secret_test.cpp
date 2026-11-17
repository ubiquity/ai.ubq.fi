// Port of tests/codex-supervisor-secret.test.ts.
#include "uos/codex/supervisor_secret.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace rt = uos::rt;
using uos::codex::supervisor_secret::RedactedText;
using uos::codex::supervisor_secret::redactSupervisorSecretParts;
using uos::codex::supervisor_secret::redactSupervisorSecrets;

namespace uos::codex::supervisor_secret {
  // Test-harness display hook (found by argument-dependent lookup).
  std::string show(const RedactedText& value) { return "{ text: " + rt::test::show(value.text) + ", redactions: " + std::to_string(value.redactions) + " }"; }
}

// `assert.ok(condition, message)` / `assert.equal(actual, false, message)`: the message names the input.
#define EXPECT_MESSAGE(condition, message) \
  do { \
    if (!(condition)) ::uos::rt::test::report(__FILE__, __LINE__, std::string("expected " #condition ": ") + (message), false); \
  } while (0)

namespace {
  bool anyIncludes(const std::vector<RedactedText>& redacted, std::string_view search) {
    for (const RedactedText& part : redacted) {
      if (rt::includes(part.text, search)) return true;
    }
    return false;
  }

  std::vector<RedactedText> eachRedacted(const std::vector<std::string>& parts) {
    std::vector<RedactedText> redacted;
    for (const std::string& part : parts) redacted.push_back(redactSupervisorSecrets(part));
    return redacted;
  }

  std::string joinedForMessage(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& part : parts) out += (out.empty() ? "" : " | ") + part;
    return out;
  }
}

TEST("backtick quoting adds a reading without narrowing the original bracket scan") {
  const std::vector<std::pair<std::string, std::string>> cases = {
    {"tokens: [", "[redacted]"},
    {"{\"api_keys\": [", "{\"[redacted]"},
    {"api_key: {a: ", "[redacted]"},
  };
  for (const auto& [prefix, expected] : cases) {
    EXPECT_EQ(redactSupervisorSecrets(prefix + "`A 'B \\'C `D ]E ").text, expected);
  }
  EXPECT_EQ(redactSupervisorSecrets("tokens = [`a]`, \"zqalpha\"] done").text, std::string("[redacted] done"));
}

TEST("credential-named arrow parameters do not redact their callbacks") {
  const std::vector<std::string> callbacks = {
    "const lengths = tokens.map(token => token.trim().length);",
    "apiKeys.forEach(key => {\n  const label = describe(key);\n  console.log(label);\n});\nnext();",
    "tokens.map(token=>token.trim().length);",
    "visit(items, secret=>{ describe(secret); });",
    "visit(\"(\", token=>token.trim());",
    "tokens.map((token) => token.trim());",
    "tokens.map((token, index) => token.trim());",
    "tokens.map(({token}) => token.trim());",
  };
  for (const std::string& text : callbacks) EXPECT_EQ(redactSupervisorSecrets(text), (RedactedText{.text = text, .redactions = 0}));
  for (const std::string& text : std::vector<std::string>{
         "'password' => 'zqsecretvalue'",
         ":api_key => 'zqsecretvalue'",
         "client_secret => zqsecretvalue",
         "items,api_key=>\"zqsecretvalue\"",
         "log('('), api_key=>zqsecretvalue",
       }) {
    EXPECT_MESSAGE(!rt::includes(redactSupervisorSecrets(text).text, "zqsecretvalue"), text);
  }
}

namespace {
  const bool perlCases = [] {
    const std::vector<std::pair<std::string, std::function<std::string(const std::string&)>>> positions = {
      {"first hash entry", [](const std::string& value) { return "my %config = (password => " + value + "); done"; }},
      {"subsequent hash entry", [](const std::string& value) { return "my %config = (mode => 'safe', password => " + value + "); done"; }},
      {"first call argument", [](const std::string& value) { return "configure(password=>" + value + "); done"; }},
      {"subsequent call argument", [](const std::string& value) { return "configure(mode => 'safe', password=>" + value + "); done"; }},
      {"nested parenthesized entry", [](const std::string& value) { return "configure(group => (password => " + value + ")); done"; }},
      {"parenthesized entry without a declaration", [](const std::string& value) { return "(password=>" + value + "); done"; }},
    };
    const std::vector<std::pair<std::string, std::string>> kinds = {
      {"quoted scalar", "'zqsecretvalue'"},
      {"bare scalar", "zqsecretvalue"},
      {"Perl scalar", "$zqsecretvalue"},
      {"array", "['zqfirstsecretvalue', 'zqsecondsecretvalue']"},
      {"hash reference", "{ first => 'zqfirstsecretvalue', second => 'zqsecondsecretvalue' }"},
    };
    for (const auto& [position, wrap] : positions) {
      for (const auto& [kind, value] : kinds) {
        rt::test::registry().push_back({"Perl " + position + " redacts " + kind + " credentials", [wrapCase = wrap, caseValue = value] {
                                          const std::string text = wrapCase(caseValue);
                                          const RedactedText redacted = redactSupervisorSecrets(text);
                                          EXPECT_MESSAGE(!rt::includes(redacted.text, "zq"), text);
                                          EXPECT_MESSAGE(redacted.redactions > 0, text);
                                        }});
      }
    }
    return true;
  }();
}

TEST("ambiguous callback-like credential values remain redacted") {
  const std::string canary = "zqsecretvalue";
  const std::string firstCanary = "zqfirstsecretvalue";
  const std::string credential = "password";
  for (const std::string& value : std::vector<std::string>{
         "passwordzqsecretvalue",
         "password.zqsecretvalue",
         "zqsecretvalue && password.trim()",
         "password.trim()+'zqsecretvalue'",
         "/*password.trim()*/'zqsecretvalue'",
         "{ describe(password); value => 'zqsecretvalue' }",
         "{ describe(password); value = 'zqsecretvalue'; }",
         "{ describe(password); /* zqsecretvalue */ }",
         "{ describe(password); nested => { value => 'zqsecretvalue' } }",
         "{ describe($password); leak(zqsecretvalue); }",
       }) {
    const std::string text = "configure(password=>" + value + "); done";
    EXPECT_MESSAGE(!rt::includes(redactSupervisorSecrets(text).text, "zq"), text);
  }
  for (const std::string& text : std::vector<std::string>{"('password'=>'" + canary + "')", "(outer=>{password=>'" + firstCanary + "'}, token=>'zqsecondsecretvalue')"}) {
    EXPECT_MESSAGE(!rt::includes(redactSupervisorSecrets(text).text, "zq"), text);
  }
  EXPECT(!rt::includes(redactSupervisorSecrets("(token = 'zqsecretvalue') => token").text, "zq"));
  // Bare-value spans end at whitespace; a method prefix must still not exempt the expression.
  const RedactedText expression = redactSupervisorSecrets("configure(" + credential + "=>" + credential + ".trim() + '" + canary + "'); done");
  EXPECT(expression.redactions > 0);
  EXPECT(!rt::includes(expression.text, credential + "=>" + credential + ".trim()"));
}

TEST("Perl fat-comma credential values redact across key, separator and value record boundaries") {
  for (const std::vector<std::string>& parts : std::vector<std::vector<std::string>>{
         {"my %config = (password => 'zqfirstsecretvalue", "zqsecondsecretvalue'); done"},
         {"configure(mode => 'safe', api_key", " => ", "'zqsecretvalue'); done"},
         {"my %config = (tokens", " => ['zqfirstsecretvalue',", "{ value => 'zqsecondsecretvalue' }]); done"},
         {"configure(group => (", "password => { value => 'zqfirstsecretvalue',", "next => 'zqsecondsecretvalue' })); done"},
       }) {
    const std::vector<RedactedText> redacted = redactSupervisorSecretParts(parts);
    EXPECT_EQ(redacted.size(), parts.size());
    EXPECT_MESSAGE(!anyIncludes(redacted, "zq"), joinedForMessage(parts));
    EXPECT_MESSAGE(!redacted.empty() && rt::endsWith(redacted.back().text, " done"), joinedForMessage(parts));
  }
}

TEST("repeated fat-comma values beyond callback lookahead remain bounded and redacted") {
  const double started = rt::performanceNow();
  const std::string entry = "password=>{" + std::string(600, ' ') + "value=>'zqsecretvalue'},";
  const std::string text = "configure(" + rt::repeat(entry, 1'000) + "); done";
  const RedactedText redacted = redactSupervisorSecrets(text);
  EXPECT(!rt::includes(redacted.text, "zq"));
  EXPECT(rt::performanceNow() - started < 2'000);
}

TEST("Perl, Ruby and Elixir string prefixes keep bracket characters inside their credential strings") {
  for (const std::string prefix : {"q", "qq", "%q", "%Q", "~s", "~S"}) {
    const std::string text = "api_keys: [" + prefix + "'zq]alpha', 'zqbravo'] done";
    EXPECT_EQ(redactSupervisorSecrets(text).text, std::string("[redacted] done"));
    const std::vector<std::string> parts = {"api_keys: [" + prefix + "'zq]alpha',", " 'zqbravo'] done"};
    EXPECT_MESSAGE(!anyIncludes(redactSupervisorSecretParts(parts), "zq"), prefix);
  }
  EXPECT_EQ(redactSupervisorSecrets("api_keys: [it's, 'zqsecretvalue'] done").text, std::string("[redacted] done"));
}

TEST("quoted credential keys redact bare values without consuming adjacent fields") {
  const std::vector<std::pair<std::string, std::string>> cases = {
    {"\"password\": hunter2hunter2", "\"[redacted]"},
    {"{\"password\": 12345678, \"model\": \"m\"}", "{\"[redacted], \"model\": \"m\"}"},
    {"'password' => SECRETCONSTANT1", "'[redacted]"},
    {"'password'=>SECRETCONSTANT1;next();", "'[redacted];next();"},
    {"{\"password\":12345678,\"model\":\"m\"}", "{\"[redacted],\"model\":\"m\"}"},
    {"{\"password\":\"zqsecretvalue\",\"model\":\"m\"}", "{\"[redacted],\"model\":\"m\"}"},
    {"{\"password\":[\"zqsecretvalue\"],\"model\":\"m\"}", "{\"[redacted],\"model\":\"m\"}"},
    {"[\"password\":12345678] done", "[\"[redacted]] done"},
  };
  for (const auto& [text, expected] : cases) EXPECT_EQ(redactSupervisorSecrets(text).text, expected);
  EXPECT_EQ(redactSupervisorSecrets("token=Bearer abcdefghijklmnop").redactions, 2);
}

TEST("closed credential values are redacted across records before their keys are lost") {
  const std::vector<std::vector<std::string>> cases = {
    {"tool output: -----BEGIN OPENSSH PRIVATE KEY-----\nzqfirstsecretvalue", "tool output: zqsecondsecretvalue\n-----END OPENSSH PRIVATE KEY----- done"},
    {"{\"api_keys\": [\"zqfirstsecretvalue\", ", "\"zqsecondsecretvalue\"]} done"},
    {"password: \"zqfirstsecretvalue", "zqsecondsecretvalue\" done"},
    {"password: 'zqfirstsecretvalue", "zqsecondsecretvalue' done"},
    {"password: `zqfirstsecretvalue", "zqsecondsecretvalue` done"},
    {"api_key: {a: \"zqfirstsecretvalue\",", "b: \"zqsecondsecretvalue\"} done"},
    {"{\"api_keys\": [", "\"zqfirstsecretvalue\", ", "\"zqsecondsecretvalue\"]} done"},
    // The closed inner credential must not disappear into an unclosed outer scan.
    {"tokens: [ api_keys: [\"zqfirstsecretvalue\",", "\"zqsecondsecretvalue\"] done"},
  };
  for (const std::vector<std::string>& parts : cases) {
    const std::vector<RedactedText> redacted = redactSupervisorSecretParts(parts);
    EXPECT_EQ(redacted.size(), parts.size());
    EXPECT_MESSAGE(!anyIncludes(redacted, "zq"), joinedForMessage(parts));
    EXPECT_MESSAGE(!redacted.empty() && rt::endsWith(redacted.back().text, " done"), joinedForMessage(parts));
  }
}

TEST("unclosed cross-record readings cannot consume later records") {
  const std::string canary = "zqsecretvalue";
  for (const std::string& text : std::vector<std::string>{
         "api_keys: [\"zqsecretvalue",
         "password: \"zqsecretvalue",
         "password: 'zqsecretvalue",
         "password: `zqsecretvalue",
         "my %config = (password => '" + canary,
         "configure(password=>['" + canary,
       }) {
    const std::vector<std::string> parts = {text, "Working on the refresh loop"};
    EXPECT_EQ(redactSupervisorSecretParts(parts), eachRedacted(parts));
  }
  // Projected closed spans cannot suppress a broader per-record fallback.
  const std::vector<std::string> parts = {"password: \"zqsecretvalue\"zqtailrun", "Working on the refresh loop"};
  EXPECT_EQ(redactSupervisorSecretParts(parts), eachRedacted(parts));
  EXPECT_EQ(redactSupervisorSecretParts({"token=Bearer abcdefghijklmnop", "done"})[0].redactions, 2);
}

TEST("cross-record bracket readings stay linear with many unclosed outer credentials") {
  const double started = rt::performanceNow();
  const std::vector<RedactedText> redacted = redactSupervisorSecretParts({rt::repeat("tokens: [", 25'000) + "zqsecretvalue", "] done"});
  EXPECT(!anyIncludes(redacted, "zq"));
  EXPECT(rt::endsWith(redacted[1].text, " done"));
  EXPECT(rt::performanceNow() - started < 2'000);
}

// Differential vectors (not in the TS file), recorded with bun 1.4.0: non-ASCII
// text, astral characters and lone surrogates, where every position the module
// computes is a UTF-16 index. The TS tests use ASCII only.
TEST("differential: non-ASCII, astral and lone-surrogate text redacts at UTF-16 positions") {
  const std::vector<std::tuple<std::string, std::string, std::int64_t>> singles = {
    {"password: \xc3\xa9\xf0\x9f\x98\x80\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9 done", "[redacted] done", 1},
    {"password:\xc2\xa0zqsecretvalue\xe3\x80\x80" "done", "[redacted]\xe3\x80\x80" "done", 1},
    {"PASSWORD\xe2\x80\xa8=\xe2\x80\xa8zqsecretvalue rest", "[redacted] rest", 1},
    {"x\xed\xa0\xbdpassword: zqsecretvalue\xed\xb8\x80 done", "x\xed\xa0\xbd[redacted] done", 1},
    {"token: [\xed\xa0\xbd, '\xed\xb8\x80zq'] done", "[redacted] done", 1},
    {"\xe4\xb8\xad\xe6\x96\x87 api_key = \"\xe5\x80\xbc\xf0\x9f\x98\x80\\\"zq\" \xe4\xb8\xad\xe6\x96\x87", "\xe4\xb8\xad\xe6\x96\x87 [redacted] \xe4\xb8\xad\xe6\x96\x87", 1},
    {"secret: {'k': ['\xf0\x9d\x90\x80', \"b\"]} \xf0\x9d\x90\x80 after", "[redacted] \xf0\x9d\x90\x80 after", 1},
    {"Bearer abcdefghijklmnop\xf0\x9f\x98\x80 then \xf0\x9f\x98\x80 token=abc\xf0\x9f\x98\x80" "defghijk", "[redacted]\xf0\x9f\x98\x80 then \xf0\x9f\x98\x80 [redacted]", 2},
    {"\xf0\x9d\x90\x80's password => 'zq\xf0\x9f\x98\x80' and it's fine", "\xf0\x9d\x90\x80's [redacted] and it's fine", 1},
    {"tokens: [x\xf0\x9d\x90\x80'y, 'zq'] done", "[redacted] done", 1},
    {"\xf0\x9f\x98\x80\xf0\x9f\x98\x80 -----BEGIN RSA PRIVATE KEY-----\x0a\xf0\x9f\x98\x80\x0a-----END RSA PRIVATE KEY----- \xf0\x9f\x98\x80", "\xf0\x9f\x98\x80\xf0\x9f\x98\x80 [redacted] \xf0\x9f\x98\x80", 1},
    {"\xc5\xbf" "ecret: zqsecretvalue Kpassword: zqsecretvalue", "\xc5\xbf" "ecret: zqsecretvalue [redacted]", 1},
  };
  const std::vector<std::pair<std::vector<std::string>, std::vector<RedactedText>>> records = {
    {{"\xf0\x9f\x98\x80 password: \"zq\xf0\x9f\x98\x80" "first", "second\xf0\x9f\x98\x80\" done"}, {{"\xf0\x9f\x98\x80 [redacted]", 1}, {"[redacted] done", 1}}},
    {{"api_keys: [\"\xc3\xa9\", ", "'\xe4\xb8\xad\xe6\x96\x87'] done", "tail \xf0\x9f\x98\x80"}, {{"[redacted]", 1}, {"[redacted] done", 1}, {"tail \xf0\x9f\x98\x80", 0}}},
    {{"\xed\xa0\xbd", "\xed\xb8\x80 token: abcdefghij"}, {{"\xed\xa0\xbd", 0}, {"\xed\xb8\x80 [redacted]", 1}}},
  };
  for (const auto& [input, text, redactions] : singles) EXPECT_EQ(redactSupervisorSecrets(input), (RedactedText{.text = text, .redactions = redactions}));
  for (const auto& [parts, expected] : records) EXPECT_EQ(redactSupervisorSecretParts(parts), expected);
}
