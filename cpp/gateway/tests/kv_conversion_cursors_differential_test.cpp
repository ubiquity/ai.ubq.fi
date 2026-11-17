// Differential cases for src/kv-conversion/cursors.ts: every expected value was recorded from
// Bun 1.4.0 running the TypeScript module on the same inputs.
#include "uos/kv_conversion/cursors.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/text_codec.hpp"

namespace {
  using namespace uos::kv_conversion::cursors;
  namespace rt = uos::rt;

  // `JSON.stringify(result)` for a transcoded value, or "<Name> <message>" for a throw.
  std::string outcome(const std::vector<std::uint8_t>& key, const rt::KvValue& value, const std::string& direction) {
    try {
      const TranscodePersistedCursorResult result = transcodePersistedCursor(key, value, direction);
      return std::string(result.changed ? "changed " : "unchanged ") + rt::jsonStringify(result.value.toJson());
    } catch (const rt::JsError& error) {
      return error.name + " " + error.message;
    }
  }

  std::string btoa(std::string_view text) { return rt::toBase64(rt::encodeUtf8(text)); }

  const std::vector<std::uint8_t> reapKey = packStringKey({"uos_ai", "sentinel_replay", "v1", "reap_cursor"});
  const std::vector<std::uint8_t> budgetKey = packStringKey({"uos_ai", "sentinel_replay", "v1", "budget"});
  const std::string denoCursor = rt::toBase64Url(packStringKey({"capture"}));
  const std::string accountingHex = rt::toHex(packStringKey({"uos_ai", "sentinel_replay", "v1", "accounting"}));
  const std::string bunCursor =
    "eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3NTZmNzM1ZjYxNjkwMDAyNzM2NTZlNzQ2OTZlNjU2YzVmNzI2NTcwNmM2MTc5MDAwMjc2MzEwMDAyNjE2MzYzNmY3NTZlNzQ2OTZlNjcwMDAyNjM2MTcwNzQ3NTcyNjUwMCJ9";
}

TEST("packStringKey writes 02, the TextEncoder bytes and 00 per part without escaping") {
  EXPECT_EQ(rt::toHex(reapKey), "02756f735f6169000273656e74696e656c5f7265706c6179000276310002726561705f637572736f7200");
  EXPECT_EQ(rt::toHex(packStringKey({})), "");
  EXPECT_EQ(rt::toHex(packStringKey({std::string("a\0b", 3), "\xC3\xA9", "\xED\xA0\x80x", "\xF0\x9F\x98\x80"})), "026100620002c3a90002efbfbd780002f09f988000");
  EXPECT_EQ(denoCursor, "AmNhcHR1cmUA");
}

TEST("the reaper cursor converts both ways and refuses malformed legacy cursors") {
  EXPECT_EQ(outcome(reapKey, rt::KvValue(denoCursor), "deno-to-bun"), "changed \"" + bunCursor + "\"");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(bunCursor), "bun-to-deno"), "changed \"AmNhcHR1cmUA\"");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(bunCursor), "sideways"), "changed \"AmNhcHR1cmUA\"");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(""), "deno-to-bun"), "Error Malformed replay reaper cursor");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(5), "deno-to-bun"), "Error Malformed replay reaper cursor");
  EXPECT_EQ(outcome(reapKey, rt::KvValue("ab+c"), "deno-to-bun"), "Error Invalid legacy cursor");
  EXPECT_EQ(outcome(reapKey, rt::KvValue("abc==="), "deno-to-bun"), "Error Invalid legacy cursor");
  EXPECT_EQ(outcome(reapKey, rt::KvValue("AB"), "deno-to-bun"), "Error Noncanonical legacy cursor");
  EXPECT_EQ(outcome(reapKey, rt::KvValue("A"), "deno-to-bun"), "Error Noncanonical legacy cursor");
  const std::string zeroByte =
    "changed \"eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3NTZmNzM1ZjYxNjkwMDAyNzM2NTZlNzQ2OTZlNjU2YzVmNzI2NTcwNmM2MTc5MDAwMjc2MzEwMDAyNjE2MzYzNmY3NTZlNzQ2OTZlNjcwMDAwIn0=\"";
  EXPECT_EQ(outcome(reapKey, rt::KvValue("AA="), "deno-to-bun"), zeroByte);
  EXPECT_EQ(outcome(reapKey, rt::KvValue("AA=="), "deno-to-bun"), zeroByte);
  EXPECT_EQ(outcome(reapKey, rt::KvValue("QQ"), "deno-to-bun"),
            "changed \"eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3NTZmNzM1ZjYxNjkwMDAyNzM2NTZlNzQ2OTZlNjU2YzVmNzI2NTcwNmM2MTc5MDAwMjc2MzEwMDAyNjE2MzYzNmY3NTZlNzQ2OTZlNjcwMDQxIn0=\"");
}

