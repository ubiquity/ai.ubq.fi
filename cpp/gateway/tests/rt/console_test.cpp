// console formatting, routing and capture. Every expected line was recorded
// from bun 1.4 with Error.stackTraceLimit = 0 (errors without stack frames, the
// form the port reproduces), one console call per row with the same arguments.
#include "uos/rt/console.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/test.hpp"
#include <unistd.h>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace uos;
using namespace std::literals;
namespace console = uos::rt::console;
using console::Method;
using console::Properties;
using console::Value;

namespace {
  constexpr double NAN_ = std::numeric_limits<double>::quiet_NaN();
  constexpr double INF_ = std::numeric_limits<double>::infinity();

  Value err(std::string name, std::string message, Properties properties = {}, std::optional<Value> cause = std::nullopt) {
    console::ErrorInfo info;
    info.name = std::move(name);
    info.message = std::move(message);
    info.properties = std::make_shared<Properties>(std::move(properties));
    if (cause) info.cause = std::make_shared<console::ErrorInfo>(cause->errorInfo());
    return Value::error(std::move(info));
  }

  Value fsError(const std::string& path) {
    try {
      rt::fs::readFileSync(path);
    } catch (const rt::fs::ErrnoError& error) {
      return Value(error);
    }
    return Value();
  }

  // Captures what a call writes to fd 1 or 2.
  std::string written(int fd, auto body) {
    std::fflush(nullptr);
    int pipes[2];
    if (::pipe(pipes) != 0) return "<pipe failed>";
    const int saved = ::dup(fd);
    ::dup2(pipes[1], fd);
    ::close(pipes[1]);
    body();
    ::dup2(saved, fd);
    ::close(saved);
    std::string out;
    char buffer[4096];
    for (ssize_t count; (count = ::read(pipes[0], buffer, sizeof buffer)) > 0;) out.append(buffer, static_cast<std::size_t>(count));
    ::close(pipes[0]);
    return out;
  }
}

