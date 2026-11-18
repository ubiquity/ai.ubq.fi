// Console formatting, routing and capture. The Bun 1.4 input vectors remain.
// Changed generic expectations are documented in docs/DEVIATIONS-FROM-BUN.md.
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

TEST("console formatting for every recorded call shape") {
  EXPECT_EQ(console::format(Method::log, {Value("plain"sv)}),
            std::string("plain"sv));
  EXPECT_EQ(console::format(Method::log, {Value("a\nb"sv), Value("c"sv)}),
            std::string("a\nb c"sv));
  EXPECT_EQ(console::format(Method::log, {}),
            std::string(""sv));
  EXPECT_EQ(console::format(Method::log, {Value(1.0), Value(-0.0), Value(0.1), Value(1e+21), Value(NAN_), Value(INF_), Value(9007199254740992), Value(-1.5e-07), Value(1.2345678901234568e+20)}),
            std::string("1 -0 0.1 1e+21 nan inf 9007199254740992 -1.5e-07 123456789012345683968"sv));
  EXPECT_EQ(console::format(Method::log, {Value(true), Value(nullptr), Value(), Value("s"sv), Value(json::parse(R"__json(["a", 1, null])__json")), Value(json::parse(R"__json({})__json")), Value(json::parse(R"__json([])__json")), Value(json::parse(R"__json({"nested": {"deep": {"x": [1, {"y": 2}]}}})__json"))}),
            std::string("true null undefined s [\"a\",1,null] {} [] {\"nested\":{\"deep\":{\"x\":[1,{\"y\":2}]}}}"sv));
  EXPECT_EQ(console::format(Method::info, {Value("[ai.ubq.fi] request_terminal"sv), Value("{\"a\":1,\"b\":\"x\"}"sv)}),
            std::string("[ai.ubq.fi] request_terminal {\"a\":1,\"b\":\"x\"}"sv));
  EXPECT_EQ(console::format(Method::error, {Value("[ai.ubq.fi] label"sv), Value(json::parse(R"__json({"error_class": "Error", "status": null, "code": null})__json"))}),
            std::string("[ai.ubq.fi] label {\"error_class\":\"Error\",\"status\":null,\"code\":null}"sv));
  EXPECT_EQ(console::format(Method::warn, {Value("[ai.ubq.fi] passkey assertion verification failed"sv), Value(json::parse(R"__json({"message": "bad sig", "expected_origin": "https://ai.ubq.fi", "expected_rp_id": "ai.ubq.fi", "credential_id_prefix": "abcdefghijkl", "has_user_handle": true, "require_user_verification": false})__json"))}),
            std::string("[ai.ubq.fi] passkey assertion verification failed {\"message\":\"bad sig\",\"expected_origin\":\"https://ai.ubq.fi\",\"expected_rp_id\":\"ai.ubq.fi\",\"credential_id_prefix\":\"abcdefghijkl\",\"has_user_handle\":true,\"require_user_verification\":false}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([1, 2, 3])__json"))}),
            std::string("[1,2,3]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([1, 2, 3, 4, 5, 6, 7, 8, 9, 10])__json"))}),
            std::string("[1,2,3,4,5,6,7,8,9,10]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10])__json"))}),
            std::string("[0,1,2,3,4,5,6,7,8,9,10]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26])__json"))}),
            std::string("[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"k": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25]})__json"))}),
            std::string("{\"k\":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25]}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100])__json"))}),
            std::string("[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,\"[truncated]\"]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([0, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000, 11000, 12000, 13000, 14000, 15000, 16000, 17000, 18000, 19000, 20000, 21000, 22000, 23000, 24000, 25000, 26000, 27000, 28000, 29000])__json"))}),
            std::string("[0,1000,2000,3000,4000,5000,6000,7000,8000,9000,10000,11000,12000,13000,14000,15000,16000,17000,18000,19000,20000,21000,22000,23000,24000,25000,26000,27000,28000,29000]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json(["s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10"])__json"))}),
            std::string("[\"s0\",\"s1\",\"s2\",\"s3\",\"s4\",\"s5\",\"s6\",\"s7\",\"s8\",\"s9\",\"s10\"]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([1, "a", true, null, 2, "b", false, 3, "c", 4, 5, 6])__json"))}),
            std::string("[1,\"a\",true,null,2,\"b\",false,3,\"c\",4,5,6]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([{"a": 1}])__json"))}),
            std::string("[{\"a\":1}]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([{"a": 1}, {"b": 2}])__json"))}),
            std::string("[{\"a\":1},{\"b\":2}]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([[1, 2], [3]])__json"))}),
            std::string("[[1,2],[3]]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json(["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"])__json"))}),
            std::string("[\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json(["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"])__json"))}),
            std::string("[\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\",\"yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy\"]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"a": [{"b": 1}]})__json"))}),
            std::string("{\"a\":[{\"b\":1}]}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"a": {}, "b": []})__json"))}),
            std::string("{\"a\":{},\"b\":[]}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"l1": {"l2": {"l3": {"l4": 1}}}})__json"))}),
            std::string("{\"l1\":{\"l2\":{\"l3\":{\"l4\":1}}}}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"l1": {"l2": {"l3": [1]}}})__json"))}),
            std::string("{\"l1\":{\"l2\":{\"l3\":[1]}}}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([[[[1]]]])__json"))}),
            std::string("[[[[1]]]]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"a": [[[[1]]]]})__json"))}),
            std::string("{\"a\":[[[[1]]]]}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([{"a": {"b": {"c": 1}}}])__json"))}),
            std::string("[{\"a\":{\"b\":{\"c\":1}}}]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"a": [[]], "b": [{}], "c": {}})__json"))}),
            std::string("{\"a\":[[]],\"b\":[{}],\"c\":{}}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json([])__json")), Value(json::parse(R"__json({})__json"))}),
            std::string("[] {}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"k": "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "k2": "y"})__json"))}),
            std::string("{\"k\":\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\",\"k2\":\"y\"}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"__proto__x": 1, "a.b": 2, "_a": 3, "A1": 4, "1a": 5, "-1": 6, "01": 7, "4294967295": 8, "4294967294": 9})__json"))}),
            std::string("{\"__proto__x\":1,\"a.b\":2,\"_a\":3,\"A1\":4,\"1a\":5,\"-1\":6,\"01\":7,\"4294967295\":8,\"4294967294\":9}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"a-b": 1, "valid_id": 2, "1": 3, "with space": 4, "ünï": 5, "": 6, "$x": 7})__json"))}),
            std::string("{\"a-b\":1,\"valid_id\":2,\"1\":3,\"with space\":4,\"ünï\":5,\"\":6,\"$x\":7}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"s": "quote \" and ' and \n newline \t tab \\ back ünï 😀"})__json")), Value(json::parse(R"__json(["x\ny"])__json"))}),
            std::string("{\"s\":\"quote \\\" and ' and \\n newline \\t tab \\\\ back ünï 😀\"} [\"x\\ny\"]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"s": "a\rb\bc\fd\u000be\u0000f\u001bgh i"})__json"))}),
            std::string("{\"s\":\"a\\rb\\bc\\fd\\u000be\\u0000f\\u001bg\177h i\"}"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json(["\t", "'", "\"", "`", "\\"])__json"))}),
            std::string("[\"\\t\",\"'\",\"\\\"\",\"`\",\"\\\\\"]"sv));
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"__json({"1": 8, "2": 9, "10": 10, "valid$": 1, "_": 2, "ä": 3, "class": 4, "true": 5, "null": 6, "a1": 7})__json"))}),
            std::string("{\"1\":8,\"2\":9,\"10\":10,\"valid$\":1,\"_\":2,\"ä\":3,\"class\":4,\"true\":5,\"null\":6,\"a1\":7}"sv));
  EXPECT_EQ(console::format(Method::log, {Value::object({{"u", Value()}, {"n", Value(nullptr)}, {"neg", Value(-0.0)}, {"nan", Value(NAN_)}, {"inf", Value(-INF_)}, {"big", Value(1e+300)}, {"tiny", Value(5e-324)}})}),
            std::string("{\"n\":null,\"neg\":0,\"nan\":null,\"inf\":null,\"big\":1E300,\"tiny\":5E-324}"sv));
  EXPECT_EQ(console::format(Method::log, {Value("100%s done %d %i %f %j %o %O %c %% end"sv), Value("A"sv), Value(4.5), Value(4.5), Value("4.5"sv), Value(json::parse(R"__json({"j": 1})__json")), Value(json::parse(R"__json({"o": 1})__json")), Value(json::parse(R"__json({"O": 1})__json")), Value("color:red"sv), Value("extra"sv)}),
            std::string("100A done 4 4 4.5 {\"j\":1} {\"o\":1} {\"O\":1}  % end extra"sv));
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
            std::string("nan"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d"sv), Value(" 7"sv)}),
            std::string("nan"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d"sv), Value("0x10"sv)}),
            std::string("nan"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d"sv), Value(json::parse(R"__json({})__json"))}),
            std::string("nan"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d"sv), Value(1e+21)}),
            std::string("1e+21"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d"sv), Value(-5.5)}),
            std::string("-5"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d"sv), Value(NAN_)}),
            std::string("nan"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%i"sv), Value("3.9"sv)}),
            std::string("3"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%f"sv), Value("3.9e2x"sv)}),
            std::string("nan"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%f"sv), Value(true)}),
            std::string("1"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%f"sv), Value(-0.0)}),
            std::string("-0"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d|%d|%d|%d|%d|%d|%d|%d|%d"sv), Value(1e+20), Value(1.5e+21), Value(-1e+21), Value(9.2233720368547758e+18), Value(123.9), Value(-0.0), Value("1e3"sv), Value("abc"sv), Value(-123.9)}),
            std::string("1e+20|1.5e+21|-1e+21|9223372036854775808|123|-0|1000|nan|-123"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%d|%d|%i|%d|%d|%d"sv), Value(INF_), Value(-INF_), Value(INF_), Value(json::parse(R"__json([])__json")), Value(json::parse(R"__json([5])__json")), Value(json::parse(R"__json([1, 2])__json"))}),
            std::string("inf|-inf|inf|nan|nan|nan"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%f|%f|%f|%f|%f|%f|%f|%f"sv), Value(1e+21), Value(-0.0), Value("1e3"sv), Value("abc"sv), Value(1.5), Value(" 2.5 "sv), Value("0x10"sv), Value(INF_)}),
            std::string("1e+21|-0|1000|nan|1.5|nan|nan|inf"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s"sv), err("Error", "boom", {}, std::nullopt)}),
            std::string("{\"name\":\"Error\",\"message\":\"boom\"}"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s"sv), Value(true)}),
            std::string("true"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s"sv), Value(1.5)}),
            std::string("1.5"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s"sv), Value(1e+21)}),
            std::string("1e+21"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s"sv), Value()}),
            std::string("undefined"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s"sv), Value(json::parse(R"__json([1, [2, 3]])__json"))}),
            std::string("[1,[2,3]]"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s|%s|%s"sv), Value(json::parse(R"__json({"a": 1})__json")), Value(json::parse(R"__json({"a": {"b": {"c": 1}}})__json")), Value(json::parse(R"__json([1, {"x": 2}])__json"))}),
            std::string("{\"a\":1}|{\"a\":{\"b\":{\"c\":1}}}|[1,{\"x\":2}]"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%s|%s"sv), Value(nullptr), Value(-0.0)}),
            std::string("null|-0"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%j"sv), Value(json::parse(R"__json({"a": [1, "x"]})__json"))}),
            std::string("{\"a\":[1,\"x\"]}"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%j"sv), Value("s"sv)}),
            std::string("\"s\""sv));
  EXPECT_EQ(console::format(Method::log, {Value("%j"sv), Value(nullptr)}),
            std::string("null"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%j"sv), Value(1.5)}),
            std::string("1.5E0"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%j|%j|%j"sv), Value(json::parse(R"__json({"a": {"b": 1}})__json")), Value(json::parse(R"__json([1, "a"])__json")), Value()}),
            std::string("{\"a\":{\"b\":1}}|[1,\"a\"]|"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%o"sv), Value(json::parse(R"__json({"a": 1})__json"))}),
            std::string("{\"a\":1}"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%O"sv), Value(json::parse(R"__json([1])__json"))}),
            std::string("[1]"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%o"sv), Value(1.0)}),
            std::string("1"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%o"sv), Value(nullptr)}),
            std::string("null"sv));
  EXPECT_EQ(console::format(Method::log, {Value("%o"sv), Value(json::parse(R"__json({"a": {"b": {"c": {"d": 1}}}})__json"))}),
            std::string("{\"a\":{\"b\":{\"c\":{\"d\":1}}}}"sv));
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
            std::string("{\"a\":1} q"sv));
  EXPECT_EQ(console::format(Method::log, {Value("a %s"sv), Value("b %s"sv), Value("c"sv)}),
            std::string("a b %s c"sv));
  EXPECT_EQ(console::format(Method::log, {err("Error", "first", {}, std::nullopt), Value("%s"sv), Value("x"sv)}),
            std::string("{\"name\":\"Error\",\"message\":\"first\"} x"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "boom", {}, std::nullopt), Value("after"sv)}),
            std::string("x {\"name\":\"Error\",\"message\":\"boom\"} after"sv));
  EXPECT_EQ(console::format(Method::error, {err("Error", "solo", {}, std::nullopt)}),
            std::string("{\"name\":\"Error\",\"message\":\"solo\"}"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "", {}, std::nullopt), Value("after"sv)}),
            std::string("x {\"name\":\"Error\",\"message\":\"\"} after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("TypeError", "bad", {}, std::nullopt), Value("after"sv)}),
            std::string("x {\"name\":\"TypeError\",\"message\":\"bad\"} after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("CustomName", "m", {}, std::nullopt), Value("after"sv)}),
            std::string("x {\"name\":\"CustomName\",\"message\":\"m\"} after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("[ai.ubq.fi] Failed:"sv), err("CodexError", "msg here", {{"code", Value("codex_auth_invalid"sv)}, {"status", Value(503.0)}}, std::nullopt)}),
            std::string("[ai.ubq.fi] Failed: {\"name\":\"CodexError\",\"message\":\"msg here\",\"properties\":{\"code\":\"codex_auth_invalid\",\"status\":503}}"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "p", {{"other", Value(1.0)}, {"code", Value("E1"sv)}, {"path", Value("/p"sv)}, {"syscall", Value("open"sv)}, {"errno", Value(-2.0)}, {"zed", Value("z"sv)}}, std::nullopt), Value("after"sv)}),
            std::string("x {\"name\":\"Error\",\"message\":\"p\",\"properties\":{\"other\":1,\"code\":\"E1\",\"path\":\"/p\",\"syscall\":\"open\",\"errno\":-2,\"zed\":\"z\"}} after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), Value::object({{"error", err("Error", "boom", {}, std::nullopt)}, {"next", Value(1.0)}})}),
            std::string("x {\"error\":{\"name\":\"Error\",\"message\":\"boom\"},\"next\":1}"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), Value::array({err("Error", "boom", {}, std::nullopt), Value(2.0)})}),
            std::string("x [{\"name\":\"Error\",\"message\":\"boom\"},2]"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "outer", {}, err("Error", "inner", {}, std::nullopt)), Value("after"sv)}),
            std::string("x {\"name\":\"Error\",\"message\":\"outer\",\"cause\":{\"name\":\"Error\",\"message\":\"inner\"}} after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "outer", {}, err("Error", "inner", {{"code", Value("C1"sv)}}, std::nullopt)), Value("after"sv)}),
            std::string("x {\"name\":\"Error\",\"message\":\"outer\",\"cause\":{\"name\":\"Error\",\"message\":\"inner\",\"properties\":{\"code\":\"C1\"}}} after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "o", {}, err("Error", "m", {}, err("Error", "i", {}, std::nullopt))), Value("after"sv)}),
            std::string("x {\"name\":\"Error\",\"message\":\"o\",\"cause\":{\"name\":\"Error\",\"message\":\"m\",\"cause\":{\"name\":\"Error\",\"message\":\"i\"}}} after"sv));
  EXPECT_EQ(console::format(Method::error, {err("Error", "o", {}, err("Error", "i", {}, std::nullopt))}),
            std::string("{\"name\":\"Error\",\"message\":\"o\",\"cause\":{\"name\":\"Error\",\"message\":\"i\"}}"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "p", {{"code", Value("E"sv)}}, std::nullopt)}),
            std::string("x {\"name\":\"Error\",\"message\":\"p\",\"properties\":{\"code\":\"E\"}}"sv));
  EXPECT_EQ(console::format(Method::error, {err("Error", "a", {}, std::nullopt), err("Error", "b", {}, std::nullopt)}),
            std::string("{\"name\":\"Error\",\"message\":\"a\"} {\"name\":\"Error\",\"message\":\"b\"}"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "line1\nline2", {}, std::nullopt), Value("after"sv)}),
            std::string("x {\"name\":\"Error\",\"message\":\"line1\\nline2\"} after"sv));
  EXPECT_EQ(console::format(Method::warn, {Value("[w]"sv), err("Error", "warned", {}, std::nullopt)}),
            std::string("[w] {\"name\":\"Error\",\"message\":\"warned\"}"sv));
  EXPECT_EQ(console::format(Method::log, {Value("[l]"sv), err("Error", "logged", {}, std::nullopt)}),
            std::string("[l] {\"name\":\"Error\",\"message\":\"logged\"}"sv));
  EXPECT_EQ(console::format(Method::info, {Value("[i]"sv), err("Error", "informed", {}, std::nullopt)}),
            std::string("[i] {\"name\":\"Error\",\"message\":\"informed\"}"sv));
  EXPECT_EQ(console::format(Method::debug, {Value("[d]"sv), err("Error", "debugged", {}, std::nullopt)}),
            std::string("[d] {\"name\":\"Error\",\"message\":\"debugged\"}"sv));
  EXPECT_EQ(console::format(Method::warn, {Value("[w]"sv), err("TypeError", "tw", {}, std::nullopt)}),
            std::string("[w] {\"name\":\"TypeError\",\"message\":\"tw\"}"sv));
  EXPECT_EQ(console::format(Method::warn, {Value("[w]"sv), err("Error", "", {}, std::nullopt)}),
            std::string("[w] {\"name\":\"Error\",\"message\":\"\"}"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "ENOENT: x", {{"code", Value("ENOENT"sv)}}, std::nullopt), Value("after"sv)}),
            std::string("x {\"name\":\"Error\",\"message\":\"ENOENT: x\",\"properties\":{\"code\":\"ENOENT\"}} after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "z", {{"syscall", Value("open"sv)}}, std::nullopt), Value("after"sv)}),
            std::string("x {\"name\":\"Error\",\"message\":\"z\",\"properties\":{\"syscall\":\"open\"}} after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), err("Error", "t", {{"s", Value("str"sv)}, {"n", Value(1.5)}, {"b", Value(true)}, {"nul", Value(nullptr)}, {"u", Value()}, {"arr", Value(json::parse(R"__json([1, "a"])__json"))}, {"obj", Value(json::parse(R"__json({"k": 1})__json"))}}, std::nullopt), Value("after"sv)}),
            std::string("x {\"name\":\"Error\",\"message\":\"t\",\"properties\":{\"s\":\"str\",\"n\":1.5E0,\"b\":true,\"nul\":null,\"arr\":[1,\"a\"],\"obj\":{\"k\":1}}} after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), Value::object({{"error", err("Error", "boom", {{"code", Value("C"sv)}, {"status", Value(5.0)}}, std::nullopt)}})}),
            std::string("x {\"error\":{\"name\":\"Error\",\"message\":\"boom\",\"properties\":{\"code\":\"C\",\"status\":5}}}"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), Value::array({err("Error", "a", {}, std::nullopt), err("TypeError", "b", {}, std::nullopt)})}),
            std::string("x [{\"name\":\"Error\",\"message\":\"a\"},{\"name\":\"TypeError\",\"message\":\"b\"}]"sv));
  EXPECT_EQ(console::format(Method::log, {Value("label"sv), Value::object({{"error", err("Error", "x", {}, std::nullopt)}}), Value("after"sv)}),
            std::string("label {\"error\":{\"name\":\"Error\",\"message\":\"x\"}} after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("[ai.ubq.fi] Failed to release local API key deletion claim after a concurrent update:"sv), Value::object({{"error", err("Error", "kv down", {}, std::nullopt)}})}),
            std::string("[ai.ubq.fi] Failed to release local API key deletion claim after a concurrent update: {\"error\":{\"name\":\"Error\",\"message\":\"kv down\"}}"sv));
  EXPECT_EQ(console::format(Method::warn, {Value("x"sv), fsError("/nonexistent-dir/x"), Value("after"sv)}),
            std::string("x {\"name\":\"Error\",\"message\":\"ENOENT: no such file or directory, open '/nonexistent-dir/x'\",\"properties\":{\"path\":\"/nonexistent-dir/x\",\"syscall\":\"open\",\"errno\":-2,\"code\":\"ENOENT\"}} after"sv));
  EXPECT_EQ(console::format(Method::error, {Value("x"sv), Value::object({{"error", fsError("/nonexistent-dir/x")}})}),
            std::string("x {\"error\":{\"name\":\"Error\",\"message\":\"ENOENT: no such file or directory, open '/nonexistent-dir/x'\",\"properties\":{\"path\":\"/nonexistent-dir/x\",\"syscall\":\"open\",\"errno\":-2,\"code\":\"ENOENT\"}}}"sv));
}

TEST("generic formatting escapes malformed input and bounds the projection") {
  const Value malformed = Value::array({Value("\xFF\n"sv)});
  EXPECT_EQ(console::format(Method::log, {malformed}), std::string("[\"\xEF\xBF\xBD\\n\"]"));
  Value nested(nullptr);
  for (int level = 0; level < 32; ++level) nested = Value::array({std::move(nested)});
  EXPECT(console::format(Method::log, {nested}).find("\"[truncated]\"") != std::string::npos);
  EXPECT_EQ(console::format(Method::log, {Value(std::string(256 * 1024 + 1, 'x'))}), "[truncated]");
}

TEST("errors from C++ exceptions: JsError, cause chains, other exceptions, fs errors") {
  EXPECT_EQ(console::format(Method::error, {Value("x"), Value(rt::JsError("boom")), Value("after")}), "x {\"name\":\"Error\",\"message\":\"boom\"} after");
  EXPECT_EQ(console::format(Method::warn, {Value("[w]"), Value(rt::JsError("warned"))}), "[w] {\"name\":\"Error\",\"message\":\"warned\"}");
  EXPECT_EQ(console::format(Method::error, {Value(rt::TypeError("bad type"))}), "{\"name\":\"TypeError\",\"message\":\"bad type\"}");
  EXPECT_EQ(console::format(Method::error, {Value(rt::JsError("", "Error"))}), "{\"name\":\"Error\",\"message\":\"\"}");
  const auto inner = std::make_shared<rt::JsError>("inner");
  const auto middle = std::make_shared<rt::JsError>("m", "Error", inner);
  EXPECT_EQ(console::format(Method::error, {Value("x"), Value(rt::JsError("o", "Error", middle)), Value("after")}), "x {\"name\":\"Error\",\"message\":\"o\",\"cause\":{\"name\":\"Error\",\"message\":\"m\",\"cause\":{\"name\":\"Error\",\"message\":\"inner\"}}} after");
  EXPECT_EQ(console::format(Method::error, {Value("x"), Value(std::runtime_error("plain"))}), "x {\"name\":\"Error\",\"message\":\"plain\"}");
  EXPECT_EQ(console::format(Method::error, {Value("x"), Value::error(rt::JsError("msg here", "CodexError"), {{"code", Value("codex_auth_invalid")}, {"status", Value(503)}})}),
            "x {\"name\":\"CodexError\",\"message\":\"msg here\",\"properties\":{\"code\":\"codex_auth_invalid\",\"status\":503}}");
  EXPECT_EQ(console::format(Method::error, {Value("x"), Value(std::make_exception_ptr(rt::RangeError("r")))}), "x {\"name\":\"RangeError\",\"message\":\"r\"}");
  EXPECT_EQ(console::format(Method::error, {Value("x"), Value(std::exception_ptr())}), "x undefined");
  EXPECT_EQ(console::format(Method::log, {Value(std::optional<std::string>()), Value(std::optional<std::string>("s"))}), "undefined s");
  EXPECT_EQ(console::format(Method::warn, {Value("x"), Value(rt::fs::errnoError(2, "open", "/nonexistent-dir/x")), Value("after")}),
            "x {\"name\":\"Error\",\"message\":\"ENOENT: no such file or directory, open '/nonexistent-dir/x'\",\"properties\":{\"path\":\"/nonexistent-dir/x\",\"syscall\":\"open\",\"errno\":-2,\"code\":\"ENOENT\"}} after");
  EXPECT_EQ(console::format(Method::log, {Value("%s"), Value(rt::TypeError("tt"))}), "{\"name\":\"TypeError\",\"message\":\"tt\"}");
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"({"a":[1,{"b":null}]})"))}), "{\"a\":[1,{\"b\":null}]}");
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
  EXPECT_EQ(written(2, [] { console::error("x", rt::JsError("boom"), "after"); }), "x {\"name\":\"Error\",\"message\":\"boom\"} after\n");
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
