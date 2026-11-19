// JSON and runtime value controls. The original vectors were recorded with Bun 1.4.
// JSON validation follows the selected library with the gateway's input bounds.
#include "uos/rt/json.hpp"
#include "uos/rt/json_document.hpp"
#include "uos/rt/limits.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace std::literals;

namespace {
  struct ParseCase {
    std::string_view input;
    bool ok;
    std::string_view expected{};  // serialized value for valid JSON
  };
  struct NumberCase {
    std::uint64_t bits;
    std::string_view expected;
  };
  struct EscapeCase {
    std::string_view text;
    std::string_view expected;
  };
  struct IndentCase {
    std::string_view source;
    int space;
    std::string_view gap;
    bool useGap;
    std::string_view expected;
  };
  struct CoerceCase {
    std::string_view source;
    std::string_view string;
    std::uint64_t number;
    bool truthy;
    std::string_view type;
  };
  struct EqualCase {
    std::string_view a, b;
    bool deepEqual;
    bool matches;
  };
  struct OrderCase {
    std::string_view start, ops, json, keys;
  };
  struct MergeCase {
    std::string_view a, b, spread, assigned;
  };
  struct StringSortCase {
    std::string_view values, sorted;
  };
  struct NumberSortCase {
    std::string_view values, sorted;
  };
  struct JsonSortCase {
    std::string_view values, sorted;
  };
  struct RecordSortCase {
    std::string_view keys, ascending, descending, chained;
  };
  struct ArrayCase {
    std::string_view source, includes, indexOf, at, flat;
  };