TEST("Bun cursors parse as JSON and must name a hex key inside the replay prefix") {
  EXPECT_EQ(outcome(reapKey, rt::KvValue(btoa("null")), "bun-to-deno"), "TypeError null is not an object (evaluating 'decoded.lastYieldedKeyBytes')");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(btoa("5")), "bun-to-deno"), "Error Invalid Bun cursor");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(btoa("[1]")), "bun-to-deno"), "Error Invalid Bun cursor");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(btoa("{x")), "bun-to-deno"), "SyntaxError Invalid JSON");
  EXPECT_EQ(outcome(reapKey, rt::KvValue("!!!!"), "bun-to-deno"), "SyntaxError Invalid JSON");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(btoa(R"({"lastYieldedKeyBytes":"AB"})")), "bun-to-deno"), "Error Invalid Bun cursor");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(btoa(R"({"lastYieldedKeyBytes":"abc"})")), "bun-to-deno"), "Error Invalid Bun cursor");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(btoa(R"({"lastYieldedKeyBytes":5})")), "bun-to-deno"), "Error Invalid Bun cursor");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(btoa(R"({"lastYieldedKeyBytes":"0102"})")), "bun-to-deno"), "Error Cursor outside expected replay prefix");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(btoa(R"({"lastYieldedKeyBytes":")" + accountingHex + "\"}")), "bun-to-deno"), "changed \"\"");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(btoa(R"({"lastYieldedKeyBytes":")" + accountingHex + "fbff3e\"}")), "bun-to-deno"), "changed \"-_8-\"");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(btoa("\xEF\xBB\xBF{\"lastYieldedKeyBytes\":\"" + accountingHex + "00\"}")), "bun-to-deno"),
            "SyntaxError Invalid JSON");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(btoa(" \n{\"lastYieldedKeyBytes\":\"" + accountingHex + "00\"} ")), "bun-to-deno"), "changed \"AA==\"");
  EXPECT_EQ(outcome(reapKey, rt::KvValue(btoa("{\"lastYieldedKeyBytes\":5,\"lastYieldedKeyBytes\":\"" + accountingHex + "01\"}")), "bun-to-deno"), "changed \"AQ==\"");
}

TEST("the budget ledger must be an object owning bootstrap_cursor") {
  EXPECT_EQ(outcome(budgetKey, rt::KvValue("x"), "deno-to-bun"), "Error Malformed replay budget ledger");
  EXPECT_EQ(outcome(budgetKey, rt::KvValue(nullptr), "deno-to-bun"), "Error Malformed replay budget ledger");
  EXPECT_EQ(outcome(budgetKey, rt::KvValue::object({{"other", 1}}), "deno-to-bun"), "Error Malformed replay budget ledger");
  EXPECT_EQ(outcome(budgetKey, rt::KvValue::array({"bootstrap_cursor"}), "deno-to-bun"), "Error Malformed replay budget ledger");
  EXPECT_EQ(outcome(budgetKey, rt::KvValue(rt::KvMap{{rt::KvValue("bootstrap_cursor"), rt::KvValue("1|x")}}), "deno-to-bun"), "Error Malformed replay budget ledger");
  EXPECT_EQ(outcome(budgetKey, rt::KvValue::object({{"bootstrap_cursor", nullptr}, {"a", 1}}), "deno-to-bun"), "unchanged {\"bootstrap_cursor\":null,\"a\":1}");
  EXPECT_EQ(outcome(budgetKey, rt::KvValue::object({{"bootstrap_cursor", rt::KvUndefined{}}}), "deno-to-bun"), "Error Malformed replay bootstrap cursor");
  EXPECT_EQ(outcome(budgetKey, rt::KvValue::object({{"bootstrap_cursor", 4}}), "deno-to-bun"), "Error Malformed replay bootstrap cursor");
}

