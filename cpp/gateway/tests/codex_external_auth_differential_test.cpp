// Differential cases for src/codex/external-auth.ts beyond tests/codex-external-native-auth.test.ts:
// JWT payload decoding (base64 and base64url, padding, whitespace, invalid characters, BOM and
// invalid UTF-8, duplicate and __proto__ keys, non-object claims), every member check of the native
// leaf, and Date.parse on `last_refresh`. Expected values were recorded from the TypeScript module
// under Bun 1.4.0 (tests/codex_external_auth_differential_vectors.inc).
#include "uos/codex/external_auth.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/text_codec.hpp"

#include <cstdio>
#include <optional>
#include <string>

#include "codex_external_auth_differential_vectors.inc"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::codex::external_auth;

  json::value identityJson(const std::optional<ExternalCodexIdentity>& identity) {
    if (!identity) return nullptr;
    return json::object{{"userId", identity->userId}, {"accountId", identity->accountId}};
  }

  json::value documentJson(const std::optional<ExternalNativeCodexDocument>& document) {
    if (!document) return nullptr;
    json::object out;
    out["auth"] = json::value_from(document->auth);
    out["userId"] = document->userId;
    out["refreshedAt"] = document->refreshedAt ? json::value(json::string(*document->refreshedAt)) : json::value(nullptr);
    return out;
  }

  void compare(const json::value& actual, const json::value& expected, const json::value& input) {
    const std::string got = rt::jsonStringify(actual);
    const std::string wanted = rt::jsonStringify(expected);
    if (got == wanted) return;
    EXPECT_EQ(got, wanted);
    std::fprintf(stderr, "      input: %s\n", rt::jsonStringify(input).c_str());
  }
}

TEST("differential: externalCodexIdentity matches Bun") {
  const json::value vectors = rt::jsonParse(kExternalAuthVectors);
  int checked = 0;
  for (const json::value& row : vectors.at("identities").as_array()) {
    const json::array& vector = row.as_array();
    compare(identityJson(externalCodexIdentity(std::string(vector[0].as_string()))), vector[1], vector[0]);
    checked += 1;
  }
  EXPECT_EQ(checked, 42);
  // A payload nested deeper than rt's JSON limit: JSC parses it and finds no claims, rt::jsonParse
  // throws RangeError inside the try; both return null.
  const std::string deep = rt::repeat("[", 10'001) + rt::repeat("]", 10'001);
  compare(identityJson(externalCodexIdentity("header." + rt::toBase64Url(rt::encodeUtf8(deep)) + ".signature")), vectors.at("deep"), json::value("deep"));
}

TEST("differential: parseExternalNativeCodexDocument matches Bun") {
  const json::value vectors = rt::jsonParse(kExternalAuthVectors);
  int checked = 0;
  for (const json::value& row : vectors.at("parsed").as_array()) {
    const json::array& vector = row.as_array();
    compare(documentJson(parseExternalNativeCodexDocument(vector[0])), vector[1], vector[0]);
    checked += 1;
  }
  EXPECT_EQ(checked, 54);
  compare(documentJson(parseExternalNativeCodexDocument(nullptr)), vectors.at("undefinedInput"), json::value("undefined"));
}

TEST("differential: the official access-only leaf parses with an empty refresh token") {
  // The parse assertions of "official access-only leaf stays out of managed uploads and donor
  // candidates" (that bun test also needs the unported codex auth, repair and routing modules).
  const json::value vectors = rt::jsonParse(kExternalAuthVectors);
  const json::value& leaf = vectors.at("parsed").as_array()[0].as_array()[0];
  const std::optional<ExternalNativeCodexDocument> parsed = parseExternalNativeCodexDocument(leaf);
  ASSERT(parsed.has_value());
  EXPECT_EQ(parsed->auth.refresh_token, "");
  EXPECT_EQ(parsed->auth.account_id, "workspace-one");
  EXPECT_EQ(parsed->userId, "user-one");
  EXPECT_EQ(parsed->refreshedAt, std::nullopt);
}

TEST("ExternalNativeCodexDocument.auth converts to and from JSON in declaration order") {
  const ExternalNativeCodexDocument::Auth auth{.access_token = "a.b.c", .refresh_token = "", .account_id = "workspace-one"};
  const json::value written = json::value_from(auth);
  EXPECT_EQ(rt::jsonStringify(written), std::string(R"({"access_token":"a.b.c","refresh_token":"","account_id":"workspace-one"})"));
  EXPECT(json::value_to<ExternalNativeCodexDocument::Auth>(written) == auth);
  EXPECT_THROWS_MESSAGE(json::value_to<ExternalNativeCodexDocument::Auth>(rt::jsonParse(R"({"access_token":"a","refresh_token":null,"account_id":"w"})")),
                        rt::TypeError, "ExternalNativeCodexDocument.auth.refresh_token is null, not string");
}