  // @@TABLES-BEGIN
  const ParseCase kParse[] = {
    {""sv, false},
    {" "sv, false},
    {"x"sv, false},
    {"abc"sv, false},
    {"{"sv, false},
    {"["sv, false},
    {"{\"a\""sv, false},
    {"{\"a\":"sv, false},
    {"{\"a\":1"sv, false},
    {"{\"a\":1,"sv, false},
    {"{\"a\":1,}"sv, false},
    {"[1,]"sv, false},
    {"[1 2]"sv, false},
    {"01"sv, false},
    {"1."sv, false},
    {".5"sv, false},
    {"-"sv, false},
    {"-a"sv, false},
    {"1e"sv, false},
    {"1e+"sv, false},
    {"+1"sv, false},
    {"\"abc"sv, false},
    {"\"\\x\""sv, false},
    {"\"\\u12\""sv, false},
    {"\"\\u12g4\""sv, false},
    {"\"a\001b\""sv, false},
    {"\"a\012b\""sv, false},
    {"tru"sv, false},
    {"true1"sv, false},
    {"nul"sv, false},
    {"nulll"sv, false},
    {"NaN"sv, false},
    {"Infinity"sv, false},
    {"-Infinity"sv, false},
    {"undefined"sv, false},
    {"'a'"sv, false},
    {"{a:1}"sv, false},
    {"{'a':1}"sv, false},
    {"1 2"sv, false},
    {"[1]x"sv, false},
    {"{}}"sv, false},
    {"\357\273\2771"sv, false},
    {"\302\2401"sv, false},
    {"1\302\240"sv, false},
    {"/*c*/1"sv, false},
    {"[,1]"sv, false},
    {"{,}"sv, false},
    {"{\"a\" 1}"sv, false},
    {"{\"a\":1 \"b\":2}"sv, false},
    {"{1:2}"sv, false},
    {"\"\\"sv, false},
    {"\"\\u"sv, false},
    {"[\"a\","sv, false},
    {"@"sv, false},
    {"\303\251"sv, false},
    {"\360\237\230\200"sv, false},
    {"{\"a\":\303\251}"sv, false},
    {"0x10"sv, false},
    {"1_000"sv, false},
    {"--1"sv, false},
    {"-01"sv, false},
    {"00"sv, false},
    {"1e5.5"sv, false},
    {"[}"sv, false},
    {"{]"sv, false},
    {"\0131"sv, false},
    {"\0141"sv, false},
    {"tRue"sv, false},
    {"True"sv, false},
    {"falsey"sv, false},
    {"fals"sv, false},
    {"[true false]"sv, false},
    {"\"\011\""sv, false},
    {"123abc"sv, false},
    {"{\"a\":1}garbage"sv, false},
    {"  [1, 2  "sv, false},
    {"[[[]]"sv, false},
    {"{\"a\":{\"b\":[1,{\"c\":}]}}"sv, false},
    {"1;"sv, false},
    {"[1];"sv, false},
    {"1;;"sv, false},
    {";"sv, false},
    {"[1;]"sv, false},
    {"{\"a\";1}"sv, false},
    {"("sv, false},
    {")"sv, false},
    {"="sv, false},
    {"(1)"sv, false},
    {"[1e]"sv, false},
    {"[1e+]"sv, false},
    {"[1.]"sv, false},
    {"[-]"sv, false},
    {"[01]"sv, false},
    {"[1 ]"sv, true, "[1]"sv},
    {"$abc"sv, false},
    {"_x"sv, false},
    {"a1$_b-c"sv, false},
    {"[1]@"sv, false},
    {"1@"sv, false},
    {"{\"a\":1}@"sv, false},
    {"\"\\\303\251\""sv, false},
    {"\"\\\001\""sv, false},
    {"\"\\\360\237\230\200\""sv, false},
    {"[1,,2]"sv, false},
    {"[1,}"sv, false},
    {"{\"a\":1,,}"sv, false},
    {"{\"a\"::1}"sv, false},
    {"{\"a\":,}"sv, false},
    {"{\"a\":}"sv, false},
    {"{\"a\":]"sv, false},
    {"[1:2]"sv, false},
    {"{\"a\":1]"sv, false},
    {"[\"a\" \"b\"]"sv, false},
    {"[[1],]"sv, false},
    {"{\"a\":{}"sv, false},
    {"{\"a\":[]"sv, false},
    {"[{}"sv, false},
    {"\"\\u12\"x"sv, false},
    {"\"\\u123"sv, false},
    {"\"\\u1234"sv, false},
    {"\"\\ud800\""sv, true, "\"\\ud800\""sv},
    {"{\"\\ud800\":1}"sv, true, "{\"\\ud800\":1}"sv},
    {"\" \""sv, true, "\" \""sv},
    {"\"\""sv, true, "\"\""sv},
    {"\"\\/\""sv, true, "\"/\""sv},
    {"-0"sv, true, "0"sv},
    {"-0.0"sv, true, "0"sv},
    {"1E400"sv, false},
    {"-1e-400"sv, true, "0"sv},
    {"1e-400"sv, true, "0"sv},
    {"0.1e1"sv, true, "1"sv},
    {"1E+2"sv, true, "100"sv},
    {"1e-2"sv, true, "0.01"sv},
    {"123456789012345678901234567890"sv, true, "1.2345678901234568e+29"sv},
    {"9007199254740993"sv, true, "9007199254740992"sv},
    {"-9007199254740993"sv, true, "-9007199254740992"sv},
    {"0.30000000000000004"sv, true, "0.30000000000000004"sv},
    {"1.7976931348623157e308"sv, true, "1.7976931348623157e+308"sv},
    {"1.7976931348623159e308"sv, false},
    {"5e-324"sv, true, "5e-324"sv},
    {"2.4703282292062327e-324"sv, true, "0"sv},
    {"2.4703282292062328e-324"sv, true, "5e-324"sv},
    {"[-0]"sv, true, "[0]"sv},
    {"{\"__proto__\":1}"sv, true, "{\"__proto__\":1}"sv},
    {"{\"a\":1,\"b\":2,\"a\":3}"sv, true, "{\"a\":3,\"b\":2}"sv},
    {"{\"2\":1,\"1\":2,\"b\":3,\"a\":4}"sv, true, "{\"1\":2,\"2\":1,\"b\":3,\"a\":4}"sv},
    {"{\"b\":1,\"a\":2,\"b\":3}"sv, true, "{\"b\":3,\"a\":2}"sv},
    {"\011\012\015 [1]\011\012\015 "sv, true, "[1]"sv},
    {"{\"a\" : 1 , \"b\" : [ 1 , 2 ] }"sv, true, "{\"a\":1,\"b\":[1,2]}"sv},
    {"[]"sv, true, "[]"sv},
    {"{}"sv, true, "{}"sv},
    {"[[]]"sv, true, "[[]]"sv},
    {"0"sv, true, "0"sv},
    {"-1"sv, true, "-1"sv},
    {"1e5"sv, true, "100000"sv},
    {"100"sv, true, "100"sv},
    {"1.5"sv, true, "1.5"sv},
    {"true"sv, true, "true"sv},
    {"false"sv, true, "false"sv},
    {"null"sv, true, "null"sv},
    {"{\"a\":1,\"b\";2}"sv, false},
    {"{\"a\":1,\"b\" 2}"sv, false},
    {"{\"a\":1,\"b\"}"sv, false},
    {"{\"a\":1,\"b\""sv, false},
    {"{\"a\":1,1:2}"sv, false},
    {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv, false},
    {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv, false},
    {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv, false},
    {"[bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb]"sv, false},
    {"1e+x"sv, false},
    {"[1e+5]"sv, true, "[100000]"sv},
    {"1E"sv, false},
    {"-1e-"sv, false},
    {"1e-5"sv, true, "0.00001"sv},
    {"1.0e"sv, false},
    {"0e"sv, false},
    {"-0e1"sv, true, "0"sv},
    {"1.5E+3"sv, true, "1500"sv},
    {"0.e1"sv, false},
    {"\"\\u12G4\""sv, false},
    {"\"\\u12\303\251\""sv, false},
    {"\"\\u1\360\237\230\200\""sv, false},
    {"\"\\u12\360\237\230\200\""sv, false},
    {"\342\202\254"sv, false},
    {"[\342\202\254]"sv, false},
    {"x\303\251"sv, false},
    {"\"\\ud83d\\ude00\""sv, true, "\"\360\237\230\200\""sv},
    {"\"\\ude00\\ud83d\""sv, true, "\"\\ude00\\ud83d\""sv},
    {"\"\\uD83D\\uDE00\""sv, true, "\"\360\237\230\200\""sv},
    {"\"\342\200\250\""sv, true, "\"\342\200\250\""sv},
    {"\"\\u0000\""sv, true, "\"\\u0000\""sv},
    {"nullx"sv, false},
    {"null x"sv, false},
    {"[true,]"sv, false},
    {"{\"a\":true,}"sv, false},
    {"{\"\":1}"sv, true, "{\"\":1}"sv},
    {"{\"a\":1}{"sv, false},
    {"[]]"sv, false},
    {"\"\\uDEAD\\uBEEF\""sv, true, "\"\\udead\353\273\257\""sv},
    {"\"a\\"sv, false},
    {"\"\\\""sv, false},
    {"\"\\u1\""sv, false},
    {"\"\\u123\""sv, false},
    {"\"\\u1234\""sv, true, "\"\341\210\264\""sv},
    {"\"\\ud83d\""sv, true, "\"\\ud83d\""sv},
    {"\"\355\240\275\""sv, false},
    {"\"\355\270\200x\""sv, false},
    {"\"\355\240\275\\ude00\""sv, false},
    {"\"\\ud83d\355\270\200\""sv, false},
    {"{\"4294967295\":1,\"4294967294\":2,\"01\":3,\"1\":4,\"-1\":5,\"0\":6}"sv, true, "{\"0\":6,\"1\":4,\"4294967294\":2,\"4294967295\":1,\"01\":3,\"-1\":5}"sv},
    {"{\"b\":1,\"10\":2,\"9\":3,\"a\":4,\"9\":5}"sv, true, "{\"9\":5,\"10\":2,\"b\":1,\"a\":4}"sv},
    {"[1.5,-2.25e-7,1e21,1e-7,123456789012,0.1,-0,1E2]"sv, true, "[1.5,-2.25e-7,1e+21,1e-7,123456789012,0.1,0,100]"sv},
    {"{\"nested\":{\"deep\":[{\"x\":[]},{},[[{}]]]}}"sv, true, "{\"nested\":{\"deep\":[{\"x\":[]},{},[[{}]]]}}"sv},
    {"\"\\b\\f\\n\\r\\t\\\"\\\\\\/\""sv, true, "\"\\b\\f\\n\\r\\t\\\"\\\\/\""sv},
    {"\"\303\251\342\202\254\360\237\230\200\""sv, true, "\"\303\251\342\202\254\360\237\230\200\""sv},
    {"[\0121\015,\0112 ]"sv, true, "[1,2]"sv},
    {"[1,2,3]\012"sv, true, "[1,2,3]"sv},
    {"\"\\u00e9\""sv, true, "\"\303\251\""sv},
    {"\342\200\250"sv, false},
    {"\"\000\""sv, false},
    {"\"\037\""sv, false},
    {"\"\177\""sv, true, "\"\177\""sv},
    {"[1, \"\342\200\251\"]"sv, true, "[1,\"\342\200\251\"]"sv},
    {"str\\\"\303\251ing\""sv, false},
    {"[{\"a\":\"1\360\237\230\200\"},\"\\ud83d\\ue00\"]"sv, false},
    {"[{\"a\":\"\355\270\200\"},\"ud83d\\ude00\"]"sv, false},
    {"\"ttr\"ing\""sv, false},
    {"[{\"\355\240\275a\":\"\360\237\230\200\"},\"\\u8d\\ude00\"]"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]]\\,\"y\":\"\303\251\"\303\251"sv, false},
    {"{\"0\":1,\"10\":2,\"x+\":[[]],\"y\":\"\303\251\"}"sv, true, "{\"0\":1,\"10\":2,\"x+\":[[]],\"y\":\"\303\251\"}"sv},
    {"\"str\\\"-ng\012\""sv, false},
    {"\"Estr\\\"in\\g\""sv, false},
    {"[{\"a\":\"\360\237\230\200\"\342\202\254},\"\\ud8d\\ude0\"]"sv, false},
    {"[{\"a\":\"\360\237\230\200\"}E\"\\ud83d\\ude00\"]"sv, false},
    {"\"st \\\"ng\""sv, true, "\"st \\\"ng\""sv},
    {"{\"a\":1,\"b\":[t.ue,fale,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"{\"a\":1,\"b\":[true,false,null],c\"{:\"x\\u0041\\n\"}"sv, false},
    {"-\355\270\2002.5E-3"sv, false},
    {"\355\270\2002.5E-3"sv, false},
    {"\"str\\\"ing\355\270\200\""sv, false},
    {"[1,-2.5e 3,0,{,k\":{}}]"sv, false},
    {"[l1,-2.5e3\"0,{\"k\":{}}]"sv, false},
    {"[]{\"a\":\"\360\237\230\200\"},\"\\ud83d\\ud[00\"]"sv, false},
    {".2.5E 3"sv, false},
    {"xstr\"ing0\""sv, false},
    {"-12f.5E-3"sv, false},
    {"{\"a\":1,b\":[true,false,null],\"tc\":\"x\\u0041\\n\"}"sv, false},
    {"[1,-2n5e\355\240\2753,0,{\"k\":{}}]"sv, false},
    {"\"str\\\" in\""sv, true, "\"str\\\" in\""sv},
    {"\"str\\\"xi\011ng/"sv, false},
    {"{\"\":1,\"10\":2,\"x\":[[]],\"y\":\"\303\251\"}"sv, true, "{\"10\":2,\"\":1,\"x\":[[]],\"y\":\"\303\251\"}"sv},
    {"\"s]r\\\"ing\""sv, true, "\"s]r\\\"ing\""sv},
    {"{[\"0\":1,\"10\":2,\"x\":[[]],\"y\":\"\303\251\"}"sv, false},
    {"[,-2.5e3,0,{\"k\":{}.]"sv, false},
    {"{\"0\":x,:10\":2,\"x\":[[]],\"y\":\"\303\251\"}"sv, false},
    {"[1,-2.ee3,0,{k\":{}}]"sv, false},
    {"-12. 5E-3 "sv, false},
    {"-1\355\270\200.5E--"sv, false},
    {"[1,-2.5\012e\303\2513,0,{\"k\":{}}]"sv, false},
    {"\"str\\ing\""sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\\u8d\\ude00\"]"sv, false},
    {"-12.5+3"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[],\"y\":\"\303\251\"x}"sv, false},
    {"-12.5tE-3"sv, false},
    {"\"str'\"ing\""sv, false},
    {"[1,-2.5e3,0,{\"k\"\342\202\254{}]"sv, false},
    {"\"\"tr\\\"\001ing\""sv, false},
    {"{\"0\":1,\"10\"2\355\270\200\"x\":[[]],y\":\"\303\251\"}"sv, false},
    {"\"s\342\202\254r\\\"ing\""sv, true, "\"s\342\202\254r\\\"ing\""sv},
    {"12. 5E-13"sv, false},
    {"[{\"\":\"\360\237\230\200\"},\"\\ud83\012\\ude00\"]"sv, false},
    {"-12.5E-}"sv, false},
    {"{\"0\":1\"10\":2,\"x\":[[]],\"y\":\"\303\251\"}"sv, false},
    {"[1,-2\01251e3,,{\"k\":{}}]"sv, false},
    {"t1,2.5e3,0,{\"k\":{}}]"sv, false},
    {"{\"a\":1,\".\":[true,false,null],\"c\":\"x\\u0041\\n\"}"sv, true, "{\"a\":1,\".\":[true,false,null],\"c\":\"xA\\n\"}"sv},
    {"-12.}5E--"sv, false},
    {"\"ster\355\240\275\"ng\""sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],\"\":\"\"9}"sv, false},
    {"{\"n\":1,10\":2,x\":[[]],\"y\":\"\303\251\"}"sv, false},
    {"[1,-2.5e3,0,{\"k\":{ }]"sv, false},
    {"\\[1,-2.5e3,0,{\".\":{}]"sv, false},
    {"-12.+E-3"sv, false},
    {"{\"a\":1\"b\":[true,false,null],\"c\":\"x\\u0l\342\202\2541\\n\"}"sv, false},
    {"{\"a\":1,\"b\":[true:fal\342\202\254se,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"{\"a\":1,1b\":[true,false,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"-e1]25E-3"sv, false},
    {"{\"0\":11,\"10\":2,\"x\":[[]9],\"y\":\"\303\251\"}"sv, false},
    {"\"str\\\"in\012g"sv, false},
    {"2.,5E-3"sv, false},
    {"{\"a\":1,\"b\":[true,falste,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"{\"0\"\001:1,\"10\":2,\"x'\":[]],\"y\":\"\303\251\"}"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\\d83d\\ude00\"]"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[]].,\"y\":\"\303\251\"\""sv, false},
    {"1,'.5e3,0,{\"k\":{}}]"sv, false},
    {"[{\"a9\":\"\360\237\230\200\"},\"\\ud83d\\ude00\"]"sv, true, "[{\"a9\":\"\360\237\230\200\"},\"\360\237\230\200\"]"sv},
    {"{\"0\":1,[\"10\":2,\"x\"[1]],\"y\":\"\303\251\"}"sv, false},
    {"[1,-2.5e3,0,\"k\":{}}]"sv, false},
    {"\001\"0\":1,\"10\":2,\"x\":[[]]\"-\":\"\303\251\"}"sv, false},
    {"{\"a\":1,\"ub\":[trxue,false,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"[{\342\202\254\"a\":\"\360\237\230\200\"},\"\\ud\0013d]ude00\"]"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\\ud8d\\ude00\"]"sv, false},
    {"\"str\\0\"{ing\""sv, false},
    {"{\"90\":1,\"10\":2,\"x\":[[]],y:\"\303\251\"}"sv, false},
    {"{\"a\":1,\"b\":[true,false,nul],e\"c[:\"x\\u0041\\n\"}"sv, false},
    {"\"s[r\\\"ing\""sv, true, "\"s[r\\\"ing\""sv},
    {"-12. x9-3"sv, false},
    {"\"str\\\"ing"sv, false},
    {"12\355\270\200.E-3"sv, false},
    {"\"str\\\"in\""sv, true, "\"str\\\"in\""sv},
    {"[\342\202\2541,--2.5e3[,0,{\"k\":{}}]"sv, false},
    {"\"str\\\"ing\"9"sv, false},
    {"\"sr\\1ing\""sv, false},
    {"\012str\\\"ing\""sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],\"y\":\"\303\251\"/}"sv, false},
    {"{\"0\":1],\"10\":2,\"x\":[[],y\":\"\303\251\"}"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\001d83d\012\\ude00\"]"sv, false},
    {"{\"a\":1,\"b\":[true,fa/lse,null],\"ac\":\"x\\u0041\\n\"}"sv, false},
    {"\"str\\\"ingx\""sv, true, "\"str\\\"ingx\""sv},
    {"{\"0\":1,\"10:2,\"x]:[[]u,\"y\":\"\303\251\"}"sv, false},
    {"{\"f0\":1,\"10\":2,\"x\":[[]],\012\":\"\303\251\"}"sv, false},
    {"[{\"a\":\"\360\237\230\200]\"},\"\\ud83d\\ude00\"]"sv, true, "[{\"a\":\"\360\237\230\200]\"},\"\360\237\230\200\"]"sv},
    {"{\"0\"\0111,\"1 \":2,\"x\":[[]],\"y\":\"\303\251}"sv, false},
    {"[{\"a\":\"\360\237\230\200\"}{\"\\ud83d\\ud00\"]"sv, false},
    {"[1,-2.\303\2515e3,0,{\"k\":{}}{"sv, false},
    {"\"str\\\"fn\""sv, true, "\"str\\\"fn\""sv},
    {"-12.n5E-3"sv, false},
    {"{\"a\":,\"b\":[true,false,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {",12.5E-"sv, false},
    {"{\"a\":1,\"b\":[true,false,null],\"c\":\"x\\u041{\\n\"}"sv, false},
    {"-1\0012.+5E-3"sv, false},
    {"{\"a\":1,\"b\":[t'rue,false,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"\"{\342\200\250tr\\ing\""sv, false},
    {"-\342\200\2502.5E-3"sv, false},
    {"-1.5-3"sv, false},
    {"12.5E,-3,"sv, false},
    {"\"st[n\\\"ing\""sv, true, "\"st[n\\\"ing\""sv},
    {"[ 1,-2.5e3,0,{\"k\":{}}]"sv, true, "[1,-2500,0,{\"k\":{}}]"sv},
    {"{\"0\":1,\"10\":2\011\"x\":[[]],\"y\":\"\303\251\"}"sv, false},
    {"{\"a\":1,\"b\":[true,false,null],\"\":\"x\\u0041\\n\"}"sv, true, "{\"a\":1,\"b\":[true,false,null],\"\":\"xA\\n\"}"sv},
    {"{\"0:1,\"10-:}2,\"x\":[[]],\"y\":\"\303\251\"}"sv, false},
    {"\"s[tr\\\"ing\"\001"sv, false},
    {"\"tr\\\"{ng"sv, false},
    {"[1,-2.5e3,0[{\"k\":{}}"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\\ud83d\\ud 00\"]"sv, false},
    {"-].5E\\-3"sv, false},
    {"[1,-2.5e3,0,{\",\342\202\254\"E:{}}]"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\\ud8\\3d\\ude00\"]"sv, false},
    {"{\"a\":1,\"b\":0[true,f/lseunull],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"{\"\":1,\"b\":[true,false,null],\"c\":\"x\\u0041\\n\"}"sv, true, "{\"\":1,\"b\":[true,false,null],\"c\":\"xA\\n\"}"sv},
    {"{\"0\":1,\"10\":2,\"x\"[[]],\"y\":\"\303\251\"}"sv, false},
    {"{\"a\":1,\"b\":[true,false,null],ac\":\"x\\u0041\\n\"}"sv, false},
    {"[\\,-2.5e3,,{\"k\":{}]"sv, false},
    {"\"str\"ig\""sv, false},
    {"\"st\\\"in\""sv, true, "\"st\\\"in\""sv},
    {"{\"a\":1,\"b\":[true,false,ull],\"c\":\"x\\u004/\\n\"}"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\\ud8\355\270\200d\\ude00:\"]"sv, false},
    {"[{\"a\"E:\"\360\237\230\200\"},\"\"\\ud83d\\ude00\"]"sv, false},
    {"\"sr\\\"ing\""sv, true, "\"sr\\\"ing\""sv},
    {"[1,-2.5e3,0,\"k\":{f}]"sv, false},
    {"-12t5-"sv, false},
    {"{\"0\":1,\"10\":2,\"\342\200\250x\":[[]],\"y\":\"\303\251\"}"sv, true, "{\"0\":1,\"10\":2,\"\342\200\250x\":[[]],\"y\":\"\303\251\"}"sv},
    {"[1,-2.5e3,0,{\"k\":{}},]"sv, false},
    {"-2'5E-3"sv, false},
    {"{\"a\":1,\"b\":[true,fase,1ull],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"12.5E-,"sv, false},
    {"{\"0\":1,\"10\":2\011,\"x\":\011[]],\"y\":\"\303\251\\}"sv, false},
    {"{\"a\":1,\"b\":[true\355\270\200false,null],\"c\":\"x\\u0x41\\n\"}"sv, false},
    {"[1,-2.5e3,',{\"k\":{}}]"sv, false},
    {"[1\011,-2.5e3,0e,{,\"k\":{}}]"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":l\303\251[]],\"y\":\"\342\200\250\303\251\"}"sv, false},
    {"{\"\":1,\"b\":[true,false,null],\"c\":\342\200\250x\\u0041\\n\"}"sv, false},
    {"\"sr\\\"\355\270\200ng"sv, false},
    {"[e{\"a\":\"\360\237\230\200\"},\"\\ud83d\\ude00\"]"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],\"Ey\":\"\303\251\"}"sv, true, "{\"0\":1,\"10\":2,\"x\":[[]],\"Ey\":\"\303\251\"}"sv},
    {"\"str\\/\"ing\""sv, false},
    {"-2.5E-3"sv, true, "-0.0025"sv},
    {"{\"a\":1,\"b\":1[true,false,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"\"st r\\\"ing\""sv, true, "\"st r\\\"ing\""sv},
    {"\"sr\\\"i\355\270\200g\""sv, false},
    {"[1,-2.5e}3,0,{\"k\":{}}]"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\\ud83d\\ude00\"\355\240\275]"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],\"y\":\"\303\251}"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],\"y:\"\303\251\355\240\275\"}"sv, false},
    {"{\"0\":1,\355\240\27510\":2,\"\" [[]],\"y\":\"\303\251\"}"sv, false},
    {"[1,-2.5e3,0,{\"k\342\200\250\":{}}]"sv, true, "[1,-2500,0,{\"k\342\200\250\":{}}]"sv},
    {"[{\"a\":\"\360\237\230\200\"},\"\\ud83d\\ude00\"e"sv, false},
    {"\"stlr\\\"in\342\200\250g\""sv, true, "\"stlr\\\"in\342\200\250g\""sv},
    {"{\"a\":1,\"b\":[/rue,false,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"\"s0tn\\\"\"ing\""sv, false},
    {"[1,-2.5e3,0,{:k\":{}}]"sv, false},
    {"[{\"a\":\360\237\230\200\"},\\ud83d\\ude00\"]"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\\ud83d\\ude}0\"] "sv, false},
    {"9[{a\":\"\360\237\230\200\"},\"\\ud83d\\ude00\"]"sv, false},
    {"12.{Et3"sv, false},
    {"{\"a\":1,\"b\011\":[true,false,null],,\"c\":\"x\\u0041\\n\"}"sv, false},
    {"[{\"0\":\"\360\237\230\200\"},\"ud83d\\ude00\"]"sv, true, "[{\"0\":\"\360\237\230\200\"},\"ud83d\\ude00\"]"sv},
    {".tr\\\"ung\""sv, false},
    {"-12t5E-3"sv, false},
    {"\"sEr\\\"\303\251ng\""sv, true, "\"sEr\\\"\303\251ng\""sv},
    {"\355\240\275-12.5E\303\2513"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\\ud3d\\ude00\"]"sv, false},
    {"{\"a\":1,\"b\":[true,false,null],\"c\":\"x\\u0041\\n\"}"sv, true, "{\"a\":1,\"b\":[true,false,null],\"c\":\"xA\\n\"}"sv},
    {"-12.\342\202\254E\342\200\2503"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\\ud83a\\ude00\"]"sv, true, "[{\"a\":\"\360\237\230\200\"},\"\360\236\250\200\"]"sv},
    {"{\"0\":1,\"10t:2,\"x\":[]][\"y\":\"\303\251\"}"sv, false},
    {"\"strf\\\"ing\":"sv, false},
    {"-1.5E-{ "sv, false},
    {",str-\"ing\""sv, false},
    {"[1,-2.5e3,0,{\"k\":-{}}]"sv, false},
    {"\"str}\\\"ing\""sv, true, "\"str}\\\"ing\""sv},
    {"\"str\\\"iung\"1"sv, false},
    {"-E\\2.5E-3"sv, false},
    {",tr\\\"ig\""sv, false},
    {"[{\"a\":\"\360\237\230\200\"}9\"\\ud83\\ude00\"]"sv, false},
    {"{\"a\":1,\"b\":[true,f\\alse,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"\"str\\\"in\"g\"{"sv, false},
    {"[-1e..5E-3"sv, false},
    {"{\"a\":1,\"b\":true,false,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"[{\"a\":\"\355\270\200\"\342\200\250,\"\\ud83d\\'de00\"]"sv, false},
    {"{x\"a\"1,\"b\":[true,false,null],\"c\":\"x\\.u0041\\n\"}"sv, false},
    {"\"str\\\"\342\200\250ng\""sv, true, "\"str\\\"\342\200\250ng\""sv},
    {"[1,-2.5e3,0,{\"k\":\342\200\250}}]"sv, false},
    {"}}0\":1{\"10\":2,\"x\":[[]],\"y\":\"\303\251\"}"sv, false},
    {"[1,-2.e3,0,{\"k:{}}]"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],\"y\":\"\303\251\"x"sv, false},
    {"][{\\a\":\"\360\237\230\200\"},\"\\ud83d\\ude00\"]"sv, false},
    {"{\"0\":1,\"10\"1:2,\"f\":[]],\"y\":\"\303\251\"}"sv, false},
    {"\"xstr\\\"ing\""sv, true, "\"xstr\\\"ing\""sv},
    {"\"stx\\\"ing\""sv, true, "\"stx\\\"ing\""sv},
    {"{\"0\":1,\"10\":2,\"x\":f[[]],\"y\":\"\303\251\"}"sv, false},
    {"{\"a\":1,f\"b\":[true,false,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"{\"0\":1,\"10\":2,\"x}:[[]],\"y\":\"\303\251\"}"sv, false},
    {"[1,-2.5e[n0,{\"k\":{}}]"sv, false},
    {"[1,f-2.ee3,0,{\"k\":{}}]"sv, false},
    {"\"str\\\"ing\""sv, true, "\"str\\\"ing\""sv},
    {"-2.5-3"sv, false},
    {"{\"a\":1,\"b\":[true,false,null[,\"c\":\"x\\u041\\n\"}"sv, false},
    {"[{-\"a\":\"\360\237\230\200\"},\"\\ud83d\\ude00\"]"sv, false},
    {"{\"a\":1,\"b\":[true,false,null],\"c\":\"'x\\u00u41\\n\"}\303\251"sv, false},
    {"{\"a\"[1\"b\":[true,false,+ull],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"-192.5E-3"sv, true, "-0.1925"sv},
    {"[1,-\303\2512/5e3,0,{\"k1\":{}}]"sv, false},
    {"{\"a\":1,\"b\":[true,false,null],\"c\":\"x\\u00'41\\n\"f"sv, false},
    {"{\"a\":1,\"b\":[true,false,null],\"c\":\"{x\\u0041\\n\"}"sv, true, "{\"a\":1,\"b\":[true,false,null],\"c\":\"{xA\\n\"}"sv},
    {"{\"0\":1,\"10\".2,\"x\":[[]],f\":\"\303\251\"}"sv, false},
    {"{\"a\":1,\"b\":[true,fle,null{,\"c\":\"x\\u0041\\n\"}"sv, false},
    {"{\"0\":1,\"10\":2,\"x\"[[,]]9\"y\":\"\303\251\"}"sv, false},
    {"-2.:E-3"sv, false},
    {"{\"0[\":1,\"10\":2,\"x\":[[],,\"y\"\"\303\251\"}"sv, false},
    {"[1,-2.5ae\355\270\200,0,{\"k\":{}}]"sv, false},
    {"[1,-2.e3,0\012{\"k\"{}}]"sv, false},
    {"[1,-2.5e3,0,{\"k\": }}]"sv, false},
    {"\"str\\aing\""sv, false},
    {"{\"0\":1,\"10\"::2\355\270\200,\"x\":[[]\001],\"y\":\"\303\251\"}"sv, false},
    {"-12e.5E\0123"sv, false},
    {"[{\"a\":\355\270\200\"\360\237\230\200\"}, \\ud83d\\udte00\"]"sv, false},
    {"[{\"\":\"\360\237\230\200\",\"\\ud83d\\ude00\"]"sv, false},
    {"{]0\":,\"10\":2,\"x\":[[]],\"\":\"\303\251\"}"sv, false},
    {"{\"a\":1,\"b\":[tre,false,nul],\"c\":\"\\u0041\\n\"}"sv, false},
    {"[a{ua\":\"\360\237\230\200\",\"\\ud83d\\ude00\"]"sv, false},
    {"-\"str\\\".ng\342\202\254\""sv, false},
    {"\"sr.\\\"ing"sv, false},
    {"\"satr\\\"[/ng\""sv, true, "\"satr\\\"[/ng\""sv},
    {"{\"a\":1,\"b\":[rue,false,null],\"c\":\"x\\u001\\n\"}"sv, false},
    {"[\001\"a\":\"\360\237\230\200\"},\"\\ud83d\\ude00f\"]"sv, false},
    {"-12.5E-{"sv, false},
    {"[1,-2.5e3,0,{k\":{}}]"sv, false},
    {"{\"a\":1,\"b\":[rue,false,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"{\"0\":1,\"E0\":2,\"x\"':[[]],\"y:\"\303\251\"}"sv, false},
    {"-2E-3"sv, true, "-0.002"sv},
    {"\303\25112.5E-,3t"sv, false},
    {"{\"0\":1,\"1[0\":2,\"x\":[[],\"\":\"\303\251\"}"sv, false},
    {"\"str\\\"ing{"sv, false},
    {"-12.5E-"sv, false},
    {"\"str \"ing\""sv, false},
    {"/[1,-2.5e3,0,{\"k\":{}}]"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":l[]],y\":\"\303\251E\"}"sv, false},
    {"-f2.u5E-3"sv, false},
    {"[{\"a\"\012:\"\360\237\230\200\"},\"\\ud83d\\ude00\"]"sv, true, "[{\"a\":\"\360\237\230\200\"},\"\360\237\230\200\"]"sv},
    {"[1,-2.5e3,0,n\"k\":{}}f"sv, false},
    {"[1,-25e3,0,{\"k\":{}}]"sv, true, "[1,-25000,0,{\"k\":{}}]"sv},
    {"\"st\\{ing\""sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]l,\"y\":\"\303\251\"}"sv, false},
    {"{\"a\":1,\"b\":[true,false,nlll],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"-12.5E-3\\"sv, false},
    {"12.-E-3"sv, false},
    {"\"tr\\\"ingx"sv, false},
    {"l12.5E-3"sv, false},
    {"[1,-2.5e3,0,{\"k\":{}}]E"sv, false},
    {"1,-2.5e3,0,{\"k/.:{}}]"sv, false},
    {"{\"0\":1,\"10\":2,\"x]\":[\011]],\"y\":\"\303\251\"}"sv, false},
    {"{\"a\":1,\"b\":[tru,e,false,null],\"\":\"x\\u0041.\\n\"}"sv, false},
    {"{\"0\":1,\"10\":2,\"{x\":[[]],\"y\":\"\303\251\"}"sv, true, "{\"0\":1,\"10\":2,\"{x\":[[]],\"y\":\"\303\251\"}"sv},
    {"\342\202\254\"s 'r\\\"ing\""sv, false},
    {"{0\":1,\"10\":2,\"x\":[[]u],\" y\":\"\303\251\"}"sv, false},
    {"-1.]5E-3\342\200\250"sv, false},
    {"\01212.5E-3"sv, true, "0.0125"sv},
    {"\"st\\\"ing\""sv, true, "\"st\\\"ing\""sv},
    {"{\"a\":1,\"b\":[tr\011e,fal.se,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"-1215E-3"sv, true, "-1.215"sv},
    {"[1,-..5e3,0,{\"k\":{}]"sv, false},
    {"\"str\\\"inEt"sv, false},
    {"Estr[\"ing\355\270\200"sv, false},
    {"{\"a\"'1,\"b\":[true,false,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"[{\"a\":l\355\240\275\"},\"\\ud83d\\9ude00\"]"sv, false},
    {"{\"1a\":1,\"b\":[true,false,null\355\240\275,\"\":\"x\\u0041\\n\"}"sv, false},
    {"{\"0\":1,\"}10\":,\"x\":[[]],\"y\":\"\303\251\"}"sv, false},
    {"[1,-2.5e3,0,{\"k\"\355\240\275}}]"sv, false},
    {"[{\"a\":\"\360\237\230\200}\"\\ud83d\\ude00\"]"sv, false},
    {"[1,2n.5e3,0,{\"k\":{}]"sv, false},
    {"{\"a\":\"\360\237\230\200\"},\"\\ud8\342\200\250d\\ude00\"]"sv, false},
    {"{\"a\":1,\"b\":[true,false,null],\"c-\"\"x\\nu0041\\n\"}"sv, false},
    {"[{\"a\":\"\355\240\275f\"},\"\\ud83d\\ude00\"]"sv, false},
    {"{\"0\"1,\"10\":2,\"x\":[[]],\"y\":\"\303\251}"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],\"y0:\"\303\251\"}"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],\"\":\"\303\251\"}"sv, true, "{\"0\":1,\"10\":2,\"x\":[[]],\"\":\"\303\251\"}"sv},
    {"[{\"a\":\"\355\240\275},\"\\ud8\342\200\2503d\\ude00\"]"sv, false},
    {"-12.\342\200\2505E-3"sv, false},
    {"\"sr\\\"ing\","sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],-\"y\":\"\303\251\"}"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\\ud83d\\ude00\"]"sv, true, "[{\"a\":\"\360\237\230\200\"},\"\360\237\230\200\"]"sv},
    {"[1,.5e3,0,{\"k\":{}}]"sv, false},
    {"[1,E2.5e30,{\"k1\":{}}]"sv, false},
    {"{\"a\":1,\"b\":[true,false,null],\"c\":\355\240\275\"x\\u00\01241\\n\"}"sv, false},
    {"[{\"a\":\"\"\355\270\200\"},\"\\ud83dt\\ude00\"]"sv, false},
    {"-12.5E-3f"sv, false},
    {"-12.5.-{3"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],\"y\":\342\200\250\"}"sv, false},
    {"{\"a\":1,\"b\":[true,flse,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"[{a\":\"\360\237\230\200\"},\"\\ud83d\\ude00\"]"sv, false},
    {"-1.5,E-3"sv, false},
    {"\"str\\\\ing\""sv, true, "\"str\\\\ing\""sv},
    {"{\"0:1,\",10\":2,\"x\":[[]],\"y\":\"\303\251\"}"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],\"y:\"\303\251\"}"sv, false},
    {"{\"a\":1,\"b\":[truel,false,null],\"c\":\"x\\u00411\\n\"}"sv, false},
    {"-12.5El\0013"sv, false},
    {"{\"a\":1,\"b\":[true,false,null],x\"c\":\"x\\u0041\\n\"}"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\\ud83d\\9de00\"]"sv, false},
    {"{\"a\":1,\"b\":[tre,false,null],\"c\":x\\u0041\\n\"}"sv, false},
    {"\"r\\\"ing\""sv, true, "\"r\\\"ing\""sv},
    {"[1,-2.5e3,0,:\"k\":e}}]"sv, false},
    {"[{\"a\":\"\360\237\230\200\"\",\"\\ud83d\\ude00\"]"sv, false},
    {"{\"a\":1,\"b\":[tr,ue,false,null],\"-c\":\"x\\u0n041\\n\"}"sv, false},
    {"{\"a\":1a,\"b\":[true,false,null],\"c\"e:\011\"x\\u0041\\n\"}"sv, false},
    {"[{\"a\"\\:\360\237\230\200\"},\"\\ud83d\\ude00\"]"sv, false},
    {"-125E-3"sv, true, "-0.125"sv},
    {"{a\":1,\"b\":[true,false,null],\"c\":\"x\\u0041\\n\"a}"sv, false},
    {"{\"a\":1,\"b\":[true,false,null],\"c\":\"x\\u[041\\n\"}"sv, false},
    {"{\"0\":,\"10\":2,\"x\":[[]],\"y\":\"\303\251\"}"sv, false},
    {"{0\":1,\"10\":2,\"x\":[[]],\"y\":\"\303\251\"}"sv, false},
    {"\"str\\linn\""sv, false},
    {"{\"0\":1,\"10\":2,\"x\"[[],\"y\":\"\303\251\"}"sv, false},
    {"[1,-2.e3,0,{\"k\":{}}]"sv, false},
    {"[{\"a.\":\"\360\237\230\200\"},\"\\ud83d\\de00\"]"sv, false},
    {"{\"a\":1,\"b\":[true,f\355\240\275alse,ull],\"c\":\"x\\u004\\n\"}"sv, false},
    {"[1,-2.59e3,0,{\"k\":{}]"sv, false},
    {"{\"0\"':1,\"1\":2,\"x\":[[]],\"y\":\"\303\251\"}"sv, false},
    {"[x,-2.53,0,{\"k\":\012{}}]"sv, false},
    {"[1,-2.5e3,0,{\"k\":{}]"sv, false},
    {"-12.5n-3"sv, false},
    {"[1,{]2.5e3n,0,{\"k\":{}}]"sv, false},
    {"12.5E-3"sv, true, "0.0125"sv},
    {"[1,-2.\\e3,0,{\"\":{}]"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],\"\"y\":\"\011\303\251\"}"sv, false},
    {"{\"a\":1,\"b\":[tru\011e,false,null],\"c\":\355\270\200x\\u0041\\n\"}"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],\"y\":\"\303\251\"["sv, false},
    {"-12.\011+5E-3["sv, false},
    {" [{\"a\":\"\360\237\230\200\"},\"l\\ud83d\\ude00\"]"sv, true, "[{\"a\":\"\360\237\230\200\"},\"l\360\237\230\200\"]"sv},
    {"[1,-2.5e3,0,\"k\":a{}}]"sv, false},
    {"[{\011\"a\":\"\355\240\275,},\"\\ud83d\\ude00\"]"sv, false},
    {"[1,-2.5]e3,0,{\"k\":a\001}}]"sv, false},
    {"-12\355\240\275.[-3"sv, false},
    {"[1,-2.5e3,0\\,{\"k\":{}}]"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"ud83d\\ude00\"]"sv, true, "[{\"a\":\"\360\237\230\200\"},\"ud83d\\ude00\"]"sv},
    {"01,-\"2.5e3,0,{\"k\":{}}]"sv, false},
    {"-\0011/2.5E-3"sv, false},
    {"\"stt\011eing\""sv, false},
    {"\"str\\\"eng\""sv, true, "\"str\\\"eng\""sv},
    {"{\"0.\":1,\"10\":2,\"x\":[[]],\"y\":\"\303\251\"}"sv, true, "{\"10\":2,\"0.\":1,\"x\":[[]],\"y\":\"\303\251\"}"sv},
    {"[{\"\342\202\254a':\"\360\237\230\200\"},\"\\ud83[d\\ude00\"]"sv, false},
    {"[{na\":\"\360\237\230\200\"{},\"\\ud83\\ude00\"]"sv, false},
    {"[1,-2.5e3,0,{\"k\":{}}/"sv, false},
    {"[n{\"a\":\"\360\237\230\200\"},\"\\ud83dude00\"a]"sv, false},
    {"[{\"a\":\"\360\237\230\200x'},\"\\u 83d\\ude00\"]"sv, false},
    {"\001str\\ ing\""sv, false},
    {"-12.5E\"3"sv, false},
    {"{\"a\":1\342\200\250,\"\"[true,false,null],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],y\":\"\"}"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},}\\ud+3d\\ude00\"]"sv, false},
    {"[{\"a\":\"\360\237\230\200\",,\"\\ud83d\\ude00\\]"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[\342\202\254[l],\"y\":\"\303\251\"}"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":/[[]],\"y\":\"\303\251\"}"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\\ud83\\ude00\"]"sv, false},
    {"{\"a\":1,\"b\":[t\011ue,fals\355\270\200e,nul],\"c\":\"x\\u0041\\n\"}"sv, false},
    {"-2.5E-\355\240\2753"sv, false},
    {"[x\355\270\200:,-2.5e3,0,{\"k\":{}}]"sv, false},
    {"-1\303\251.5E-3"sv, false},
    {"\0112.5E-3"sv, true, "0.0025"sv},
    {"[{\"a\":\"\355\240\2759\355\270\200\"},\"\\ud83d.\\ude00\"]"sv, false},
    {"1,-12.5e3,0,{\"k\":{}}]"sv, false},
    {"\"fstr\\\"ing\""sv, true, "\"fstr\\\"ing\""sv},
    {"\"+str\\\"ing\""sv, true, "\"+str\\\"ing\""sv},
    {"[1,-2.5e3,10,{\"\303\251\":{}}\355\270\200]"sv, false},
    {"E{\"a\":\"\360\237\230\200\"},\"\\ud83d\\ude00\"]"sv, false},
    {"/-2.5E-"sv, false},
    {"[1,-2.5ee3l0,{\"k\":{}}]"sv, false},
    {"str\\\"in+\"\012"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\\ud83d\\ude.00\"]"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],f\"y\":\"\303\251\"}"sv, false},
    {"[{\"a\":\"\355\270\200\"},\"nu\"83d\\ude00\"]"sv, false},
    {"-12u5{-3"sv, false},
    {"[1,2.5e3,0,{\"k\"{\303\251}}]"sv, false},
    {"[1,-.5e3,0,{\"k\"{}}]"sv, false},
    {"{\"0\"a:1,\"10\":2,\"x\":[[]],\"y\":\342\200\250\303\251\"}"sv, false},
    {"[{\"a{\":\"\360\237\230\200\"},\"\\ud83d\\de00,]"sv, false},
    {"{\"0:1,\"10\":2,\"x\":[[]],\"y\":\"\303\251\"}"sv, false},
    {"{\"0\":1,\"10\"]:2,\"x\":[[]],\"y\":\"\303\251\"}"sv, false},
    {"[1,\"2.5e3,0,'\"k\":{}}]"sv, false},
    {"[1,-2.5'3,:,{\"k\":{}]"sv, false},
    {"[1,-2.5e3,\342\200\250,{\"k\":{}}]"sv, false},
    {"112.:{5E-3"sv, false},
    {"[E\"+\":\"\360\237\230\200\"},\"\\ud83d\\ude0x0\"]"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],\"\": \303\251f\"}"sv, false},
    {"[{\"a\":\"E\360\237\230\200\"},\"\\ud83d\\ude00\"]"sv, true, "[{\"a\":\"E\360\237\230\200\"},\"\360\237\230\200\"]"sv},
    {"{\"a\":1,\"b\":[true,false,null],\"ct\":\"x\\u0041\\n\"}"sv, true, "{\"a\":1,\"b\":[true,false,null],\"ct\":\"xA\\n\"}"sv},
    {"[{\"a\":\"\360\237\230\200},\"\\ud8d\\ude00\"]"sv, false},
    {"\"str.\\\"ing\""sv, true, "\"str.\\\"ing\""sv},
    {"[{\"a\":\"\360\237\230\200\"},\"\\ud83d\\ude00\"["sv, false},
    {"[1n,-2.5e3,0,{\011k\":{}}]"sv, false},
    {"{\"0\":1,\"10\":[,\"x\":[[]],\"y\":\"\303\251\"}"sv, false},
    {"{\"0\":1,\"10\":2,\"x\":[[]],\"y\":\"\303\251\".}"sv, false},
    {"-12.5fE-3"sv, false},
    {"\"st\\\"ingu"sv, false},
    {"{\"a\355\270\200\":1,\"b\":[true,f'alse,null]+,\"c\":\"x\\u0041\\n\"}"sv, false},
    {"[1,-2.15e3,0,{\"k:{}}]"sv, false},
    {"[1,-2.5e3-0,\"k\":{}}]"sv, false},
    {"\"str\"ing\"\355\270\200"sv, false},
    {"[{\"a\" :\"\360\237\230\200/},\"\\ud83d\\ude00\"]"sv, false},
    {"[{\"a\":\"\360\237\230\200\"} \"\\ud83d\\ude00\"]"sv, false},
    {"-f,2.n5E-3"sv, false},
    {"[{\"a\":\"\360\237\230\200\"},\"\\u83d\\ude00\"]"sv, false},
    {"\"str\\\"/ng\""sv, true, "\"str\\\"/ng\""sv},
  };
  const NumberCase kNumbers[] = {
    {0x0000000000000000ULL, "0"sv},
    {0x8000000000000000ULL, "0"sv},
    {0x7ff8000000000000ULL, "null"sv},
    {0x7ff0000000000000ULL, "null"sv},
    {0xfff0000000000000ULL, "null"sv},
    {0x3ff0000000000000ULL, "1"sv},
    {0xbff0000000000000ULL, "-1"sv},
    {0x3fb999999999999aULL, "0.1"sv},
    {0x3fd3333333333334ULL, "0.30000000000000004"sv},
    {0x444b1ae4d6e2ef50ULL, "1e+21"sv},
    {0x4415af1d78b58c40ULL, "100000000000000000000"sv},
    {0x3eb0c6f7a0b5ed8dULL, "0.000001"sv},
    {0x3e7ad7f29abcaf48ULL, "1e-7"sv},
    {0x441ac53a7e04bcdaULL, "123456789012345680000"sv},
    {0x0000000000000001ULL, "5e-324"sv},
    {0x7fefffffffffffffULL, "1.7976931348623157e+308"sv},
    {0x4340000000000000ULL, "9007199254740992"sv},
    {0x433fffffffffffffULL, "9007199254740991"sv},
    {0xc33fffffffffffffULL, "-9007199254740991"sv},
    {0x4340000000000001ULL, "9007199254740994"sv},
    {0x43e0000000000000ULL, "9223372036854776000"sv},
    {0x43f0000000000000ULL, "18446744073709552000"sv},
    {0x41f0000000000000ULL, "4294967296"sv},
    {0x3ff8000000000000ULL, "1.5"sv},
    {0xbe5ad7f29abcaf48ULL, "-2.5e-8"sv},
    {0x4059000000000000ULL, "100"sv},
    {0x7e37e43c8800759cULL, "1e+300"sv},
    {0x400921fb54442d18ULL, "3.141592653589793"sv},
    {0x4340000000000000ULL, "9007199254740992"sv},
    {0x3c36b082c2148b8eULL, "1.23e-18"sv},
    {0x087c8fcca92d22ceULL, "8.650240331737417e-268"sv},
    {0xa3e86af47861084fULL, "-1.0498285246902819e-135"sv},
    {0x74705b9b9f63875aULL, "7.495531270007016e+252"sv},
    {0x1e9ea35e9455c2a4ULL, "3.405062123527288e-161"sv},
    {0xac2bc02451ca6c53ULL, "-6.4959556694004945e-96"sv},
    {0x94404c13258e3cb2ULL, "-3.872800879223473e-211"sv},
    {0xa119ba7e5c901bf5ULL, "-3.143956063482241e-149"sv},
    {0xa23c1442363cc718ULL, "-8.994681941964098e-144"sv},
    {0x752d9e97191ca0e8ULL, "2.7796105339149366e+256"sv},
    {0x2d3d2b68825d8740ULL, "8.949757077600599e-91"sv},
    {0x64065d5a5a041b0cULL, "6.914325391820973e+173"sv},
    {0xf5db007e2f45d814ULL, "-5.189565684200522e+259"sv},
    {0x1ca554d771c216c8ULL, "1.1039568017188534e-170"sv},
    {0x507ae7e59c4e7566ULL, "4.984774839933535e+79"sv},
    {0x0eb11ef78092e040ULL, "6.573121353812858e-238"sv},
    {0x4fcb4fade1e16af2ULL, "2.470645584342188e+76"sv},
    {0x5b56d90d8e901ff6ULL, "1.0135932420025527e+132"sv},
    {0x20bdb16144aa9f09ULL, "5.669368172437728e-151"sv},
    {0xfe3d212c19090820ULL, "-1.2192419828269623e+300"sv},
    {0x39af90e6bdd592dfULL, "7.781624928530572e-31"sv},
    {0xfe40d61874f72f79ULL, "-1.40939467263271e+300"sv},
    {0xb210b955027332c8ULL, "-1.550814769737529e-67"sv},
    {0xe16a1b7de2b8a59fULL, "-1.8352327385339571e+161"sv},
    {0x376ceddaf028ee91ULL, "1.0377813556675832e-41"sv},
    {0x52adb5ec6618bb64ULL, "1.8913012287601272e+90"sv},
    {0x5ff4781ece0ddcc5ULL, "1.7152960677825547e+154"sv},
    {0xc73d87ef8fcbddefULL, "-1.5333371422766028e+35"sv},
    {0x7abebd540f99101eULL, "1.7855566916126062e+283"sv},
    {0x9ec21cb377d65139ULL, "-1.6103543471371257e-160"sv},
    {0x03d4d7e1bc3fd5d5ULL, "3.3418766980104424e-290"sv},
    {0x962f8477b28bb3c7ULL, "-8.0419989208273e-202"sv},
    {0x5a95067eb10dd133ULL, "2.2772034390028073e+128"sv},
    {0x6e5d206680612c8eULL, "4.2113957602398326e+223"sv},
    {0x1be770b64bc8838eULL, "2.9616609852944623e-174"sv},
    {0x2c94a412bac1bb39ULL, "6.184580094962838e-94"sv},
    {0x397d60aa386529deULL, "9.052671110594253e-32"sv},
    {0x368a49518baa86a1ULL, "5.7554832351020915e-46"sv},
    {0xb4a7a8de76354800ULL, "-4.824574498345139e-55"sv},
    {0x3991d4da62583ebfULL, "2.197896801498203e-31"sv},
    {0xd47263789bb8c1b6ULL, "-6.284435810858927e+98"sv},
    {0xf2464e514a020177ULL, "-2.97472497993546e+242"sv},
    {0x4fb0684115b54abdULL, "7.421206870832906e+75"sv},
    {0x20b42d4c06a20782ULL, "3.852454482641555e-151"sv},
    {0x8c1ef1cb16d0a3d8ULL, "-2.701264792827612e-250"sv},
    {0x57dff32d9efaf9f0ULL, "1.967016787094062e+115"sv},
    {0xf957bb52c98275ccULL, "-3.286589563276598e+276"sv},
    {0x604d7c7137a44990ULL, "7.9068799944530585e+155"sv},
    {0x6c54cdbb49993e0aULL, "7.003517986977243e+213"sv},
    {0xa5516de69550db90ULL, "-6.286107468321877e-129"sv},
    {0x4a710ca117463333ULL, "3.9868205074116155e+50"sv},
    {0xf5274fc3873b142cULL, "-2.1876507336551247e+256"sv},
    {0x52623893bb7a174bULL, "7.24938303006386e+88"sv},
    {0xcbf27bc215c54967ULL, "-7.251398257199063e+57"sv},
    {0x21d71573ac5b8d2fULL, "1.1553919578240292e-145"sv},
    {0x623e79d00d81c512ULL, "1.7549841402955905e+165"sv},
    {0x91506fe309247715ULL, "-2.775411226586895e-225"sv},
    {0x7f140a36340416ffULL, "1.3742665066674057e+304"sv},
    {0x6dae1fd264d517f8ULL, "2.1267863822775141e+220"sv},
    {0x0364e94d57edcb0bULL, "2.619364925869544e-292"sv},
    {0x3f1755ed60f97838ULL, "0.0000890184530759349"sv},
    {0xb035a7279ef9ecafULL, "-1.8699851139329242e-76"sv},
    {0xcee7fe3915381389ULL, "-1.3247520316741953e+72"sv},
    {0xfe6e3d8d022fad22ULL, "-1.0125901282743813e+301"sv},
    {0x06f94a07d88e06bbULL, "4.565188951231127e-275"sv},
    {0x137438771b28db0aULL, "5.865654018838177e-215"sv},
    {0x097afeee8245a21dULL, "5.358218659040229e-263"sv},
    {0xf6c53ade62449031ULL, "-1.3370119988709273e+264"sv},
    {0xf81bca10aa68dd3cULL, "-3.6702360220725443e+270"sv},
    {0x882fbebb80e4c9a7ULL, "-3.0044839294452138e-269"sv},
    {0x23d48caffa67846fULL, "4.417580786504621e-136"sv},
    {0x4155e99080000000ULL, "5744194"sv},
    {0xbff0000000000000ULL, "-1"sv},
    {0x4140890a80000000ULL, "2167317"sv},
    {0x43a27039b6400000ULL, "664312672497434600"sv},
    {0x4332644b42e00000ULL, "5176823988092928"sv},
    {0x41341e2e00000000ULL, "1318446"sv},
    {0xc033000000000000ULL, "-19"sv},
    {0x40a64e0000000000ULL, "2855"sv},
    {0xc17fc1b770000000ULL, "-33299319"sv},
    {0x42820b3014c00000ULL, "2479907575808"sv},
    {0x42cb9843d2c00000ULL, "60681573728256"sv},
    {0xc25924817a400000ULL, "-431946590464"sv},
    {0xc0d1744000000000ULL, "-17873"sv},
    {0x4231debbe1800000ULL, "76751298944"sv},
    {0xc096380000000000ULL, "-1422"sv},
    {0x43ae0f4277000000ULL, "1083011512328519700"sv},
    {0x42f3d714c4200000ULL, "349031796703232"sv},
    {0x432a0a2905a00000ULL, "3664760349392896"sv},
    {0x4010000000000000ULL, "4"sv},
    {0x4082380000000000ULL, "583"sv},
    {0x4008000000000000ULL, "3"sv},
    {0x4207677ded800000ULL, "12565069232"sv},
    {0x4316300a38400000ULL, "1561317484789760"sv},
    {0xc3b7de775c000000ULL, "-1719943344722804700"sv},
    {0x42c9bbf1e7e00000ULL, "56590016167936"sv},
    {0x416c257500000000ULL, "14756776"sv},
    {0x4254d3721a800000ULL, "357787265536"sv},
    {0x0000000000000000ULL, "0"sv},
    {0xc2106151e6e00000ULL, "-17588058552"sv},
    {0xc2c11689be000000ULL, "-37576995766272"sv},
    {0x40c1508000000000ULL, "8865"sv},
    {0xc2c169d9b7800000ULL, "-38292643840000"sv},
    {0x438d9bda20600000ULL, "266692335134310400"sv},
    {0xc0ab780000000000ULL, "-3516"sv},
    {0xc0ce528000000000ULL, "-15525"sv},
    {0xc008000000000000ULL, "-3"sv},
    {0xc2a4d51046400000ULL, "-11452666814464"sv},
    {0xc3d87a9ecb000000ULL, "-7055587195138081000"sv},
    {0xc38fc0468c000000ULL, "-285988272226697200"sv},
    {0x410b3b4000000000ULL, "223080"sv},
  };
  const EscapeCase kEscapes[] = {
    {"\000"sv, "\"\\u0000\""sv},
    {"\001"sv, "\"\\u0001\""sv},
    {"\002"sv, "\"\\u0002\""sv},
    {"\003"sv, "\"\\u0003\""sv},
    {"\004"sv, "\"\\u0004\""sv},
    {"\005"sv, "\"\\u0005\""sv},
    {"\006"sv, "\"\\u0006\""sv},
    {"\007"sv, "\"\\u0007\""sv},
    {"\010"sv, "\"\\b\""sv},
    {"\011"sv, "\"\\t\""sv},
    {"\012"sv, "\"\\n\""sv},
    {"\013"sv, "\"\\u000b\""sv},
    {"\014"sv, "\"\\f\""sv},
    {"\015"sv, "\"\\r\""sv},
    {"\016"sv, "\"\\u000e\""sv},
    {"\017"sv, "\"\\u000f\""sv},
    {"\020"sv, "\"\\u0010\""sv},
    {"\021"sv, "\"\\u0011\""sv},
    {"\022"sv, "\"\\u0012\""sv},
    {"\023"sv, "\"\\u0013\""sv},
    {"\024"sv, "\"\\u0014\""sv},
    {"\025"sv, "\"\\u0015\""sv},
    {"\026"sv, "\"\\u0016\""sv},
    {"\027"sv, "\"\\u0017\""sv},
    {"\030"sv, "\"\\u0018\""sv},
    {"\031"sv, "\"\\u0019\""sv},
    {"\032"sv, "\"\\u001a\""sv},
    {"\033"sv, "\"\\u001b\""sv},
    {"\034"sv, "\"\\u001c\""sv},
    {"\035"sv, "\"\\u001d\""sv},
    {"\036"sv, "\"\\u001e\""sv},
    {"\037"sv, "\"\\u001f\""sv},
    {" "sv, "\" \""sv},
    {"!"sv, "\"!\""sv},
    {"\""sv, "\"\\\"\""sv},
    {"#"sv, "\"#\""sv},
    {"$"sv, "\"$\""sv},
    {"%"sv, "\"%\""sv},
    {"&"sv, "\"&\""sv},
    {"'"sv, "\"'\""sv},
    {"("sv, "\"(\""sv},
    {")"sv, "\")\""sv},
    {"*"sv, "\"*\""sv},
    {"+"sv, "\"+\""sv},
    {","sv, "\",\""sv},
    {"-"sv, "\"-\""sv},
    {"."sv, "\".\""sv},
    {"/"sv, "\"/\""sv},
    {"0"sv, "\"0\""sv},
    {"1"sv, "\"1\""sv},
    {"2"sv, "\"2\""sv},
    {"3"sv, "\"3\""sv},
    {"4"sv, "\"4\""sv},
    {"5"sv, "\"5\""sv},
    {"6"sv, "\"6\""sv},
    {"7"sv, "\"7\""sv},
    {"8"sv, "\"8\""sv},
    {"9"sv, "\"9\""sv},
    {":"sv, "\":\""sv},
    {";"sv, "\";\""sv},
    {"<"sv, "\"<\""sv},
    {"="sv, "\"=\""sv},
    {">"sv, "\">\""sv},
    {"\?"sv, "\"\?\""sv},
    {"@"sv, "\"@\""sv},
    {"A"sv, "\"A\""sv},
    {"B"sv, "\"B\""sv},
    {"C"sv, "\"C\""sv},
    {"D"sv, "\"D\""sv},
    {"E"sv, "\"E\""sv},
    {"F"sv, "\"F\""sv},
    {"G"sv, "\"G\""sv},
    {"H"sv, "\"H\""sv},
    {"I"sv, "\"I\""sv},
    {"J"sv, "\"J\""sv},
    {"K"sv, "\"K\""sv},
    {"L"sv, "\"L\""sv},
    {"M"sv, "\"M\""sv},
    {"N"sv, "\"N\""sv},
    {"O"sv, "\"O\""sv},
    {"P"sv, "\"P\""sv},
    {"Q"sv, "\"Q\""sv},
    {"R"sv, "\"R\""sv},
    {"S"sv, "\"S\""sv},
    {"T"sv, "\"T\""sv},
    {"U"sv, "\"U\""sv},
    {"V"sv, "\"V\""sv},
    {"W"sv, "\"W\""sv},
    {"X"sv, "\"X\""sv},
    {"Y"sv, "\"Y\""sv},
    {"Z"sv, "\"Z\""sv},
    {"["sv, "\"[\""sv},
    {"\\"sv, "\"\\\\\""sv},
    {"]"sv, "\"]\""sv},
    {"^"sv, "\"^\""sv},
    {"_"sv, "\"_\""sv},
    {"`"sv, "\"`\""sv},
    {"a"sv, "\"a\""sv},
    {"b"sv, "\"b\""sv},
    {"c"sv, "\"c\""sv},
    {"d"sv, "\"d\""sv},
    {"e"sv, "\"e\""sv},
    {"f"sv, "\"f\""sv},
    {"g"sv, "\"g\""sv},
    {"h"sv, "\"h\""sv},
    {"i"sv, "\"i\""sv},
    {"j"sv, "\"j\""sv},
    {"k"sv, "\"k\""sv},
    {"l"sv, "\"l\""sv},
    {"m"sv, "\"m\""sv},
    {"n"sv, "\"n\""sv},
    {"o"sv, "\"o\""sv},
    {"p"sv, "\"p\""sv},
    {"q"sv, "\"q\""sv},
    {"r"sv, "\"r\""sv},
    {"s"sv, "\"s\""sv},
    {"t"sv, "\"t\""sv},
    {"u"sv, "\"u\""sv},
    {"v"sv, "\"v\""sv},
    {"w"sv, "\"w\""sv},
    {"x"sv, "\"x\""sv},
    {"y"sv, "\"y\""sv},
    {"z"sv, "\"z\""sv},
    {"{"sv, "\"{\""sv},
    {"|"sv, "\"|\""sv},
    {"}"sv, "\"}\""sv},
    {"~"sv, "\"~\""sv},
    {"\177"sv, "\"\177\""sv},
    {"\302\200"sv, "\"\302\200\""sv},
    {"\302\237"sv, "\"\302\237\""sv},
    {"\302\240"sv, "\"\302\240\""sv},
    {"\302\255"sv, "\"\302\255\""sv},
    {"\303\277"sv, "\"\303\277\""sv},
    {"\304\200"sv, "\"\304\200\""sv},
    {"\337\277"sv, "\"\337\277\""sv},
    {"\340\240\200"sv, "\"\340\240\200\""sv},
    {"\357\273\277"sv, "\"\357\273\277\""sv},
    {"\357\277\275"sv, "\"\357\277\275\""sv},
    {"\357\277\277"sv, "\"\357\277\277\""sv},
    {"\342\200\250"sv, "\"\342\200\250\""sv},
    {"\342\200\251"sv, "\"\342\200\251\""sv},
    {"\342\200\213"sv, "\"\342\200\213\""sv},
    {"\355\237\277"sv, "\"\355\237\277\""sv},
    {"\356\200\200"sv, "\"\356\200\200\""sv},
    {"\355\240\200"sv, "\"\\ud800\""sv},
    {"\355\257\277"sv, "\"\\udbff\""sv},
    {"\355\260\200"sv, "\"\\udc00\""sv},
    {"\355\277\277"sv, "\"\\udfff\""sv},
    {"\360\237\230\200"sv, "\"\360\237\230\200\""sv},
    {"\355\270\200\355\240\275"sv, "\"\\ude00\\ud83d\""sv},
    {"\355\240\275\355\240\275"sv, "\"\\ud83d\\ud83d\""sv},
    {"\355\270\200\355\270\200"sv, "\"\\ude00\\ude00\""sv},
    {"a\355\240\200b"sv, "\"a\\ud800b\""sv},
    {"\360\220\200\200"sv, "\"\360\220\200\200\""sv},
    {"\364\217\277\277"sv, "\"\364\217\277\277\""sv},
    {"x\355\240\275"sv, "\"x\\ud83d\""sv},
    {"\355\270\200y"sv, "\"\\ude00y\""sv},
    {""sv, "\"\""sv},
    {"plain ascii"sv, "\"plain ascii\""sv},
    {"quote\"back\\slash/"sv, "\"quote\\\"back\\\\slash/\""sv},
    {"tab\011new\012line\015\012"sv, "\"tab\\tnew\\nline\\r\\n\""sv},
    {"\000\001\037\177"sv, "\"\\u0000\\u0001\\u001f\177\""sv},
    {"mixed \303\251\342\202\254\360\237\230\200 \355\240\200 end"sv, "\"mixed \303\251\342\202\254\360\237\230\200 \\ud800 end\""sv},
    {"\342\200\250\342\200\251"sv, "\"\342\200\250\342\200\251\""sv},
    {"\360\237\230\200\355\240\275"sv, "\"\360\237\230\200\\ud83d\""sv},
    {"\355\270\200\360\237\230\200"sv, "\"\\ude00\360\237\230\200\""sv},
    {"\"\355\277\277\012\342\200\250\355\270\200\012\355\240\275"sv, "\"\\\"\\udfff\\n\342\200\250\\ude00\\n\\ud83d\""sv},
    {"\355\277\277\\\355\270\200\342\200\250"sv, "\"\\udfff\\\\\\ude00\342\200\250\""sv},
    {"\355\240\200\355\240\200A\"\\"sv, "\"\\ud800\\ud800A\\\"\\\\\""sv},
    {"\342\202\254\177\177\012\033"sv, "\"\342\202\254\177\177\\n\\u001b\""sv},
    {"\342\200\250\355\270\200\342\202\254A\"\"\303\251\355\240\275"sv, "\"\342\200\250\\ude00\342\202\254A\\\"\\\"\303\251\\ud83d\""sv},
    {"\"\012\177\303\251\033\012"sv, "\"\\\"\\n\177\303\251\\u001b\\n\""sv},
    {"\"\"\355\270\200\355\240\200\"A\355\240\200\342\200\250"sv, "\"\\\"\\\"\\ude00\\ud800\\\"A\\ud800\342\200\250\""sv},
    {"A\\\342\202\254A\177\012\\\303\251"sv, "\"A\\\\\342\202\254A\177\\n\\\\\303\251\""sv},
    {"\303\251A"sv, "\"\303\251A\""sv},
    {"\355\240\275\012\033\355\277\277A\\"sv, "\"\\ud83d\\n\\u001b\\udfffA\\\\\""sv},
    {"\355\277\277\355\240\275\\\012"sv, "\"\\udfff\\ud83d\\\\\\n\""sv},
    {"\355\270\200\355\240\275\\\177\355\277\277"sv, "\"\\ude00\\ud83d\\\\\177\\udfff\""sv},
    {"\360\220\217\277"sv, "\"\360\220\217\277\""sv},
    {"\342\200\250\355\270\200\303\251\342\200\250\303\251\355\270\200\303\251"sv, "\"\342\200\250\\ude00\303\251\342\200\250\303\251\\ude00\303\251\""sv},
    {"\""sv, "\"\\\"\""sv},
    {"\355\240\275\"\342\202\254\355\270\200\355\240\275\033\033\012"sv, "\"\\ud83d\\\"\342\202\254\\ude00\\ud83d\\u001b\\u001b\\n\""sv},
    {"\355\277\277\355\240\275\177\177\012\\"sv, "\"\\udfff\\ud83d\177\177\\n\\\\\""sv},
    {"\\\\\303\251A"sv, "\"\\\\\\\\\303\251A\""sv},
    {"\342\200\250"sv, "\"\342\200\250\""sv},
    {"\342\200\250A\"\\\342\202\254\\"sv, "\"\342\200\250A\\\"\\\\\342\202\254\\\\\""sv},
    {"A\"\342\200\250\342\200\250\355\270\200\012\355\270\200\177"sv, "\"A\\\"\342\200\250\342\200\250\\ude00\\n\\ude00\177\""sv},
    {"\355\270\200\355\240\275\342\202\254"sv, "\"\\ude00\\ud83d\342\202\254\""sv},
    {"\355\277\277A\355\277\277\342\200\250\\\355\277\277\355\277\277"sv, "\"\\udfffA\\udfff\342\200\250\\\\\\udfff\\udfff\""sv},
    {"\012"sv, "\"\\n\""sv},
    {"\303\251\355\240\200\360\220\210\200\355\240\275\355\240\275\342\202\254"sv, "\"\303\251\\ud800\360\220\210\200\\ud83d\\ud83d\342\202\254\""sv},
    {"\177\355\240\275\342\200\250A"sv, "\"\177\\ud83d\342\200\250A\""sv},
    {"A\177"sv, "\"A\177\""sv},
    {"\342\200\250\012\\\355\270\200\012"sv, "\"\342\200\250\\n\\\\\\ude00\\n\""sv},
    {"\"\342\200\250"sv, "\"\\\"\342\200\250\""sv},
    {"A\355\277\277A\177\342\200\250\012"sv, "\"A\\udfffA\177\342\200\250\\n\""sv},
    {"\355\270\200\355\240\275"sv, "\"\\ude00\\ud83d\""sv},
    {"\033\355\270\200A\355\240\200\\\303\251\355\240\275"sv, "\"\\u001b\\ude00A\\ud800\\\\\303\251\\ud83d\""sv},
    {"\303\251\\"sv, "\"\303\251\\\\\""sv},
    {"\342\200\250\355\277\277\033\"\355\240\200\\"sv, "\"\342\200\250\\udfff\\u001b\\\"\\ud800\\\\\""sv},
    {"\355\240\200\177\177\355\240\200\355\240\200\\\012\342\202\254"sv, "\"\\ud800\177\177\\ud800\\ud800\\\\\\n\342\202\254\""sv},
    {"\"A\033\012\355\240\275\342\200\250\342\202\254"sv, "\"\\\"A\\u001b\\n\\ud83d\342\200\250\342\202\254\""sv},
    {"\342\202\254A\\"sv, "\"\342\202\254A\\\\\""sv},
    {"\342\202\254\\\342\200\250A\355\277\277\342\202\254"sv, "\"\342\202\254\\\\\342\200\250A\\udfff\342\202\254\""sv},
    {"\303\251"sv, "\"\303\251\""sv},
    {"\012A"sv, "\"\\nA\""sv},
    {"\360\237\230\200\342\202\254\033"sv, "\"\360\237\230\200\342\202\254\\u001b\""sv},
    {"\355\277\277\355\277\277\355\240\275\177"sv, "\"\\udfff\\udfff\\ud83d\177\""sv},
    {"\360\220\217\277\177\355\277\277A\342\202\254"sv, "\"\360\220\217\277\177\\udfffA\342\202\254\""sv},
    {"\355\270\200A\\\355\277\277"sv, "\"\\ude00A\\\\\\udfff\""sv},
    {"\355\240\275A\355\277\277\033\177\303\251\303\251"sv, "\"\\ud83dA\\udfff\\u001b\177\303\251\303\251\""sv},
    {"\355\270\200\355\270\200\177\355\277\277\\\342\200\250"sv, "\"\\ude00\\ude00\177\\udfff\\\\\342\200\250\""sv},
    {"A\355\270\200A\355\277\277\"\355\240\200\\\\"sv, "\"A\\ude00A\\udfff\\\"\\ud800\\\\\\\\\""sv},
    {"\033\342\202\254\012\355\240\275"sv, "\"\\u001b\342\202\254\\n\\ud83d\""sv},
    {"\033\\"sv, "\"\\u001b\\\\\""sv},
    {"\355\240\275\342\200\250\355\240\200\342\200\250\355\240\275\033"sv, "\"\\ud83d\342\200\250\\ud800\342\200\250\\ud83d\\u001b\""sv},
    {"\355\277\277A\342\200\250\355\277\277\355\240\275\355\240\275\""sv, "\"\\udfffA\342\200\250\\udfff\\ud83d\\ud83d\\\"\""sv},
    {"\355\270\200\177\012\012\""sv, "\"\\ude00\177\\n\\n\\\"\""sv},
    {"\355\277\277"sv, "\"\\udfff\""sv},
    {"\012"sv, "\"\\n\""sv},
    {"\\\177\355\240\275\"\355\270\200"sv, "\"\\\\\177\\ud83d\\\"\\ude00\""sv},
    {"\355\240\275\012\"\012\342\200\250"sv, "\"\\ud83d\\n\\\"\\n\342\200\250\""sv},
    {"\"\177\""sv, "\"\\\"\177\\\"\""sv},
    {"\355\277\277"sv, "\"\\udfff\""sv},
    {"\342\200\250\355\270\200\355\240\200\033\303\251"sv, "\"\342\200\250\\ude00\\ud800\\u001b\303\251\""sv},
    {"\012"sv, "\"\\n\""sv},
  };
  const IndentCase kIndents[] = {
    {"null"sv, 2, ""sv, false, "null"sv},
    {"null"sv, 0, ""sv, false, "null"sv},
    {"null"sv, 1, ""sv, false, "null"sv},
    {"null"sv, 4, ""sv, false, "null"sv},
    {"null"sv, 10, ""sv, false, "null"sv},
    {"null"sv, 11, ""sv, false, "null"sv},
    {"null"sv, -3, ""sv, false, "null"sv},
    {"null"sv, 0, "\011"sv, true, "null"sv},
    {"null"sv, 0, ""sv, true, "null"sv},
    {"null"sv, 0, "ab"sv, true, "null"sv},
    {"null"sv, 0, "12345678901"sv, true, "null"sv},
    {"null"sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "null"sv},
    {"null"sv, 0, "123456789\360\237\230\200"sv, true, "null"sv},
    {"null"sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "null"sv},
    {"true"sv, 2, ""sv, false, "true"sv},
    {"true"sv, 0, ""sv, false, "true"sv},
    {"true"sv, 1, ""sv, false, "true"sv},
    {"true"sv, 4, ""sv, false, "true"sv},
    {"true"sv, 10, ""sv, false, "true"sv},
    {"true"sv, 11, ""sv, false, "true"sv},
    {"true"sv, -3, ""sv, false, "true"sv},
    {"true"sv, 0, "\011"sv, true, "true"sv},
    {"true"sv, 0, ""sv, true, "true"sv},
    {"true"sv, 0, "ab"sv, true, "true"sv},
    {"true"sv, 0, "12345678901"sv, true, "true"sv},
    {"true"sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "true"sv},
    {"true"sv, 0, "123456789\360\237\230\200"sv, true, "true"sv},
    {"true"sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "true"sv},
    {"0"sv, 2, ""sv, false, "0"sv},
    {"0"sv, 0, ""sv, false, "0"sv},
    {"0"sv, 1, ""sv, false, "0"sv},
    {"0"sv, 4, ""sv, false, "0"sv},
    {"0"sv, 10, ""sv, false, "0"sv},
    {"0"sv, 11, ""sv, false, "0"sv},
    {"0"sv, -3, ""sv, false, "0"sv},
    {"0"sv, 0, "\011"sv, true, "0"sv},
    {"0"sv, 0, ""sv, true, "0"sv},
    {"0"sv, 0, "ab"sv, true, "0"sv},
    {"0"sv, 0, "12345678901"sv, true, "0"sv},
    {"0"sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "0"sv},
    {"0"sv, 0, "123456789\360\237\230\200"sv, true, "0"sv},
    {"0"sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "0"sv},
    {"\"\""sv, 2, ""sv, false, "\"\""sv},
    {"\"\""sv, 0, ""sv, false, "\"\""sv},
    {"\"\""sv, 1, ""sv, false, "\"\""sv},
    {"\"\""sv, 4, ""sv, false, "\"\""sv},
    {"\"\""sv, 10, ""sv, false, "\"\""sv},
    {"\"\""sv, 11, ""sv, false, "\"\""sv},
    {"\"\""sv, -3, ""sv, false, "\"\""sv},
    {"\"\""sv, 0, "\011"sv, true, "\"\""sv},
    {"\"\""sv, 0, ""sv, true, "\"\""sv},
    {"\"\""sv, 0, "ab"sv, true, "\"\""sv},
    {"\"\""sv, 0, "12345678901"sv, true, "\"\""sv},
    {"\"\""sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "\"\""sv},
    {"\"\""sv, 0, "123456789\360\237\230\200"sv, true, "\"\""sv},
    {"\"\""sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "\"\""sv},
    {"[]"sv, 2, ""sv, false, "[]"sv},
    {"[]"sv, 0, ""sv, false, "[]"sv},
    {"[]"sv, 1, ""sv, false, "[]"sv},
    {"[]"sv, 4, ""sv, false, "[]"sv},
    {"[]"sv, 10, ""sv, false, "[]"sv},
    {"[]"sv, 11, ""sv, false, "[]"sv},
    {"[]"sv, -3, ""sv, false, "[]"sv},
    {"[]"sv, 0, "\011"sv, true, "[]"sv},
    {"[]"sv, 0, ""sv, true, "[]"sv},
    {"[]"sv, 0, "ab"sv, true, "[]"sv},
    {"[]"sv, 0, "12345678901"sv, true, "[]"sv},
    {"[]"sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "[]"sv},
    {"[]"sv, 0, "123456789\360\237\230\200"sv, true, "[]"sv},
    {"[]"sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "[]"sv},
    {"{}"sv, 2, ""sv, false, "{}"sv},
    {"{}"sv, 0, ""sv, false, "{}"sv},
    {"{}"sv, 1, ""sv, false, "{}"sv},
    {"{}"sv, 4, ""sv, false, "{}"sv},
    {"{}"sv, 10, ""sv, false, "{}"sv},
    {"{}"sv, 11, ""sv, false, "{}"sv},
    {"{}"sv, -3, ""sv, false, "{}"sv},
    {"{}"sv, 0, "\011"sv, true, "{}"sv},
    {"{}"sv, 0, ""sv, true, "{}"sv},
    {"{}"sv, 0, "ab"sv, true, "{}"sv},
    {"{}"sv, 0, "12345678901"sv, true, "{}"sv},
    {"{}"sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "{}"sv},
    {"{}"sv, 0, "123456789\360\237\230\200"sv, true, "{}"sv},
    {"{}"sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "{}"sv},
    {"[[]]"sv, 2, ""sv, false, "[\012  []\012]"sv},
    {"[[]]"sv, 0, ""sv, false, "[[]]"sv},
    {"[[]]"sv, 1, ""sv, false, "[\012 []\012]"sv},
    {"[[]]"sv, 4, ""sv, false, "[\012    []\012]"sv},
    {"[[]]"sv, 10, ""sv, false, "[\012          []\012]"sv},
    {"[[]]"sv, 11, ""sv, false, "[\012          []\012]"sv},
    {"[[]]"sv, -3, ""sv, false, "[[]]"sv},
    {"[[]]"sv, 0, "\011"sv, true, "[\012\011[]\012]"sv},
    {"[[]]"sv, 0, ""sv, true, "[[]]"sv},
    {"[[]]"sv, 0, "ab"sv, true, "[\012ab[]\012]"sv},
    {"[[]]"sv, 0, "12345678901"sv, true, "[\0121234567890[]\012]"sv},
    {"[[]]"sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "[\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251[]\012]"sv},
    {"[[]]"sv, 0, "123456789\360\237\230\200"sv, true, "[\012123456789\355\240\275[]\012]"sv},
    {"[[]]"sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "[\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200[]\012]"sv},
    {"{\"a\":{}}"sv, 2, ""sv, false, "{\012  \"a\": {}\012}"sv},
    {"{\"a\":{}}"sv, 0, ""sv, false, "{\"a\":{}}"sv},
    {"{\"a\":{}}"sv, 1, ""sv, false, "{\012 \"a\": {}\012}"sv},
    {"{\"a\":{}}"sv, 4, ""sv, false, "{\012    \"a\": {}\012}"sv},
    {"{\"a\":{}}"sv, 10, ""sv, false, "{\012          \"a\": {}\012}"sv},
    {"{\"a\":{}}"sv, 11, ""sv, false, "{\012          \"a\": {}\012}"sv},
    {"{\"a\":{}}"sv, -3, ""sv, false, "{\"a\":{}}"sv},
    {"{\"a\":{}}"sv, 0, "\011"sv, true, "{\012\011\"a\": {}\012}"sv},
    {"{\"a\":{}}"sv, 0, ""sv, true, "{\"a\":{}}"sv},
    {"{\"a\":{}}"sv, 0, "ab"sv, true, "{\012ab\"a\": {}\012}"sv},
    {"{\"a\":{}}"sv, 0, "12345678901"sv, true, "{\0121234567890\"a\": {}\012}"sv},
    {"{\"a\":{}}"sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "{\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"a\": {}\012}"sv},
    {"{\"a\":{}}"sv, 0, "123456789\360\237\230\200"sv, true, "{\012123456789\355\240\275\"a\": {}\012}"sv},
    {"{\"a\":{}}"sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "{\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"a\": {}\012}"sv},
    {"{\"a\":[]}"sv, 2, ""sv, false, "{\012  \"a\": []\012}"sv},
    {"{\"a\":[]}"sv, 0, ""sv, false, "{\"a\":[]}"sv},
    {"{\"a\":[]}"sv, 1, ""sv, false, "{\012 \"a\": []\012}"sv},
    {"{\"a\":[]}"sv, 4, ""sv, false, "{\012    \"a\": []\012}"sv},
    {"{\"a\":[]}"sv, 10, ""sv, false, "{\012          \"a\": []\012}"sv},
    {"{\"a\":[]}"sv, 11, ""sv, false, "{\012          \"a\": []\012}"sv},
    {"{\"a\":[]}"sv, -3, ""sv, false, "{\"a\":[]}"sv},
    {"{\"a\":[]}"sv, 0, "\011"sv, true, "{\012\011\"a\": []\012}"sv},
    {"{\"a\":[]}"sv, 0, ""sv, true, "{\"a\":[]}"sv},
    {"{\"a\":[]}"sv, 0, "ab"sv, true, "{\012ab\"a\": []\012}"sv},
    {"{\"a\":[]}"sv, 0, "12345678901"sv, true, "{\0121234567890\"a\": []\012}"sv},
    {"{\"a\":[]}"sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "{\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"a\": []\012}"sv},
    {"{\"a\":[]}"sv, 0, "123456789\360\237\230\200"sv, true, "{\012123456789\355\240\275\"a\": []\012}"sv},
    {"{\"a\":[]}"sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "{\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"a\": []\012}"sv},
    {"[1,2,3]"sv, 2, ""sv, false, "[\012  1,\012  2,\012  3\012]"sv},
    {"[1,2,3]"sv, 0, ""sv, false, "[1,2,3]"sv},
    {"[1,2,3]"sv, 1, ""sv, false, "[\012 1,\012 2,\012 3\012]"sv},
    {"[1,2,3]"sv, 4, ""sv, false, "[\012    1,\012    2,\012    3\012]"sv},
    {"[1,2,3]"sv, 10, ""sv, false, "[\012          1,\012          2,\012          3\012]"sv},
    {"[1,2,3]"sv, 11, ""sv, false, "[\012          1,\012          2,\012          3\012]"sv},
    {"[1,2,3]"sv, -3, ""sv, false, "[1,2,3]"sv},
    {"[1,2,3]"sv, 0, "\011"sv, true, "[\012\0111,\012\0112,\012\0113\012]"sv},
    {"[1,2,3]"sv, 0, ""sv, true, "[1,2,3]"sv},
    {"[1,2,3]"sv, 0, "ab"sv, true, "[\012ab1,\012ab2,\012ab3\012]"sv},
    {"[1,2,3]"sv, 0, "12345678901"sv, true, "[\01212345678901,\01212345678902,\01212345678903\012]"sv},
    {"[1,2,3]"sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "[\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\2511,\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\2512,\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\2513\012]"sv},
    {"[1,2,3]"sv, 0, "123456789\360\237\230\200"sv, true, "[\012123456789\355\240\2751,\012123456789\355\240\2752,\012123456789\355\240\2753\012]"sv},
    {"[1,2,3]"sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "[\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\2001,\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\2002,\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\2003\012]"sv},
    {"{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv, 2, ""sv, false, "{\012  \"a\": 1,\012  \"b\": [\012    1,\012    {\012      \"c\": null\012    }\012  ],\012  \"d\": {\012    \"e\": \"f\"\012  }\012}"sv},
    {"{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv, 0, ""sv, false, "{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv},
    {"{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv, 1, ""sv, false, "{\012 \"a\": 1,\012 \"b\": [\012  1,\012  {\012   \"c\": null\012  }\012 ],\012 \"d\": {\012  \"e\": \"f\"\012 }\012}"sv},
    {"{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv, 4, ""sv, false, "{\012    \"a\": 1,\012    \"b\": [\012        1,\012        {\012            \"c\": null\012        }\012    ],\012    \"d\": {\012        \"e\": \"f\"\012    }\012}"sv},
    {"{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv, 10, ""sv, false, "{\012          \"a\": 1,\012          \"b\": [\012                    1,\012                    {\012                              \"c\": null\012                    }\012          ],\012          \"d\": {\012                    \"e\": \"f\"\012          }\012}"sv},
    {"{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv, 11, ""sv, false, "{\012          \"a\": 1,\012          \"b\": [\012                    1,\012                    {\012                              \"c\": null\012                    }\012          ],\012          \"d\": {\012                    \"e\": \"f\"\012          }\012}"sv},
    {"{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv, -3, ""sv, false, "{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv},
    {"{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv, 0, "\011"sv, true, "{\012\011\"a\": 1,\012\011\"b\": [\012\011\0111,\012\011\011{\012\011\011\011\"c\": null\012\011\011}\012\011],\012\011\"d\": {\012\011\011\"e\": \"f\"\012\011}\012}"sv},
    {"{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv, 0, ""sv, true, "{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv},
    {"{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv, 0, "ab"sv, true, "{\012ab\"a\": 1,\012ab\"b\": [\012abab1,\012abab{\012ababab\"c\": null\012abab}\012ab],\012ab\"d\": {\012abab\"e\": \"f\"\012ab}\012}"sv},
    {"{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv, 0, "12345678901"sv, true, "{\0121234567890\"a\": 1,\0121234567890\"b\": [\012123456789012345678901,\01212345678901234567890{\012123456789012345678901234567890\"c\": null\01212345678901234567890}\0121234567890],\0121234567890\"d\": {\01212345678901234567890\"e\": \"f\"\0121234567890}\012}"sv},
    {"{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "{\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"a\": 1,\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"b\": [\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\2511,\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251{\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"c\": null\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251}\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251],\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"d\": {\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"e\": \"f\"\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251}\012}"sv},
    {"{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv, 0, "123456789\360\237\230\200"sv, true, "{\012123456789\355\240\275\"a\": 1,\012123456789\355\240\275\"b\": [\012123456789\355\240\275123456789\355\240\2751,\012123456789\355\240\275123456789\355\240\275{\012123456789\355\240\275123456789\355\240\275123456789\355\240\275\"c\": null\012123456789\355\240\275123456789\355\240\275}\012123456789\355\240\275],\012123456789\355\240\275\"d\": {\012123456789\355\240\275123456789\355\240\275\"e\": \"f\"\012123456789\355\240\275}\012}"sv},
    {"{\"a\":1,\"b\":[1,{\"c\":null}],\"d\":{\"e\":\"f\"}}"sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "{\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"a\": 1,\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"b\": [\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\2001,\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200{\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"c\": null\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200}\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200],\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"d\": {\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"e\": \"f\"\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200}\012}"sv},
    {"[{\"x\":[[],{}]},\"s\",-0.5,true]"sv, 2, ""sv, false, "[\012  {\012    \"x\": [\012      [],\012      {}\012    ]\012  },\012  \"s\",\012  -0.5,\012  true\012]"sv},
    {"[{\"x\":[[],{}]},\"s\",-0.5,true]"sv, 0, ""sv, false, "[{\"x\":[[],{}]},\"s\",-0.5,true]"sv},
    {"[{\"x\":[[],{}]},\"s\",-0.5,true]"sv, 1, ""sv, false, "[\012 {\012  \"x\": [\012   [],\012   {}\012  ]\012 },\012 \"s\",\012 -0.5,\012 true\012]"sv},
    {"[{\"x\":[[],{}]},\"s\",-0.5,true]"sv, 4, ""sv, false, "[\012    {\012        \"x\": [\012            [],\012            {}\012        ]\012    },\012    \"s\",\012    -0.5,\012    true\012]"sv},
    {"[{\"x\":[[],{}]},\"s\",-0.5,true]"sv, 10, ""sv, false, "[\012          {\012                    \"x\": [\012                              [],\012                              {}\012                    ]\012          },\012          \"s\",\012          -0.5,\012          true\012]"sv},
    {"[{\"x\":[[],{}]},\"s\",-0.5,true]"sv, 11, ""sv, false, "[\012          {\012                    \"x\": [\012                              [],\012                              {}\012                    ]\012          },\012          \"s\",\012          -0.5,\012          true\012]"sv},
    {"[{\"x\":[[],{}]},\"s\",-0.5,true]"sv, -3, ""sv, false, "[{\"x\":[[],{}]},\"s\",-0.5,true]"sv},
    {"[{\"x\":[[],{}]},\"s\",-0.5,true]"sv, 0, "\011"sv, true, "[\012\011{\012\011\011\"x\": [\012\011\011\011[],\012\011\011\011{}\012\011\011]\012\011},\012\011\"s\",\012\011-0.5,\012\011true\012]"sv},
    {"[{\"x\":[[],{}]},\"s\",-0.5,true]"sv, 0, ""sv, true, "[{\"x\":[[],{}]},\"s\",-0.5,true]"sv},
    {"[{\"x\":[[],{}]},\"s\",-0.5,true]"sv, 0, "ab"sv, true, "[\012ab{\012abab\"x\": [\012ababab[],\012ababab{}\012abab]\012ab},\012ab\"s\",\012ab-0.5,\012abtrue\012]"sv},
    {"[{\"x\":[[],{}]},\"s\",-0.5,true]"sv, 0, "12345678901"sv, true, "[\0121234567890{\01212345678901234567890\"x\": [\012123456789012345678901234567890[],\012123456789012345678901234567890{}\01212345678901234567890]\0121234567890},\0121234567890\"s\",\0121234567890-0.5,\0121234567890true\012]"sv},
    {"[{\"x\":[[],{}]},\"s\",-0.5,true]"sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "[\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251{\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"x\": [\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251[],\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251{}\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251]\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251},\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"s\",\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251-0.5,\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251true\012]"sv},
    {"[{\"x\":[[],{}]},\"s\",-0.5,true]"sv, 0, "123456789\360\237\230\200"sv, true, "[\012123456789\355\240\275{\012123456789\355\240\275123456789\355\240\275\"x\": [\012123456789\355\240\275123456789\355\240\275123456789\355\240\275[],\012123456789\355\240\275123456789\355\240\275123456789\355\240\275{}\012123456789\355\240\275123456789\355\240\275]\012123456789\355\240\275},\012123456789\355\240\275\"s\",\012123456789\355\240\275-0.5,\012123456789\355\240\275true\012]"sv},
    {"[{\"x\":[[],{}]},\"s\",-0.5,true]"sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "[\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200{\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"x\": [\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200[],\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200{}\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200]\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200},\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"s\",\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200-0.5,\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200true\012]"sv},
    {"{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv, 2, ""sv, false, "{\012  \"z\": 1,\012  \"10\": 2,\012  \"2\": 3,\012  \"a\": {\012    \"1\": 1,\012    \"0\": 0\012  }\012}"sv},
    {"{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv, 0, ""sv, false, "{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv},
    {"{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv, 1, ""sv, false, "{\012 \"z\": 1,\012 \"10\": 2,\012 \"2\": 3,\012 \"a\": {\012  \"1\": 1,\012  \"0\": 0\012 }\012}"sv},
    {"{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv, 4, ""sv, false, "{\012    \"z\": 1,\012    \"10\": 2,\012    \"2\": 3,\012    \"a\": {\012        \"1\": 1,\012        \"0\": 0\012    }\012}"sv},
    {"{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv, 10, ""sv, false, "{\012          \"z\": 1,\012          \"10\": 2,\012          \"2\": 3,\012          \"a\": {\012                    \"1\": 1,\012                    \"0\": 0\012          }\012}"sv},
    {"{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv, 11, ""sv, false, "{\012          \"z\": 1,\012          \"10\": 2,\012          \"2\": 3,\012          \"a\": {\012                    \"1\": 1,\012                    \"0\": 0\012          }\012}"sv},
    {"{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv, -3, ""sv, false, "{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv},
    {"{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv, 0, "\011"sv, true, "{\012\011\"z\": 1,\012\011\"10\": 2,\012\011\"2\": 3,\012\011\"a\": {\012\011\011\"1\": 1,\012\011\011\"0\": 0\012\011}\012}"sv},
    {"{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv, 0, ""sv, true, "{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv},
    {"{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv, 0, "ab"sv, true, "{\012ab\"z\": 1,\012ab\"10\": 2,\012ab\"2\": 3,\012ab\"a\": {\012abab\"1\": 1,\012abab\"0\": 0\012ab}\012}"sv},
    {"{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv, 0, "12345678901"sv, true, "{\0121234567890\"z\": 1,\0121234567890\"10\": 2,\0121234567890\"2\": 3,\0121234567890\"a\": {\01212345678901234567890\"1\": 1,\01212345678901234567890\"0\": 0\0121234567890}\012}"sv},
    {"{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "{\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"z\": 1,\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"10\": 2,\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"2\": 3,\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"a\": {\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"1\": 1,\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"0\": 0\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251}\012}"sv},
    {"{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv, 0, "123456789\360\237\230\200"sv, true, "{\012123456789\355\240\275\"z\": 1,\012123456789\355\240\275\"10\": 2,\012123456789\355\240\275\"2\": 3,\012123456789\355\240\275\"a\": {\012123456789\355\240\275123456789\355\240\275\"1\": 1,\012123456789\355\240\275123456789\355\240\275\"0\": 0\012123456789\355\240\275}\012}"sv},
    {"{\"z\":1,\"10\":2,\"2\":3,\"a\":{\"1\":1,\"0\":0}}"sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "{\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"z\": 1,\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"10\": 2,\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"2\": 3,\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"a\": {\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"1\": 1,\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"0\": 0\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200}\012}"sv},
    {"[[[1]],[[2,[3]]]]"sv, 2, ""sv, false, "[\012  [\012    [\012      1\012    ]\012  ],\012  [\012    [\012      2,\012      [\012        3\012      ]\012    ]\012  ]\012]"sv},
    {"[[[1]],[[2,[3]]]]"sv, 0, ""sv, false, "[[[1]],[[2,[3]]]]"sv},
    {"[[[1]],[[2,[3]]]]"sv, 1, ""sv, false, "[\012 [\012  [\012   1\012  ]\012 ],\012 [\012  [\012   2,\012   [\012    3\012   ]\012  ]\012 ]\012]"sv},
    {"[[[1]],[[2,[3]]]]"sv, 4, ""sv, false, "[\012    [\012        [\012            1\012        ]\012    ],\012    [\012        [\012            2,\012            [\012                3\012            ]\012        ]\012    ]\012]"sv},
    {"[[[1]],[[2,[3]]]]"sv, 10, ""sv, false, "[\012          [\012                    [\012                              1\012                    ]\012          ],\012          [\012                    [\012                              2,\012                              [\012                                        3\012                              ]\012                    ]\012          ]\012]"sv},
    {"[[[1]],[[2,[3]]]]"sv, 11, ""sv, false, "[\012          [\012                    [\012                              1\012                    ]\012          ],\012          [\012                    [\012                              2,\012                              [\012                                        3\012                              ]\012                    ]\012          ]\012]"sv},
    {"[[[1]],[[2,[3]]]]"sv, -3, ""sv, false, "[[[1]],[[2,[3]]]]"sv},
    {"[[[1]],[[2,[3]]]]"sv, 0, "\011"sv, true, "[\012\011[\012\011\011[\012\011\011\0111\012\011\011]\012\011],\012\011[\012\011\011[\012\011\011\0112,\012\011\011\011[\012\011\011\011\0113\012\011\011\011]\012\011\011]\012\011]\012]"sv},
    {"[[[1]],[[2,[3]]]]"sv, 0, ""sv, true, "[[[1]],[[2,[3]]]]"sv},
    {"[[[1]],[[2,[3]]]]"sv, 0, "ab"sv, true, "[\012ab[\012abab[\012ababab1\012abab]\012ab],\012ab[\012abab[\012ababab2,\012ababab[\012abababab3\012ababab]\012abab]\012ab]\012]"sv},
    {"[[[1]],[[2,[3]]]]"sv, 0, "12345678901"sv, true, "[\0121234567890[\01212345678901234567890[\0121234567890123456789012345678901\01212345678901234567890]\0121234567890],\0121234567890[\01212345678901234567890[\0121234567890123456789012345678902,\012123456789012345678901234567890[\01212345678901234567890123456789012345678903\012123456789012345678901234567890]\01212345678901234567890]\0121234567890]\012]"sv},
    {"[[[1]],[[2,[3]]]]"sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "[\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251[\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251[\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\2511\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251]\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251],\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251[\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251[\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\2512,\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251[\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\2513\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251]\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251]\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251]\012]"sv},
    {"[[[1]],[[2,[3]]]]"sv, 0, "123456789\360\237\230\200"sv, true, "[\012123456789\355\240\275[\012123456789\355\240\275123456789\355\240\275[\012123456789\355\240\275123456789\355\240\275123456789\355\240\2751\012123456789\355\240\275123456789\355\240\275]\012123456789\355\240\275],\012123456789\355\240\275[\012123456789\355\240\275123456789\355\240\275[\012123456789\355\240\275123456789\355\240\275123456789\355\240\2752,\012123456789\355\240\275123456789\355\240\275123456789\355\240\275[\012123456789\355\240\275123456789\355\240\275123456789\355\240\275123456789\355\240\2753\012123456789\355\240\275123456789\355\240\275123456789\355\240\275]\012123456789\355\240\275123456789\355\240\275]\012123456789\355\240\275]\012]"sv},
    {"[[[1]],[[2,[3]]]]"sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "[\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200[\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200[\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\2001\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200]\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200],\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200[\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200[\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\2002,\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200[\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\2003\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200]\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200]\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200]\012]"sv},
    {"{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv, 2, ""sv, false, "{\012  \"\303\251\": \"\360\237\230\200\",\012  \"k\\\"ey\": \"v\\nal\"\012}"sv},
    {"{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv, 0, ""sv, false, "{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv},
    {"{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv, 1, ""sv, false, "{\012 \"\303\251\": \"\360\237\230\200\",\012 \"k\\\"ey\": \"v\\nal\"\012}"sv},
    {"{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv, 4, ""sv, false, "{\012    \"\303\251\": \"\360\237\230\200\",\012    \"k\\\"ey\": \"v\\nal\"\012}"sv},
    {"{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv, 10, ""sv, false, "{\012          \"\303\251\": \"\360\237\230\200\",\012          \"k\\\"ey\": \"v\\nal\"\012}"sv},
    {"{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv, 11, ""sv, false, "{\012          \"\303\251\": \"\360\237\230\200\",\012          \"k\\\"ey\": \"v\\nal\"\012}"sv},
    {"{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv, -3, ""sv, false, "{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv},
    {"{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv, 0, "\011"sv, true, "{\012\011\"\303\251\": \"\360\237\230\200\",\012\011\"k\\\"ey\": \"v\\nal\"\012}"sv},
    {"{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv, 0, ""sv, true, "{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv},
    {"{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv, 0, "ab"sv, true, "{\012ab\"\303\251\": \"\360\237\230\200\",\012ab\"k\\\"ey\": \"v\\nal\"\012}"sv},
    {"{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv, 0, "12345678901"sv, true, "{\0121234567890\"\303\251\": \"\360\237\230\200\",\0121234567890\"k\\\"ey\": \"v\\nal\"\012}"sv},
    {"{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "{\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"\303\251\": \"\360\237\230\200\",\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"k\\\"ey\": \"v\\nal\"\012}"sv},
    {"{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv, 0, "123456789\360\237\230\200"sv, true, "{\012123456789\355\240\275\"\303\251\": \"\360\237\230\200\",\012123456789\355\240\275\"k\\\"ey\": \"v\\nal\"\012}"sv},
    {"{\"\303\251\":\"\360\237\230\200\",\"k\\\"ey\":\"v\\nal\"}"sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "{\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"\303\251\": \"\360\237\230\200\",\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"k\\\"ey\": \"v\\nal\"\012}"sv},
    {"{\"__proto__\":{\"a\":1},\"b\":2}"sv, 2, ""sv, false, "{\012  \"__proto__\": {\012    \"a\": 1\012  },\012  \"b\": 2\012}"sv},
    {"{\"__proto__\":{\"a\":1},\"b\":2}"sv, 0, ""sv, false, "{\"__proto__\":{\"a\":1},\"b\":2}"sv},
    {"{\"__proto__\":{\"a\":1},\"b\":2}"sv, 1, ""sv, false, "{\012 \"__proto__\": {\012  \"a\": 1\012 },\012 \"b\": 2\012}"sv},
    {"{\"__proto__\":{\"a\":1},\"b\":2}"sv, 4, ""sv, false, "{\012    \"__proto__\": {\012        \"a\": 1\012    },\012    \"b\": 2\012}"sv},
    {"{\"__proto__\":{\"a\":1},\"b\":2}"sv, 10, ""sv, false, "{\012          \"__proto__\": {\012                    \"a\": 1\012          },\012          \"b\": 2\012}"sv},
    {"{\"__proto__\":{\"a\":1},\"b\":2}"sv, 11, ""sv, false, "{\012          \"__proto__\": {\012                    \"a\": 1\012          },\012          \"b\": 2\012}"sv},
    {"{\"__proto__\":{\"a\":1},\"b\":2}"sv, -3, ""sv, false, "{\"__proto__\":{\"a\":1},\"b\":2}"sv},
    {"{\"__proto__\":{\"a\":1},\"b\":2}"sv, 0, "\011"sv, true, "{\012\011\"__proto__\": {\012\011\011\"a\": 1\012\011},\012\011\"b\": 2\012}"sv},
    {"{\"__proto__\":{\"a\":1},\"b\":2}"sv, 0, ""sv, true, "{\"__proto__\":{\"a\":1},\"b\":2}"sv},
    {"{\"__proto__\":{\"a\":1},\"b\":2}"sv, 0, "ab"sv, true, "{\012ab\"__proto__\": {\012abab\"a\": 1\012ab},\012ab\"b\": 2\012}"sv},
    {"{\"__proto__\":{\"a\":1},\"b\":2}"sv, 0, "12345678901"sv, true, "{\0121234567890\"__proto__\": {\01212345678901234567890\"a\": 1\0121234567890},\0121234567890\"b\": 2\012}"sv},
    {"{\"__proto__\":{\"a\":1},\"b\":2}"sv, 0, "\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251"sv, true, "{\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"__proto__\": {\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"a\": 1\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251},\012\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\"b\": 2\012}"sv},
    {"{\"__proto__\":{\"a\":1},\"b\":2}"sv, 0, "123456789\360\237\230\200"sv, true, "{\012123456789\355\240\275\"__proto__\": {\012123456789\355\240\275123456789\355\240\275\"a\": 1\012123456789\355\240\275},\012123456789\355\240\275\"b\": 2\012}"sv},
    {"{\"__proto__\":{\"a\":1},\"b\":2}"sv, 0, "\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200"sv, true, "{\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"__proto__\": {\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"a\": 1\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200},\012\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\360\237\230\200\"b\": 2\012}"sv},
  };
  const CoerceCase kCoercions[] = {
    {"null"sv, "null"sv, 0x0000000000000000ULL, false, "object"sv},
    {"true"sv, "true"sv, 0x3ff0000000000000ULL, true, "boolean"sv},
    {"false"sv, "false"sv, 0x0000000000000000ULL, false, "boolean"sv},
    {"0"sv, "0"sv, 0x0000000000000000ULL, false, "number"sv},
    {"-0"sv, "0"sv, 0x8000000000000000ULL, false, "number"sv},
    {"1"sv, "1"sv, 0x3ff0000000000000ULL, true, "number"sv},
    {"-1.5"sv, "-1.5"sv, 0xbff8000000000000ULL, true, "number"sv},
    {"1e21"sv, "1e+21"sv, 0x444b1ae4d6e2ef50ULL, true, "number"sv},
    {"1e-7"sv, "1e-7"sv, 0x3e7ad7f29abcaf48ULL, true, "number"sv},
    {"\"\""sv, ""sv, 0x0000000000000000ULL, false, "string"sv},
    {"\" \""sv, " "sv, 0x0000000000000000ULL, true, "string"sv},
    {"\" 12 \""sv, " 12 "sv, 0x4028000000000000ULL, true, "string"sv},
    {"\"0x1f\""sv, "0x1f"sv, 0x403f000000000000ULL, true, "string"sv},
    {"\"0b101\""sv, "0b101"sv, 0x4014000000000000ULL, true, "string"sv},
    {"\"1e3\""sv, "1e3"sv, 0x408f400000000000ULL, true, "string"sv},
    {"\"abc\""sv, "abc"sv, 0x7ff8000000000000ULL, true, "string"sv},
    {"\"Infinity\""sv, "Infinity"sv, 0x7ff0000000000000ULL, true, "string"sv},
    {"\"-Infinity\""sv, "-Infinity"sv, 0xfff0000000000000ULL, true, "string"sv},
    {"\"\\u00a012\\u2028\""sv, "\302\24012\342\200\250"sv, 0x4028000000000000ULL, true, "string"sv},
    {"[]"sv, ""sv, 0x0000000000000000ULL, true, "object"sv},
    {"[1]"sv, "1"sv, 0x3ff0000000000000ULL, true, "object"sv},
    {"[1,2]"sv, "1,2"sv, 0x7ff8000000000000ULL, true, "object"sv},
    {"[null]"sv, ""sv, 0x0000000000000000ULL, true, "object"sv},
    {"[null,1]"sv, ",1"sv, 0x7ff8000000000000ULL, true, "object"sv},
    {"[[]]"sv, ""sv, 0x0000000000000000ULL, true, "object"sv},
    {"[[1],[2,[3]]]"sv, "1,2,3"sv, 0x7ff8000000000000ULL, true, "object"sv},
    {"[\" 7 \"]"sv, " 7 "sv, 0x401c000000000000ULL, true, "object"sv},
    {"[true]"sv, "true"sv, 0x7ff8000000000000ULL, true, "object"sv},
    {"[[null],[]]"sv, ","sv, 0x7ff8000000000000ULL, true, "object"sv},
    {"{}"sv, "[object Object]"sv, 0x7ff8000000000000ULL, true, "object"sv},
    {"{\"a\":1}"sv, "[object Object]"sv, 0x7ff8000000000000ULL, true, "object"sv},
    {"[{}]"sv, "[object Object]"sv, 0x7ff8000000000000ULL, true, "object"sv},
    {"[\"a\",null,\"b\"]"sv, "a,,b"sv, 0x7ff8000000000000ULL, true, "object"sv},
    {"[0.1,1e21]"sv, "0.1,1e+21"sv, 0x7ff8000000000000ULL, true, "object"sv},
    {"[-0]"sv, "0"sv, 0x0000000000000000ULL, true, "object"sv},
    {"\"-0\""sv, "-0"sv, 0x8000000000000000ULL, true, "string"sv},
    {"\"\\ud83d\""sv, "\355\240\275"sv, 0x7ff8000000000000ULL, true, "string"sv},
  };
  const EqualCase kEquality[] = {
    {"0"sv, "-0"sv, false, false},
    {"1"sv, "1"sv, true, false},
    {"1"sv, "1.0"sv, true, false},
    {"[0]"sv, "[-0]"sv, false, false},
    {"{\"a\":0}"sv, "{\"a\":-0}"sv, false, false},
    {"{\"a\":1,\"b\":2}"sv, "{\"b\":2,\"a\":1}"sv, true, true},
    {"[1,2]"sv, "[2,1]"sv, false, false},
    {"[]"sv, "{}"sv, false, true},
    {"{\"a\":[]}"sv, "{\"a\":{}}"sv, false, true},
    {"{\"a\":{}}"sv, "{\"a\":[]}"sv, false, false},
    {"{\"a\":1,\"b\":2}"sv, "{\"a\":1}"sv, false, true},
    {"{\"a\":1}"sv, "{\"a\":1,\"b\":2}"sv, false, false},
    {"{\"a\":[1,2]}"sv, "{\"a\":[1]}"sv, false, false},
    {"{\"a\":[{\"x\":1,\"y\":2}]}"sv, "{\"a\":[{\"x\":1}]}"sv, false, true},
    {"[{\"x\":1,\"y\":2}]"sv, "[{\"x\":1}]"sv, false, true},
    {"{\"a\":{\"b\":1}}"sv, "{\"a\":{}}"sv, false, true},
    {"{\"a\":null}"sv, "{}"sv, false, true},
    {"{\"a\":null}"sv, "{\"a\":null}"sv, true, true},
    {"null"sv, "null"sv, true, false},
    {"\"a\""sv, "\"a\""sv, true, false},
    {"\"a\""sv, "\"b\""sv, false, false},
    {"true"sv, "true"sv, true, false},
    {"true"sv, "1"sv, false, false},
    {"null"sv, "0"sv, false, false},
    {"\"\""sv, "0"sv, false, false},
    {"\"1\""sv, "1"sv, false, false},
    {"{\"a\":[1,[2,3]]}"sv, "{\"a\":[1,[2]]}"sv, false, false},
    {"{\"a\":[{\"b\":[1,2]}]}"sv, "{\"a\":[{\"b\":[1]}]}"sv, false, false},
    {"{\"a\":[{\"b\":{\"c\":1,\"d\":2}}]}"sv, "{\"a\":[{\"b\":{\"c\":1}}]}"sv, false, true},
    {"[1,{\"a\":[]}]"sv, "[1,{\"a\":[]}]"sv, true, true},
    {"{\"x\":{\"y\":{\"z\":[1,{\"w\":null}]}}}"sv, "{\"x\":{\"y\":{\"z\":[1,{\"w\":null}]}}}"sv, true, true},
    {"{\"x\":{\"y\":{\"z\":[1,{\"w\":null}]}}}"sv, "{\"x\":{\"y\":{\"z\":[1,{\"w\":0}]}}}"sv, false, false},
    {"[1,2,3]"sv, "{\"0\":1,\"1\":2,\"2\":3}"sv, false, true},
    {"[1,2,3]"sv, "{\"1\":2}"sv, false, true},
    {"[1,2,3]"sv, "{\"length\":3}"sv, false, true},
    {"[1,2,3]"sv, "{\"length\":2}"sv, false, false},
    {"\"\360\237\230\200\""sv, "\"\\ud83d\\ude00\""sv, true, false},
    {"1e21"sv, "1000000000000000000000"sv, true, false},
  };
  const OrderCase kOrders[] = {
    {"{\"1\":2,\"b\":1}"sv, "s4294967295=80;d1;d2;sconstructor=29;s4294967295=7;s-1=93;s01=57;sb=38;D1.5=67"sv, "{\"b\":38,\"4294967295\":7,\"constructor\":29,\"-1\":93,\"01\":57,\"1.5\":67}"sv, "[\"b\",\"4294967295\",\"constructor\",\"-1\",\"01\",\"1.5\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "d4294967295;d1;Dconstructor=85;sz=88;sb=66;s=69;s9=56;s__proto__=9"sv, "{\"9\":56,\"x\":1,\"y\":2,\"z\":88,\"constructor\":85,\"b\":66,\"\":69}"sv, "[\"9\",\"x\",\"y\",\"z\",\"constructor\",\"b\",\"\"]"sv},
    {"{\"a\":1}"sv, "Db=95;s10=72;d4294967294;s=88"sv, "{\"10\":72,\"a\":1,\"b\":95,\"\":88}"sv, "[\"10\",\"a\",\"b\",\"\"]"sv},
    {"{\"a\":1}"sv, "s4294967294=5;sconstructor=57;s-1=39"sv, "{\"4294967294\":5,\"a\":1,\"constructor\":57,\"-1\":39}"sv, "[\"4294967294\",\"a\",\"constructor\",\"-1\"]"sv},
    {"{\"a\":1}"sv, "s2=87;s1=7;D-1=25;s2=96;db;D2=39;s10=11"sv, "{\"1\":7,\"2\":39,\"10\":11,\"a\":1,\"-1\":25}"sv, "[\"1\",\"2\",\"10\",\"a\",\"-1\"]"sv},
    {"{\"__proto__\":0}"sv, "d1.5;D10=13;s4294967295=81;Dz=47;sa=68;s01=38;D0=51"sv, "{\"0\":51,\"10\":13,\"__proto__\":0,\"4294967295\":81,\"z\":47,\"a\":68,\"01\":38}"sv, "[\"0\",\"10\",\"__proto__\",\"4294967295\",\"z\",\"a\",\"01\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "D4294967295=67;sb=57;Dz=11;s1.5=31;Da=32"sv, "{\"x\":1,\"y\":2,\"z\":11,\"4294967295\":67,\"b\":57,\"1.5\":31,\"a\":32}"sv, "[\"x\",\"y\",\"z\",\"4294967295\",\"b\",\"1.5\",\"a\"]"sv},
    {"{\"__proto__\":0}"sv, "D4294967295=75;D__proto__=4;Db=72;D-1=52"sv, "{\"__proto__\":4,\"4294967295\":75,\"b\":72,\"-1\":52}"sv, "[\"__proto__\",\"4294967295\",\"b\",\"-1\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "s0=4;s1=16;slength=44;s__proto__=38;s1=24"sv, "{\"0\":4,\"1\":24,\"x\":1,\"y\":2,\"z\":3,\"length\":44}"sv, "[\"0\",\"1\",\"x\",\"y\",\"z\",\"length\"]"sv},
    {"{\"a\":1}"sv, "s=64;s=53;d9;D2=58;s4294967294=42;s0=38;D0=45;D01=21;D9=48;d1"sv, "{\"0\":45,\"2\":58,\"9\":48,\"4294967294\":42,\"a\":1,\"\":53,\"01\":21}"sv, "[\"0\",\"2\",\"9\",\"4294967294\",\"a\",\"\",\"01\"]"sv},
    {"{\"1\":2,\"b\":1}"sv, "s10=5;sa=95;sb=48;sb=0;s=1;sconstructor=81;D4294967295=77;d9"sv, "{\"1\":2,\"10\":5,\"b\":0,\"a\":95,\"\":1,\"constructor\":81,\"4294967295\":77}"sv, "[\"1\",\"10\",\"b\",\"a\",\"\",\"constructor\",\"4294967295\"]"sv},
    {"{\"1\":2,\"b\":1}"sv, "s-1=49;d__proto__;s2=68;s9=8"sv, "{\"1\":2,\"2\":68,\"9\":8,\"b\":1,\"-1\":49}"sv, "[\"1\",\"2\",\"9\",\"b\",\"-1\"]"sv},
    {"{\"1\":2,\"b\":1}"sv, "D__proto__=27;s=61;s__proto__=30;s0=80"sv, "{\"0\":80,\"1\":2,\"b\":1,\"__proto__\":30,\"\":61}"sv, "[\"0\",\"1\",\"b\",\"__proto__\",\"\"]"sv},
    {"{\"__proto__\":0}"sv, "sz=75;s9=87;Dlength=10;s=15;d2"sv, "{\"9\":87,\"__proto__\":0,\"z\":75,\"length\":10,\"\":15}"sv, "[\"9\",\"__proto__\",\"z\",\"length\",\"\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "d1;d0;s4294967295=69;sb=0;Dconstructor=39;D10=44;D4294967294=23"sv, "{\"10\":44,\"4294967294\":23,\"x\":1,\"y\":2,\"z\":3,\"4294967295\":69,\"b\":0,\"constructor\":39}"sv, "[\"10\",\"4294967294\",\"x\",\"y\",\"z\",\"4294967295\",\"b\",\"constructor\"]"sv},
    {"{}"sv, "d1.5;s9=66;sa=64;D1.5=59;s10=41;sa=43;D9=54;s2=30"sv, "{\"2\":30,\"9\":54,\"10\":41,\"a\":43,\"1.5\":59}"sv, "[\"2\",\"9\",\"10\",\"a\",\"1.5\"]"sv},
    {"{\"1\":2,\"b\":1}"sv, "D2=16;da;s0=10;s0=8;s9=52;s4294967295=8;Db=26;D01=79"sv, "{\"0\":8,\"1\":2,\"2\":16,\"9\":52,\"b\":26,\"4294967295\":8,\"01\":79}"sv, "[\"0\",\"1\",\"2\",\"9\",\"b\",\"4294967295\",\"01\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "s01=56;s__proto__=85;d__proto__"sv, "{\"x\":1,\"y\":2,\"z\":3,\"01\":56}"sv, "[\"x\",\"y\",\"z\",\"01\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "sz=80;s1.5=91;s__proto__=18;s01=6"sv, "{\"x\":1,\"y\":2,\"z\":80,\"1.5\":91,\"01\":6}"sv, "[\"x\",\"y\",\"z\",\"1.5\",\"01\"]"sv},
    {"{\"a\":1}"sv, "d4294967295;db;s=47;d10;d__proto__;s9=20;d4294967294;s10=28"sv, "{\"9\":20,\"10\":28,\"a\":1,\"\":47}"sv, "[\"9\",\"10\",\"a\",\"\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "d0;sconstructor=20;s9=2"sv, "{\"9\":2,\"x\":1,\"y\":2,\"z\":3,\"constructor\":20}"sv, "[\"9\",\"x\",\"y\",\"z\",\"constructor\"]"sv},
    {"{}"sv, "s1=23;d;sz=75;d01;s=94;slength=73;d1;D1.5=86;d10"sv, "{\"z\":75,\"\":94,\"length\":73,\"1.5\":86}"sv, "[\"z\",\"\",\"length\",\"1.5\"]"sv},
    {"{\"__proto__\":0}"sv, "s01=22;D1=31;Db=31;Dlength=79"sv, "{\"1\":31,\"__proto__\":0,\"01\":22,\"b\":31,\"length\":79}"sv, "[\"1\",\"__proto__\",\"01\",\"b\",\"length\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "D0=72;s10=9;s4294967294=22;slength=99;s10=6;dconstructor;D1=1;D1.5=16;d"sv, "{\"0\":72,\"1\":1,\"10\":6,\"4294967294\":22,\"x\":1,\"y\":2,\"z\":3,\"length\":99,\"1.5\":16}"sv, "[\"0\",\"1\",\"10\",\"4294967294\",\"x\",\"y\",\"z\",\"length\",\"1.5\"]"sv},
    {"{\"a\":1}"sv, "slength=28;s2=45;D1=57;d10;Dz=58"sv, "{\"1\":57,\"2\":45,\"a\":1,\"length\":28,\"z\":58}"sv, "[\"1\",\"2\",\"a\",\"length\",\"z\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "s4294967294=81;Dlength=30;s2=80;d;sb=68;d0"sv, "{\"2\":80,\"4294967294\":81,\"x\":1,\"y\":2,\"z\":3,\"length\":30,\"b\":68}"sv, "[\"2\",\"4294967294\",\"x\",\"y\",\"z\",\"length\",\"b\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "da;D__proto__=90;s10=7;d4294967294;s0=90;d10;Db=45;d1.5;s4294967295=73"sv, "{\"0\":90,\"x\":1,\"y\":2,\"z\":3,\"__proto__\":90,\"b\":45,\"4294967295\":73}"sv, "[\"0\",\"x\",\"y\",\"z\",\"__proto__\",\"b\",\"4294967295\"]"sv},
    {"{\"1\":2,\"b\":1}"sv, "s4294967295=71;D9=13;s=20;Db=10"sv, "{\"1\":2,\"9\":13,\"b\":10,\"4294967295\":71,\"\":20}"sv, "[\"1\",\"9\",\"b\",\"4294967295\",\"\"]"sv},
    {"{}"sv, "s10=47;Dconstructor=15;Db=85"sv, "{\"10\":47,\"constructor\":15,\"b\":85}"sv, "[\"10\",\"constructor\",\"b\"]"sv},
    {"{\"1\":2,\"b\":1}"sv, "s4294967295=66;dz;s4294967294=68;s0=51;Da=45;s10=93;d1;dconstructor;d0;s01=84"sv, "{\"10\":93,\"4294967294\":68,\"b\":1,\"4294967295\":66,\"a\":45,\"01\":84}"sv, "[\"10\",\"4294967294\",\"b\",\"4294967295\",\"a\",\"01\"]"sv},
    {"{\"a\":1}"sv, "s-1=5;dlength;d9;Da=84;dlength;s1.5=43;d1;Dlength=5;s4294967295=28;d"sv, "{\"a\":84,\"-1\":5,\"1.5\":43,\"length\":5,\"4294967295\":28}"sv, "[\"a\",\"-1\",\"1.5\",\"length\",\"4294967295\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "sa=66;Db=93;d-1;s9=20;D01=23"sv, "{\"9\":20,\"x\":1,\"y\":2,\"z\":3,\"a\":66,\"b\":93,\"01\":23}"sv, "[\"9\",\"x\",\"y\",\"z\",\"a\",\"b\",\"01\"]"sv},
    {"{\"__proto__\":0}"sv, "Da=54;sa=77;d4294967295;Dlength=90"sv, "{\"__proto__\":0,\"a\":77,\"length\":90}"sv, "[\"__proto__\",\"a\",\"length\"]"sv},
    {"{\"1\":2,\"b\":1}"sv, "s1.5=49;Db=15;d4294967294;D0=25;slength=20"sv, "{\"0\":25,\"1\":2,\"b\":15,\"1.5\":49,\"length\":20}"sv, "[\"0\",\"1\",\"b\",\"1.5\",\"length\"]"sv},
    {"{\"__proto__\":0}"sv, "s9=87;dz;s4294967294=67;D01=41;s-1=61;d10;d__proto__;D=0;s__proto__=76;D9=40"sv, "{\"9\":40,\"4294967294\":67,\"01\":41,\"-1\":61,\"\":0}"sv, "[\"9\",\"4294967294\",\"01\",\"-1\",\"\"]"sv},
    {"{\"a\":1}"sv, "dlength;s4294967294=44;s=56"sv, "{\"4294967294\":44,\"a\":1,\"\":56}"sv, "[\"4294967294\",\"a\",\"\"]"sv},
    {"{\"a\":1}"sv, "s1=47;s1.5=67;d01;s0=93;dlength;sz=9;dz;sconstructor=67;d4294967294;d01"sv, "{\"0\":93,\"1\":47,\"a\":1,\"1.5\":67,\"constructor\":67}"sv, "[\"0\",\"1\",\"a\",\"1.5\",\"constructor\"]"sv},
    {"{}"sv, "s01=72;s01=73;sb=67;s4294967295=89;s-1=64;s10=67;d__proto__;D4294967294=36;dconstructor;s__proto__=90"sv, "{\"10\":67,\"4294967294\":36,\"01\":73,\"b\":67,\"4294967295\":89,\"-1\":64}"sv, "[\"10\",\"4294967294\",\"01\",\"b\",\"4294967295\",\"-1\"]"sv},
    {"{\"a\":1}"sv, "slength=99;s4294967295=77;s4294967295=6;d01;d1;D0=91;D1=31;d9"sv, "{\"0\":91,\"1\":31,\"a\":1,\"length\":99,\"4294967295\":6}"sv, "[\"0\",\"1\",\"a\",\"length\",\"4294967295\"]"sv},
    {"{\"1\":2,\"b\":1}"sv, "Da=13;Dz=56;s01=20;d__proto__;s4294967295=74;d01;sz=47;D01=0;sz=34"sv, "{\"1\":2,\"b\":1,\"a\":13,\"z\":34,\"4294967295\":74,\"01\":0}"sv, "[\"1\",\"b\",\"a\",\"z\",\"4294967295\",\"01\"]"sv},
    {"{}"sv, "s4294967295=5;sconstructor=14;d1;Dlength=74"sv, "{\"4294967295\":5,\"constructor\":14,\"length\":74}"sv, "[\"4294967295\",\"constructor\",\"length\"]"sv},
    {"{\"a\":1}"sv, "s2=82;dlength;sconstructor=31;D4294967294=56;sconstructor=61"sv, "{\"2\":82,\"4294967294\":56,\"a\":1,\"constructor\":61}"sv, "[\"2\",\"4294967294\",\"a\",\"constructor\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "dconstructor;sa=96;s4294967295=63;Dlength=21;d10;d__proto__"sv, "{\"x\":1,\"y\":2,\"z\":3,\"a\":96,\"4294967295\":63,\"length\":21}"sv, "[\"x\",\"y\",\"z\",\"a\",\"4294967295\",\"length\"]"sv},
    {"{\"a\":1}"sv, "d0;s9=45;dconstructor;D__proto__=29"sv, "{\"9\":45,\"a\":1,\"__proto__\":29}"sv, "[\"9\",\"a\",\"__proto__\"]"sv},
    {"{\"__proto__\":0}"sv, "s4294967294=0;d10;sconstructor=3;s=93;s1.5=20;s2=68;s2=89;D2=76"sv, "{\"2\":76,\"4294967294\":0,\"__proto__\":0,\"constructor\":3,\"\":93,\"1.5\":20}"sv, "[\"2\",\"4294967294\",\"__proto__\",\"constructor\",\"\",\"1.5\"]"sv},
    {"{\"a\":1}"sv, "dz;s0=71;s2=24;s2=84;d"sv, "{\"0\":71,\"2\":84,\"a\":1}"sv, "[\"0\",\"2\",\"a\"]"sv},
    {"{\"a\":1}"sv, "sb=0;s01=49;s=93;s01=43;Db=17;d01;s1.5=99;db"sv, "{\"a\":1,\"\":93,\"1.5\":99}"sv, "[\"a\",\"\",\"1.5\"]"sv},
    {"{\"1\":2,\"b\":1}"sv, "s4294967294=47;d10;D01=41;dconstructor"sv, "{\"1\":2,\"4294967294\":47,\"b\":1,\"01\":41}"sv, "[\"1\",\"4294967294\",\"b\",\"01\"]"sv},
    {"{\"__proto__\":0}"sv, "s2=63;D4294967294=81;d0;D4294967294=62;D1.5=53;sconstructor=17;s-1=21;sconstructor=23;d2"sv, "{\"4294967294\":62,\"__proto__\":0,\"1.5\":53,\"constructor\":23,\"-1\":21}"sv, "[\"4294967294\",\"__proto__\",\"1.5\",\"constructor\",\"-1\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "s1.5=46;d-1;dlength"sv, "{\"x\":1,\"y\":2,\"z\":3,\"1.5\":46}"sv, "[\"x\",\"y\",\"z\",\"1.5\"]"sv},
    {"{}"sv, "s2=31;D-1=40;Da=95;d4294967294;Dz=20;d0;dz;sconstructor=60;s__proto__=79"sv, "{\"2\":31,\"-1\":40,\"a\":95,\"constructor\":60}"sv, "[\"2\",\"-1\",\"a\",\"constructor\"]"sv},
    {"{\"a\":1}"sv, "d-1;D10=6;s1.5=41;d10;slength=12;d2;s10=42"sv, "{\"10\":42,\"a\":1,\"1.5\":41,\"length\":12}"sv, "[\"10\",\"a\",\"1.5\",\"length\"]"sv},
    {"{}"sv, "D1=58;dlength;d1;s__proto__=19;s2=61;s10=32;dz;D__proto__=69;D4294967294=19"sv, "{\"2\":61,\"10\":32,\"4294967294\":19,\"__proto__\":69}"sv, "[\"2\",\"10\",\"4294967294\",\"__proto__\"]"sv},
    {"{\"a\":1}"sv, "slength=67;D9=76;D1.5=87;s1.5=29;D-1=51;d4294967295;d0;s2=76"sv, "{\"2\":76,\"9\":76,\"a\":1,\"length\":67,\"1.5\":29,\"-1\":51}"sv, "[\"2\",\"9\",\"a\",\"length\",\"1.5\",\"-1\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "s01=63;dconstructor;s9=59;da;D__proto__=79;D=63;d-1;sa=4;d0"sv, "{\"9\":59,\"x\":1,\"y\":2,\"z\":3,\"01\":63,\"__proto__\":79,\"\":63,\"a\":4}"sv, "[\"9\",\"x\",\"y\",\"z\",\"01\",\"__proto__\",\"\",\"a\"]"sv},
    {"{\"a\":1}"sv, "s2=48;s4294967295=40;s1.5=80;s__proto__=57;s4294967294=48"sv, "{\"2\":48,\"4294967294\":48,\"a\":1,\"4294967295\":40,\"1.5\":80}"sv, "[\"2\",\"4294967294\",\"a\",\"4294967295\",\"1.5\"]"sv},
    {"{\"a\":1}"sv, "s1=42;sb=49;d2"sv, "{\"1\":42,\"a\":1,\"b\":49}"sv, "[\"1\",\"a\",\"b\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "s1=9;s9=27;db;D4294967294=2;s__proto__=4;Db=70;D10=16"sv, "{\"1\":9,\"9\":27,\"10\":16,\"4294967294\":2,\"x\":1,\"y\":2,\"z\":3,\"b\":70}"sv, "[\"1\",\"9\",\"10\",\"4294967294\",\"x\",\"y\",\"z\",\"b\"]"sv},
    {"{\"__proto__\":0}"sv, "s01=3;sz=68;D4294967295=8;s0=6;s4294967295=77;s=20;D10=21;dconstructor;sz=99"sv, "{\"0\":6,\"10\":21,\"__proto__\":0,\"01\":3,\"z\":99,\"4294967295\":77,\"\":20}"sv, "[\"0\",\"10\",\"__proto__\",\"01\",\"z\",\"4294967295\",\"\"]"sv},
    {"{}"sv, "s10=61;d4294967295;s4294967294=38;s4294967294=36;s-1=41"sv, "{\"10\":61,\"4294967294\":36,\"-1\":41}"sv, "[\"10\",\"4294967294\",\"-1\"]"sv},
    {"{\"a\":1}"sv, "d-1;D4294967295=82;s1.5=94;s01=26;D4294967295=45;D01=28;s1.5=84;sz=20"sv, "{\"a\":1,\"4294967295\":45,\"1.5\":84,\"01\":28,\"z\":20}"sv, "[\"a\",\"4294967295\",\"1.5\",\"01\",\"z\"]"sv},
    {"{}"sv, "sa=7;sa=28;Db=74;sb=36;dlength;s4294967294=42;d4294967295;d2;s=38"sv, "{\"4294967294\":42,\"a\":28,\"b\":36,\"\":38}"sv, "[\"4294967294\",\"a\",\"b\",\"\"]"sv},
    {"{\"__proto__\":0}"sv, "s2=97;d-1;s0=81;dz"sv, "{\"0\":81,\"2\":97,\"__proto__\":0}"sv, "[\"0\",\"2\",\"__proto__\"]"sv},
    {"{\"a\":1}"sv, "slength=90;s1.5=10;sa=61"sv, "{\"a\":61,\"length\":90,\"1.5\":10}"sv, "[\"a\",\"length\",\"1.5\"]"sv},
    {"{\"__proto__\":0}"sv, "s2=80;s4294967294=93;s__proto__=9;sb=25"sv, "{\"2\":80,\"4294967294\":93,\"__proto__\":9,\"b\":25}"sv, "[\"2\",\"4294967294\",\"__proto__\",\"b\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "Da=44;s__proto__=17;D1=98;da;D2=16"sv, "{\"1\":98,\"2\":16,\"x\":1,\"y\":2,\"z\":3}"sv, "[\"1\",\"2\",\"x\",\"y\",\"z\"]"sv},
    {"{\"1\":2,\"b\":1}"sv, "s0=92;Db=17;s1.5=82;s01=3;sa=67;s4294967294=46;s-1=53;d9"sv, "{\"0\":92,\"1\":2,\"4294967294\":46,\"b\":17,\"1.5\":82,\"01\":3,\"a\":67,\"-1\":53}"sv, "[\"0\",\"1\",\"4294967294\",\"b\",\"1.5\",\"01\",\"a\",\"-1\"]"sv},
    {"{\"a\":1}"sv, "D2=2;Db=46;d1.5;s0=96;D4294967295=49;D-1=61"sv, "{\"0\":96,\"2\":2,\"a\":1,\"b\":46,\"4294967295\":49,\"-1\":61}"sv, "[\"0\",\"2\",\"a\",\"b\",\"4294967295\",\"-1\"]"sv},
    {"{\"x\":1,\"y\":2,\"z\":3}"sv, "s-1=38;s10=31;sz=51;s0=37;s1.5=11;sb=56;s=17;d;d0;s=30"sv, "{\"10\":31,\"x\":1,\"y\":2,\"z\":51,\"-1\":38,\"1.5\":11,\"b\":56,\"\":30}"sv, "[\"10\",\"x\",\"y\",\"z\",\"-1\",\"1.5\",\"b\",\"\"]"sv},
    {"{\"__proto__\":0}"sv, "D1.5=29;da;sz=25;dlength;sb=71;s1=22;s10=73;s-1=69;d__proto__;D10=61"sv, "{\"1\":22,\"10\":61,\"1.5\":29,\"z\":25,\"b\":71,\"-1\":69}"sv, "[\"1\",\"10\",\"1.5\",\"z\",\"b\",\"-1\"]"sv},
  };
  const MergeCase kMerges[] = {
    {"{}"sv, "{}"sv, "{}"sv, "{}"sv},
    {"{}"sv, "{\"a\":1}"sv, "{\"a\":1}"sv, "{\"a\":1}"sv},
    {"{}"sv, "{\"b\":2,\"a\":3}"sv, "{\"b\":2,\"a\":3}"sv, "{\"b\":2,\"a\":3}"sv},
    {"{}"sv, "{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":4}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":4}"sv},
    {"{}"sv, "{\"__proto__\":5,\"c\":6}"sv, "{\"__proto__\":5,\"c\":6}"sv, "{\"c\":6}"sv},
    {"{}"sv, "{\"__proto__\":{\"x\":1}}"sv, "{\"__proto__\":{\"x\":1}}"sv, "{}"sv},
    {"{}"sv, "{\"a\":null,\"10\":7}"sv, "{\"10\":7,\"a\":null}"sv, "{\"10\":7,\"a\":null}"sv},
    {"{}"sv, "{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"z\":{\"n\":1},\"a\":[1]}"sv},
    {"{\"a\":1}"sv, "{}"sv, "{\"a\":1}"sv, "{\"a\":1}"sv},
    {"{\"a\":1}"sv, "{\"a\":1}"sv, "{\"a\":1}"sv, "{\"a\":1}"sv},
    {"{\"a\":1}"sv, "{\"b\":2,\"a\":3}"sv, "{\"a\":3,\"b\":2}"sv, "{\"a\":3,\"b\":2}"sv},
    {"{\"a\":1}"sv, "{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":4}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":4}"sv},
    {"{\"a\":1}"sv, "{\"__proto__\":5,\"c\":6}"sv, "{\"a\":1,\"__proto__\":5,\"c\":6}"sv, "{\"a\":1,\"c\":6}"sv},
    {"{\"a\":1}"sv, "{\"__proto__\":{\"x\":1}}"sv, "{\"a\":1,\"__proto__\":{\"x\":1}}"sv, "{\"a\":1}"sv},
    {"{\"a\":1}"sv, "{\"a\":null,\"10\":7}"sv, "{\"10\":7,\"a\":null}"sv, "{\"10\":7,\"a\":null}"sv},
    {"{\"a\":1}"sv, "{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"a\":[1],\"z\":{\"n\":1}}"sv, "{\"a\":[1],\"z\":{\"n\":1}}"sv},
    {"{\"b\":2,\"a\":3}"sv, "{}"sv, "{\"b\":2,\"a\":3}"sv, "{\"b\":2,\"a\":3}"sv},
    {"{\"b\":2,\"a\":3}"sv, "{\"a\":1}"sv, "{\"b\":2,\"a\":1}"sv, "{\"b\":2,\"a\":1}"sv},
    {"{\"b\":2,\"a\":3}"sv, "{\"b\":2,\"a\":3}"sv, "{\"b\":2,\"a\":3}"sv, "{\"b\":2,\"a\":3}"sv},
    {"{\"b\":2,\"a\":3}"sv, "{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{\"1\":\"one\",\"2\":\"two\",\"b\":2,\"a\":4}"sv, "{\"1\":\"one\",\"2\":\"two\",\"b\":2,\"a\":4}"sv},
    {"{\"b\":2,\"a\":3}"sv, "{\"__proto__\":5,\"c\":6}"sv, "{\"b\":2,\"a\":3,\"__proto__\":5,\"c\":6}"sv, "{\"b\":2,\"a\":3,\"c\":6}"sv},
    {"{\"b\":2,\"a\":3}"sv, "{\"__proto__\":{\"x\":1}}"sv, "{\"b\":2,\"a\":3,\"__proto__\":{\"x\":1}}"sv, "{\"b\":2,\"a\":3}"sv},
    {"{\"b\":2,\"a\":3}"sv, "{\"a\":null,\"10\":7}"sv, "{\"10\":7,\"b\":2,\"a\":null}"sv, "{\"10\":7,\"b\":2,\"a\":null}"sv},
    {"{\"b\":2,\"a\":3}"sv, "{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"b\":2,\"a\":[1],\"z\":{\"n\":1}}"sv, "{\"b\":2,\"a\":[1],\"z\":{\"n\":1}}"sv},
    {"{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":4}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":4}"sv},
    {"{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{\"a\":1}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":1}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":1}"sv},
    {"{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{\"b\":2,\"a\":3}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":3,\"b\":2}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":3,\"b\":2}"sv},
    {"{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":4}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":4}"sv},
    {"{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{\"__proto__\":5,\"c\":6}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":4,\"__proto__\":5,\"c\":6}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":4,\"c\":6}"sv},
    {"{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{\"__proto__\":{\"x\":1}}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":4,\"__proto__\":{\"x\":1}}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":4}"sv},
    {"{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{\"a\":null,\"10\":7}"sv, "{\"1\":\"one\",\"2\":\"two\",\"10\":7,\"a\":null}"sv, "{\"1\":\"one\",\"2\":\"two\",\"10\":7,\"a\":null}"sv},
    {"{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":[1],\"z\":{\"n\":1}}"sv, "{\"1\":\"one\",\"2\":\"two\",\"a\":[1],\"z\":{\"n\":1}}"sv},
    {"{\"__proto__\":5,\"c\":6}"sv, "{}"sv, "{\"__proto__\":5,\"c\":6}"sv, "{\"__proto__\":5,\"c\":6}"sv},
    {"{\"__proto__\":5,\"c\":6}"sv, "{\"a\":1}"sv, "{\"__proto__\":5,\"c\":6,\"a\":1}"sv, "{\"__proto__\":5,\"c\":6,\"a\":1}"sv},
    {"{\"__proto__\":5,\"c\":6}"sv, "{\"b\":2,\"a\":3}"sv, "{\"__proto__\":5,\"c\":6,\"b\":2,\"a\":3}"sv, "{\"__proto__\":5,\"c\":6,\"b\":2,\"a\":3}"sv},
    {"{\"__proto__\":5,\"c\":6}"sv, "{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{\"1\":\"one\",\"2\":\"two\",\"__proto__\":5,\"c\":6,\"a\":4}"sv, "{\"1\":\"one\",\"2\":\"two\",\"__proto__\":5,\"c\":6,\"a\":4}"sv},
    {"{\"__proto__\":5,\"c\":6}"sv, "{\"__proto__\":5,\"c\":6}"sv, "{\"__proto__\":5,\"c\":6}"sv, "{\"__proto__\":5,\"c\":6}"sv},
    {"{\"__proto__\":5,\"c\":6}"sv, "{\"__proto__\":{\"x\":1}}"sv, "{\"__proto__\":{\"x\":1},\"c\":6}"sv, "{\"__proto__\":{\"x\":1},\"c\":6}"sv},
    {"{\"__proto__\":5,\"c\":6}"sv, "{\"a\":null,\"10\":7}"sv, "{\"10\":7,\"__proto__\":5,\"c\":6,\"a\":null}"sv, "{\"10\":7,\"__proto__\":5,\"c\":6,\"a\":null}"sv},
    {"{\"__proto__\":5,\"c\":6}"sv, "{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"__proto__\":5,\"c\":6,\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"__proto__\":5,\"c\":6,\"z\":{\"n\":1},\"a\":[1]}"sv},
    {"{\"__proto__\":{\"x\":1}}"sv, "{}"sv, "{\"__proto__\":{\"x\":1}}"sv, "{\"__proto__\":{\"x\":1}}"sv},
    {"{\"__proto__\":{\"x\":1}}"sv, "{\"a\":1}"sv, "{\"__proto__\":{\"x\":1},\"a\":1}"sv, "{\"__proto__\":{\"x\":1},\"a\":1}"sv},
    {"{\"__proto__\":{\"x\":1}}"sv, "{\"b\":2,\"a\":3}"sv, "{\"__proto__\":{\"x\":1},\"b\":2,\"a\":3}"sv, "{\"__proto__\":{\"x\":1},\"b\":2,\"a\":3}"sv},
    {"{\"__proto__\":{\"x\":1}}"sv, "{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{\"1\":\"one\",\"2\":\"two\",\"__proto__\":{\"x\":1},\"a\":4}"sv, "{\"1\":\"one\",\"2\":\"two\",\"__proto__\":{\"x\":1},\"a\":4}"sv},
    {"{\"__proto__\":{\"x\":1}}"sv, "{\"__proto__\":5,\"c\":6}"sv, "{\"__proto__\":5,\"c\":6}"sv, "{\"__proto__\":5,\"c\":6}"sv},
    {"{\"__proto__\":{\"x\":1}}"sv, "{\"__proto__\":{\"x\":1}}"sv, "{\"__proto__\":{\"x\":1}}"sv, "{\"__proto__\":{\"x\":1}}"sv},
    {"{\"__proto__\":{\"x\":1}}"sv, "{\"a\":null,\"10\":7}"sv, "{\"10\":7,\"__proto__\":{\"x\":1},\"a\":null}"sv, "{\"10\":7,\"__proto__\":{\"x\":1},\"a\":null}"sv},
    {"{\"__proto__\":{\"x\":1}}"sv, "{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"__proto__\":{\"x\":1},\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"__proto__\":{\"x\":1},\"z\":{\"n\":1},\"a\":[1]}"sv},
    {"{\"a\":null,\"10\":7}"sv, "{}"sv, "{\"10\":7,\"a\":null}"sv, "{\"10\":7,\"a\":null}"sv},
    {"{\"a\":null,\"10\":7}"sv, "{\"a\":1}"sv, "{\"10\":7,\"a\":1}"sv, "{\"10\":7,\"a\":1}"sv},
    {"{\"a\":null,\"10\":7}"sv, "{\"b\":2,\"a\":3}"sv, "{\"10\":7,\"a\":3,\"b\":2}"sv, "{\"10\":7,\"a\":3,\"b\":2}"sv},
    {"{\"a\":null,\"10\":7}"sv, "{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{\"1\":\"one\",\"2\":\"two\",\"10\":7,\"a\":4}"sv, "{\"1\":\"one\",\"2\":\"two\",\"10\":7,\"a\":4}"sv},
    {"{\"a\":null,\"10\":7}"sv, "{\"__proto__\":5,\"c\":6}"sv, "{\"10\":7,\"a\":null,\"__proto__\":5,\"c\":6}"sv, "{\"10\":7,\"a\":null,\"c\":6}"sv},
    {"{\"a\":null,\"10\":7}"sv, "{\"__proto__\":{\"x\":1}}"sv, "{\"10\":7,\"a\":null,\"__proto__\":{\"x\":1}}"sv, "{\"10\":7,\"a\":null}"sv},
    {"{\"a\":null,\"10\":7}"sv, "{\"a\":null,\"10\":7}"sv, "{\"10\":7,\"a\":null}"sv, "{\"10\":7,\"a\":null}"sv},
    {"{\"a\":null,\"10\":7}"sv, "{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"10\":7,\"a\":[1],\"z\":{\"n\":1}}"sv, "{\"10\":7,\"a\":[1],\"z\":{\"n\":1}}"sv},
    {"{\"z\":{\"n\":1},\"a\":[1]}"sv, "{}"sv, "{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"z\":{\"n\":1},\"a\":[1]}"sv},
    {"{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"a\":1}"sv, "{\"z\":{\"n\":1},\"a\":1}"sv, "{\"z\":{\"n\":1},\"a\":1}"sv},
    {"{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"b\":2,\"a\":3}"sv, "{\"z\":{\"n\":1},\"a\":3,\"b\":2}"sv, "{\"z\":{\"n\":1},\"a\":3,\"b\":2}"sv},
    {"{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"2\":\"two\",\"a\":4,\"1\":\"one\"}"sv, "{\"1\":\"one\",\"2\":\"two\",\"z\":{\"n\":1},\"a\":4}"sv, "{\"1\":\"one\",\"2\":\"two\",\"z\":{\"n\":1},\"a\":4}"sv},
    {"{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"__proto__\":5,\"c\":6}"sv, "{\"z\":{\"n\":1},\"a\":[1],\"__proto__\":5,\"c\":6}"sv, "{\"z\":{\"n\":1},\"a\":[1],\"c\":6}"sv},
    {"{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"__proto__\":{\"x\":1}}"sv, "{\"z\":{\"n\":1},\"a\":[1],\"__proto__\":{\"x\":1}}"sv, "{\"z\":{\"n\":1},\"a\":[1]}"sv},
    {"{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"a\":null,\"10\":7}"sv, "{\"10\":7,\"z\":{\"n\":1},\"a\":null}"sv, "{\"10\":7,\"z\":{\"n\":1},\"a\":null}"sv},
    {"{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"z\":{\"n\":1},\"a\":[1]}"sv, "{\"z\":{\"n\":1},\"a\":[1]}"sv},
  };
  const StringSortCase kStringSorts[] = {
    {"[\"b\",\"a\",\"c\"]"sv, "[\"a\",\"b\",\"c\"]"sv},
    {"[\"B\",\"a\",\"A\",\"b\"]"sv, "[\"A\",\"B\",\"a\",\"b\"]"sv},
    {"[\"10\",\"9\",\"1\",\"100\"]"sv, "[\"1\",\"10\",\"100\",\"9\"]"sv},
    {"[\"\",\"a\",\" \"]"sv, "[\"\",\" \",\"a\"]"sv},
    {"[\"\360\237\230\200\",\"\357\277\277\",\"\356\200\200\",\"z\",\"\\ud800\"]"sv, "[\"z\",\"\\ud800\",\"\360\237\230\200\",\"\356\200\200\",\"\357\277\277\"]"sv},
    {"[\"\303\251\",\"e\",\"f\",\"\303\250\"]"sv, "[\"e\",\"f\",\"\303\250\",\"\303\251\"]"sv},
    {"[\"a\",\"a\",\"aa\",\"A\"]"sv, "[\"A\",\"a\",\"a\",\"aa\"]"sv},
    {"[\"\360\237\230\200\",\"\\ud83d\",\"\360\237\216\211\",\"\357\275\236\"]"sv, "[\"\360\237\216\211\",\"\\ud83d\",\"\360\237\230\200\",\"\357\275\236\"]"sv},
    {"[\"\",\"\",\"\",\"aAa\",\"\303\251b\",\"\",\"\\ude00\",\"\",\"\357\277\277\"]"sv, "[\"\",\"\",\"\",\"\",\"\",\"aAa\",\"\303\251b\",\"\\ude00\",\"\357\277\277\"]"sv},
    {"[\" a1\",\"\\ud83d\",\"\356\200\200a\356\200\200\",\"\",\"\\ude00\\ud83d1\",\"\\ud83d1 \",\"A\",\"1a\",\"\",\"1\356\200\200\"]"sv, "[\"\",\"\",\" a1\",\"1a\",\"1\356\200\200\",\"A\",\"\\ud83d\",\"\\ud83d1 \",\"\\ude00\\ud83d1\",\"\356\200\200a\356\200\200\"]"sv},
    {"[\"\356\200\200\",\"11\357\277\277\",\"a\",\"a\356\200\200 \",\"\",\"\356\200\200\",\"1\"]"sv, "[\"\",\"1\",\"11\357\277\277\",\"a\",\"a\356\200\200 \",\"\356\200\200\",\"\356\200\200\"]"sv},
    {"[\"1\",\"1a\",\" \",\"\356\200\200\",\"\\ude00 \",\"\"]"sv, "[\"\",\" \",\"1\",\"1a\",\"\\ude00 \",\"\356\200\200\"]"sv},
    {"[\"\\ude00 \",\"a1b\",\"\",\"\357\277\277\\ude00\\ude00\",\"\356\200\200AA\",\"\\ude00\\ude00\303\251\"]"sv, "[\"\",\"a1b\",\"\\ude00 \",\"\\ude00\\ude00\303\251\",\"\356\200\200AA\",\"\357\277\277\\ude00\\ude00\"]"sv},
    {"[\"\357\277\277 \",\"ba\\ud83d\",\" \357\277\277\303\251\",\"\",\"\356\200\200\303\251\\ude00\",\"1\\ud83d\356\200\200\",\"\",\"A \356\200\200\",\"\"]"sv, "[\"\",\"\",\"\",\" \357\277\277\303\251\",\"1\\ud83d\356\200\200\",\"A \356\200\200\",\"ba\\ud83d\",\"\356\200\200\303\251\\ude00\",\"\357\277\277 \"]"sv},
    {"[\" \356\200\200\",\"a \357\277\277\",\"\",\"Aa\",\"\",\"\",\"A\"]"sv, "[\"\",\"\",\"\",\" \356\200\200\",\"A\",\"Aa\",\"a \357\277\277\"]"sv},
    {"[\"  \",\"\",\"\",\"\",\"A\",\"\357\277\277\357\277\277\",\"A\356\200\2001\"]"sv, "[\"\",\"\",\"\",\"  \",\"A\",\"A\356\200\2001\",\"\357\277\277\357\277\277\"]"sv},
    {"[\"\303\251bb\",\"1\303\251\357\277\277\",\"a\",\"\\ud83d\357\277\277\",\"\",\"\356\200\200\"]"sv, "[\"\",\"1\303\251\357\277\277\",\"a\",\"\303\251bb\",\"\\ud83d\357\277\277\",\"\356\200\200\"]"sv},
    {"[\"\",\"\356\200\200\",\"\303\251A \",\"\357\277\277\356\200\200\",\"\",\"\",\"A\356\200\200\",\"\"]"sv, "[\"\",\"\",\"\",\"\",\"A\356\200\200\",\"\303\251A \",\"\356\200\200\",\"\357\277\277\356\200\200\"]"sv},
    {"[\"A1\",\"\356\200\200\357\277\277\",\"\",\" 1b\"]"sv, "[\"\",\" 1b\",\"A1\",\"\356\200\200\357\277\277\"]"sv},
    {"[\"1\\ude001\",\"\",\"\",\"b\",\"bb\\ud83d\",\"\",\"Aa\",\"\\ude00 \",\"\"]"sv, "[\"\",\"\",\"\",\"\",\"1\\ude001\",\"Aa\",\"b\",\"bb\\ud83d\",\"\\ude00 \"]"sv},
    {"[\"a\",\"\",\"ab\",\"aba\",\"1\\ud83d\",\"1\",\"\356\200\200\357\277\277\",\"\"]"sv, "[\"\",\"\",\"1\",\"1\\ud83d\",\"a\",\"ab\",\"aba\",\"\356\200\200\357\277\277\"]"sv},
    {"[\"\303\2511\",\"\",\" \",\"\357\277\277\\ud83d\",\"\\ude00\\ude00\",\"\357\277\277\303\251\",\"\\ude00\"]"sv, "[\"\",\" \",\"\303\2511\",\"\\ude00\",\"\\ude00\\ude00\",\"\357\277\277\303\251\",\"\357\277\277\\ud83d\"]"sv},
    {"[\"\\ude00\303\251\",\"\",\"\356\200\2001\",\"Aba\"]"sv, "[\"\",\"Aba\",\"\\ude00\303\251\",\"\356\200\2001\"]"sv},
    {"[\"\\ud83d\",\"ab\\ud83d\",\"Aa \",\"\",\" \303\251\"]"sv, "[\"\",\" \303\251\",\"Aa \",\"ab\\ud83d\",\"\\ud83d\"]"sv},
    {"[\"1 A\",\"\\ud83dA \",\"\303\251b\303\251\",\"\\ud83d\357\277\2771\",\"A\"]"sv, "[\"1 A\",\"A\",\"\303\251b\303\251\",\"\\ud83dA \",\"\\ud83d\357\277\2771\"]"sv},
    {"[\"\303\251\357\277\277A\",\"\",\"\303\251a\303\251\",\"bbb\",\"1\303\251\",\"A\",\"\"]"sv, "[\"\",\"\",\"1\303\251\",\"A\",\"bbb\",\"\303\251a\303\251\",\"\303\251\357\277\277A\"]"sv},
    {"[\"\\ud83d \",\"b1\",\" \",\"A1 \",\"b\303\251\\ud83d\",\"\357\277\277b\",\"\303\251a1\",\"\",\"\",\"1\303\251\"]"sv, "[\"\",\"\",\" \",\"1\303\251\",\"A1 \",\"b1\",\"b\303\251\\ud83d\",\"\303\251a1\",\"\\ud83d \",\"\357\277\277b\"]"sv},
    {"[\"\",\"\\ud83d\",\"\",\"a \",\" \",\"A\303\251\303\251\",\"\\ud83d\356\200\200\",\"\"]"sv, "[\"\",\"\",\"\",\" \",\"A\303\251\303\251\",\"a \",\"\\ud83d\",\"\\ud83d\356\200\200\"]"sv},
  };
  const NumberSortCase kNumberSorts[] = {
    {"10 9 1 100"sv, "1 10 100 9"sv},
    {"-1 -10 0 -0 2.5 1e+21 1e-7 NaN Infinity -Infinity"sv, "-1 -10 -Infinity 0 -0 1e+21 1e-7 2.5 Infinity NaN"sv},
    {"3 20 100 1000 25"sv, "100 1000 20 25 3"sv},
    {"0.5 0.25 0.125 1"sv, "0.125 0.25 0.5 1"sv},
    {"Infinity 1.5 33 2 10 33"sv, "1.5 10 2 33 33 Infinity"sv},
    {"30000000000 -2.5 33 1e-7 -1 -1 1.5 1.5 30000000000 0 30000000000"sv, "-1 -1 -2.5 0 1.5 1.5 1e-7 30000000000 30000000000 30000000000 33"sv},
    {"10 0 0 1e+21 10"sv, "0 0 10 10 1e+21"sv},
    {"1 100 1 0 0"sv, "0 0 1 1 100"sv},
    {"NaN 1.5 100 1 -2.5 0 -1 10 1 1e+21 NaN"sv, "-1 -2.5 0 1 1 1.5 10 100 1e+21 NaN NaN"sv},
    {"-0 33 10 1e-7 1 10 10 -0"sv, "-0 -0 1 10 10 10 1e-7 33"sv},
    {"NaN Infinity 1e-7 NaN 100 33 1 2 100 -2.5 1e-7"sv, "-2.5 1 100 100 1e-7 1e-7 2 33 Infinity NaN NaN"sv},
    {"-0 NaN 10 -2.5 2 0 30000000000 NaN"sv, "-2.5 -0 0 10 2 30000000000 NaN NaN"sv},
    {"0 0 1e+21 1"sv, "0 0 1 1e+21"sv},
    {"1.5 Infinity 2 -0 -0 -2.5 1e+21 30000000000"sv, "-2.5 -0 -0 1.5 1e+21 2 30000000000 Infinity"sv},
    {"2 1 100 Infinity 2 1"sv, "1 1 100 2 2 Infinity"sv},
    {"1e-7 NaN 2 0"sv, "0 1e-7 2 NaN"sv},
    {"NaN 30000000000 NaN 30000000000 10"sv, "10 30000000000 30000000000 NaN NaN"sv},
    {"30000000000 Infinity"sv, "30000000000 Infinity"sv},
    {"-1 2 -1 NaN 30000000000 -0 -0 100 100 NaN 2"sv, "-1 -1 -0 -0 100 100 2 2 30000000000 NaN NaN"sv},
    {"-2.5 NaN NaN 30000000000 1e+21 1.5 0 Infinity -1 2 10"sv, "-1 -2.5 0 1.5 10 1e+21 2 30000000000 Infinity NaN NaN"sv},
    {"10 2 2 30000000000 10 100 1 2 1.5"sv, "1 1.5 10 10 100 2 2 2 30000000000"sv},
    {"1e+21 33 2 0 1.5 30000000000 10 1.5 1e-7 1e+21"sv, "0 1.5 1.5 10 1e+21 1e+21 1e-7 2 30000000000 33"sv},
    {"2 10 33"sv, "10 2 33"sv},
    {"1e+21 Infinity"sv, "1e+21 Infinity"sv},
  };
  const JsonSortCase kJsonSorts[] = {
    {"[3,\"3\",null,true,\"a\",[1,2],\"1,2\",{},[],\"\"]"sv, "[[],\"\",[1,2],\"1,2\",3,\"3\",{},\"a\",null,true]"sv},
    {"[10,9,1,\"10\",\"9\"]"sv, "[1,10,\"10\",9,\"9\"]"sv},
    {"[[2],[1,1],[1],\"1\"]"sv, "[[1],\"1\",[1,1],[2]]"sv},
    {"[false,\"false\",true,null,\"null\"]"sv, "[false,\"false\",null,\"null\",true]"sv},
  };
  const RecordSortCase kRecordSorts[] = {
    {"4 0 4"sv, "1 0 2"sv, "0 2 1"sv, "2 1 0"sv},
    {"0 0 1 2 3 2"sv, "0 1 2 3 5 4"sv, "4 3 5 2 0 1"sv, "5 3 1 0 4 2"sv},
    {"3 3 3 2"sv, "3 0 1 2"sv, "0 1 2 3"sv, "3 2 1 0"sv},
    {"4 4 1 4 1 1 0 4 3"sv, "6 2 4 5 8 0 1 3 7"sv, "0 1 3 7 8 2 4 5 6"sv, "7 6 3 1 0 8 5 4 2"sv},
    {"2"sv, "0"sv, "0"sv, "0"sv},
    {"3 3 4 2 1 3 2 3 0 4"sv, "8 4 3 6 0 1 5 7 2 9"sv, "2 9 0 1 5 7 3 6 4 8"sv, "9 8 6 3 2 7 5 4 1 0"sv},
    {"4 3 0 3 4 4 3 2 2 1 1"sv, "2 9 10 7 8 1 3 6 0 4 5"sv, "0 4 5 1 3 6 7 8 9 10 2"sv, "8 7 5 4 2 0 10 9 6 3 1"sv},
    {"2 3 4 0 0 2 0 3 3"sv, "3 4 6 0 5 1 7 8 2"sv, "2 1 7 8 0 5 3 4 6"sv, "6 5 4 3 2 0 8 7 1"sv},
    {"1"sv, "0"sv, "0"sv, "0"sv},
    {"2"sv, "0"sv, "0"sv, "0"sv},
    {"4 4 1 3 2 1"sv, "2 5 4 3 0 1"sv, "0 1 3 4 2 5"sv, "4 1 0 5 3 2"sv},
    {"0 4 3 4 0"sv, "0 4 2 1 3"sv, "1 3 2 0 4"sv, "4 3 1 0 2"sv},
    {"0 0 2 4"sv, "0 1 2 3"sv, "3 2 0 1"sv, "3 2 1 0"sv},
    {"2 1 3 2 3 2 4 3"sv, "1 0 3 5 2 4 7 6"sv, "6 2 4 7 0 3 5 1"sv, "6 5 3 0 7 4 2 1"sv},
    {"3 1 4 2"sv, "1 3 0 2"sv, "2 0 3 1"sv, "3 2 1 0"sv},
    {"1 3 0 0 1 2 1 4"sv, "2 3 0 4 6 5 1 7"sv, "7 1 5 0 4 6 2 3"sv, "7 5 3 2 6 4 1 0"sv},
    {"3 4 4 3 1 3 1"sv, "4 6 0 3 5 1 2"sv, "1 2 0 3 5 4 6"sv, "2 1 6 5 4 3 0"sv},
    {"1 0 2 0"sv, "1 3 0 2"sv, "2 0 1 3"sv, "3 2 1 0"sv},
    {"0 1 4 0 3 0 0 2 2 3"sv, "0 3 5 6 1 7 8 4 9 2"sv, "2 4 9 7 8 1 0 3 5 6"sv, "8 7 6 5 3 2 0 9 4 1"sv},
    {"4 0 0 1 2"sv, "1 2 3 4 0"sv, "0 4 3 1 2"sv, "4 2 1 0 3"sv},
    {"2 2 0 2 1 1 2 3 2 3 2 2 4 0 0 3 1 1 0 4 1"sv, "2 13 14 18 4 5 16 17 20 0 1 3 6 8 10 11 7 9 15 12 19"sv, "12 19 7 9 15 0 1 3 6 8 10 11 4 5 16 17 20 2 13 14 18"sv, "19 18 14 13 12 11 10 8 6 3 2 1 0 20 17 16 15 9 7 5 4"sv},
    {"2 2 3 0 4 0 4 3 0 4 0 2 2 0 1 1 0 2 2 4 3 0 0 3 1 0 2 0 3 3 4 1 3 1 0 4 2 3 4 4 2 1 0 0 3 0 1 4 0 2 1 3 0 1 4 3 0 4 0 2 4"sv, "3 5 8 10 13 16 21 22 25 27 34 42 43 45 48 52 56 58 14 15 24 31 33 41 46 50 53 0 1 11 12 17 18 26 36 40 49 59 2 7 20 23 28 29 32 37 44 51 55 4 6 9 19 30 35 38 39 47 54 57 60"sv, "4 6 9 19 30 35 38 39 47 54 57 60 2 7 20 23 28 29 32 37 44 51 55 0 1 11 12 17 18 26 36 40 49 59 14 15 24 31 33 41 46 50 53 3 5 8 10 13 16 21 22 25 27 34 42 43 45 48 52 56 58"sv, "60 59 58 57 56 54 52 49 48 47 45 43 42 40 39 38 36 35 34 30 27 26 25 22 21 19 18 17 16 13 12 11 10 9 8 6 5 4 3 1 0 55 53 51 50 46 44 41 37 33 32 31 29 28 24 23 20 15 14 7 2"sv},
    {"0 3 0 1 2 1 4 3 0 4 3 3 0 2 1 4 1 3 2 0 1 4 4 2 2 3 3 4 1 1 3 2 0 4 3 1 1 4 4 1 0 4 4 0 3 1 0 3 3 4 3 4 3 0 3 1 1 2 0 3 0"sv, "0 2 8 12 19 32 40 43 46 53 58 60 3 5 14 16 20 28 29 35 36 39 45 55 56 4 13 18 23 24 31 57 1 7 10 11 17 25 26 30 34 44 47 48 50 52 54 59 6 9 15 21 22 27 33 37 38 41 42 49 51"sv, "6 9 15 21 22 27 33 37 38 41 42 49 51 1 7 10 11 17 25 26 30 34 44 47 48 50 52 54 59 4 13 18 23 24 31 57 3 5 14 16 20 28 29 35 36 39 45 55 56 0 2 8 12 19 32 40 43 46 53 58 60"sv, "60 58 57 53 51 49 46 43 42 41 40 38 37 33 32 31 27 24 23 22 21 19 18 15 13 12 9 8 6 4 2 0 59 56 55 54 52 50 48 47 45 44 39 36 35 34 30 29 28 26 25 20 17 16 14 11 10 7 5 3 1"sv},
    {"4 4 4 2 2 0 4 4 2 2 2 2 4 0 0 1 2"sv, "5 13 14 15 3 4 8 9 10 11 16 0 1 2 6 7 12"sv, "0 1 2 6 7 12 3 4 8 9 10 11 16 15 5 13 14"sv, "16 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0 15"sv},
    {"3 4 3 0 4 2 0 1 4 0 4 0 4"sv, "3 6 9 11 7 5 0 2 1 4 8 10 12"sv, "1 4 8 10 12 0 2 5 7 3 6 9 11"sv, "12 11 10 9 8 6 5 4 3 1 7 2 0"sv},
    {"4"sv, "0"sv, "0"sv, "0"sv},
    {"3 3 1 0 4 4 1 4 1 2 0 4 2 2 0 0 0 1 4 3 0 0 1 0 3 3 2 0 1 2 3 4 4 0 0 3 0 0 1 3 3 4"sv, "3 10 14 15 16 20 21 23 27 33 34 36 37 2 6 8 17 22 28 38 9 12 13 26 29 0 1 19 24 25 30 35 39 40 4 5 7 11 18 31 32 41"sv, "4 5 7 11 18 31 32 41 0 1 19 24 25 30 35 39 40 9 12 13 26 29 2 6 8 17 22 28 38 3 10 14 15 16 20 21 23 27 33 34 36 37"sv, "41 37 36 34 33 32 31 29 27 26 23 21 20 18 16 15 14 13 12 11 10 9 7 5 4 3 40 39 38 35 30 28 25 24 22 19 17 8 6 2 1 0"sv},
    {"4 4 4 1 4 1 1 3 0 1 2 4 0 4 3 3 0 3 1 1"sv, "8 12 16 3 5 6 9 18 19 10 7 14 15 17 0 1 2 4 11 13"sv, "0 1 2 4 11 13 7 14 15 17 10 3 5 6 9 18 19 8 12 16"sv, "16 13 12 11 10 8 4 2 1 0 19 18 17 15 14 9 7 6 5 3"sv},
    {"3 4 1 4 1"sv, "2 4 0 1 3"sv, "1 3 0 2 4"sv, "3 1 4 2 0"sv},
    {"1 0 3 1 2 3 0 3 0 1 4 0 0 4 2 1 1 3 4 3 0 1 1 2 2 4 0 0 4 4 0 2 3 1 1 1 1 3 4 4 4 4 2 2 4 0 1 2 0 3 2 4 2 1 1 2 1 3 2 4 2 0 3 0 1 3 3 1 4"sv, "1 6 8 11 12 20 26 27 30 45 48 61 63 0 3 9 15 16 21 22 33 34 35 36 46 53 54 56 64 67 4 14 23 24 31 42 43 47 50 52 55 58 60 2 5 7 17 19 32 37 49 57 62 65 66 10 13 18 25 28 29 38 39 40 41 44 51 59 68"sv, "10 13 18 25 28 29 38 39 40 41 44 51 59 68 2 5 7 17 19 32 37 49 57 62 65 66 4 14 23 24 31 42 43 47 50 52 55 58 60 0 3 9 15 16 21 22 33 34 35 36 46 53 54 56 64 67 1 6 8 11 12 20 26 27 30 45 48 61 63"sv, "68 63 61 60 59 58 55 52 51 50 48 47 45 44 43 42 41 40 39 38 31 30 29 28 27 26 25 24 23 20 18 14 13 12 11 10 8 6 4 1 67 66 65 64 62 57 56 54 53 49 46 37 36 35 34 33 32 22 21 19 17 16 15 9 7 5 3 2 0"sv},
  };
  const ArrayCase kArrays[] = {
    {"[]"sv, "000000000000"sv, "-1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1"sv, "undefined undefined undefined undefined undefined undefined undefined undefined undefined undefined undefined"sv, "[] [] [] [] [] []"sv},
    {"[1,2,3]"sv, "001000000000"sv, "-1 -1 0 -1 -1 -1 -1 -1 -1 -1 -1 -1"sv, "undefined 1 2 3 1 2 3 undefined 1 1 1"sv, "[1,2,3] [1,2,3] [1,2,3] [1,2,3] [1,2,3] [1,2,3]"sv},
    {"[0,\"0\",null,false,\"\"]"sv, "110111100000"sv, "0 0 -1 1 2 3 4 -1 -1 -1 -1 -1"sv, "\"0\" null false \"\" 0 \"0\" null false 0 0 0"sv, "[0,\"0\",null,false,\"\"] [0,\"0\",null,false,\"\"] [0,\"0\",null,false,\"\"] [0,\"0\",null,false,\"\"] [0,\"0\",null,false,\"\"] [0,\"0\",null,false,\"\"]"sv},
    {"[[1,[2,[3,[4]]]],5,[]]"sv, "000000000000"sv, "-1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1"sv, "undefined [1,[2,[3,[4]]]] 5 [] [1,[2,[3,[4]]]] 5 [] undefined [1,[2,[3,[4]]]] [1,[2,[3,[4]]]] [1,[2,[3,[4]]]]"sv, "[[1,[2,[3,[4]]]],5,[]] [1,[2,[3,[4]]],5] [1,2,[3,[4]],5] [1,2,3,4,5] [[1,[2,[3,[4]]]],5,[]] [[1,[2,[3,[4]]]],5,[]]"sv},
    {"[{\"a\":1},[1],\"x\"]"sv, "000000000100"sv, "-1 -1 -1 -1 -1 -1 -1 -1 -1 2 -1 -1"sv, "undefined {\"a\":1} [1] \"x\" {\"a\":1} [1] \"x\" undefined {\"a\":1} {\"a\":1} {\"a\":1}"sv, "[{\"a\":1},[1],\"x\"] [{\"a\":1},1,\"x\"] [{\"a\":1},1,\"x\"] [{\"a\":1},1,\"x\"] [{\"a\":1},[1],\"x\"] [{\"a\":1},[1],\"x\"]"sv},
    {"[-0,1e21,1.5]"sv, "110000011000"sv, "0 0 -1 -1 -1 -1 -1 1 2 -1 -1 -1"sv, "undefined 0 1e+21 1.5 0 1e+21 1.5 undefined 0 0 0"sv, "[0,1e+21,1.5] [0,1e+21,1.5] [0,1e+21,1.5] [0,1e+21,1.5] [0,1e+21,1.5] [0,1e+21,1.5]"sv},
  };
  constexpr std::string_view kReplacerDoc = "{\"a\":1,\"_hidden\":2,\"big\":42,\"list\":[5,50,{\"_x\":1,\"y\":99}],\"nested\":{\"k\":\"v\",\"n\":11},\"drop\":\"me\"}"sv;
  constexpr std::string_view kReplacer1 = "{\"a\":1,\"big\":\"42\",\"list\":[5,\"50\",{\"y\":\"99\"}],\"nested\":{\"k\":\"v\",\"n\":\"11\"},\"drop\":\"me\"}"sv;
  constexpr std::string_view kReplacer2 = "{\012  \"a\": 1,\012  \"_hidden\": 2,\012  \"big\": 42,\012  \"list\": [\012    null,\012    3\012  ],\012  \"nested\": {\012    \"k\": \"v\",\012    \"n\": 11\012  }\012}"sv;
  constexpr bool kReplacer3Undefined = true;
  constexpr std::string_view kReplacerKeys = "|a|_hidden|big|list|0|1|2|_x|y|nested|k|n|drop"sv;
  constexpr std::string_view kReplacer4 = "{\"a\":1,\"_hidden\":2,\"big\":42,\"list\":[5,50,{\"_x\":1,\"y\":99}],\"nested\":{\"1\":\"a\",\"2\":\"b\",\"z\":11},\"drop\":\"me\"}"sv;
  constexpr std::string_view kReplacer5 = "ok"sv;
  // @@TABLES-END

  double fromBits(std::uint64_t bits) { return std::bit_cast<double>(bits); }

  std::string printable(std::string_view text) { return rt::jsonQuote(rt::toWellFormed(text)); }

  std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    if (text.empty()) return parts;
    std::size_t start = 0;
    while (true) {
      const std::size_t end = text.find(separator, start);
      parts.emplace_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
    return parts;
  }

  std::string showNumber(double value) { return rt::sameValue(value, -0.0) ? "-0" : rt::numberToString(value); }

  std::string joinNumbers(const std::vector<double>& values) {
    std::string out;
    for (std::size_t index = 0; index < values.size(); ++index) out += (index ? " " : "") + showNumber(values[index]);
    return out;
  }

  std::string joinIds(const std::vector<std::pair<double, int>>& records) {
    std::string out;
    for (std::size_t index = 0; index < records.size(); ++index) out += (index ? " " : "") + std::to_string(records[index].second);
    return out;
  }

  json::array keysArray(const json::object& object) {
    json::array keys;
    for (const auto& key : rt::objectKeys(object)) keys.emplace_back(key);
    return keys;
  }
}

TEST("JSON.parse validates whole documents and preserves the values of supported JSON") {
  int accepted = 0, rejected = 0;
  for (const auto& c : kParse) {
    try {
      const json::value value = rt::jsonParse(c.input);
      if (!c.ok) {
        ::uos::rt::test::report(__FILE__, __LINE__, "JSON.parse(" + printable(c.input) + ") should throw SyntaxError", false);
        continue;
      }
      const std::string rendered = rt::jsonStringify(value);
      const bool matches = rendered == c.expected ||
                           ((value.is_object() || value.is_array()) &&
                            rt::deepEqual(rt::jsonParse(rendered), rt::jsonParse(c.expected)));
      if (!matches) {
        ::uos::rt::test::report(__FILE__, __LINE__, "JSON.parse(" + printable(c.input) + "): " + printable(rendered) + " != " + printable(c.expected), false);
      }
      ++accepted;
    } catch (const rt::SyntaxError& error) {
      if (c.ok) {
        ::uos::rt::test::report(__FILE__, __LINE__, "JSON.parse(" + printable(c.input) + ") threw " + error.message, false);
        continue;
      }
      EXPECT_EQ(error.message, "Invalid JSON");
      ++rejected;
    }
  }
  EXPECT(accepted > 100);
  EXPECT(rejected > 300);
}

TEST("JSON.parse keeps -0, stores canonical numbers, last duplicate wins in place, __proto__ is a key") {
  const json::value negativeZero = rt::jsonParse("-0");
  ASSERT(negativeZero.is_double());
  EXPECT(std::signbit(negativeZero.get_double()));
  EXPECT(rt::jsonParse("[-0.0]").get_array()[0].is_double());
  EXPECT_EQ(rt::jsonParse("1"), json::value(1));
  EXPECT_EQ(rt::jsonParse("1.0"), json::value(1));
  EXPECT_EQ(rt::jsonParse("1e2"), json::value(100));
  EXPECT_EQ(rt::jsonParse("-9007199254740991"), json::value(-9007199254740991));
  EXPECT(rt::jsonParse("9007199254740992").is_double());
  EXPECT(rt::jsonParse("0.5").is_double());
  EXPECT_EQ(rt::jsonParse(R"({"ok":true,"n":[1,2.5]})"), json::value({{"ok", true}, {"n", {1, 2.5}}}));
  const json::value duplicate = rt::jsonParse(R"({"b":1,"a":2,"b":3})");
  EXPECT_EQ(rt::jsonStringify(duplicate), R"({"b":3,"a":2})");
  EXPECT_EQ(std::string(duplicate.get_object().begin()->key()), "b");
  const json::value proto = rt::jsonParse(R"({"__proto__":{"x":1}})");
  EXPECT(rt::hasOwn(proto.get_object(), "__proto__"));
  const json::value indexes = rt::jsonParse(R"({"b":1,"2":2,"a":3,"1":4})");
  std::string storage;
  for (const auto& member : indexes.get_object()) storage += std::string(member.key()) + ",";
  EXPECT_EQ(storage, "b,2,a,1,");
  EXPECT_THROWS_MESSAGE(rt::jsonParse("[1,]"), rt::SyntaxError, "Invalid JSON");
  EXPECT_EQ(rt::jsonParse("\"\\ud83d\\ude00\"").get_string(), "\xF0\x9F\x98\x80");
  EXPECT_EQ(rt::jsonParse("\"\\ud800\"").get_string(), "\xED\xA0\x80");
  EXPECT_THROWS_MESSAGE(rt::jsonParse("\"\xED\xA0\xBD\\ude00\""), rt::SyntaxError, "Invalid JSON");
  EXPECT_THROWS_MESSAGE(rt::jsonParse("\"\xED\xA0\xBD\xED\xB8\x80\""), rt::SyntaxError, "Invalid JSON");
}

TEST("Response.json() / Request.json() parse like JSON.parse with Bun's body messages") {
  // new Response(bytes).json() in Bun 1.4: [] -> "Unexpected end of JSON input";
  // [0xEF,0xBB,0xBF], [0x20], [0x7B], "[1,]" -> "Failed to parse JSON"; [0xEF,0xBB,0xBF,0x31] -> 1.
  EXPECT_THROWS_MESSAGE(rt::jsonParseBody("", true), rt::SyntaxError, "Unexpected end of JSON input");
  EXPECT_THROWS_MESSAGE(rt::jsonParseBody("", false), rt::SyntaxError, "Failed to parse JSON");
  EXPECT_THROWS_MESSAGE(rt::jsonParseBody(" ", false), rt::SyntaxError, "Failed to parse JSON");
  EXPECT_THROWS_MESSAGE(rt::jsonParseBody("{", false), rt::SyntaxError, "Failed to parse JSON");
  EXPECT_THROWS_MESSAGE(rt::jsonParseBody("[1,]", false), rt::SyntaxError, "Failed to parse JSON");
  EXPECT_EQ(rt::jsonParseBody("1", false), json::value(1));
  EXPECT_EQ(rt::jsonStringify(rt::jsonParseBody(R"({"b":1,"2":2,"a":3,"b":4})", false)), R"({"b":4,"2":2,"a":3})");
}

TEST("JSON.stringify formats every number kind like JavaScriptCore") {
  for (const auto& c : kNumbers) {
    const double number = fromBits(c.bits);
    EXPECT_EQ(rt::jsonStringify(rt::makeNumber(number)), std::string(c.expected));
    EXPECT_EQ(rt::jsonStringify(json::value(number)), std::string(c.expected));
    if (std::isfinite(number) && std::trunc(number) == number && std::fabs(number) < 9.2e18) {
      EXPECT_EQ(rt::jsonStringify(json::value(static_cast<std::int64_t>(number))), std::string(c.expected));
    }
  }
  EXPECT_EQ(rt::jsonStringify(json::value(std::int64_t{9007199254740993})), "9007199254740992");
  EXPECT_EQ(rt::jsonStringify(json::value(std::uint64_t{18446744073709551615u})), "18446744073709552000");
  EXPECT_EQ(rt::jsonStringify(json::value(std::int64_t{-9223372036854775807 - 1})), "-9223372036854776000");
}

TEST("JSON.stringify escapes strings like JavaScriptCore") {
  for (const auto& c : kEscapes) {
    EXPECT_EQ(rt::jsonQuote(c.text), std::string(c.expected));
    EXPECT_EQ(rt::jsonStringify(json::value(json::string(c.text))), std::string(c.expected));
  }
  // A split surrogate pair (non-canonical WTF-8) is the pair; invalid bytes read as U+FFFD.
  EXPECT_EQ(rt::jsonQuote("\xED\xA0\xBD\xED\xB8\x80"), "\"\xF0\x9F\x98\x80\"");
  EXPECT_EQ(rt::jsonQuote("a\xFF" "b"), "\"a\xEF\xBF\xBD" "b\"");
}

TEST("JSON.stringify indents values in storage order") {
  for (const auto& c : kIndents) {
    const json::value value = rt::jsonParse(c.source);
    const std::string actual = c.useGap ? rt::jsonStringify(value, c.gap) : rt::jsonStringify(value, c.space);
    if (actual != c.expected) ::uos::rt::test::report(__FILE__, __LINE__, "JSON.stringify(" + std::string(c.source) + ", null, " + (c.useGap ? printable(c.gap) : std::to_string(c.space)) + ") = " + printable(actual) + ", want " + printable(c.expected), false);
  }
}

TEST("JSON.stringify with a replacer function") {
  const json::value document = rt::jsonParse(kReplacerDoc);
  const rt::JsonReplacer hideAndStringify = [](std::string_view key, const json::value& value) -> std::optional<json::value> {
    if (key.starts_with("_")) return std::nullopt;
    if (rt::isNumber(value) && rt::toNumber(value) > 10) return json::value(rt::toString(value));
    return value;
  };
  EXPECT_EQ(rt::jsonStringify(document, hideAndStringify), std::optional<std::string>(std::string(kReplacer1)));
  const rt::JsonReplacer dropAndReplace = [](std::string_view key, const json::value& value) -> std::optional<json::value> {
    if (key == "drop") return std::nullopt;
    if (key == "list") return json::value(json::array{nullptr, static_cast<std::int64_t>(value.get_array().size())});
    return value;
  };
  EXPECT_EQ(rt::jsonStringify(document, dropAndReplace, "  "), std::optional<std::string>(std::string(kReplacer2)));
  const rt::JsonReplacer dropRoot = [](std::string_view key, const json::value& value) -> std::optional<json::value> {
    if (key.empty()) return std::nullopt;
    return value;
  };
  EXPECT_EQ(rt::jsonStringify(document, dropRoot).has_value(), !kReplacer3Undefined);
  std::string keys;
  const rt::JsonReplacer recordKeys = [&keys](std::string_view key, const json::value& value) -> std::optional<json::value> {
    keys += (keys.empty() && key.empty() ? "" : "|") + std::string(key);
    return value;
  };
  (void)rt::jsonStringify(document, recordKeys);
  EXPECT_EQ(keys, std::string(kReplacerKeys));
  const rt::JsonReplacer swapNested = [](std::string_view key, const json::value& value) -> std::optional<json::value> {
    if (key != "nested") return value;
    json::object replacement;
    rt::objectDefine(replacement, "2", "b");
    rt::objectDefine(replacement, "1", "a");
    rt::objectDefine(replacement, "z", *rt::get(value, "n"));
    return json::value(std::move(replacement));
  };
  EXPECT_EQ(rt::jsonStringify(document, swapNested), std::optional<std::string>(std::string(kReplacer4)));
  // optional-telemetry-queue.ts measureJsonPayloadBytes: a throwing replacer aborts JSON.stringify.
  const rt::JsonReplacer rejectNonFinite = [](std::string_view, const json::value& value) -> std::optional<json::value> {
    if (rt::isNumber(value) && !std::isfinite(rt::toNumber(value))) throw rt::TypeError("non-finite number");
    return value;
  };
  EXPECT_EQ(std::string(kReplacer5), "ok");
  EXPECT(rt::jsonStringify(document, rejectNonFinite).has_value());
  json::value withInfinity = document;
  withInfinity.get_object()["big"] = rt::Number::POSITIVE_INFINITY;
  EXPECT_THROWS_MESSAGE(rt::jsonStringify(withInfinity, rejectNonFinite), rt::TypeError, "non-finite number");
}

TEST("String(v), Number(v), Boolean(v) and typeof v on JSON values") {
  for (const auto& c : kCoercions) {
    const json::value value = rt::jsonParse(c.source);
    EXPECT_EQ(rt::toString(value), std::string(c.string));
    const double number = rt::toNumber(value);
    const double expected = fromBits(c.number);
    if (!rt::sameValue(number, expected)) ::uos::rt::test::report(__FILE__, __LINE__, "Number(" + std::string(c.source) + ") = " + showNumber(number) + ", want " + showNumber(expected), false);
    EXPECT_EQ(rt::truthy(value), c.truthy);
    EXPECT_EQ(std::string(rt::typeOf(value)), std::string(c.type));
  }
  EXPECT_EQ(std::string(rt::typeOf(nullptr)), "undefined");
  EXPECT_EQ(rt::toString(static_cast<const json::value*>(nullptr)), "undefined");
  EXPECT(std::isnan(rt::toNumber(static_cast<const json::value*>(nullptr))));
  EXPECT(!rt::truthy(static_cast<const json::value*>(nullptr)));
  EXPECT(rt::isNullish(nullptr));
  const json::value null = nullptr;
  EXPECT(rt::isNullish(&null));
  EXPECT(!rt::truthy(rt::Number::NaN));
  EXPECT(!rt::truthy(-0.0));
  EXPECT(rt::truthy(std::string_view("0")));
  EXPECT(!rt::truthy(std::optional<std::string>()));
  EXPECT(!rt::truthy(std::optional<std::string>("")));
  EXPECT(rt::truthy(std::int64_t{-1}));
}

TEST("Number.isFinite / isNaN / isInteger / isSafeInteger on unknown values") {
  EXPECT(rt::Number::isSafeInteger(rt::jsonParse("9007199254740991")));
  EXPECT(!rt::Number::isSafeInteger(rt::jsonParse("9007199254740992")));
  EXPECT(!rt::Number::isSafeInteger(rt::jsonParse("\"1\"")));
  EXPECT(!rt::Number::isInteger(rt::jsonParse("1.5")));
  EXPECT(rt::Number::isInteger(rt::jsonParse("1e300")));
  EXPECT(!rt::Number::isFinite(rt::jsonParse("\"1\"")));
  EXPECT(rt::Number::isFinite(rt::jsonParse("-0")));
  EXPECT(!rt::Number::isFinite(json::value(rt::Number::POSITIVE_INFINITY)));
  EXPECT(!rt::Number::isNaN(rt::jsonParse("\"x\"")));
  EXPECT(rt::Number::isNaN(json::value(rt::Number::NaN)));
  EXPECT(rt::isNaN(rt::jsonParse("\"x\"")));
  EXPECT(!rt::isNaN(rt::jsonParse("\" 12 \"")));
  EXPECT(!rt::isFinite(rt::jsonParse("{}")));
  EXPECT(rt::isFinite(rt::jsonParse("[]")));
}

TEST("deepEqual and matchesObject agree with assert.deepEqual, toEqual, Bun.deepEquals and toMatchObject") {
  for (const auto& c : kEquality) {
    const json::value a = rt::jsonParse(c.a), b = rt::jsonParse(c.b);
    if (rt::deepEqual(a, b) != c.deepEqual) ::uos::rt::test::report(__FILE__, __LINE__, "deepEqual(" + std::string(c.a) + ", " + std::string(c.b) + ")", false);
    if (rt::matchesObject(a, b) != c.matches) ::uos::rt::test::report(__FILE__, __LINE__, "matchesObject(" + std::string(c.a) + ", " + std::string(c.b) + ")", false);
  }
  EXPECT(rt::deepEqual(json::value(rt::Number::NaN), json::value(rt::Number::NaN)));
  EXPECT(rt::deepEqual(json::value(1), json::value(1.0)));
  EXPECT(rt::deepEqual(json::value(std::uint64_t{7}), json::value(7.0)));
}

TEST("strict equality, SameValue and SameValueZero") {
  EXPECT(rt::strictEquals(json::value(0), json::value(-0.0)));
  EXPECT(!rt::strictEquals(json::value(rt::Number::NaN), json::value(rt::Number::NaN)));
  EXPECT(rt::strictEquals(json::value(1), json::value(1.0)));
  EXPECT(!rt::strictEquals(json::value("1"), json::value(1)));
  EXPECT(rt::strictEquals(json::value(nullptr), json::value(nullptr)));
  const json::value object = rt::jsonParse("{}");
  const json::value same = object;
  EXPECT(rt::strictEquals(object, object));
  EXPECT(!rt::strictEquals(object, same));
  EXPECT(rt::sameValue(rt::Number::NaN, rt::Number::NaN));
  EXPECT(!rt::sameValue(0.0, -0.0));
  EXPECT(rt::sameValueZero(0.0, -0.0));
  EXPECT(!rt::sameValueZero(1.0, rt::Number::NaN));
}

TEST("property order follows JS enumeration under [[Set]], CreateDataProperty and delete") {
  for (const auto& c : kOrders) {
    json::value document = rt::jsonParse(c.start);
    json::object& object = document.get_object();
    for (const auto& op : split(c.ops, ';')) {
      const char kind = op[0];
      const std::size_t equals = op.find('=');
      const std::string key = op.substr(1, equals == std::string::npos ? std::string::npos : equals - 1);
      if (kind == 'd') {
        rt::objectDelete(object, key);
        continue;
      }
      const json::value value = rt::makeNumber(rt::stringToNumber(op.substr(equals + 1)));
      if (kind == 's') rt::objectSet(object, key, value);
      else rt::objectDefine(object, key, value);
    }
    if (rt::jsonStringify(document) != c.json || rt::jsonStringify(json::value(keysArray(object))) != c.keys)
      ::uos::rt::test::report(__FILE__, __LINE__, std::string(c.start) + " then " + std::string(c.ops) + ": " + rt::jsonStringify(document) + " keys " + rt::jsonStringify(json::value(keysArray(object))) + ", want " + std::string(c.json) + " keys " + std::string(c.keys), false);
    // Storage stays in JS order, so plain iteration agrees with Object.keys.
    json::array storage;
    for (const auto& member : object) storage.emplace_back(member.key());
    EXPECT_EQ(rt::jsonStringify(json::value(storage)), std::string(c.keys));
  }
}

TEST("object spread and Object.assign") {
  for (const auto& c : kMerges) {
    const json::value a = rt::jsonParse(c.a), b = rt::jsonParse(c.b);
    EXPECT_EQ(rt::jsonStringify(json::value(rt::objectSpread(a.get_object(), b.get_object()))), std::string(c.spread));
    json::value target = a;
    rt::objectAssign(target.get_object(), b.get_object());
    const json::value expected = rt::jsonParse(c.assigned);
    EXPECT(rt::deepEqual(target, expected));
    EXPECT_EQ(rt::objectKeys(target.get_object()), rt::objectKeys(expected.get_object()));
  }
  json::value nested = rt::jsonParse(R"({"b":0,"child":{"2":"two","1":"one","b":1}})");
  rt::objectAssign(nested.get_object(), nested.get_object().at("child").get_object());
  EXPECT_EQ(rt::jsonStringify(nested), R"({"1":"one","2":"two","b":1,"child":{"2":"two","1":"one","b":1}})");
  const json::object one = rt::jsonParse(R"({"a":1})").get_object();
  EXPECT_EQ(rt::jsonStringify(json::value(rt::objectSpread(one))), R"({"a":1})");
  EXPECT_EQ(rt::jsonStringify(json::value(rt::objectFromEntries({{"b", 1}, {"1", 2}, {"b", 3}, {"__proto__", 4}}))), R"({"1":2,"b":3,"__proto__":4})");
}

TEST("members, typed getters and Object.keys/values/entries") {
  json::value document = rt::jsonParse(R"({"s":"x","n":1.5,"i":3,"b":false,"o":{"k":null},"a":[1],"z":null,"1":"one"})");
  const json::object& object = document.get_object();
  EXPECT_EQ(rt::getString(object, "s"), std::optional<std::string_view>("x"));
  EXPECT_EQ(rt::getString(object, "n"), std::nullopt);
  EXPECT_EQ(rt::getNumber(object, "n"), std::optional<double>(1.5));
  EXPECT_EQ(rt::getNumber(object, "i"), std::optional<double>(3));
  EXPECT_EQ(rt::getNumber(object, "s"), std::nullopt);
  EXPECT_EQ(rt::getBool(object, "b"), std::optional<bool>(false));
  EXPECT(rt::getObject(object, "o") != nullptr);
  EXPECT(rt::getObject(object, "a") == nullptr);
  EXPECT(rt::getArray(object, "a") != nullptr);
  EXPECT(rt::getObject(object, "z") == nullptr);
  EXPECT(rt::get(object, "missing") == nullptr);
  EXPECT(rt::get(object, "toString") == nullptr);
  EXPECT(rt::get(object, "z") != nullptr && rt::get(object, "z")->is_null());
  EXPECT(rt::isRecord(rt::get(object, "o")));
  EXPECT(!rt::isRecord(rt::get(object, "a")));
  EXPECT(!rt::isRecord(rt::get(object, "z")));
  EXPECT(rt::get(rt::get(object, "o"), "k") != nullptr);
  EXPECT(rt::get(rt::get(object, "a"), "0") == nullptr);
  EXPECT_EQ(rt::getString(document, "s"), std::optional<std::string_view>("x"));
  EXPECT(rt::get(rt::get(object, "a")->get_array(), 0) != nullptr);
  EXPECT(rt::get(rt::get(object, "a")->get_array(), 1) == nullptr);
  EXPECT(rt::hasOwn(object, "z"));
  EXPECT(!rt::hasOwn(object, "constructor"));
  EXPECT_EQ(rt::objectKeys(object), std::vector<std::string>({"1", "s", "n", "i", "b", "o", "a", "z"}));
  EXPECT_EQ(rt::objectValues(object).size(), 8u);
  EXPECT_EQ(rt::jsonStringify(*rt::objectValues(object)[0]), "\"one\"");
  const auto entries = rt::objectEntries(object);
  EXPECT_EQ(std::string(entries[1]->key()), "s");
  json::object built;
  built["b"] = 1;
  built["2"] = 2;
  built["a"] = 3;
  EXPECT_EQ(rt::objectKeys(built), std::vector<std::string>({"2", "b", "a"}));
  EXPECT_EQ(rt::jsonStringify(json::value(built)), R"({"b":1,"2":2,"a":3})");
  json::object mutableObject = document.get_object();
  json::object* inner = rt::getObject(mutableObject, "o");
  ASSERT(inner != nullptr);
  (*inner)["k"] = 5;
  EXPECT_EQ(rt::jsonStringify(json::value(*inner)), R"({"k":5})");
  json::array* list = rt::getArray(mutableObject, "a");
  ASSERT(list != nullptr);
  list->push_back(2);
  EXPECT_EQ(list->size(), 2u);
  for (auto* entry : rt::objectEntries(mutableObject)) entry->value() = rt::toString(entry->value());
  EXPECT_EQ(rt::jsonStringify(json::value(mutableObject)), R"({"s":"x","n":"1.5","i":"3","b":"false","o":"[object Object]","a":"1,2","z":"null","1":"one"})");
}

TEST("canonical numbers: makeNumber, canonicalNumbers and writers") {
  EXPECT(rt::makeNumber(1.0).is_int64());
  EXPECT(rt::makeNumber(-9007199254740991.0).is_int64());
  EXPECT(rt::makeNumber(9007199254740992.0).is_double());
  EXPECT(rt::makeNumber(0.5).is_double());
  EXPECT(rt::makeNumber(-0.0).is_double());
  EXPECT(rt::makeNumber(0.0).is_int64());
  EXPECT(rt::makeNumber(rt::Number::NaN).is_double());
  EXPECT(rt::makeNumber(std::int64_t{9007199254740993}).is_double());
  EXPECT_EQ(rt::makeNumber(std::int64_t{9007199254740993}).get_double(), 9007199254740992.0);
  EXPECT(rt::isNumber(json::value(std::uint64_t{1})));
  EXPECT(!rt::isNumber(json::value("1")));
  const json::value canonical = rt::canonicalNumbers(json::value({{"a", 2.0}, {"b", {3.0, std::uint64_t{4}, -0.0, 0.25}}}));
  EXPECT_EQ(canonical, rt::jsonParse(R"({"a":2,"b":[3,4,-0,0.25]})"));
  EXPECT(canonical.get_object().at("b").get_array()[2].is_double());
  json::object object;
  rt::objectSet(object, "n", 7.0);
  EXPECT(object.at("n").is_int64());
  rt::objectDefine(object, "m", std::uint64_t{8});
  EXPECT(object.at("m").is_int64());
  EXPECT_EQ(rt::getNumber(json::value(std::int64_t{9007199254740993})), std::optional<double>(9007199254740992.0));
}

TEST("default sort orders by String(element) in UTF-16 code unit order") {
  for (const auto& c : kStringSorts) {
    const json::value parsed = rt::jsonParse(c.values);
    std::vector<std::string> values;
    for (const auto& value : parsed.get_array()) values.emplace_back(value.get_string());
    rt::sort(values);
    json::array sorted;
    for (const auto& value : values) sorted.emplace_back(value);
    EXPECT_EQ(rt::jsonStringify(json::value(sorted)), std::string(c.sorted));
    EXPECT_EQ(rt::jsonStringify(json::value(rt::toSorted(rt::jsonParse(c.values).get_array()))), std::string(c.sorted));
  }
  for (const auto& c : kNumberSorts) {
    std::vector<double> values;
    for (const auto& token : split(c.values, ' ')) values.push_back(rt::stringToNumber(token));
    const std::vector<double> copy = rt::toSorted(values);
    EXPECT_EQ(joinNumbers(rt::sort(values)), std::string(c.sorted));
    EXPECT_EQ(joinNumbers(copy), std::string(c.sorted));
  }
  for (const auto& c : kJsonSorts) {
    json::array values = rt::jsonParse(c.values).get_array();
    EXPECT_EQ(rt::jsonStringify(json::value(rt::sort(values))), std::string(c.sorted));
  }
}

TEST("comparator sort is stable and follows JS comparator results") {
  for (const auto& c : kRecordSorts) {
    std::vector<std::pair<double, int>> records;
    int id = 0;
    for (const auto& token : split(c.keys, ' ')) records.emplace_back(rt::stringToNumber(token), id++);
    const auto ascending = rt::toSorted(records, [](const auto& a, const auto& b) { return a.first - b.first; });
    const auto descending = rt::toSorted(records, [](const auto& a, const auto& b) { return b.first - a.first; });
    const auto chained = rt::toSorted(records, [](const auto& a, const auto& b) {
      const double parity = rt::mod(a.first, 2) - rt::mod(b.first, 2);
      return rt::truthy(parity) ? parity : static_cast<double>(b.second - a.second);
    });
    EXPECT_EQ(joinIds(ascending), std::string(c.ascending));
    EXPECT_EQ(joinIds(descending), std::string(c.descending));
    EXPECT_EQ(joinIds(chained), std::string(c.chained));
  }
  // Comparator results are numbers: NaN and 0 keep the input order.
  std::vector<int> values{3, 1, 2};
  rt::sort(values, [](int, int) { return rt::Number::NaN; });
  EXPECT_EQ(values, std::vector<int>({3, 1, 2}));
  json::array documents = rt::jsonParse(R"([{"k":2,"id":"a"},{"k":1,"id":"b"},{"k":2,"id":"c"},{"k":1,"id":"d"}])").get_array();
  rt::sort(documents, [](const json::value& a, const json::value& b) { return *rt::getNumber(a, "k") - *rt::getNumber(b, "k"); });
  EXPECT_EQ(rt::jsonStringify(json::value(documents)), R"([{"k":1,"id":"b"},{"k":1,"id":"d"},{"k":2,"id":"a"},{"k":2,"id":"c"}])");
}

TEST("sort stays memory-safe and a permutation under inconsistent comparators") {
  std::mt19937 random(12345);
  for (int round = 0; round < 50; ++round) {
    std::vector<int> values;
    const int size = static_cast<int>(random() % 200);
    for (int index = 0; index < size; ++index) values.push_back(index);
    rt::sort(values, [&random](int, int) { return static_cast<double>(static_cast<int>(random() % 3) - 1); });
    std::vector<int> seen(values.size(), 0);
    for (const int value : values) ++seen[static_cast<std::size_t>(value)];
    for (const int count : seen) EXPECT_EQ(count, 1);
  }
}

TEST("includes, indexOf, at and flat on arrays") {
  const std::vector<std::string_view> probes{"0", "-0", "1", "\"0\"", "null", "false", "\"\"", "1e21", "1.5", "\"x\"", "[1]", "{\"a\":1}"};
  for (const auto& c : kArrays) {
    const json::array values = rt::jsonParse(c.source).get_array();
    std::string includes, indexes;
    for (const auto& probe : probes) {
      const json::value search = rt::jsonParse(probe);
      includes += rt::includes(values, search) ? '1' : '0';
      indexes += (indexes.empty() ? "" : " ") + std::to_string(rt::indexOf(values, search));
    }
    EXPECT_EQ(includes, std::string(c.includes));
    EXPECT_EQ(indexes, std::string(c.indexOf));
    std::string ats;
    for (const double index : {-4.0, -3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0, 0.5, -0.5, rt::Number::NaN}) {
      const json::value* element = rt::at(values, index);
      ats += (ats.empty() ? "" : " ") + (element ? rt::jsonStringify(*element) : std::string("undefined"));
    }
    EXPECT_EQ(ats, std::string(c.at));
    std::string flats;
    for (const double depth : {0.0, 1.0, 2.0, rt::Number::POSITIVE_INFINITY, -1.0, rt::Number::NaN}) flats += (flats.empty() ? "" : " ") + rt::jsonStringify(json::value(rt::flat(values, depth)));
    EXPECT_EQ(flats, std::string(c.flat));
  }
  const std::vector<double> numbers{1, rt::Number::NaN, -0.0};
  EXPECT(rt::includes(numbers, rt::Number::NaN));
  EXPECT(rt::includes(numbers, 0.0));
  EXPECT_EQ(rt::indexOf(numbers, rt::Number::NaN), -1);
  EXPECT_EQ(rt::indexOf(numbers, 0.0), 2);
  const std::vector<std::string> words{"a", "b"};
  EXPECT(rt::includes(words, "b"));
  EXPECT_EQ(rt::indexOf(words, "c"), -1);
  EXPECT_EQ(*rt::at(words, -1), "b");
  EXPECT(rt::at(words, 2) == nullptr);
  EXPECT(rt::at(words, -3) == nullptr);
  const json::value array = rt::jsonParse("[{\"a\":1}]");
  EXPECT(rt::includes(array.get_array(), array.get_array()[0]));
  EXPECT_EQ(rt::flat(std::vector<std::vector<int>>{{1, 2}, {}, {3}}), std::vector<int>({1, 2, 3}));
}

namespace {
  json::value nestedArrayValue(std::size_t depth) {
    json::value value = json::array{};
    for (std::size_t level = 1; level < depth; ++level) {
      json::array parent;
      parent.emplace_back(std::move(value));
      value = std::move(parent);
    }
    return value;
  }

  json::value nestedObjectValue(std::size_t depth) {
    json::value value = 1;
    for (std::size_t level = 0; level < depth; ++level) {
      json::object parent;
      parent.emplace("a", std::move(value));
      value = std::move(parent);
    }
    return value;
  }

  std::string nestedArrays(std::size_t depth) { return std::string(depth, '[') + std::string(depth, ']'); }
  std::string nestedObjects(std::size_t depth) {
    std::string text;
    for (std::size_t level = 0; level < depth; ++level) text += "{\"a\":";
    return text + "1" + std::string(depth, '}');
  }
}

TEST("JSON parse accepts 128 nesting levels and refuses the next") {
  constexpr auto limit = rt::limits::kJsonDepth;
  const std::string deepest = nestedArrays(limit);
  const json::value arrays = rt::jsonParse(deepest);
  EXPECT(arrays.is_array());
  EXPECT(rt::jsonParse(nestedObjects(limit)).is_object());
  EXPECT_THROWS_MESSAGE(rt::jsonParse("[" + deepest + "]"), rt::SyntaxError, "Invalid JSON");
  EXPECT_THROWS_MESSAGE(rt::jsonParse(std::string(limit + 1, '[')), rt::SyntaxError, "Invalid JSON");
  EXPECT_THROWS_MESSAGE(rt::jsonParse(nestedObjects(limit + 1)), rt::SyntaxError, "Invalid JSON");
}

TEST("JSON parse accepts the exact document byte limit and refuses the next byte") {
  std::string document(rt::limits::kJsonDocumentBytes, 'a');
  document.front() = '"';
  document.back() = '"';
  EXPECT_EQ(rt::jsonParse(document).get_string().size(), document.size() - 2);
  document += ' ';
  EXPECT_THROWS_MESSAGE(rt::jsonParse(document), rt::SyntaxError, "Invalid JSON");
}

TEST("explicit JSON allowances respect both route limits and the image ceiling") {
  EXPECT_EQ(rt::jsonParseWithLimit("null", 4), json::value(nullptr));
  EXPECT_THROWS_MESSAGE(rt::jsonParseWithLimit("null ", 4), rt::SyntaxError, "Invalid JSON");

  std::string document = "null";
  document.resize(rt::limits::kImageEditJsonBytes, ' ');
  EXPECT_EQ(rt::jsonParseWithLimit(document, rt::limits::kImageEditJsonBytes), json::value(nullptr));
  document.push_back(' ');
  EXPECT_THROWS_MESSAGE(rt::jsonParseWithLimit(document, rt::limits::kImageEditJsonBytes), rt::SyntaxError, "Invalid JSON");
}

TEST("JSON values at the nesting limit can be copied and destroyed") {
  static_assert(rt::kJsonMaxDepth == 128);
  const auto array = rt::jsonParse(nestedArrays(rt::kJsonMaxDepth));
  const auto object = rt::jsonParse(nestedObjects(rt::kJsonMaxDepth));
  const json::value arrayCopy = array;
  const json::value objectCopy = object;
  EXPECT(arrayCopy.is_array());
  EXPECT(objectCopy.is_object());
}

TEST("depth: jsonStringify at the limit and past it") {
  const std::string deepest = nestedArrays(rt::kJsonMaxDepth);
  json::value value = nestedArrayValue(rt::kJsonMaxDepth);
  EXPECT_EQ(rt::jsonStringify(value), deepest);
  EXPECT_EQ(rt::jsonStringify(nestedObjectValue(rt::kJsonMaxDepth)), nestedObjects(rt::kJsonMaxDepth));
  value = json::value(json::array{std::move(value)});
  EXPECT_THROWS_MESSAGE(rt::jsonStringify(value), rt::RangeError, "Maximum call stack size exceeded.");
}

TEST("depth: deepEqual, matchesObject, String() and flat at the limit and past it") {
  json::value value = nestedArrayValue(rt::kJsonMaxDepth);
  EXPECT(rt::deepEqual(value, value));
  EXPECT(rt::matchesObject(value, value));
  EXPECT_EQ(rt::toString(value), "");
  EXPECT_EQ(rt::flat(value.get_array(), rt::Number::POSITIVE_INFINITY).size(), 0u);
  value = json::value(json::array{std::move(value)});
  EXPECT_THROWS_MESSAGE(rt::deepEqual(value, value), rt::RangeError, "Maximum call stack size exceeded.");
  EXPECT_THROWS_MESSAGE(rt::matchesObject(value, value), rt::RangeError, "Maximum call stack size exceeded.");
  EXPECT_THROWS_MESSAGE(rt::toString(value), rt::RangeError, "Maximum call stack size exceeded.");
  EXPECT_THROWS_MESSAGE(rt::canonicalNumbers(value), rt::RangeError, "Maximum call stack size exceeded.");
}

TEST("element is array[index] with a computed number index, not array.at") {
  // bun: ["x", "y", "z"][i] reads "x" for 0 and -0, "y" for 1, "z" for 2, and undefined for
  // every other index below; [].at(-1) / [1.5] differ from at().
  const std::vector<std::string> values{"x", "y", "z"};
  ASSERT(rt::element(values, 0) != nullptr);
  EXPECT_EQ(*rt::element(values, 0), std::string("x"));
  ASSERT(rt::element(values, -0.0) != nullptr);
  EXPECT_EQ(*rt::element(values, -0.0), std::string("x"));
  ASSERT(rt::element(values, 1) != nullptr);
  EXPECT_EQ(*rt::element(values, 1), std::string("y"));
  ASSERT(rt::element(values, 2.0) != nullptr);
  EXPECT_EQ(*rt::element(values, 2.0), std::string("z"));
  const double inf = std::numeric_limits<double>::infinity();
  for (const double index : {3.0, -1.0, 1.5, -0.5, 0.9999999999999999, std::nan(""), inf, -inf, 4294967294.0, 4294967295.0, 9007199254740992.0, 1e21, 5e-324})
    EXPECT(rt::element(values, index) == nullptr);
  EXPECT_EQ(*rt::at(values, -1), std::string("z"));
  EXPECT_EQ(*rt::at(values, 1.5), std::string("y"));
  const std::vector<std::string> none;
  EXPECT(rt::element(none, 0) == nullptr);
  EXPECT(rt::element(none, -0.0) == nullptr);
  std::vector<int> numbers{1, 2};
  *rt::element(numbers, 1.0) = 5;
  EXPECT_EQ(numbers[1], 5);
  const json::array array{"x", "y"};
  ASSERT(rt::element(array, 1) != nullptr);
  EXPECT_EQ(*rt::element(array, 1), json::value("y"));
  ASSERT(rt::element(array, -0.0) != nullptr);
  EXPECT_EQ(*rt::element(array, -0.0), json::value("x"));
  EXPECT(rt::element(array, 2) == nullptr);
  EXPECT(rt::element(array, 0.5) == nullptr);
  EXPECT(rt::element(array, -1) == nullptr);
  json::array mutableArray{1, 2};
  *rt::element(mutableArray, 0) = "a";
  EXPECT_EQ(mutableArray[0], json::value("a"));
}

TEST("rt::get with an integer index reads the element: a literal 0 is index 0, never a null key") {
  const json::value array = rt::jsonParse(R"(["a","b"])");
  const json::value object = rt::jsonParse(R"({"0":"zero","1":"one","x":1})");
  ASSERT(rt::get(array, 0) != nullptr);
  EXPECT(rt::get(array, 0)->as_string() == "a");
  EXPECT(rt::get(array, 1)->as_string() == "b");
  EXPECT(rt::get(array, 2) == nullptr);
  EXPECT(rt::get(array, -1) == nullptr);
  EXPECT(rt::get(array, 1u)->as_string() == "b");
  EXPECT(rt::get(array, 1LL)->as_string() == "b");
  EXPECT(rt::get(array, std::size_t{1})->as_string() == "b");
  // obj[0] is obj["0"]; an absent index and a non-container read nothing.
  EXPECT(rt::get(object, 0)->as_string() == "zero");
  EXPECT(rt::get(object, 7) == nullptr);
  EXPECT(rt::get(object, -1) == nullptr);
  EXPECT(rt::get(json::value("str"), 0) == nullptr);
  EXPECT(rt::get(json::value(nullptr), 0) == nullptr);
  const json::value* pointer = &array;
  EXPECT(rt::get(pointer, 1)->as_string() == "b");
  const json::value* none = nullptr;
  EXPECT(rt::get(none, 0) == nullptr);
  // json::array and json::object arguments.
  const json::array& items = array.as_array();
  EXPECT(rt::get(items, 0)->as_string() == "a");
  EXPECT(rt::get(items, -1) == nullptr);
  EXPECT(rt::get(items, std::size_t{1})->as_string() == "b");
  const json::object& members = object.as_object();
  EXPECT(rt::get(members, 1)->as_string() == "one");
  json::object editable = object.as_object();
  json::value* member = rt::get(editable, 1);
  ASSERT(member != nullptr);
  *member = "uno";
  EXPECT(rt::get(editable, "1")->as_string() == "uno");
  // String keys are unchanged.
  EXPECT(rt::getNumber(rt::get(object, "x")) == std::optional<double>(1.0));
  EXPECT(rt::get(object, std::string("0"))->as_string() == "zero");
}

TEST("JSON.parse projects decoded duplicates, input order and numeric values into owned storage") {
  const json::value duplicate = rt::jsonParse(R"({"a":{"gone":[{"leaf":1}]},"b":{"k":1,"\u006b":2},"a":null})");
  EXPECT_EQ(rt::jsonStringify(duplicate), R"({"a":null,"b":{"k":2}})");
  const auto& duplicateObject = duplicate.get_object();
  EXPECT_EQ(std::string(duplicateObject.begin()->key()), "a");

  const json::value indexed = rt::jsonParse(R"({"outer":{"ordinary":true,"2":2,"\u0031":1,"__pr\u006fto__":null}})");
  const auto* nested = rt::getObject(indexed, "outer");
  ASSERT(nested != nullptr);
  std::string storage;
  for (const auto& member : *nested) {
    storage += std::string(member.key());
    storage += ',';
  }
  EXPECT_EQ(storage, "ordinary,2,1,__proto__,");
  EXPECT(rt::hasOwn(*nested, "__proto__"));

  const json::value numeric = rt::jsonParse(R"({"n":[-0e7,9007199254740993,1.0,5e-324]})");
  const auto* values = rt::getArray(numeric, "n");
  ASSERT(values != nullptr);
  ASSERT(values->size() == 4);
  ASSERT((*values)[0].is_double());
  ASSERT((*values)[1].is_double());
  ASSERT((*values)[2].is_int64());
  ASSERT((*values)[3].is_double());
  EXPECT_EQ(std::bit_cast<std::uint64_t>((*values)[0].get_double()), 0x8000000000000000ULL);
  EXPECT_EQ(std::bit_cast<std::uint64_t>((*values)[1].get_double()), 0x4340000000000000ULL);
  EXPECT_EQ((*values)[2].get_int64(), 1);
  EXPECT_EQ(std::bit_cast<std::uint64_t>((*values)[3].get_double()), 1ULL);
}