TEST("the bootstrap cursor keeps its prefix digits and position and converts its suffix") {
  EXPECT_EQ(outcome(budgetKey, rt::KvValue::object({{"bootstrap_cursor", "99|bad"}}), "deno-to-bun"), "Error Unsupported replay bootstrap prefix");
  EXPECT_EQ(outcome(budgetKey, rt::KvValue::object({{"bootstrap_cursor", "7|" + denoCursor}}), "deno-to-bun"), "Error Unsupported replay bootstrap prefix");
  EXPECT_EQ(
    outcome(budgetKey, rt::KvValue::object({{"bootstrap_cursor", "6|" + denoCursor}}), "deno-to-bun"),
    "changed {\"bootstrap_cursor\":\"6|eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3NTZmNzM1ZjYxNjkwMDAyNzM2NTZlNzQ2OTZlNjU2YzVmNzI2NTcwNmM2MTc5MDAwMjc2MzEwMDAyNzI2NTczNjU3Mjc2NjE3NDY5NmY2ZTAwMDI2MzYxNzA3NDc1NzI2NTAwIn0=\"}");
  EXPECT_EQ(
    outcome(budgetKey, rt::KvValue::object({{"bootstrap_cursor", "04|" + denoCursor}, {"z", 1}}), "deno-to-bun"),
    "changed {\"bootstrap_cursor\":\"04|eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3NTZmNzM1ZjYxNjkwMDAyNzM2NTZlNzQ2OTZlNjU2YzVmNzI2NTcwNmM2MTc5MDAwMjc2MzEwMDAyNmQ2MTZlNjk2NjY1NzM3NDAwMDI2MzYxNzA3NDc1NzI2NTAwIn0=\",\"z\":1}");
  EXPECT_EQ(outcome(budgetKey, rt::KvValue::object({{"bootstrap_cursor", "3|"}}), "deno-to-bun"), "unchanged {\"bootstrap_cursor\":\"3|\"}");
  EXPECT_EQ(outcome(budgetKey, rt::KvValue::object({{"bootstrap_cursor", "3"}}), "deno-to-bun"), "Error Unsupported replay bootstrap prefix");
  EXPECT_EQ(outcome(budgetKey, rt::KvValue::object({{"bootstrap_cursor", "3|a\nb"}}), "deno-to-bun"), "Error Unsupported replay bootstrap prefix");
  EXPECT_EQ(outcome(budgetKey, rt::KvValue::object({{"bootstrap_cursor", "3||"}}), "deno-to-bun"), "Error Invalid legacy cursor");
  EXPECT_EQ(outcome(budgetKey, rt::KvValue::object({{"first", 1}, {"bootstrap_cursor", "0|" + denoCursor}, {"last", 2}}), "deno-to-bun"),
            "changed {\"first\":1,\"bootstrap_cursor\":\"0|" + bunCursor + "\",\"last\":2}");
  EXPECT_EQ(outcome(budgetKey, rt::KvValue::object({{"bootstrap_cursor", "99999999999999999999|x"}}), "deno-to-bun"), "Error Unsupported replay bootstrap prefix");
  const std::string dedupe = btoa("{\"lastYieldedKeyBytes\":\"" + rt::toHex(packStringKey({"uos_ai", "sentinel_replay", "v1", "dedupe"})) + "0263617074757265\"}");
  EXPECT_EQ(outcome(budgetKey, rt::KvValue::object({{"bootstrap_cursor", "2|" + dedupe}}), "bun-to-deno"), "changed {\"bootstrap_cursor\":\"2|AmNhcHR1cmU=\"}");
}

TEST("other keys keep their value unchanged") {
  EXPECT_EQ(outcome(packStringKey({"uos_ai", "paid_fallback"}), rt::KvValue::object({{"bootstrap_cursor", 9}}), "deno-to-bun"), "unchanged {\"bootstrap_cursor\":9}");
}

TEST("the TS round trip keeps counters next to the converted bootstrap cursor") {
  const rt::KvValue value = rt::KvValue::object({{"bootstrap_cursor", "4|" + denoCursor}, {"stored_bytes", 123}, {"records", 7}});
  const TranscodePersistedCursorResult forward = transcodePersistedCursor(budgetKey, value, "deno-to-bun");
  EXPECT_EQ(rt::jsonStringify(forward.value.toJson()),
            "{\"bootstrap_cursor\":\"4|eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3NTZmNzM1ZjYxNjkwMDAyNzM2NTZlNzQ2OTZlNjU2YzVmNzI2NTcwNmM2MTc5MDAwMjc2MzEwMDAyNmQ2MTZlNjk2NjY1NzM3NDAwMDI2MzYxNzA3NDc1NzI2NTAwIn0=\",\"stored_bytes\":123,\"records\":7}");
  const TranscodePersistedCursorResult back = transcodePersistedCursor(budgetKey, forward.value, "bun-to-deno");
  EXPECT_EQ(back.changed, true);
  EXPECT_EQ(rt::jsonStringify(back.value.toJson()), "{\"bootstrap_cursor\":\"4|AmNhcHR1cmUA\",\"stored_bytes\":123,\"records\":7}");
}
