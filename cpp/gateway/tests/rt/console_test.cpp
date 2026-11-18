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
    info.properties = Value::object(std::move(properties)).jsonValue().as_object();
    if (cause) info.cause = std::make_shared<console::ErrorInfo>(cause->errorInfo());
    return Value(std::move(info));
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
  const struct {
    Method method;
    std::vector<Value> args;
    std::string_view expected;
  } cases[]{
    {Method::log, {"plain"sv}, "plain"sv},
    {Method::log, {"a\nb"sv, "c"sv}, "a\nb c"sv},
    {Method::log, {}, ""sv},
    {Method::log, {1.0, -0.0, 0.1, 1e+21, NAN_, INF_, 9007199254740992, -1.5e-07, 1.2345678901234568e+20}, "1 0 0.1 1e+21 null null 9007199254740992 -1.5e-7 123456789012345680000"sv},
    {Method::log, {true, nullptr, Value(), "s"sv, json::parse(R"__json(["a", 1, null])__json"), json::parse(R"__json({})__json"), json::parse(R"__json([])__json"), json::parse(R"__json({"nested": {"deep": {"x": [1, {"y": 2}]}}})__json")}, "true null undefined s [\"a\",1,null] {} [] {\"nested\":{\"deep\":{\"x\":[1,{\"y\":2}]}}}"sv},
    {Method::info, {"[ai.ubq.fi] request_terminal"sv, "{\"a\":1,\"b\":\"x\"}"sv}, "[ai.ubq.fi] request_terminal {\"a\":1,\"b\":\"x\"}"sv},
    {Method::error, {"[ai.ubq.fi] label"sv, json::parse(R"__json({"error_class": "Error", "status": null, "code": null})__json")}, "[ai.ubq.fi] label {\"error_class\":\"Error\",\"status\":null,\"code\":null}"sv},
    {Method::warn, {"[ai.ubq.fi] passkey assertion verification failed"sv, json::parse(R"__json({"message": "bad sig", "expected_origin": "https://ai.ubq.fi", "expected_rp_id": "ai.ubq.fi", "credential_id_prefix": "abcdefghijkl", "has_user_handle": true, "require_user_verification": false})__json")}, "[ai.ubq.fi] passkey assertion verification failed {\"message\":\"bad sig\",\"expected_origin\":\"https://ai.ubq.fi\",\"expected_rp_id\":\"ai.ubq.fi\",\"credential_id_prefix\":\"abcdefghijkl\",\"has_user_handle\":true,\"require_user_verification\":false}"sv},

    // Structured values
    {Method::log, {json::parse(R"__json([1, 2, 3])__json")}, "[1,2,3]"sv},
    {Method::log, {json::parse(R"__json([1, 2, 3, 4, 5, 6, 7, 8, 9, 10])__json")}, "[1,2,3,4,5,6,7,8,9,10]"sv},
    {Method::log, {json::parse(R"__json([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10])__json")}, "[0,1,2,3,4,5,6,7,8,9,10]"sv},
    {Method::log, {json::parse(R"__json([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26])__json")}, "[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26]"sv},
    {Method::log, {json::parse(R"__json({"k": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25]})__json")}, "{\"k\":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25]}"sv},
    {Method::log, {json::parse(R"__json([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100])__json")}, "[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,\"[truncated]\"]"sv},
    {Method::log, {json::parse(R"__json([0, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000, 11000, 12000, 13000, 14000, 15000, 16000, 17000, 18000, 19000, 20000, 21000, 22000, 23000, 24000, 25000, 26000, 27000, 28000, 29000])__json")}, "[0,1000,2000,3000,4000,5000,6000,7000,8000,9000,10000,11000,12000,13000,14000,15000,16000,17000,18000,19000,20000,21000,22000,23000,24000,25000,26000,27000,28000,29000]"sv},
    {Method::log, {json::parse(R"__json(["s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10"])__json")}, "[\"s0\",\"s1\",\"s2\",\"s3\",\"s4\",\"s5\",\"s6\",\"s7\",\"s8\",\"s9\",\"s10\"]"sv},
    {Method::log, {json::parse(R"__json([1, "a", true, null, 2, "b", false, 3, "c", 4, 5, 6])__json")}, "[1,\"a\",true,null,2,\"b\",false,3,\"c\",4,5,6]"sv},
    {Method::log, {json::parse(R"__json([{"a": 1}])__json")}, "[{\"a\":1}]"sv},
    {Method::log, {json::parse(R"__json([{"a": 1}, {"b": 2}])__json")}, "[{\"a\":1},{\"b\":2}]"sv},
    {Method::log, {json::parse(R"__json([[1, 2], [3]])__json")}, "[[1,2],[3]]"sv},
    {Method::log, {json::parse(R"__json(["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"])__json")}, "[\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"]"sv},
    {Method::log, {json::parse(R"__json(["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"])__json")}, "[\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\",\"yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy\"]"sv},
    {Method::log, {json::parse(R"__json({"a": [{"b": 1}]})__json")}, "{\"a\":[{\"b\":1}]}"sv},
    {Method::log, {json::parse(R"__json({"a": {}, "b": []})__json")}, "{\"a\":{},\"b\":[]}"sv},
    {Method::log, {json::parse(R"__json({"l1": {"l2": {"l3": {"l4": 1}}}})__json")}, "{\"l1\":{\"l2\":{\"l3\":{\"l4\":1}}}}"sv},
    {Method::log, {json::parse(R"__json({"l1": {"l2": {"l3": [1]}}})__json")}, "{\"l1\":{\"l2\":{\"l3\":[1]}}}"sv},
    {Method::log, {json::parse(R"__json([[[[1]]]])__json")}, "[[[[1]]]]"sv},
    {Method::log, {json::parse(R"__json({"a": [[[[1]]]]})__json")}, "{\"a\":[[[[1]]]]}"sv},
    {Method::log, {json::parse(R"__json([{"a": {"b": {"c": 1}}}])__json")}, "[{\"a\":{\"b\":{\"c\":1}}}]"sv},
    {Method::log, {json::parse(R"__json({"a": [[]], "b": [{}], "c": {}})__json")}, "{\"a\":[[]],\"b\":[{}],\"c\":{}}"sv},
    {Method::log, {json::parse(R"__json([])__json"), json::parse(R"__json({})__json")}, "[] {}"sv},
    {Method::log, {json::parse(R"__json({"k": "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "k2": "y"})__json")}, "{\"k\":\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\",\"k2\":\"y\"}"sv},
    {Method::log, {json::parse(R"__json({"__proto__x": 1, "a.b": 2, "_a": 3, "A1": 4, "1a": 5, "-1": 6, "01": 7, "4294967295": 8, "4294967294": 9})__json")}, "{\"__proto__x\":1,\"a.b\":2,\"_a\":3,\"A1\":4,\"1a\":5,\"-1\":6,\"01\":7,\"4294967295\":8,\"4294967294\":9}"sv},
    {Method::log, {json::parse(R"__json({"a-b": 1, "valid_id": 2, "1": 3, "with space": 4, "ünï": 5, "": 6, "$x": 7})__json")}, "{\"a-b\":1,\"valid_id\":2,\"1\":3,\"with space\":4,\"ünï\":5,\"\":6,\"$x\":7}"sv},
    {Method::log, {json::parse(R"__json({"s": "quote \" and ' and \n newline \t tab \\ back ünï 😀"})__json"), json::parse(R"__json(["x\ny"])__json")}, "{\"s\":\"quote \\\" and ' and \\n newline \\t tab \\\\ back ünï 😀\"} [\"x\\ny\"]"sv},
    {Method::log, {json::parse(R"__json({"s": "a\rb\bc\fd\u000be\u0000f\u001bgh i"})__json")}, "{\"s\":\"a\\rb\\bc\\fd\\u000be\\u0000f\\u001bg\177h i\"}"sv},
    {Method::log, {json::parse(R"__json(["\t", "'", "\"", "`", "\\"])__json")}, "[\"\\t\",\"'\",\"\\\"\",\"`\",\"\\\\\"]"sv},
    {Method::log, {json::parse(R"__json({"1": 8, "2": 9, "10": 10, "valid$": 1, "_": 2, "ä": 3, "class": 4, "true": 5, "null": 6, "a1": 7})__json")}, "{\"1\":8,\"2\":9,\"10\":10,\"valid$\":1,\"_\":2,\"ä\":3,\"class\":4,\"true\":5,\"null\":6,\"a1\":7}"sv},
    {Method::log, {Value::object({{"u", Value()}, {"n", nullptr}, {"neg", -0.0}, {"nan", NAN_}, {"inf", -INF_}, {"big", 1e+300}, {"tiny", 5e-324}})}, "{\"n\":null,\"neg\":0,\"nan\":null,\"inf\":null,\"big\":1e+300,\"tiny\":5e-324}"sv},

    // Literal percent tokens
    {Method::log, {"100%s done %d %i %f %j %o %O %c %% end"sv, "A"sv, 4.5, 4.5, "4.5"sv, json::parse(R"__json({"j": 1})__json"), json::parse(R"__json({"o": 1})__json"), json::parse(R"__json({"O": 1})__json"), "color:red"sv, "extra"sv}, "100%s done %d %i %f %j %o %O %c %% end A 4.5 4.5 4.5 {\"j\":1} {\"o\":1} {\"O\":1} color:red extra"sv},
    {Method::log, {"a %s b"sv}, "a %s b"sv},
    {Method::log, {5.0, "%s"sv, "x"sv}, "5 %s x"sv},
    {Method::log, {"a"sv, "%s"sv, "x"sv}, "a %s x"sv},
    {Method::log, {"%s"sv, "%s"sv, "y"sv}, "%s %s y"sv},
    {Method::log, {"a %% b"sv}, "a %% b"sv},
    {Method::log, {"100%% done"sv, 1.0}, "100%% done 1"sv},
    {Method::log, {"%d"sv, true}, "%d true"sv},
    {Method::log, {"%d"sv, nullptr}, "%d null"sv},
    {Method::log, {"%d"sv, "12px"sv}, "%d 12px"sv},
    {Method::log, {"%d"sv, " 7"sv}, "%d  7"sv},
    {Method::log, {"%d"sv, "0x10"sv}, "%d 0x10"sv},
    {Method::log, {"%d"sv, json::parse(R"__json({})__json")}, "%d {}"sv},
    {Method::log, {"%d"sv, 1e+21}, "%d 1e+21"sv},
    {Method::log, {"%d"sv, -5.5}, "%d -5.5"sv},
    {Method::log, {"%d"sv, NAN_}, "%d null"sv},
    {Method::log, {"%i"sv, "3.9"sv}, "%i 3.9"sv},
    {Method::log, {"%f"sv, "3.9e2x"sv}, "%f 3.9e2x"sv},
    {Method::log, {"%f"sv, true}, "%f true"sv},
    {Method::log, {"%f"sv, -0.0}, "%f 0"sv},
    {Method::log, {"%d|%d|%d|%d|%d|%d|%d|%d|%d"sv, 1e+20, 1.5e+21, -1e+21, 9.2233720368547758e+18, 123.9, -0.0, "1e3"sv, "abc"sv, -123.9}, "%d|%d|%d|%d|%d|%d|%d|%d|%d 100000000000000000000 1.5e+21 -1e+21 9223372036854776000 123.9 0 1e3 abc -123.9"sv},
    {Method::log, {"%d|%d|%i|%d|%d|%d"sv, INF_, -INF_, INF_, json::parse(R"__json([])__json"), json::parse(R"__json([5])__json"), json::parse(R"__json([1, 2])__json")}, "%d|%d|%i|%d|%d|%d null null null [] [5] [1,2]"sv},
    {Method::log, {"%f|%f|%f|%f|%f|%f|%f|%f"sv, 1e+21, -0.0, "1e3"sv, "abc"sv, 1.5, " 2.5 "sv, "0x10"sv, INF_}, "%f|%f|%f|%f|%f|%f|%f|%f 1e+21 0 1e3 abc 1.5  2.5  0x10 null"sv},
    {Method::log, {"%s"sv, err("Error", "boom", {}, std::nullopt)}, "%s {\"name\":\"Error\",\"message\":\"boom\"}"sv},
    {Method::log, {"%s"sv, true}, "%s true"sv},
    {Method::log, {"%s"sv, 1.5}, "%s 1.5"sv},
    {Method::log, {"%s"sv, 1e+21}, "%s 1e+21"sv},
    {Method::log, {"%s"sv, Value()}, "%s undefined"sv},
    {Method::log, {"%s"sv, json::parse(R"__json([1, [2, 3]])__json")}, "%s [1,[2,3]]"sv},
    {Method::log, {"%s|%s|%s"sv, json::parse(R"__json({"a": 1})__json"), json::parse(R"__json({"a": {"b": {"c": 1}}})__json"), json::parse(R"__json([1, {"x": 2}])__json")}, "%s|%s|%s {\"a\":1} {\"a\":{\"b\":{\"c\":1}}} [1,{\"x\":2}]"sv},
    {Method::log, {"%s|%s"sv, nullptr, -0.0}, "%s|%s null 0"sv},
    {Method::log, {"%j"sv, json::parse(R"__json({"a": [1, "x"]})__json")}, "%j {\"a\":[1,\"x\"]}"sv},
    {Method::log, {"%j"sv, "s"sv}, "%j s"sv},
    {Method::log, {"%j"sv, nullptr}, "%j null"sv},
    {Method::log, {"%j"sv, 1.5}, "%j 1.5"sv},
    {Method::log, {"%j|%j|%j"sv, json::parse(R"__json({"a": {"b": 1}})__json"), json::parse(R"__json([1, "a"])__json"), Value()}, "%j|%j|%j {\"a\":{\"b\":1}} [1,\"a\"] undefined"sv},
    {Method::log, {"%o"sv, json::parse(R"__json({"a": 1})__json")}, "%o {\"a\":1}"sv},
    {Method::log, {"%O"sv, json::parse(R"__json([1])__json")}, "%O [1]"sv},
    {Method::log, {"%o"sv, 1.0}, "%o 1"sv},
    {Method::log, {"%o"sv, nullptr}, "%o null"sv},
    {Method::log, {"%o"sv, json::parse(R"__json({"a": {"b": {"c": {"d": 1}}}})__json")}, "%o {\"a\":{\"b\":{\"c\":{\"d\":1}}}}"sv},
    {Method::log, {"%c%s"sv, "css"sv, "after"sv}, "%c%s css after"sv},
    {Method::log, {"x %s %s"sv, "only"sv}, "x %s %s only"sv},
    {Method::log, {"%s:%s"sv, "a"sv, "b"sv, "c"sv}, "%s:%s a b c"sv},
    {Method::log, {"%5d"sv, 1.0}, "%5d 1"sv},
    {Method::log, {"%-s"sv, 1.0}, "%-s 1"sv},
    {Method::log, {"%.2f"sv, 1.234}, "%.2f 1.234"sv},
    {Method::log, {"tail %"sv}, "tail %"sv},
    {Method::log, {"tail %"sv, 1.0}, "tail % 1"sv},
    {Method::log, {"%%"sv}, "%%"sv},
    {Method::log, {"%%"sv, 1.0}, "%% 1"sv},
    {Method::log, {"%%s"sv, 1.0}, "%%s 1"sv},
    {Method::log, {"\303\251%s"sv, "x"sv}, "\303\251%s x"sv},
    {Method::log, {1.0, "%d"sv, "2"sv}, "1 %d 2"sv},
    {Method::log, {json::parse(R"__json({"a": 1})__json"), "%s"sv, "q"sv}, "{\"a\":1} %s q"sv},
    {Method::log, {"a %s"sv, "b %s"sv, "c"sv}, "a %s b %s c"sv},
    {Method::log, {err("Error", "first", {}, std::nullopt), "%s"sv, "x"sv}, "{\"name\":\"Error\",\"message\":\"first\"} %s x"sv},

    // Error diagnostics
    {Method::error, {"x"sv, err("Error", "boom", {}, std::nullopt), "after"sv}, "x {\"name\":\"Error\",\"message\":\"boom\"} after"sv},
    {Method::error, {err("Error", "solo", {}, std::nullopt)}, "{\"name\":\"Error\",\"message\":\"solo\"}"sv},
    {Method::error, {"x"sv, err("Error", "", {}, std::nullopt), "after"sv}, "x {\"name\":\"Error\",\"message\":\"\"} after"sv},
    {Method::error, {"x"sv, err("TypeError", "bad", {}, std::nullopt), "after"sv}, "x {\"name\":\"TypeError\",\"message\":\"bad\"} after"sv},
    {Method::error, {"x"sv, err("CustomName", "m", {}, std::nullopt), "after"sv}, "x {\"name\":\"CustomName\",\"message\":\"m\"} after"sv},
    {Method::error, {"[ai.ubq.fi] Failed:"sv, err("CodexError", "msg here", {{"code", "codex_auth_invalid"sv}, {"status", 503.0}}, std::nullopt)}, "[ai.ubq.fi] Failed: {\"name\":\"CodexError\",\"message\":\"msg here\",\"properties\":{\"code\":\"codex_auth_invalid\",\"status\":503}}"sv},
    {Method::error, {"x"sv, err("Error", "p", {{"other", 1.0}, {"code", "E1"sv}, {"path", "/p"sv}, {"syscall", "open"sv}, {"errno", -2.0}, {"zed", "z"sv}}, std::nullopt), "after"sv}, "x {\"name\":\"Error\",\"message\":\"p\",\"properties\":{\"other\":1,\"code\":\"E1\",\"path\":\"/p\",\"syscall\":\"open\",\"errno\":-2,\"zed\":\"z\"}} after"sv},
    {Method::error, {"x"sv, Value::object({{"error", err("Error", "boom", {}, std::nullopt)}, {"next", 1.0}})}, "x {\"error\":{\"name\":\"Error\",\"message\":\"boom\"},\"next\":1}"sv},
    {Method::error, {"x"sv, Value::array({err("Error", "boom", {}, std::nullopt), 2.0})}, "x [{\"name\":\"Error\",\"message\":\"boom\"},2]"sv},
    {Method::error, {"x"sv, err("Error", "outer", {}, err("Error", "inner", {}, std::nullopt)), "after"sv}, "x {\"name\":\"Error\",\"message\":\"outer\",\"cause\":{\"name\":\"Error\",\"message\":\"inner\"}} after"sv},
    {Method::error, {"x"sv, err("Error", "outer", {}, err("Error", "inner", {{"code", "C1"sv}}, std::nullopt)), "after"sv}, "x {\"name\":\"Error\",\"message\":\"outer\",\"cause\":{\"name\":\"Error\",\"message\":\"inner\",\"properties\":{\"code\":\"C1\"}}} after"sv},
    {Method::error, {"x"sv, err("Error", "o", {}, err("Error", "m", {}, err("Error", "i", {}, std::nullopt))), "after"sv}, "x {\"name\":\"Error\",\"message\":\"o\",\"cause\":{\"name\":\"Error\",\"message\":\"m\",\"cause\":{\"name\":\"Error\",\"message\":\"i\"}}} after"sv},
    {Method::error, {err("Error", "o", {}, err("Error", "i", {}, std::nullopt))}, "{\"name\":\"Error\",\"message\":\"o\",\"cause\":{\"name\":\"Error\",\"message\":\"i\"}}"sv},
    {Method::error, {"x"sv, err("Error", "p", {{"code", "E"sv}}, std::nullopt)}, "x {\"name\":\"Error\",\"message\":\"p\",\"properties\":{\"code\":\"E\"}}"sv},
    {Method::error, {err("Error", "a", {}, std::nullopt), err("Error", "b", {}, std::nullopt)}, "{\"name\":\"Error\",\"message\":\"a\"} {\"name\":\"Error\",\"message\":\"b\"}"sv},
    {Method::error, {"x"sv, err("Error", "line1\nline2", {}, std::nullopt), "after"sv}, "x {\"name\":\"Error\",\"message\":\"line1\\nline2\"} after"sv},
    {Method::warn, {"[w]"sv, err("Error", "warned", {}, std::nullopt)}, "[w] {\"name\":\"Error\",\"message\":\"warned\"}"sv},
    {Method::log, {"[l]"sv, err("Error", "logged", {}, std::nullopt)}, "[l] {\"name\":\"Error\",\"message\":\"logged\"}"sv},
    {Method::info, {"[i]"sv, err("Error", "informed", {}, std::nullopt)}, "[i] {\"name\":\"Error\",\"message\":\"informed\"}"sv},
    {Method::debug, {"[d]"sv, err("Error", "debugged", {}, std::nullopt)}, "[d] {\"name\":\"Error\",\"message\":\"debugged\"}"sv},
    {Method::warn, {"[w]"sv, err("TypeError", "tw", {}, std::nullopt)}, "[w] {\"name\":\"TypeError\",\"message\":\"tw\"}"sv},
    {Method::warn, {"[w]"sv, err("Error", "", {}, std::nullopt)}, "[w] {\"name\":\"Error\",\"message\":\"\"}"sv},
    {Method::error, {"x"sv, err("Error", "ENOENT: x", {{"code", "ENOENT"sv}}, std::nullopt), "after"sv}, "x {\"name\":\"Error\",\"message\":\"ENOENT: x\",\"properties\":{\"code\":\"ENOENT\"}} after"sv},
    {Method::error, {"x"sv, err("Error", "z", {{"syscall", "open"sv}}, std::nullopt), "after"sv}, "x {\"name\":\"Error\",\"message\":\"z\",\"properties\":{\"syscall\":\"open\"}} after"sv},
    {Method::error, {"x"sv, err("Error", "t", {{"s", "str"sv}, {"n", 1.5}, {"b", true}, {"nul", nullptr}, {"u", Value()}, {"arr", json::parse(R"__json([1, "a"])__json")}, {"obj", json::parse(R"__json({"k": 1})__json")}}, std::nullopt), "after"sv}, "x {\"name\":\"Error\",\"message\":\"t\",\"properties\":{\"s\":\"str\",\"n\":1.5,\"b\":true,\"nul\":null,\"arr\":[1,\"a\"],\"obj\":{\"k\":1}}} after"sv},
    {Method::error, {"x"sv, Value::object({{"error", err("Error", "boom", {{"code", "C"sv}, {"status", 5.0}}, std::nullopt)}})}, "x {\"error\":{\"name\":\"Error\",\"message\":\"boom\",\"properties\":{\"code\":\"C\",\"status\":5}}}"sv},
    {Method::error, {"x"sv, Value::array({err("Error", "a", {}, std::nullopt), err("TypeError", "b", {}, std::nullopt)})}, "x [{\"name\":\"Error\",\"message\":\"a\"},{\"name\":\"TypeError\",\"message\":\"b\"}]"sv},
    {Method::log, {"label"sv, Value::object({{"error", err("Error", "x", {}, std::nullopt)}}), "after"sv}, "label {\"error\":{\"name\":\"Error\",\"message\":\"x\"}} after"sv},
    {Method::error, {"[ai.ubq.fi] Failed to release local API key deletion claim after a concurrent update:"sv, Value::object({{"error", err("Error", "kv down", {}, std::nullopt)}})}, "[ai.ubq.fi] Failed to release local API key deletion claim after a concurrent update: {\"error\":{\"name\":\"Error\",\"message\":\"kv down\"}}"sv},
    {Method::warn, {"x"sv, fsError("/nonexistent-dir/x"), "after"sv}, "x {\"name\":\"Error\",\"message\":\"ENOENT: no such file or directory, open '/nonexistent-dir/x'\",\"properties\":{\"path\":\"/nonexistent-dir/x\",\"syscall\":\"open\",\"errno\":-2,\"code\":\"ENOENT\"}} after"sv},
    {Method::error, {"x"sv, Value::object({{"error", fsError("/nonexistent-dir/x")}})}, "x {\"error\":{\"name\":\"Error\",\"message\":\"ENOENT: no such file or directory, open '/nonexistent-dir/x'\",\"properties\":{\"path\":\"/nonexistent-dir/x\",\"syscall\":\"open\",\"errno\":-2,\"code\":\"ENOENT\"}}}"sv},
  };
  for (const auto& [method, args, expected] : cases) EXPECT_EQ(console::format(method, args), expected);
}

TEST("generic formatting escapes malformed input and bounds the projection") {
  const Value malformed = Value::array({Value("\xFF\n"sv)});
  EXPECT_EQ(console::format(Method::log, {malformed}), std::string("[\"\xEF\xBF\xBD\\n\"]"));
  const json::object unsafe{{"\xED\xA0\x80", "\xED\xBF\xBF\n"}, {"reading", INF_}};
  EXPECT_EQ(console::format(Method::warn, {unsafe}), R"({"\ud800":"\udfff\n","reading":null})");
  EXPECT_EQ(console::format(Method::log, {json::object{{"large", std::string(32 * 1024, 'x')}}}), R"({"large":"[truncated]"})");
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
  rt::JsError coded("msg here", "CodexError");
  coded.code = "codex_auth_invalid";
  console::ErrorInfo diagnostic = Value(coded).errorInfo();
  EXPECT_EQ(diagnostic.properties.at("code").as_string(), "codex_auth_invalid");
  diagnostic.properties.emplace("status", 503);
  EXPECT_EQ(console::format(Method::error, {"x", Value(std::move(diagnostic))}),
            "x {\"name\":\"CodexError\",\"message\":\"msg here\",\"properties\":{\"code\":\"codex_auth_invalid\",\"status\":503}}");
  EXPECT_EQ(console::format(Method::error, {Value("x"), Value(std::make_exception_ptr(rt::RangeError("r")))}), "x {\"name\":\"RangeError\",\"message\":\"r\"}");
  EXPECT_EQ(console::format(Method::error, {Value("x"), Value(std::exception_ptr())}), "x undefined");
  EXPECT_EQ(console::format(Method::log, {Value(std::optional<std::string>()), Value(std::optional<std::string>("s"))}), "undefined s");
  EXPECT_EQ(console::format(Method::warn, {Value("x"), Value(rt::fs::errnoError(2, "open", "/nonexistent-dir/x")), Value("after")}),
            "x {\"name\":\"Error\",\"message\":\"ENOENT: no such file or directory, open '/nonexistent-dir/x'\",\"properties\":{\"path\":\"/nonexistent-dir/x\",\"syscall\":\"open\",\"errno\":-2,\"code\":\"ENOENT\"}} after");
  EXPECT_EQ(console::format(Method::log, {Value("%s"), Value(rt::TypeError("tt"))}), "%s {\"name\":\"TypeError\",\"message\":\"tt\"}");
  EXPECT_EQ(console::format(Method::log, {Value(json::parse(R"({"a":[1,{"b":null}]})"))}), "{\"a\":[1,{\"b\":null}]}");
}

TEST("Capture owns structured fields after the caller's JSON storage is gone") {
  console::Capture capture({Method::info}, true);
  const std::string message(128, '%');
  const json::object expected{{"message", message}};
  const auto output = written(1, [&] {
    json::monotonic_resource arena;
    json::object transient(expected, &arena);
    console::info("fields %s", transient);
    transient.clear();
  });
  ASSERT_EQ(capture.calls().size(), std::size_t(1));
  const auto& call = capture.calls().front();
  EXPECT_EQ(call.args.at(1).jsonValue(), json::value(expected));
  EXPECT_EQ(call.text, "fields %s {\"message\":\"" + message + "\"}");
  EXPECT_EQ(output, call.text + "\n");
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
