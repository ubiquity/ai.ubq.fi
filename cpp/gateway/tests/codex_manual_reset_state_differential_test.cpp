// Differential cases for src/codex/manual-reset-state.ts, recorded from the TypeScript under Bun
// 1.4.0: parseManualResetIntent over valid, boundary and malformed rows (inputs are the
// JSON.stringify of the oracle's inputs; results its JSON.stringify), and the intent key.
#include "uos/codex/manual_reset_state.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::codex::manual_reset_state;

namespace {
  struct Vector {
    const char* name;
    const char* input;
    const char* result;
  };
  const std::vector<Vector> vectors = {
    {"valid", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":null})json", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":null})json"},
    {"promoted null", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":null,"quota_signal_at_ms":null})json", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":null,"quota_signal_at_ms":null})json"},
    {"quota signal number", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":1700000000500})json", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":1700000000500})json"},
    {"promoted equals created", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000000000,"quota_signal_at_ms":null})json", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000000000,"quota_signal_at_ms":null})json"},
    {"promoted before created", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1699999999999,"quota_signal_at_ms":null})json", R"json(null)json"},
    {"zero values", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":0,"routing_generation":0,"created_at_ms":0,"promoted_at_ms":0,"quota_signal_at_ms":0})json", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":0,"routing_generation":0,"created_at_ms":0,"promoted_at_ms":0,"quota_signal_at_ms":0})json"},
    {"negative zero", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":0,"created_at_ms":1700000000000,"promoted_at_ms":null,"quota_signal_at_ms":null})json", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":0,"created_at_ms":1700000000000,"promoted_at_ms":null,"quota_signal_at_ms":null})json"},
    {"max safe", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":9007199254740991,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":null})json", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":9007199254740991,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":null})json"},
    {"beyond safe", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":9007199254740992,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":null})json", R"json(null)json"},
    {"fractional", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1.5,"promoted_at_ms":1700000001000,"quota_signal_at_ms":null})json", R"json(null)json"},
    {"negative generation", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":-1,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":null})json", R"json(null)json"},
    {"string time", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":"later","routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":null})json", R"json(null)json"},
    {"v string", R"json({"v":"1","account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":null})json", R"json(null)json"},
    {"v two", R"json({"v":2,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":null})json", R"json(null)json"},
    {"extra key", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":null,"access_token":"unapproved"})json", R"json(null)json"},
    {"missing key", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"quota_signal_at_ms":null})json", R"json(null)json"},
    {"missing replaced", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"quota_signal_at_ms":null,"other":1})json", R"json(null)json"},
    {"uppercase hash", R"json({"v":1,"account_id_hash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":null})json", R"json(null)json"},
    {"short credential", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":null})json", R"json(null)json"},
    {"credential number", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":5,"quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":null})json", R"json(null)json"},
    {"quota signal negative", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":-5})json", R"json(null)json"},
    {"quota signal string", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":"x"})json", R"json(null)json"},
    {"promoted bool", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":1700000060000,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":true,"quota_signal_at_ms":null})json", R"json(null)json"},
    {"null", R"json(null)json", R"json(null)json"},
    {"array", R"json([1,2,3,4,5,6,7,8])json", R"json(null)json"},
    {"string", R"json("intent")json", R"json(null)json"},
    {"reordered keys", R"json({"quota_signal_at_ms":null,"promoted_at_ms":null,"created_at_ms":5,"routing_generation":1,"quota_reset_at_ms":9,"credential_version":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","account_id_hash":"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","v":1})json", R"json({"quota_signal_at_ms":null,"promoted_at_ms":null,"created_at_ms":5,"routing_generation":1,"quota_reset_at_ms":9,"credential_version":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","account_id_hash":"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","v":1})json"},
    {"infinite", R"json({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quota_reset_at_ms":null,"routing_generation":2,"created_at_ms":1700000000000,"promoted_at_ms":1700000001000,"quota_signal_at_ms":null})json", R"json(null)json"},
  };
}

TEST("parseManualResetIntent matches the oracle") {
  for (const Vector& vector : vectors) {
    const std::optional<ManualIntent> parsed = parseManualResetIntent(rt::jsonParse(vector.input));
    const json::value actual = parsed ? json::value_from(*parsed) : json::value(nullptr);
    // The TS returns the stored object itself; the struct writes its members in declaration
    // order, so the comparison is structural ("reordered keys" differs only in member order).
    if (!rt::deepEqual(actual, rt::jsonParse(vector.result))) std::fprintf(stderr, "    %s: %s\n", vector.name, rt::jsonStringify(actual).c_str());
    EXPECT(rt::deepEqual(actual, rt::jsonParse(vector.result)));
  }
  EXPECT_EQ(vectors.size(), std::size_t{28});
}

TEST("the intent key extends the retained prefix") {
  const rt::KvKey expected{"uos_ai", "codex_manual_reset", "v1", "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"};
  EXPECT(intentKey("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee") == expected);
  EXPECT(CODEX_MANUAL_RESET_PREFIX == (rt::KvKey{"uos_ai", "codex_manual_reset", "v1"}));
}

TEST("ManualIntent round-trips through its strict row codec") {
  const std::optional<ManualIntent> parsed = parseManualResetIntent(rt::jsonParse(vectors[0].input));
  ASSERT(parsed.has_value());
  EXPECT(json::value_to<ManualIntent>(json::value_from(*parsed)) == *parsed);
  EXPECT_EQ(rt::jsonStringify(json::value_from(*parsed)), std::string(vectors[0].result));
}