TEST("console formatting matches bun for every recorded call shape") {
  EXPECT_EQ(console::format(Method::log, {Value("plain"sv)}),
            std::string("plain"sv));
  EXPECT_EQ(console::format(Method::log, {Value("a\nb"sv), Value("c"sv)}),
            std::string("a\nb c"sv));
  EXPECT_EQ(console::format(Method::log, {}),
            std::string(""sv));
  EXPECT_EQ(console::format(Method::log, {Value(1.0), Value(-0.0), Value(0.1), Value(1e+21), Value(NAN_), Value(INF_), Value(9007199254740992), Value(-1.5e-07), Value(1.2345678901234568e+20)}),
            std::string("1 -0 0.1 1e+21 NaN Infinity 9007199254740992 -1.5e-7 123456789012345680000"sv));
  EXPECT_EQ(console::format(Method::log, {Value(true), Value(nullptr), Value(), Value("s"sv), Value(json::parse(R"__json(["a", 1, null])__json")), Value(json::parse(R"__json({})__json")), Value(json::parse(R"__json([])__json")), Value(json::parse(R"__json({"nested": {"deep": {"x": [1, {"y": 2}]}}})__json"))}),
            std::string("true null undefined s [ \"a\", 1, null ] {} [] {\n  nested: {\n    deep: {\n      x: [ 1, [Object ...] ],\n    },\n  },\n}"sv));
  EXPECT_EQ(console::format(Method::info, {Value("[ai.ubq.fi] request_terminal"sv), Value("{\"a\":1,\"b\":\"x\"}"sv)}),
            std::string("[ai.ubq.fi] request_terminal {\"a\":1,\"b\":\"x\"}"sv));
  EXPECT_EQ(console::format(Method::error, {Value("[ai.ubq.fi] label"sv), Value(json::parse(R"__json({"error_class": "Error", "status": null, "code": null})__json"))}),
            std::string("[ai.ubq.fi] label {\n  error_class: \"Error\",\n  status: null,\n  code: null,\n}"sv));
  EXPECT_EQ(console::format(Method::warn, {Value("[ai.ubq.fi] passkey assertion verification failed"sv), Value(json::parse(R"__json({"message": "bad sig", "expected_origin": "https://ai.ubq.fi", "expected_rp_id": "ai.ubq.fi", "credential_id_prefix": "abcdefghijkl", "has_user_handle": true, "require_user_verification": false})__json"))}),
            std::string("[ai.ubq.fi] passkey assertion verification failed {\n  message: \"bad sig\",\n  expected_origin: \"https://ai.ubq.fi\",\n  expected_rp_id: \"ai.ubq.fi\",\n  credential_id_prefix: \"abcdefghijkl\",\n  has_user_handle: true,\n  require_user_verification: false,\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([1, 2, 3])__json"))}),
            std::string("[ 1, 2, 3 ]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([1, 2, 3, 4, 5, 6, 7, 8, 9, 10])__json"))}),
            std::string("[ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 ]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10])__json"))}),
            std::string("[\n  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10\n]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26])__json"))}),
            std::string("[\n  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,\n  23, 24, 25, 26\n]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"k": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25]})__json"))}),
            std::string("{\n  k: [\n    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,\n    22, 23, 24, 25\n  ],\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100])__json"))}),
            std::string("[\n  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,\n  23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42,\n  43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62,\n  63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82,\n  83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99,\n  ... 1 more items\n]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([0, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000, 11000, 12000, 13000, 14000, 15000, 16000, 17000, 18000, 19000, 20000, 21000, 22000, 23000, 24000, 25000, 26000, 27000, 28000, 29000])__json"))}),
            std::string("[\n  0, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000, 11000, 12000, 13000,\n  14000, 15000, 16000, 17000, 18000, 19000, 20000, 21000, 22000, 23000, 24000, 25000,\n  26000, 27000, 28000, 29000\n]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json(["s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10"])__json"))}),
            std::string("[\n  \"s0\", \"s1\", \"s2\", \"s3\", \"s4\", \"s5\", \"s6\", \"s7\", \"s8\", \"s9\", \"s10\"\n]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([1, "a", true, null, 2, "b", false, 3, "c", 4, 5, 6])__json"))}),
            std::string("[\n  1, \"a\", true, null, 2, \"b\", false, 3, \"c\", 4, 5, 6\n]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([{"a": 1}])__json"))}),
            std::string("[\n  {\n    a: 1,\n  }\n]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([{"a": 1}, {"b": 2}])__json"))}),
            std::string("[\n  {\n    a: 1,\n  }, {\n    b: 2,\n  }\n]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([[1, 2], [3]])__json"))}),
            std::string("[\n  [ 1, 2 ], [ 3 ]\n]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json(["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"])__json"))}),
            std::string("[ \"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\" ]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json(["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"])__json"))}),
            std::string("[ \"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\", \"yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy\" ]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"a": [{"b": 1}]})__json"))}),
            std::string("{\n  a: [\n    {\n      b: 1,\n    }\n  ],\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"a": {}, "b": []})__json"))}),
            std::string("{\n  a: {},\n  b: [],\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"l1": {"l2": {"l3": {"l4": 1}}}})__json"))}),
            std::string("{\n  l1: {\n    l2: {\n      l3: [Object ...],\n    },\n  },\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"l1": {"l2": {"l3": [1]}}})__json"))}),
            std::string("{\n  l1: {\n    l2: {\n      l3: [ 1 ],\n    },\n  },\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([[[[1]]]])__json"))}),
            std::string("[\n  [\n    [\n      [ 1 ]\n    ]\n  ]\n]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"a": [[[[1]]]]})__json"))}),
            std::string("{\n  a: [\n    [\n      [\n        [ 1 ]\n      ]\n    ]\n  ],\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([{"a": {"b": {"c": 1}}}])__json"))}),
            std::string("[\n  {\n    a: {\n      b: [Object ...],\n    },\n  }\n]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"a": [[]], "b": [{}], "c": {}})__json"))}),
            std::string("{\n  a: [\n    []\n  ],\n  b: [\n    {}\n  ],\n  c: {},\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([])__json")), Value(json::parse(R"__json({})__json"))}),
            std::string("[] {}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"k": "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "k2": "y"})__json"))}),
            std::string("{\n  k: \"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\",\n  k2: \"y\",\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"__proto__x": 1, "a.b": 2, "_a": 3, "A1": 4, "1a": 5, "-1": 6, "01": 7, "4294967295": 8, "4294967294": 9})__json"))}),
            std::string("{\n  \"4294967294\": 9,\n  __proto__x: 1,\n  \"a.b\": 2,\n  _a: 3,\n  A1: 4,\n  \"1a\": 5,\n  \"-1\": 6,\n  \"01\": 7,\n  \"4294967295\": 8,\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"a-b": 1, "valid_id": 2, "1": 3, "with space": 4, "ünï": 5, "": 6, "$x": 7})__json"))}),
            std::string("{\n  \"1\": 3,\n  \"a-b\": 1,\n  valid_id: 2,\n  \"with space\": 4,\n  \"\303\274n\303\257\": 5,\n  \"\": 6,\n  $x: 7,\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"s": "quote \" and ' and \n newline \t tab \\ back ünï 😀"})__json")), Value(json::parse(R"__json(["x\ny"])__json"))}),
            std::string("{\n  s: \"quote \\\" and ' and \\n newline \\t tab \\\\ back \303\274n\303\257 \360\237\230\200\",\n} [\n  \"x\\ny\"\n]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"s": "a\rb\bc\fd\u000be\u0000f\u001bgh i"})__json"))}),
            std::string("{\n  s: \"a\\rb\\bc\\fd\\u000be\\u0000f\\u001bg\177h\342\200\250i\",\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json(["\t", "'", "\"", "`", "\\"])__json"))}),
            std::string("[ \"\\t\", \"'\", \"\\\"\", \"`\", \"\\\\\" ]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"1": 8, "2": 9, "10": 10, "valid$": 1, "_": 2, "ä": 3, "class": 4, "true": 5, "null": 6, "a1": 7})__json"))}),
            std::string("{\n  \"1\": 8,\n  \"2\": 9,\n  \"10\": 10,\n  valid$: 1,\n  _: 2,\n  \"\303\244\": 3,\n  class: 4,\n  true: 5,\n  null: 6,\n  a1: 7,\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value::object({{"u", Value()}, {"n", Value(nullptr)}, {"neg", Value(-0.0)}, {"nan", Value(NAN_)}, {"inf", Value(-INF_)}, {"big", Value(1e+300)}, {"tiny", Value(5e-324)}})}),
            std::string("{\n  u: undefined,\n  n: null,\n  neg: -0,\n  nan: NaN,\n  inf: -Infinity,\n  big: 1e+300,\n  tiny: 5e-324,\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value("100%s done %d %i %f %j %o %O %c %% end"sv), Value("A"sv), Value(4.5), Value(4.5), Value("4.5"sv), Value(json::parse(R"__json({"j": 1})__json")), Value(json::parse(R"__json({"o": 1})__json")), Value(json::parse(R"__json({"O": 1})__json")), Value("color:red"sv), Value("extra"sv)}),
            std::string("100A done 4 4 4.5 {\"j\":1} {\n  o: 1,\n} {\n  O: 1,\n}  % end extra"sv));
  EXPECT_EQ(console::format(Method::log, {Value("a %s b"sv)}),
            std::string("a %s b"sv));
  EXPECT_EQ(console::format(Method::log, {Value(5.0), Value("%s"sv), Value("x"sv)}),
            std::string("5 x"sv));
  EXPECT_EQ(console::format(Method::log, {Value("a"sv), Value("%s"sv), Value("x"sv)}),
            std::string("a x"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s"sv), Value("%s"sv), Value("y"sv)}),
            std::string("%s y"sv));
  EXPECT_EQ(console::format(Method::log, {Value("a %% b"sv)}),
            std::string("a %% b"sv));
  EXPECT_EQ(console::format(Method::log, {Value("100%% done"sv), Value(1.0)}),
            std::string("100% done 1"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d"sv), Value(true)}),
            std::string("1"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d"sv), Value(nullptr)}),
            std::string("0"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d"sv), Value("12px"sv)}),
            std::string("NaN"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d"sv), Value(" 7"sv)}),
            std::string("7"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d"sv), Value("0x10"sv)}),
            std::string("16"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d"sv), Value(json::parse(R"__json({})__json"))}),
            std::string("NaN"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d"sv), Value(1e+21)}),
            std::string("1"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d"sv), Value(-5.5)}),
            std::string("-5"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d"sv), Value(NAN_)}),
            std::string("NaN"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%i"sv), Value("3.9"sv)}),
            std::string("3"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%f"sv), Value("3.9e2x"sv)}),
            std::string("NaN"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%f"sv), Value(true)}),
            std::string("1"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%f"sv), Value(-0.0)}),
            std::string("0"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d|%d|%d|%d|%d|%d|%d|%d|%d"sv), Value(1e+20), Value(1.5e+21), Value(-1e+21), Value(9.2233720368547758e+18), Value(123.9), Value(-0.0), Value("1e3"sv), Value("abc"sv), Value(-123.9)}),
            std::string("9223372036854775807|1|-1|9223372036854775807|123|0|1000|NaN|-123"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d|%d|%i|%d|%d|%d"sv), Value(INF_), Value(-INF_), Value(INF_), Value(json::parse(R"__json([])__json")), Value(json::parse(R"__json([5])__json")), Value(json::parse(R"__json([1, 2])__json"))}),
            std::string("NaN|NaN|NaN|0|5|NaN"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%f|%f|%f|%f|%f|%f|%f|%f"sv), Value(1e+21), Value(-0.0), Value("1e3"sv), Value("abc"sv), Value(1.5), Value(" 2.5 "sv), Value("0x10"sv), Value(INF_)}),
            std::string("1e+21|0|1000|NaN|1.5|2.5|16|Infinity"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s"sv), err("Error", "boom", {}, std::nullopt)}),
            std::string("Error: boom"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s"sv), Value(true)}),
            std::string("true"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s"sv), Value(1.5)}),
            std::string("1.5"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s"sv), Value(1e+21)}),
            std::string("1e+21"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s"sv), Value()}),
            std::string("undefined"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s"sv), Value(json::parse(R"__json([1, [2, 3]])__json"))}),
            std::string("1,2,3"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s|%s|%s"sv), Value(json::parse(R"__json({"a": 1})__json")), Value(json::parse(R"__json({"a": {"b": {"c": 1}}})__json")), Value(json::parse(R"__json([1, {"x": 2}])__json"))}),
            std::string("[object Object]|[object Object]|1,[object Object]"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s|%s"sv), Value(nullptr), Value(-0.0)}),
            std::string("null|0"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%j"sv), Value(json::parse(R"__json({"a": [1, "x"]})__json"))}),
            std::string("{\"a\":[1,\"x\"]}"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%j"sv), Value("s"sv)}),
            std::string("\"s\""sv));
  EXPECT_EQ(console::format(Method::log, {Value("%j"sv), Value(nullptr)}),
            std::string("null"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%j"sv), Value(1.5)}),
            std::string("1.5"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%j|%j|%j"sv), Value(json::parse(R"__json({"a": {"b": 1}})__json")), Value(json::parse(R"__json([1, "a"])__json")), Value()}),
            std::string("{\"a\":{\"b\":1}}|[1,\"a\"]|"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%o"sv), Value(json::parse(R"__json({"a": 1})__json"))}),
            std::string("{\n  a: 1,\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%O"sv), Value(json::parse(R"__json([1])__json"))}),
            std::string("[ 1 ]"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%o"sv), Value(1.0)}),
            std::string("1"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%o"sv), Value(nullptr)}),
            std::string("null"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%o"sv), Value(json::parse(R"__json({"a": {"b": {"c": {"d": 1}}}})__json"))}),
            std::string("{\n  a: {\n    b: {\n      c: [Object ...],\n    },\n  },\n}"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%c%s"sv), Value("css"sv), Value("after"sv)}),
            std::string("after"sv));
  EXPECT_EQ(console::format(Method::log, {Value("x %s %s"sv), Value("only"sv)}),
            std::string("x only %s"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s:%s"sv), Value("a"sv), Value("b"sv), Value("c"sv)}),
            std::string("a:b c"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%5d"sv), Value(1.0)}),
            std::string("%5d 1"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%-s"sv), Value(1.0)}),
            std::string("%-s 1"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%.2f"sv), Value(1.234)}),
            std::string("%.2f 1.234"sv));
  EXPECT_EQ(console::format(Method::log, {Value("tail %"sv)}),
            std::string("tail %"sv));
  EXPECT_EQ(console::format(Method::log, {Value("tail %"sv), Value(1.0)}),
            std::string("tail % 1"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%%"sv)}),
            std::string("%%"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%%"sv), Value(1.0)}),
            std::string("% 1"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%%s"sv), Value(1.0)}),
            std::string("%s 1"sv));
  EXPECT_EQ(console::format(Method::log, {Value("\303\251%s"sv), Value("x"sv)}),
            std::string("\303\251x"sv));
  EXPECT_EQ(console::format(Method::log, {Value(1.0), Value("%d"sv), Value("2"sv)}),
            std::string("1 2"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"a": 1})__json")), Value("%s"sv), Value("q"sv)}),
            std::string("{\n  a: 1,\n} q"sv));
  EXPECT_EQ(console::format(Method::log, {Value("a %s"sv), Value("b %s"sv), Value("c"sv)}),
            std::string("a b %s c"sv));
  EXPECT_EQ(console::format(Method::log, {err("Error", "first", {}, std::nullopt), Value("%s"sv), Value("x"sv)}),
            std::string("error: first\n x"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "boom", {}, std::nullopt), Value("after"sv)}),
            std::string("x error: boom\n after"sv));
  EXPECT_EQ(console::format(Method::error, {err("Error", "solo", {}, std::nullopt)}),
            std::string("error: solo\n"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "", {}, std::nullopt), Value("after"sv)}),
            std::string("x Error: \n after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("TypeError", "bad", {}, std::nullopt), Value("after"sv)}),
            std::string("x TypeError: bad\n after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("CustomName", "m", {}, std::nullopt), Value("after"sv)}),
            std::string("x CustomName: m\n after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("[ai.ubq.fi] Failed:"sv), err("CodexError", "msg here", {{"code", Value("codex_auth_invalid"sv)}, {"status", Value(503.0)}}, std::nullopt)}),
            std::string("[ai.ubq.fi] Failed: CodexError: msg here\n status: 503,\n   code: \"codex_auth_invalid\"\n\n"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "p", {{"other", Value(1.0)}, {"code", Value("E1"sv)}, {"path", Value("/p"sv)}, {"syscall", Value("open"sv)}, {"errno", Value(-2.0)}, {"zed", Value("z"sv)}}, std::nullopt), Value("after"sv)}),
            std::string("x error: p\n   other: 1,\n    path: \"/p\",\n syscall: \"open\",\n   errno: -2,\n     zed: \"z\",\n    code: \"E1\"\n\n after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), Value::object({{"error", err("Error", "boom", {}, std::nullopt)}, {"next", Value(1.0)}})}),
            std::string("x {\n  error: error: boom\n,\n  next: 1,\n}"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), Value::array({err("Error", "boom", {}, std::nullopt), Value(2.0)})}),
            std::string("x [\n  error: boom\n, 2\n]"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "outer", {}, err("Error", "inner", {}, std::nullopt)), Value("after"sv)}),
            std::string("x error: outer\n\nerror: inner\n after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "outer", {}, err("Error", "inner", {{"code", Value("C1"sv)}}, std::nullopt)), Value("after"sv)}),
            std::string("x error: outer\n\nerror: inner\n code: \"C1\"\n\n after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "o", {}, err("Error", "m", {}, err("Error", "i", {}, std::nullopt))), Value("after"sv)}),
            std::string("x error: o\n\nerror: m\n\nerror: i\n after"sv));
  EXPECT_EQ(console::format(Method::error, {err("Error", "o", {}, err("Error", "i", {}, std::nullopt))}),
            std::string("error: o\n\nerror: i\n"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "p", {{"code", Value("E"sv)}}, std::nullopt)}),
            std::string("x error: p\n code: \"E\"\n\n"sv));
  EXPECT_EQ(console::format(Method::error, {err("Error", "a", {}, std::nullopt), err("Error", "b", {}, std::nullopt)}),
            std::string("error: a\n error: b\n"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "line1\nline2", {}, std::nullopt), Value("after"sv)}),
            std::string("x error: line1\nline2\n after"sv));
  EXPECT_EQ(console::format(Method::warn, {Value("[w]"sv), err("Error", "warned", {}, std::nullopt)}),
            std::string("[w] warn: warned\n"sv));
  EXPECT_EQ(console::format(Method::log, {Value("[l]"sv), err("Error", "logged", {}, std::nullopt)}),
            std::string("[l] error: logged\n"sv));
  EXPECT_EQ(console::format(Method::info, {Value("[i]"sv), err("Error", "informed", {}, std::nullopt)}),
            std::string("[i] error: informed\n"sv));
  EXPECT_EQ(console::format(Method::debug, {Value("[d]"sv), err("Error", "debugged", {}, std::nullopt)}),
            std::string("[d] error: debugged\n"sv));
  EXPECT_EQ(console::format(Method::warn, {Value("[w]"sv), err("TypeError", "tw", {}, std::nullopt)}),
            std::string("[w] TypeError: tw\n"sv));
  EXPECT_EQ(console::format(Method::warn, {Value("[w]"sv), err("Error", "", {}, std::nullopt)}),
            std::string("[w] Error: \n"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "ENOENT: x", {{"code", Value("ENOENT"sv)}}, std::nullopt), Value("after"sv)}),
            std::string("x error: ENOENT: x\n code: \"ENOENT\"\n\n after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "z", {{"syscall", Value("open"sv)}}, std::nullopt), Value("after"sv)}),
            std::string("x error: z\n syscall: \"open\",\n\n after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "t", {{"s", Value("str"sv)}, {"n", Value(1.5)}, {"b", Value(true)}, {"nul", Value(nullptr)}, {"u", Value()}, {"arr", Value(json::parse(R"__json([1, "a"])__json"))}, {"obj", Value(json::parse(R"__json({"k": 1})__json"))}}, std::nullopt), Value("after"sv)}),
            std::string("x error: t\n   s: \"str\",\n   n: 1.5,\n   b: true,\n nul: null,\n   u: undefined,\n arr: [ 1, \"a\" ],\n obj: {\n  k: 1,\n},\n\n after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), Value::object({{"error", err("Error", "boom", {{"code", Value("C"sv)}, {"status", Value(5.0)}}, std::nullopt)}})}),
            std::string("x {\n  error: error: boom\n status: 5,\n   code: \"C\"\n\n,\n}"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), Value::array({err("Error", "a", {}, std::nullopt), err("TypeError", "b", {}, std::nullopt)})}),
            std::string("x [\n  error: a\n, TypeError: b\n\n]"sv));
  EXPECT_EQ(console::format(Method::log, {Value("label"sv), Value::object({{"error", err("Error", "x", {}, std::nullopt)}}), Value("after"sv)}),
            std::string("label {\n  error: error: x\n,\n} after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("[ai.ubq.fi] Failed to release local API key deletion claim after a concurrent update:"sv), Value::object({{"error", err("Error", "kv down", {}, std::nullopt)}})}),
            std::string("[ai.ubq.fi] Failed to release local API key deletion claim after a concurrent update: {\n  error: error: kv down\n,\n}"sv));
  EXPECT_EQ(console::format(Method::warn, {Value("x"sv), fsError("/nonexistent-dir/x"), Value("after"sv)}),
            std::string("x ENOENT: no such file or directory, open '/nonexistent-dir/x'\n    path: \"/nonexistent-dir/x\",\n syscall: \"open\",\n   errno: -2,\n    code: \"ENOENT\"\n\n after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), Value::object({{"error", fsError("/nonexistent-dir/x")}})}),
            std::string("x {\n  error: ENOENT: no such file or directory, open '/nonexistent-dir/x'\n    path: \"/nonexistent-dir/x\",\n syscall: \"open\",\n   errno: -2,\n    code: \"ENOENT\"\n\n,\n}"sv));
}

TEST("errors from C++ exceptions: JsError, cause chains, other exceptions, fs errors") {
  EXPECT_EQ(console::format(Method::error, {Value("x"), Value(rt::JsError("boom")), Value("after")}), "x error: boom\n after");
  EXPECT_EQ(console::format(Method::warn, {Value("[w]"), Value(rt::JsError("warned"))}), "[w] warn: warned\n");
  EXPECT_EQ(console::format(Method::error, {Value(rt::TypeError("bad type"))}), "TypeError: bad type\n");
  EXPECT_EQ(console::format(Method::error, {Value(rt::JsError("", "Error"))}), "Error: \n");
  const auto inner = std::make_shared<rt::JsError>("inner");
  const auto middle = std::make_shared<rt::JsError>("m", "Error", inner);
  EXPECT_EQ(console::format(Method::error, {Value("x"), Value(rt::JsError("o", "Error", middle)), Value("after")}), "x error: o\n\nerror: m\n\nerror: inner\n after");
  EXPECT_EQ(console::format(Method::error, {Value("x"), Value(std::runtime_error("plain"))}), "x error: plain\n");
  EXPECT_EQ(console::format(Method::error, {Value("x"), Value::error(rt::JsError("msg here", "CodexError"), {{"code", Value("codex_auth_invalid")}, {"status", Value(503)}})}),
            "x CodexError: msg here\n status: 503,\n   code: \"codex_auth_invalid\"\n\n");
  EXPECT_EQ(console::format(Method::error, {Value("x"), Value(std::make_exception_ptr(rt::RangeError("r")))}), "x RangeError: r\n");
  EXPECT_EQ(console::format(Method::error, {Value("x"), Value(std::exception_ptr())}), "x undefined");
  EXPECT_EQ(console::format(Method::log, {Value(std::optional<std::string>()), Value(std::optional<std::string>("s"))}), "undefined s");
  EXPECT_EQ(console::format(Method::warn, {Value("x"), Value(rt::fs::errnoError(2, "open", "/nonexistent-dir/x")), Value("after")}),
            "x ENOENT: no such file or directory, open '/nonexistent-dir/x'\n    path: \"/nonexistent-dir/x\",\n syscall: \"open\",\n   errno: -2,\n    code: \"ENOENT\"\n\n after");
  EXPECT_EQ(console::format(Method::log, {Value("%s"), Value(rt::TypeError("tt"))}), "TypeError: tt");
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"({"a":[1,{"b":null}]})"))}), "{\n  a: [\n    1, {\n      b: null,\n    }\n  ],\n}");
}

TEST("Capture records arguments like a replaced console method") {
  console::Capture capture({Method::info});
  console::info("[ai.ubq.fi] request_terminal", json::serialize(json::value({{"status", 200}})));
  console::info(1, true);
  ASSERT_EQ(capture.calls().size(), std::size_t(2));
  const auto& first = capture.calls()[0];
  EXPECT(first.method == Method::info);
  EXPECT(first.args[0] == "[ai.ubq.fi] request_terminal");
  EXPECT_EQ(json::parse(first.args[1].string()), json::parse(R"({"status":200})"));
  EXPECT_EQ(first.text, "[ai.ubq.fi] request_terminal {\"status\":200}");
  EXPECT_EQ(capture.calls()[1].args[0].number(), 1.0);
  EXPECT_EQ(capture.calls()[1].text, "1 true");
  EXPECT_EQ(capture.calls(Method::warn).size(), std::size_t(0));
  capture.clear();
  EXPECT(capture.calls().empty());
  // warn is not captured here: it reaches stderr.
  EXPECT_EQ(written(2, [] { console::warn("to stderr"); }), "to stderr\n");
}

TEST("routing: log/info/debug to stdout, warn/error to stderr; nested and passthrough captures") {
  EXPECT_EQ(written(1, [] { console::log("L"); }), "L\n");
  EXPECT_EQ(written(1, [] { console::info("I"); }), "I\n");
  EXPECT_EQ(written(1, [] { console::debug("D"); }), "D\n");
  EXPECT_EQ(written(2, [] { console::warn("W"); }), "W\n");
  EXPECT_EQ(written(2, [] { console::error("E"); }), "E\n");
  EXPECT_EQ(written(1, [] { console::log(); }), "\n");
  EXPECT_EQ(written(2, [] { console::error("x", rt::JsError("boom"), "after"); }), "x error: boom\n after\n");
  console::Capture outer({Method::error});
  {
    console::Capture inner({Method::error}, true);
    EXPECT_EQ(written(2, [] { console::error("both"); }), "");
    EXPECT_EQ(inner.calls().size(), std::size_t(1));
  }
  EXPECT_EQ(outer.calls().size(), std::size_t(1));
  EXPECT_EQ(outer.calls()[0].text, "both");
  {
    console::Capture swallow({Method::error, Method::warn});
    EXPECT_EQ(written(2, [] { console::warn("hidden"); }), "");
  }
  console::Capture through({Method::log}, true);
  EXPECT_EQ(written(1, [] { console::log("seen"); }), "seen\n");
  EXPECT_EQ(through.calls().size(), std::size_t(1));
}


TEST("Capture observer runs synchronously after storage and keeps mutable callback state") {
  console::Capture capture({Method::info});
  std::vector<std::string> order{"before"};
  capture.setObserver([count = 0, &capture, &order](const console::Call& call) mutable {
    ++count;
    EXPECT_EQ(capture.calls().back().text, call.text);
    EXPECT(call.method == Method::info);
    order.push_back(std::to_string(count) + ":" + call.text);
  });
  console::info("first");
  order.push_back("between");
  console::info("second");
  order.push_back("after");
  EXPECT_EQ(order, std::vector<std::string>({"before", "1:first", "between", "2:second", "after"}));
  EXPECT_EQ(capture.calls().size(), std::size_t(2));
  capture.setObserver({});
  console::info("unobserved");
  EXPECT_EQ(order.size(), std::size_t(5));
  EXPECT_EQ(capture.calls().size(), std::size_t(3));
}

TEST("Capture observer has a stable call during clear and nested console writes") {
  console::Capture capture({Method::info});
  std::vector<std::string> observed;
  capture.setObserver([&](const console::Call& call) {
    observed.push_back(call.text);
    if (call.text == "outer") {
      capture.clear();
      for (int index = 0; index < 32; ++index) console::info("nested", index);
      EXPECT_EQ(call.text, "outer");
      EXPECT(call.args[0] == "outer");
    }
  });
  console::info("outer");
  ASSERT_EQ(observed.size(), std::size_t(33));
  EXPECT_EQ(observed.front(), "outer");
  EXPECT_EQ(observed.back(), "nested 31");
  EXPECT_EQ(capture.calls().size(), std::size_t(32));
}

TEST("Capture observer may clear itself and releases callback state at scope end") {
  auto token = std::make_shared<int>(7);
  std::weak_ptr<int> weak = token;
  bool observed = false;
  {
    console::Capture capture({Method::info});
    capture.setObserver([held = token, &capture, &weak, &observed](const console::Call&) {
      capture.setObserver({});
      EXPECT(!weak.expired());
      EXPECT_EQ(*held, 7);
      observed = true;
    });
    token.reset();
    console::info("once");
    EXPECT(observed);
    EXPECT(weak.expired());
    auto second = std::make_shared<int>(9);
    weak = second;
    capture.setObserver([held = second](const console::Call&) { EXPECT_EQ(*held, 9); });
    second.reset();
    EXPECT(!weak.expired());
  }
  EXPECT(weak.expired());
  console::Capture next({Method::info});
  console::info("new scope");
  EXPECT_EQ(next.calls().size(), std::size_t(1));
}

TEST("Capture observer keeps nested passthrough order and propagates callback errors") {
  std::vector<std::string> order;
  console::Capture outer({Method::info});
  outer.setObserver([&](const console::Call&) { order.push_back("outer"); });
  {
    console::Capture inner({Method::info}, true);
    inner.setObserver([&](const console::Call&) { order.push_back("inner"); });
    EXPECT_EQ(written(1, [] { console::info("both"); }), "");
    EXPECT_EQ(order, std::vector<std::string>({"inner", "outer"}));
    EXPECT_EQ(inner.calls().size(), std::size_t(1));
  }
  outer.setObserver([](const console::Call&) { throw rt::JsError("observer failure"); });
  EXPECT_EQ(written(1, [] { EXPECT_THROWS_MESSAGE(console::info("recorded"), rt::JsError, "observer failure"); }), "");
  EXPECT_EQ(outer.calls().size(), std::size_t(2));
  EXPECT_EQ(outer.calls().back().text, "recorded");
}

TEST("Capture observer API preserves the pre-observer object size and alignment") {
  struct PreviousLayout { std::vector<Method> methods; bool passthrough; std::vector<console::Call> calls; };
  EXPECT_EQ(sizeof(console::Capture), sizeof(PreviousLayout));
  EXPECT_EQ(alignof(console::Capture), alignof(PreviousLayout));
}
