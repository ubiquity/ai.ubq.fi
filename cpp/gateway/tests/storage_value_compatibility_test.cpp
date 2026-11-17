// Differential cases for src/storage/value-compatibility.ts (no direct bun test):
// the outcomes were recorded by running assertPortableKvValue under bun 1.4.0 on
// the JS values these KvValues model. Values with accessor properties, or an
// Error with extra own properties, have no rt::KvValue form.
#include "uos/storage/value_compatibility.hpp"

#include "uos/rt/kv.hpp"
#include "uos/rt/test.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
using uos::storage::value_compatibility::assertPortableKvValue;

namespace {
  // `String(error)` of what assertPortableKvValue threw, or "ok".
  std::string outcome(const rt::KvValue& value) {
    try {
      assertPortableKvValue(value);
      return "ok";
    } catch (const rt::JsError& error) {
      return error.toString();
    }
  }

  rt::KvValue error(std::string name, std::string message) {
    rt::KvError value;
    value.name = std::move(name);
    value.message = std::move(message);
    return rt::KvValue(std::move(value));
  }

  // `new Error("inner", { cause })`
  rt::KvValue withCause(rt::KvValue cause) {
    rt::KvError value;
    value.message = "inner";
    value.cause = std::make_shared<const rt::KvValue>(std::move(cause));
    return rt::KvValue(std::move(value));
  }

  const std::string causeRefused = "TypeError: Bun KV cannot preserve Error.cause; persist an explicit error record instead";
}

TEST("assertPortableKvValue accepts primitives and errors without a cause") {
  EXPECT_EQ(outcome(rt::KvValue()), std::string("ok"));
  EXPECT_EQ(outcome(rt::KvValue(nullptr)), std::string("ok"));
  EXPECT_EQ(outcome(rt::KvValue(1)), std::string("ok"));
  EXPECT_EQ(outcome(rt::KvValue("text")), std::string("ok"));
  EXPECT_EQ(outcome(rt::KvValue(rt::BigInt(10))), std::string("ok"));
  EXPECT_EQ(outcome(error("Error", "plain")), std::string("ok"));
  EXPECT_EQ(outcome(error("TypeError", "typed")), std::string("ok"));
  EXPECT_EQ(outcome(rt::KvValue::object({{"e", error("Error", "plain")}, {"n", rt::KvValue::array({1, 2})}})), std::string("ok"));
}

TEST("assertPortableKvValue refuses an Error.cause of any value") {
  EXPECT_EQ(outcome(withCause(rt::KvValue("why"))), causeRefused);
  EXPECT_EQ(outcome(withCause(rt::KvValue())), causeRefused);
  EXPECT_EQ(outcome(withCause(error("Error", "root"))), causeRefused);
}

TEST("assertPortableKvValue finds a caused error anywhere in arrays, objects, maps and sets") {
  EXPECT_EQ(outcome(rt::KvValue::array({1, rt::KvValue::array({2, withCause(rt::KvValue(1))})})), causeRefused);
  EXPECT_EQ(outcome(rt::KvValue::object({{"a", rt::KvValue::object({{"b", withCause(rt::KvValue(nullptr))}})}})), causeRefused);
  EXPECT_EQ(outcome(rt::KvValue(rt::KvMap{{withCause(rt::KvValue(1)), rt::KvValue(1)}})), causeRefused);
  EXPECT_EQ(outcome(rt::KvValue(rt::KvMap{{rt::KvValue("k"), withCause(rt::KvValue(1))}})), causeRefused);
  EXPECT_EQ(outcome(rt::KvValue(rt::KvSet{rt::KvValue(1), withCause(rt::KvValue(1))})), causeRefused);
}

TEST("assertPortableKvValue accepts maps, sets, typed and opaque values without causes") {
  EXPECT_EQ(outcome(rt::KvValue(rt::KvMap{{rt::KvValue("k"), rt::KvValue(rt::KvSet{rt::KvValue(1), rt::KvValue::object({{"x", 2}})})}})), std::string("ok"));
  rt::KvArrayBufferView view;
  view.kind = rt::KvViewKind::DataView;
  view.buffer = rt::KvBytes(4);
  view.byteLength = 4;
  EXPECT_EQ(outcome(rt::KvValue::object({
              {"bytes", rt::KvValue(rt::KvBytes{1})},
              {"buffer", rt::KvValue(rt::KvArrayBuffer{rt::KvBytes(2)})},
              {"view", rt::KvValue(view)},
              {"date", rt::KvValue(rt::KvDate{0})},
              {"regexp", rt::KvValue(rt::KvRegExp{"a", "g"})},
              {"u64", rt::KvValue(rt::KvU64(5))},
            })),
            std::string("ok"));
  EXPECT_EQ(outcome(rt::KvValue(rt::KvArray{rt::KvValue(rt::KvHole{}), rt::KvValue(1)})), std::string("ok"));
}
