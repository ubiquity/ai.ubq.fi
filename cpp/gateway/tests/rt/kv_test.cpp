// uos::rt kv. Expected values come from the oracle: the @deno/kv 0.14 SDK with the
// repository's patch, its native SQLite addon behind src/storage/native-kv.ts and
// Bun 1.4's node:v8 serializer, run under bun on throwaway databases. Tests named
// after tests/storage-bun-kv.test.ts port that file's store-semantics cases to
// MemoryKv (docs/rt/kv.md lists the ones that need a file or a process).
#include "uos/rt/kv.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/weak.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

using namespace uos;
using namespace uos::rt;
using boost::asio::experimental::awaitable_operators::operator&&;

namespace {
  constexpr double INF = std::numeric_limits<double>::infinity();
  const double NAN_ = std::numeric_limits<double>::quiet_NaN();

  double fakeNow = 1'700'000'000'000.0;
  std::function<double()> testClock() {
    return [] { return fakeNow; };
  }

  // Coroutine tests run on a private io_context (one thread, posted handlers are
  // macrotasks), which is what MemoryKv needs from the event loop.
  void runTask(Task<void> task) {
    boost::asio::io_context io;
    std::exception_ptr failure;
    bool finished = false;
    boost::asio::co_spawn(io, std::move(task), [&](std::exception_ptr error) {
      failure = error;
      finished = true;
    });
    io.run();
    if (failure) std::rethrow_exception(failure);
    if (!finished) throw std::logic_error("coroutine test did not finish");
  }

  // "Name: message" of the rejection, or "resolved".
  template <class T> Task<std::string> outcome(Task<T> task) {
    try {
      co_await std::move(task);
    } catch (const JsError& error) {
      co_return error.toString();
    }
    co_return "resolved";
  }

  template <class F> std::string thrown(F&& function) {
    try {
      function();
    } catch (const JsError& error) {
      return error.toString();
    }
    return "returned";
  }

  std::string hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : bytes) {
      out += digits[byte >> 4];
      out += digits[byte & 15];
    }
    return out;
  }

  KvBytes unhex(std::string_view text) {
    KvBytes out;
    for (std::size_t index = 0; index + 1 < text.size(); index += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(text.substr(index, 2)), nullptr, 16)));
    return out;
  }

  std::vector<KvValue> valuesOf(const std::vector<KvEntry<>>& entries) {
    std::vector<KvValue> out;
    for (const auto& entry : entries) out.push_back(entry.value);
    return out;
  }

  std::string base64(std::string_view text) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t index = 0;
    for (; index + 2 < text.size(); index += 3) {
      const unsigned chunk = (static_cast<unsigned>(static_cast<unsigned char>(text[index])) << 16) | (static_cast<unsigned>(static_cast<unsigned char>(text[index + 1])) << 8) | static_cast<unsigned>(static_cast<unsigned char>(text[index + 2]));
      for (int shift = 18; shift >= 0; shift -= 6) out += alphabet[(chunk >> shift) & 63];
    }
    if (text.size() - index == 1) {
      const unsigned chunk = static_cast<unsigned>(static_cast<unsigned char>(text[index])) << 16;
      out += alphabet[(chunk >> 18) & 63];
      out += alphabet[(chunk >> 12) & 63];
      out += "==";
    } else if (text.size() - index == 2) {
      const unsigned chunk = (static_cast<unsigned>(static_cast<unsigned char>(text[index])) << 16) | (static_cast<unsigned>(static_cast<unsigned char>(text[index + 1])) << 8);
      out += alphabet[(chunk >> 18) & 63];
      out += alphabet[(chunk >> 12) & 63];
      out += alphabet[(chunk >> 6) & 63];
      out += '=';
    }
    return out;
  }

  // GCC 12.2 also destroys a braced aggregate temporary with non-trivial members
  // twice when it sits in a co_await expression, so tests compare against names.
  const KvCommitResult conflict{false, ""};

  KvValue u64Object(std::uint64_t value) { return KvValue::object({{"value", BigInt::fromUint64(value)}}); }

  // Builders without std::initializer_list: GCC 12.2 cannot keep a braced list's
  // array alive across co_await, so coroutine tests build values with these.
  template <class... T> std::vector<KvValue> vals(T&&... items) {
    std::vector<KvValue> out;
    (out.emplace_back(std::forward<T>(items)), ...);
    return out;
  }
  template <class... T> KvBytes bytes(T... items) { return KvBytes{static_cast<std::uint8_t>(items)...}; }
  template <class... T> KvValue arr(T&&... items) {
    KvArray out;
    (out.emplace_back(std::forward<T>(items)), ...);
    return out;
  }
  inline void objSet(KvObject&) {}
  template <class K, class V, class... Rest> void objSet(KvObject& object, K&& key, V&& value, Rest&&... rest) {
    object.set(std::string(std::forward<K>(key)), KvValue(std::forward<V>(value)));
    objSet(object, std::forward<Rest>(rest)...);
  }
  template <class... T> KvValue obj(T&&... items) {
    KvObject object;
    objSet(object, std::forward<T>(items)...);
    return object;
  }
  template <class... T> std::vector<KvKey> keys(T&&... items) {
    std::vector<KvKey> out;
    (out.emplace_back(std::forward<T>(items)), ...);
    return out;
  }
  template <class... T> std::vector<AtomicCheck> checks(T&&... items) {
    std::vector<AtomicCheck> out;
    (out.emplace_back(std::forward<T>(items)), ...);
    return out;
  }
}

#define KV_TEST_IMPL(name, id) \
  static Task<void> UOS_TEST_JOIN(kvTestBody, id)(); \
  TEST(name) { runTask(UOS_TEST_JOIN(kvTestBody, id)()); } \
  static Task<void> UOS_TEST_JOIN(kvTestBody, id)()
#define KV_TEST(name) KV_TEST_IMPL(name, __COUNTER__)

// ---- Key codec ---------------------------------------------------------------------------------

namespace {
  struct PartVector {
    KvKeyPart part;
    const char* packed;
  };

  std::vector<PartVector> partVectors() {
    return {
      {KvBytes{}, "0100"},
      {KvBytes{0}, "0100ff00"},
      {KvBytes{0, 0}, "0100ff00ff00"},
      {KvBytes{0, 255}, "0100ffff00"},
      {KvBytes{1}, "010100"},
      {KvBytes{255}, "01ff00"},
      {"", "0200"},
      {std::string("\0", 1), "0200ff00"},
      {"a", "026100"},
      {std::string("a\0b", 3), "026100ff6200"},
      {"ab", "02616200"},
      {"b", "026200"},
      {"é", "02c3a900"},
      {"\U0001F600", "02f09f988000"},
      {"￿", "02efbfbf00"},
      {"\xed\xa0\x80", "02efbfbd00"},
      {"\xed\xa0\xbd\xed\xb8\x80", "02f09f988000"},
      {BigInt::fromString("-18446744073709551616"), "0bf6feffffffffffffffff"},
      {BigInt::fromString("-18446744073709551615"), "0c0000000000000000"},
      {BigInt::fromString("-9223372036854775808"), "0c7fffffffffffffff"},
      {BigInt(-256), "12feff"},
      {BigInt(-255), "1300"},
      {BigInt(-1), "13fe"},
      {BigInt(0), "14"},
      {BigInt(1), "1501"},
      {BigInt(255), "15ff"},
      {BigInt(256), "160100"},
      {BigInt::fromString("9223372036854775808"), "1c8000000000000000"},
      {BigInt::fromString("18446744073709551615"), "1cffffffffffffffff"},
      {BigInt::fromString("18446744073709551616"), "1d09010000000000000000"},
      {BigInt::fromString("1208925819614629174706176"), "1d0b0100000000000000000000"},
      {BigInt::fromString("-1208925819614629174706176"), "0bf4feffffffffffffffffffff"},
      {-INF, "21000fffffffffffff"},
      {-1.7976931348623157e308, "210010000000000000"},
      {-1.5, "214007ffffffffffff"},
      {-1, "21400fffffffffffff"},
      {-5e-324, "217ffffffffffffffe"},
      {-0.0, "217fffffffffffffff"},
      {0, "218000000000000000"},
      {5e-324, "218000000000000001"},
      {1, "21bff0000000000000"},
      {1.5, "21bff8000000000000"},
      {9007199254740991.0, "21c33fffffffffffff"},
      {INF, "21fff0000000000000"},
      {NAN_, "21fff8000000000000"},
      {false, "26"},
      {true, "27"},
    };
  }
}

TEST("packKeyPart matches the SDK tuple codec for every part type") {
  for (const PartVector& vector : partVectors()) EXPECT_EQ(hex(packKeyPart(vector.part)), std::string(vector.packed));
  EXPECT_EQ(hex(packKey({"p", 3})), std::string("02700021c008000000000000"));
  EXPECT_EQ(hex(packKey({})), std::string(""));
  // A negative NaN is the one NaN in JavaScript.
  EXPECT_EQ(hex(packKeyPart(-std::numeric_limits<double>::quiet_NaN())), std::string("21fff8000000000000"));
  // A bigint longer than 255 bytes keeps the low byte of its length, as the SDK's Uint8Array does.
  const KvBytes huge = packKeyPart(BigInt(1) << BigInt(2400));
  EXPECT_EQ(huge.size(), 303u);
  EXPECT_EQ(static_cast<int>(huge[0]), 29);
  EXPECT_EQ(static_cast<int>(huge[1]), 45);
  EXPECT_EQ(static_cast<int>(huge[2]), 1);
}

TEST("key parts order like their encodings") {
  std::vector<PartVector> vectors = partVectors();
  std::vector<std::size_t> order(vectors.size());
  for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return vectors[a].part < vectors[b].part; });
  EXPECT_EQ(order, (std::vector<std::size_t>{0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 15, 14, 13, 16, 31, 17, 18, 19, 20, 21, 22,
                                             23, 24, 25, 26, 27, 28, 29, 30, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46}));
  const std::vector<KvKey> keys{{"a"}, {"a", KvBytes{1}}, {"a", "b"}, {"a", "b", "c"}, {"a", BigInt(1)}, {"a", 1}, {"a", true}, {std::string("a\0", 2)}, {"b"}};
  for (std::size_t index = 1; index < keys.size(); ++index) EXPECT_EQ(compareKvKeys(keys[index - 1], keys[index]), -1);
  EXPECT_EQ(compareKvKeys({"a", 1}, {"a", 1.0}), 0);
  EXPECT(KvKeyPart(-0.0) != KvKeyPart(0.0));
  EXPECT(KvKeyPart(NAN_) == KvKeyPart(-NAN_));
  EXPECT(KvKeyPart("\xed\xa0\xbd\xed\xb8\x80") == KvKeyPart("\U0001F600"));
}

TEST("unpackKey decodes like the SDK and keeps its errors") {
  const auto unpack = [](std::initializer_list<std::uint8_t> bytes) { return unpackKey(KvBytes(bytes)); };
  EXPECT_EQ(thrown([&] { unpack({2, 0x61, 0, 0x30}); }), std::string("Error: Unsupported typecode: 48 in key: [2, 97, 0, 48] after a"));
  EXPECT_EQ(thrown([&] { unpack({1, 1, 2, 0, 0x15, 5, 0x21, 0xbf, 0xf0, 0, 0, 0, 0, 0, 0, 0x27, 0x99}); }),
            std::string("Error: Unsupported typecode: 153 in key: [1, 1, 2, 0, 21, 5, 33, 191, 240, 0, 0, 0, 0, 0, 0, 39, 153] after 1,2, 5, 1, true"));
  EXPECT_EQ(thrown([&] { unpack({0x21, 1, 2}); }), std::string("RangeError: Out of bounds access"));
  EXPECT_EQ(thrown([&] { unpack({0x21}); }), std::string("RangeError: Out of bounds access"));
  EXPECT_EQ(thrown([&] { unpack({0x16, 1}); }), std::string("TypeError: Invalid argument type in ToBigInt operation"));
  EXPECT_EQ(thrown([&] { unpack({0x1d}); }), std::string("TypeError: Invalid argument type in ToBigInt operation"));
  EXPECT_EQ(thrown([&] { unpack({0x12, 0xfe}); }), std::string("RangeError: Not an integer"));
  EXPECT_EQ(thrown([&] { unpack({0x0b}); }), std::string("RangeError: Not an integer"));
  EXPECT_EQ(unpack({2, 0x61, 0x62}), (KvKey{"ab"}));
  EXPECT_EQ(unpack({2, 0x61, 0, 0xff}), (KvKey{std::string("a\0", 2)}));
  EXPECT_EQ(unpack({2, 0xff, 0xfe, 0}), (KvKey{"��"}));
  EXPECT_EQ(unpack({2, 0xef, 0xbb, 0xbf, 0x61, 0}), (KvKey{"a"}));
  EXPECT_EQ(unpack({2, 0xef, 0xbb, 0xbf, 0xef, 0xbb, 0xbf, 0x61, 0}), (KvKey{"﻿a"}));
  EXPECT_EQ(unpack({2, 0xed, 0xa0, 0x80, 0x61, 0}), (KvKey{"���a"}));
  EXPECT_EQ(unpack({2, 0xf4, 0x90, 0x80, 0x80, 0}), (KvKey{"����"}));
  const KvKey mixed = unpack({1, 1, 2, 0, 0x15, 5, 0x21, 0xbf, 0xf0, 0, 0, 0, 0, 0, 0, 0x27, 0x26, 0x14, 0x0b, 0xf6, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
  EXPECT_EQ(mixed, (KvKey{KvBytes{1, 2}, BigInt(5), 1, true, false, BigInt(0), BigInt::fromString("-18446744073709551616")}));
  EXPECT(unpack({}).empty());
  for (const PartVector& vector : partVectors()) {
    const KvKey round = unpackKey(packKeyPart(vector.part));
    ASSERT_EQ(round.size(), 1u);
    EXPECT_EQ(hex(packKeyPart(round[0])), std::string(vector.packed));
  }
  EXPECT(std::signbit(unpackKey(packKeyPart(-0.0))[0].number()));
}

TEST("String(part) and typed part access") {
  EXPECT_EQ(KvKeyPart(KvBytes{1, 2}).toString(), std::string("1,2"));
  EXPECT_EQ(KvKeyPart(1.5).toString(), std::string("1.5"));
  EXPECT_EQ(KvKeyPart(-0.0).toString(), std::string("0"));
  EXPECT_EQ(KvKeyPart(1e21).toString(), std::string("1e+21"));
  EXPECT_EQ(KvKeyPart(BigInt(-7)).toString(), std::string("-7"));
  EXPECT_EQ(KvKeyPart(true).toString(), std::string("true"));
  EXPECT_EQ(KvKeyPart("x").string(), std::string("x"));
  EXPECT_EQ(thrown([] { (void)KvKeyPart("x").number(); }), std::string("TypeError: KV key part is string, not number"));
  EXPECT_EQ(kvKey(KvKey{"a", 1}, "b", KvKey{2}), (KvKey{"a", 1, "b", 2}));
}

TEST("versionstamps and list cursors use the SDK's formats") {
  EXPECT_EQ(formatVersionstamp(1), std::string("00000000000000010000"));
  EXPECT_EQ(formatVersionstamp(0x67), std::string("00000000000000670000"));
  EXPECT_EQ(formatVersionstamp(std::numeric_limits<std::uint64_t>::max()), std::string("ffffffffffffffff0000"));
  const KvBytes p3 = packKey({"p", 3});
  const std::string cursor = packCursor(p3);
  EXPECT_EQ(cursor, std::string("eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3MDAwMjFjMDA4MDAwMDAwMDAwMDAwIn0="));
  EXPECT_EQ(unpackCursor(cursor), p3);
  EXPECT_EQ(unpackCursor("eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3MDAwMjFjMDA4MDAwMDAwMDAwMDAwIn0"), p3);
  EXPECT_EQ(unpackCursor(" " + cursor + "\n"), p3);
  EXPECT_EQ(unpackCursor(base64("{\"lastYieldedKeyBytes\":\"02700021C008000000000000\"}")), p3);
  EXPECT_EQ(unpackCursor(base64("{\"other\":1,\"lastYieldedKeyBytes\":\"\"}")), KvBytes{});
  for (const std::string& bad : {std::string("garbage"), std::string(""), base64("5"), base64("null"), base64("{\"lastYieldedKeyBytes\":\"zz\"}"),
                                base64("{\"lastYieldedKeyBytes\":\"abc\"}"), base64("{\"lastYieldedKeyBytes\":1}"), base64("[\"02\"]"), std::string("e30=@")})
    EXPECT_EQ(thrown([&] { unpackCursor(bad); }), std::string("Error: Invalid cursor"));
}

TEST("list cursor bytes are bounded before base64 and JSON parsing") {
  const KvBytes key = packKey({"p", 3});
  std::string cursor = packCursor(key);
  cursor.resize(limits::kKvCursorBytes, ' ');
  EXPECT_EQ(unpackCursor(cursor), key);
  cursor.push_back(' ');
  EXPECT_THROWS_MESSAGE(unpackCursor(cursor), JsError, "Invalid cursor");
}

// ---- Values ------------------------------------------------------------------------------------

namespace {
  KvError errorWithStack(std::string name, std::optional<std::string> message, std::optional<std::string> stack, std::uint32_t column = 137) {
    KvError error;
    error.name = std::move(name);
    error.message = std::move(message);
    error.line = 40;
    error.column = column;
    error.sourceURL = "/tmp/claude-1002/kv/probe/gen.ts";
    error.stack = std::move(stack);
    return error;
  }

  KvValue mapNums() {
    KvMap map;
    map.set(1, "one");
    map.set(-0.0, "zero");
    map.set(NAN_, "nan");
    map.set("1", 1);
    return map;
  }

  KvValue sparse() {
    KvArray array(5, KvHole{});
    array[1] = 1;
    array[3] = 2;
    return array;
  }

  KvValue pool300() {
    KvObject object;
    for (int index = 0; index < 300; ++index)
      object.set("k" + std::to_string(index), index % 3 == 0 ? KvValue("k" + std::to_string(index - 3)) : KvValue(index));
    return object;
  }

  KvValue record() {
    return KvValue::object({
      {"id", "k1"},
      {"tags", KvValue::array({"a", "b", "a"})},
      {"nested", KvValue::object({{"id", "k2"}, {"tags", KvArray{}}, {"limits", KvValue::object({{"rpm", 60}, {"tpm", 1.5}})}})},
      {"list", KvValue::array({KvValue::object({{"id", "x"}}), KvValue::object({{"id", "y"}, {"note", "id"}})})},
      {"ok", true},
      {"none", nullptr},
      {"big", BigInt(10)},
      {"bytes", KvBytes{9}},
      {"when", KvDate{5}},
      {"u", KvU64(7)},
    });
  }

  const KvBytes eight{1, 2, 3, 4, 5, 6, 7, 8};

  struct SerializeVector {
    const char* name;
    KvValue value;
    const char* bytes;
  };

  std::vector<SerializeVector> serializeVectors() {
    return {
      {"undefined", KvValue(), "0e00000003"},
      {"null", nullptr, "0e00000004"},
      {"true", true, "0e00000009"},
      {"false", false, "0e00000008"},
      {"0", 0, "0e00000006"},
      {"1", 1, "0e00000007"},
      {"2", 2, "0e0000000502000000"},
      {"-1", -1, "0e00000005ffffffff"},
      {"2147483647", 2147483647, "0e00000005ffffff7f"},
      {"2147483648", 2147483648.0, "0e0000000a000000000000e041"},
      {"-2147483648", -2147483648.0, "0e0000000500000080"},
      {"-2147483649", -2147483649.0, "0e0000000a000020000000e0c1"},
      {"1.5", 1.5, "0e0000000a000000000000f83f"},
      {"-0", -0.0, "0e0000000a0000000000000080"},
      {"NaN", NAN_, "0e0000000a000000000000f87f"},
      {"Infinity", INF, "0e0000000a000000000000f07f"},
      {"-Infinity", -INF, "0e0000000a000000000000f0ff"},
      {"1e21", 1e21, "0e0000000a50efe2d6e41a4b44"},
      {"''", "", "0e00000011"},
      {"'a'", "a", "0e000000100100008061"},
      {"'ab'", "ab", "0e00000010020000806162"},
      {"'é'", "é", "0e0000001001000080e9"},
      {"'λ'", "λ", "0e0000001001000000bb03"},
      {"'😀a'", "\U0001F600a", "0e00000010030000003dd800de6100"},
      {"lone", "\xed\xa0\x80", "0e000000100100000000d8"},
      {"'ÿ'", "ÿ", "0e0000001001000080ff"},
      {"'Ā'", "Ā", "0e00000010010000000001"},
      {"0n", BigInt(0), "0e0000002f0000000000"},
      {"1n", BigInt(1), "0e0000002f00010000000100000000000000"},
      {"-1n", BigInt(-1), "0e0000002f01010000000100000000000000"},
      {"2^63n", BigInt::fromString("9223372036854775808"), "0e0000002f00010000000000000000000080"},
      {"2^64-1n", BigInt::fromString("18446744073709551615"), "0e0000002f0001000000ffffffffffffffff"},
      {"2^64n", BigInt::fromString("18446744073709551616"), "0e0000002f000200000000000000000000000100000000000000"},
      {"-(2^64)n", BigInt::fromString("-18446744073709551616"), "0e0000002f010200000000000000000000000100000000000000"},
      {"2^80n", BigInt::fromString("1208925819614629174706176"), "0e0000002f000200000000000000000000000000010000000000"},
      {"2^128n", BigInt(1) << BigInt(128), "0e0000002f0003000000000000000000000000000000000000000100000000000000"},
      {"[]", KvArray{}, "0e0000000100000000ffffffff"},
      {"[1]", KvValue::array({1}), "0e00000001010000000000000007ffffffff"},
      {"[1,'a']", KvValue::array({1, "a"}), "0e0000000102000000000000000701000000100100008061ffffffff"},
      {"sparse", sparse(), "0e00000001050000000100000007030000000502000000ffffffff"},
      {"[undefined]", KvValue::array({KvValue()}), "0e00000001010000000000000003ffffffff"},
      {"[null,true,'λ']", KvValue::array({nullptr, true, "λ"}), "0e000000010300000000000000040100000009020000001001000000bb03ffffffff"},
      {"{}", KvObject{}, "0e00000002ffffffff"},
      {"{a:1}", KvValue::object({{"a", 1}}), "0e00000002010000806107ffffffff"},
      {"{a:1,b:'x'}", KvValue::object({{"a", 1}, {"b", "x"}}), "0e000000020100008061070100008062100100008078ffffffff"},
      {"{a:undefined}", KvValue::object({{"a", KvValue()}}), "0e00000002010000806103ffffffff"},
      {"{'1':1,a:2}", KvValue::object({{"a", 2}, {"1", 1}}), "0e0000000201000080310701000080610502000000ffffffff"},
      {"{'':1}", KvValue::object({{"", 1}}), "0e000000020000008007ffffffff"},
      {"[{a:1},{a:2}]", KvValue::array({KvValue::object({{"a", 1}}), KvValue::object({{"a", 2}})}),
       "0e00000001020000000000000002010000806107ffffffff0100000002feffffff000502000000ffffffffffffffff"},
      {"['x','x']", KvValue::array({"x", "x"}), "0e0000000102000000000000001001000080780100000010feffffff00ffffffff"},
      {"{x:'x'}", KvValue::object({{"x", "x"}}), "0e00000002010000807810feffffff00ffffffff"},
      {"{k:'k',k2:['k']}", KvValue::object({{"k", "k"}, {"k2", KvValue::array({"k"})}}),
       "0e00000002010000806b10feffffff00020000806b3201010000000000000010feffffff00ffffffffffffffff"},
      {"[{'':''},{'':1}]", KvValue::array({KvValue::object({{"", ""}}), KvValue::object({{"", 1}})}),
       "0e000000010200000000000000020000008011ffffffff0100000002feffffff0007ffffffffffffffff"},
      {"['λ','λ',{λ:1}]", KvValue::array({"λ", "λ", KvValue::object({{"λ", 1}})}),
       "0e0000000103000000000000001001000000bb030100000010feffffff000200000002feffffff0007ffffffffffffffff"},
      {"Uint8Array[1,2]", KvBytes{1, 2}, "0e0000001602000000000000000002000000000000001502000000000000000102"},
      {"{u:Uint8Array[1,2]}", KvValue::object({{"u", KvBytes{1, 2}}}), "0e0000000201000080751602000000000000000002000000000000001502000000000000000102ffffffff"},
      {"Uint8Array[]", KvBytes{}, "0e000000160200000000000000000000000000000000150000000000000000"},
      {"DataView(buf,1,2)", KvArrayBufferView{KvViewKind::DataView, eight, 1, 2},
       "0e0000001600010000000000000002000000000000001508000000000000000102030405060708"},
      {"Int16Array(buf,0,1)", KvArrayBufferView{KvViewKind::Int16Array, eight, 0, 2},
       "0e0000001604000000000000000002000000000000001508000000000000000102030405060708"},
      {"Float64Array(buf)", KvArrayBufferView{KvViewKind::Float64Array, eight, 0, 8},
       "0e0000001609000000000000000008000000000000001508000000000000000102030405060708"},
      {"BigUint64Array(buf)", KvArrayBufferView{KvViewKind::BigUint64Array, eight, 0, 8},
       "0e000000160b000000000000000008000000000000001508000000000000000102030405060708"},
      {"Uint8Array(buf,2,3)", KvArrayBufferView{KvViewKind::Uint8Array, eight, 2, 3},
       "0e0000001602020000000000000003000000000000001508000000000000000102030405060708"},
      {"ArrayBuffer", KvArrayBuffer{eight}, "0e0000001508000000000000000102030405060708"},
      {"Date(0)", KvDate{0}, "0e0000000b0000000000000000"},
      {"Date(1700000000000)", KvDate{1700000000000.0}, "0e0000000b00008056febc7842"},
      {"Date(NaN)", KvDate{NAN_}, "0e0000000b000000000000f87f"},
      {"Date(-1)", KvDate{-1}, "0e0000000b000000000000f0bf"},
      {"Map", KvMap{{"k", 1}}, "0e0000001e10010000806b071fffffffff"},
      {"Map empty", KvMap{}, "0e0000001e1fffffffff"},
      {"Map obj key", KvMap{{KvValue::object({{"a", 1}}), KvValue::array({1})}}, "0e0000001e02010000806107ffffffff01010000000000000007ffffffff1fffffffff"},
      {"Map nums", mapNums(), "0e0000001e0710030000806f6e650610040000807a65726f0a000000000000f87f10030000806e616e100100008031071fffffffff"},
      {"Set", KvSet{1}, "0e0000001d0720ffffffff"},
      {"Set empty", KvSet{}, "0e0000001d20ffffffff"},
      {"Set strs", KvSet{"a", "b", "a"}, "0e0000001d10010000806110010000806220ffffffff"},
      {"RegExp /a/g", KvRegExp{"a", "g"}, "0e0000001201000080610100008067"},
      {"RegExp /a/", KvRegExp{"a", ""}, "0e00000012010000806100000080"},
      {"RegExp all", KvRegExp{"a", "dgimsuy"}, "0e000000120100008061070000806467696d737579"},
      {"RegExp /x+/gi", KvRegExp{"x+", "ig"}, "0e0000001202000080782b020000806769"},
      {"Error('m') stack 's'", errorWithStack("Error", "m", "s"),
       "0e000000370000000000010000806d280000008900000000000000200000802f746d702f636c617564652d313030322f6b762f70726f62652f67656e2e7473000000000100008073"},
      {"RangeError('') stack ''", errorWithStack("RangeError", "", ""),
       "0e00000037020000000000000080280000008900000000000000200000802f746d702f636c617564652d313030322f6b762f70726f62652f67656e2e747300000000feffffff00"},
      {"TypeError('x') no stack", errorWithStack("TypeError", "x", std::nullopt),
       "0e0000003705000000000100008078280000008900000000000000200000802f746d702f636c617564652d313030322f6b762f70726f62652f67656e2e747301000000"},
      {"Error() no message", errorWithStack("Error", std::nullopt, "s", 120),
       "0e000000370001000000280000007800000000000000200000802f746d702f636c617564652d313030322f6b762f70726f62652f67656e2e7473000000000100008073"},
      {"URIError('é') stack 'λ'", errorWithStack("URIError", "é", "λ"),
       "0e00000037060000000001000080e9280000008900000000000000200000802f746d702f636c617564652d313030322f6b762f70726f62652f67656e2e74730000000001000000bb03"},
      {"nested KvU64", KvValue::object({{"c", KvU64(3)}}), "0e000000020100008063020500008076616c75652f00010000000300000000000000ffffffffffffffff"},
      {"record", record(),
       "0e0000000202000080696410020000806b310400008074616773010300000000000000100100008061010000001001000080620200000010feffffff03ffffffff060000806e6573746564"
       "02feffffff0010020000806b32feffffff020100000000ffffffff060000806c696d697473020300008072706d053c0000000300008074706d0a000000000000f83fffffffffffffffff04"
       "0000806c69737401020000000000000002feffffff00100100008078ffffffff0100000002feffffff00100100008079040000806e6f746510feffffff00ffffffffffffffff020000806f"
       "6b09040000806e6f6e6504030000806269672f00010000000a0000000000000005000080627974657316020000000000000000010000000000000015010000000000000009040000807768"
       "656e0b00000000000014400100008075020500008076616c75652f00010000000700000000000000ffffffffffffffff"},
    };
  }
}

TEST("kvJscSerialize writes Bun's node:v8 serialize bytes") {
  for (const SerializeVector& vector : serializeVectors()) {
    const std::string actual = hex(kvJscSerialize(vector.value));
    if (actual != vector.bytes) ::uos::rt::test::report(__FILE__, __LINE__, std::string(vector.name) + "\n      actual:   " + actual + "\n      expected: " + vector.bytes, false);
  }
}

TEST("kvJscSerialize widens string pool references past 255 entries") {
  const KvBytes bytes = kvJscSerialize(pool300());
  EXPECT_EQ(bytes.size(), 3912u);
  const std::string text = hex(bytes);
  EXPECT_EQ(text.substr(0, 60), std::string("0e00000002020000806b3010030000806b2d33020000806b310702000080"));
  // k297 = "k294" through a two-byte pool index, then k298 = 298, k299 = 299.
  EXPECT_EQ(text.substr(text.size() - 80), std::string("32393710feffffff2701040000806b323938052a010000040000806b323939052b010000ffffffff"));
}

TEST("KvObject keeps JavaScript property order") {
  KvObject object;
  for (const auto& [key, value] : std::vector<std::pair<std::string, int>>{{"b", 1}, {"2", 2}, {"a", 3}, {"1", 4}, {"-1", 5}, {"01", 6}, {"4294967294", 7}, {"4294967295", 8}})
    object.set(key, value);
  std::vector<std::string> keys;
  for (const auto& entry : object) keys.push_back(entry.first);
  EXPECT_EQ(keys, (std::vector<std::string>{"1", "2", "4294967294", "b", "a", "-1", "01", "4294967295"}));
  object.set("b", 9);
  EXPECT_EQ(object.begin()[3].first, std::string("b"));
  EXPECT_EQ(object.at("b"), KvValue(9));
  EXPECT(object.erase("a"));
  EXPECT(!object.contains("a"));
  EXPECT(object.at("a").isUndefined());
  EXPECT_EQ(KvValue::object({{"a", 1}, {"b", 2}}), KvValue::object({{"b", 2}, {"a", 1}}));
  EXPECT(KvValue::object({{"a", 1}}) != KvValue::object({{"a", 1.5}}));
}

TEST("Map and Set keys are unique under SameValueZero") {
  KvMap map;
  map.set(-0.0, "a");
  map.set(0, "b");
  map.set(NAN_, 1);
  map.set(NAN_, 2);
  map.set(KvObject{}, 1);
  map.set(KvObject{}, 2);
  EXPECT_EQ(map.size(), 4u);
  EXPECT(!std::signbit(map.begin()->first.asNumber()));
  EXPECT_EQ(map.begin()->second, KvValue("b"));
  EXPECT_EQ(map.begin()[1].second, KvValue(2));
  const KvSet set{1, 1.0, "1", -0.0, 0};
  EXPECT_EQ(set.size(), 3u);
  EXPECT_EQ(KvValue(KvSet{1, 2}), KvValue(KvSet{2, 1}));
}

TEST("JSON interop reads like json::parse of JSON.stringify") {
  const KvValue parsed(json::parse(R"({"a":1,"b":[true,null,"x",1.5],"c":{"2":0,"1":1}})"));
  EXPECT_EQ(parsed, KvValue::object({{"a", 1}, {"b", KvValue::array({true, nullptr, "x", 1.5})}, {"c", KvValue::object({{"1", 1}, {"2", 0}})}}));
  EXPECT_EQ(parsed["c"].asObject().begin()->first, std::string("1"));
  KvArray withHole{1, KvValue(), KvHole{}};
  const json::value converted =
    KvValue::object({{"n", 3}, {"u", 1e19}, {"big", 1e21}, {"neg", -0.0}, {"frac", 0.5}, {"nan", NAN_}, {"skip", KvValue()}, {"list", withHole}}).toJson();
  const json::object& object = converted.as_object();
  EXPECT_EQ(object.at("n").kind(), json::kind::int64);
  EXPECT_EQ(object.at("n").as_int64(), 3);
  EXPECT_EQ(object.at("u").kind(), json::kind::uint64);
  EXPECT_EQ(object.at("u").as_uint64(), 10000000000000000000ull);
  EXPECT_EQ(object.at("big").kind(), json::kind::double_);
  EXPECT(std::signbit(object.at("neg").as_double()));
  EXPECT_EQ(object.at("frac").as_double(), 0.5);
  EXPECT(std::isnan(object.at("nan").as_double()));
  EXPECT(!object.contains("skip"));
  EXPECT_EQ(json::serialize(object.at("list")), std::string("[1,null,null]"));
  EXPECT_EQ(KvValue().toJson(), json::value(nullptr));
  EXPECT_EQ(thrown([] { (void)KvValue(BigInt(1)).toJson(); }), std::string("TypeError: KV value is bigint, not JSON"));
  EXPECT_EQ(thrown([] { (void)kvValueAs<json::object>(KvValue("x")); }), std::string("TypeError: KV value is string, not Object"));
  EXPECT(!kvValueAs<std::optional<json::object>>(KvValue(nullptr)));
  EXPECT_EQ(kvValueAs<KvU64>(KvValue(KvU64(4))), KvU64(4));
  EXPECT_EQ(thrown([] { (void)KvValue("x").asNumber(); }), std::string("TypeError: KV value is string, not number"));
}

TEST("kvStoredValue reproduces what a native round trip returns") {
  EXPECT_EQ(kvStoredValue(KvValue::object({{"c", KvU64(3)}, {"arr", KvValue::array({KvU64(4)})}})),
            KvValue::object({{"c", KvValue::object({{"value", BigInt(3)}})}, {"arr", KvValue::array({KvValue::object({{"value", BigInt(4)}})})}}));
  EXPECT_EQ(kvStoredValue(KvU64(5)), KvValue(KvU64(5)));
  KvError custom;
  custom.name = "CustomError";
  custom.message = "x";
  EXPECT_EQ(kvStoredValue(custom).as<KvError>().name, std::string("Error"));
  KvError typed;
  typed.name = "TypeError";
  EXPECT_EQ(kvStoredValue(typed).as<KvError>().name, std::string("TypeError"));
  EXPECT_EQ(kvStoredValue(KvRegExp{"x+", "yig"}), KvValue(KvRegExp{"x+", "giy"}));
  const KvBytes backing{9, 1, 2, 9};
  EXPECT_EQ(kvStoredValue(KvArrayBufferView{KvViewKind::Uint8Array, backing, 1, 2}), KvValue(KvBytes{1, 2}));
  EXPECT_EQ(kvStoredValue(KvValue::object({{"s", KvArrayBufferView{KvViewKind::Uint8Array, backing, 1, 2}}})),
            KvValue::object({{"s", KvArrayBufferView{KvViewKind::Uint8Array, backing, 1, 2}}}));
  EXPECT_EQ(kvStoredValue(KvValue::object({{"w", KvArrayBufferView{KvViewKind::Uint8Array, backing, 0, 4}}})), KvValue::object({{"w", backing}}));
  EXPECT_EQ(kvStoredValue(sparse()), sparse());
  EXPECT_EQ(kvStoredValue(KvValue::object({{"z", -0.0}, {"n", NAN_}, {"i", -INF}})), KvValue::object({{"z", -0.0}, {"n", NAN_}, {"i", -INF}}));
}

TEST("assertPortableKvValue refuses Error.cause anywhere in the graph") {
  KvError plain;
  plain.message = "inner";
  EXPECT_EQ(thrown([&] { assertPortableKvValue(KvValue::object({{"e", plain}})); }), std::string("returned"));
  KvError caused = plain;
  caused.cause = std::make_shared<KvValue>();
  const KvValue graph = KvValue::object({{"records", KvMap{{"failure", KvValue::array({KvSet{caused}})}}}});
  EXPECT_EQ(thrown([&] { assertPortableKvValue(graph); }), std::string("TypeError: Bun KV cannot preserve Error.cause; persist an explicit error record instead"));
}

TEST("truthiness, kind names and member access") {
  for (const KvValue& falsy : {KvValue(), KvValue(nullptr), KvValue(false), KvValue(0), KvValue(-0.0), KvValue(NAN_), KvValue(""), KvValue(BigInt(0))}) EXPECT(!falsy.truthy());
  for (const KvValue& truthy : {KvValue("0"), KvValue(KvObject{}), KvValue(KvArray{}), KvValue(KvBytes{}), KvValue(BigInt(-1)), KvValue(KvU64(0))}) EXPECT(truthy.truthy());
  EXPECT_EQ(KvValue(KvArrayBufferView{KvViewKind::Int16Array, KvBytes{1, 2}, 0, 2}).kind(), std::string_view("Int16Array"));
  EXPECT_EQ(KvValue(KvBytes{}).kind(), std::string_view("Uint8Array"));
  EXPECT_EQ(KvValue(KvHole{}).kind(), std::string_view("hole"));
  const KvValue value = KvValue::object({{"a", KvValue::array({1, KvHole{}})}});
  EXPECT_EQ(value["a"][0], KvValue(1));
  EXPECT(value["a"][1].isUndefined());
  EXPECT(value["a"][7].isUndefined());
  EXPECT(value["missing"]["deeper"].isUndefined());
  EXPECT_EQ(show(value), std::string("{ \"a\": [1, <hole>] }"));
}

TEST("KvU64 validates like new KvU64(n) and wraps like sum") {
  EXPECT_EQ(thrown([] { (void)KvU64(-1); }), std::string("Error: value must be a positive bigint"));
  EXPECT_EQ(thrown([] { (void)KvU64(BigInt(-1)); }), std::string("Error: value must be a positive bigint"));
  EXPECT_EQ(thrown([] { (void)KvU64(BigInt(1) << BigInt(64)); }), std::string("Error: value must fit in a 64-bit unsigned integer"));
  EXPECT_EQ(KvU64(BigInt::fromString("18446744073709551615")).value, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(KvU64(std::numeric_limits<std::uint64_t>::max() - 1).sum(KvU64(5)), KvU64(3));
  EXPECT_EQ(KvU64(10).min(KvU64(3)), KvU64(3));
  EXPECT_EQ(KvU64(10).max(KvU64(30)), KvU64(30));
  EXPECT_EQ(KvU64(7).toBigInt(), BigInt(7));
}

// ---- MemoryKv: tests/storage-bun-kv.test.ts ------------------------------------------------------

KV_TEST("native Bun SQLite retains typed values and KvU64 identity") {
  MemoryKv kv(testClock());
  const KvValue value = KvValue::object({{"bytes", KvBytes{0, 128, 255}},
                                         {"amount", BigInt::fromString("1208925819614629174706176")},
                                         {"empty", KvValue()},
                                         {"nested", KvValue::array({nullptr, true, "λ"})}});
  const KvCommitResult committed = co_await kv.set({"typed"}, value);
  EXPECT(committed.ok);
  EXPECT_EQ((co_await kv.get({"typed"})).value, value);
  co_await kv.set({"counter"}, KvU64(12));
  const KvValue counter = (co_await kv.get({"counter"})).value;
  EXPECT(counter.isU64());
  EXPECT_EQ(counter.asU64().value, 12u);
  co_await kv.set({"undefined"}, KvValue());
  const KvEntryMaybe<> stored = co_await kv.get({"undefined"});
  EXPECT(stored.value.isUndefined());
  EXPECT(stored.versionstamp.has_value());
}

KV_TEST("atomic compare-and-set refuses stale checks without partial writes") {
  MemoryKv kv(testClock());
  const KvEntryMaybe<> absent = co_await kv.get({"quota"});
  auto operation = kv.atomic();
  operation->check(absent).set({"quota"}, KvValue::object({{"reserved", 1}})).set({"audit"}, "created");
  EXPECT((co_await operation->commit()).ok);
  const auto entries = co_await kv.getMany(keys(KvKey{"quota"}, KvKey{"audit"}));
  EXPECT_EQ(entries[0].versionstamp, entries[1].versionstamp);
  auto stale = kv.atomic();
  stale->check(absent).set({"quota"}, KvValue::object({{"reserved", 2}})).set({"partial"}, true);
  EXPECT_EQ(co_await stale->commit(), conflict);
  EXPECT(!(co_await kv.get({"partial"})).versionstamp);
  EXPECT_EQ(entries[0].value, KvValue::object({{"reserved", 1}}));
}

KV_TEST("native check budget preserves paired analytics sums with two and ten caller checks") {
  MemoryKv kv(testClock());
  const std::vector<std::pair<std::string, std::int64_t>> counters{
    {"sample_count", 1}, {"input_tokens", 100}, {"cached_input_tokens", 25}, {"usage_reported_sample_count", 1}, {"request_cache_hit_sample_count", 1}};
  const std::vector<int> checkCounts{2, 10};
  for (const int checkCount : checkCounts) {
    const std::string prefix = "analytics-" + std::to_string(checkCount);
    std::vector<KvKey> fenceKeys;
    for (int index = 0; index < checkCount; ++index) fenceKeys.push_back({prefix, "fence", index});
    const auto fences = co_await kv.getMany(fenceKeys);
    auto operation = kv.atomic();
    operation->check(fences).set({prefix, "marker"}, true).set({prefix, "cardinality"}, 1);
    for (const auto& [counter, amount] : counters) operation->sum({prefix, "dimension", counter}, amount).sum({prefix, "aggregate", counter}, amount);
    EXPECT((co_await operation->commit()).ok);
    for (const auto& [counter, amount] : counters) {
      EXPECT_EQ((co_await kv.get({prefix, "dimension", counter})).value, KvValue(KvU64(amount)));
      EXPECT_EQ((co_await kv.get({prefix, "aggregate", counter})).value, KvValue(KvU64(amount)));
    }
  }
}

KV_TEST("maximum caller checks retain stale CAS and late wrong-type rollback without partial counters") {
  MemoryKv kv(testClock());
  std::vector<KvKey> keys;
  for (int index = 0; index < 10; ++index) keys.push_back({"max-fence", index});
  const auto fences = co_await kv.getMany(keys);
  co_await kv.set(keys[9], "changed");
  auto stale = kv.atomic();
  stale->check(fences).set({"stale-marker"}, true).sum({"stale-sum"}, 1);
  EXPECT_EQ(co_await stale->commit(), conflict);
  EXPECT(!(co_await kv.get({"stale-marker"})).versionstamp);
  EXPECT(!(co_await kv.get({"stale-sum"})).versionstamp);
  const auto current = co_await kv.getMany(keys);
  co_await kv.set({"late-wrong-type"}, BigInt(7));
  auto failed = kv.atomic();
  failed->check(current).set({"late-marker"}, true);
  for (int index = 0; index < 9; index += 1) failed->sum({"late-sum", index}, 1);
  failed->sum({"late-wrong-type"}, 1);
  EXPECT_EQ(co_await outcome(failed->commit()), std::string("TypeError: Atomic arithmetic requires an absent key or KvU64 value"));
  EXPECT(!(co_await kv.get({"late-marker"})).versionstamp);
  for (int index = 0; index < 9; index += 1) EXPECT(!(co_await kv.get({"late-sum", index})).versionstamp);
  EXPECT_EQ((co_await kv.get({"late-wrong-type"})).value, KvValue(BigInt(7)));
}

namespace {
  struct CountingFacade : ForwardingKv {
    using ForwardingKv::ForwardingKv;
    int lists = 0;
    int atomics = 0;
    std::unique_ptr<KvListIterator> list(KvListSelector selector, KvListOptions options = {}) override {
      lists += 1;
      return ForwardingKv::list(std::move(selector), std::move(options));
    }
    std::unique_ptr<AtomicOperation> atomic() override {
      atomics += 1;
      return ForwardingKv::atomic();
    }
  };
}

KV_TEST("native facade permits captured list and atomic instance overrides without recursion") {
  MemoryKv native(testClock());
  CountingFacade kv(native);
  co_await kv.set({"facade"}, "root");
  co_await kv.set({"facade", 1}, "child");
  auto iterator = kv.list({.prefix = KvKey{"facade"}}, {.limit = 1});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*iterator)), vals("child"));
  EXPECT_EQ(kv.lists, 1);
  const KvEntryMaybe<> absent = co_await kv.get({"facade-claim"});
  auto claim = kv.atomic();
  claim->check(absent).set({"facade-claim"}, "owned");
  EXPECT((co_await claim->commit()).ok);
  auto stale = kv.atomic();
  stale->check(absent).set({"facade-stale"}, true);
  EXPECT_EQ(co_await stale->commit(), conflict);
  EXPECT(!(co_await kv.get({"facade-stale"})).versionstamp);
  co_await kv.set({"facade-wrong-sum"}, BigInt(3));
  auto partial = kv.atomic();
  partial->set({"facade-partial"}, true).sum({"facade-wrong-sum"}, 1);
  EXPECT((co_await outcome(partial->commit())).find("KvU64") != std::string::npos);
  EXPECT(!(co_await kv.get({"facade-partial"})).versionstamp);
  KvError error;
  error.message = "fixture";
  error.cause = std::make_shared<KvValue>("unsupported");
  auto valuePartial = kv.atomic();
  valuePartial->set({"facade-value-partial"}, true).set({"facade-value"}, error);
  EXPECT((co_await outcome(valuePartial->commit())).find("Error.cause") != std::string::npos);
  EXPECT(!(co_await kv.get({"facade-value-partial"})).versionstamp);
  EXPECT_EQ(kv.atomics, 4);
}

KV_TEST("native TTL retains its absolute deadline and expires across restart") {
  auto database = std::make_shared<MemoryKvDatabase>(testClock());
  MemoryKv kv(database);
  const double before = fakeNow;
  co_await kv.set({"expiring"}, "challenge", {.expireIn = 20});
  const double after = fakeNow;
  co_await kv.set({"durable"}, "ledger");
  const double expiry = database->row({"expiring"})->expirationMs;
  EXPECT(expiry >= before + 20);
  EXPECT(expiry <= after + 20);
  EXPECT_EQ(database->row({"durable"})->expirationMs, -1.0);
  // Native TTL permits delayed visibility; startup sweeps expired rows directly.
  kv.close();
  fakeNow = after + 25;
  MemoryKv reopened(database);
  EXPECT(!(co_await reopened.get({"expiring"})).versionstamp);
  EXPECT_EQ((co_await reopened.get({"durable"})).value, KvValue("ledger"));
}

KV_TEST("native list preserves heterogeneous key order and resumable ranges") {
  MemoryKv kv(testClock());
  const std::vector<KvKeyPart> parts{KvBytes{0}, KvBytes{2}, KvBytes{10}, "a", "é", BigInt(-2), BigInt(0), BigInt(2), -2, 0, 2, false, true};
  std::vector<KvValue> indexes;
  for (std::size_t index = 0; index < parts.size(); index += 1) {
    co_await kv.set({"ordered", parts[index]}, index);
    indexes.emplace_back(index);
  }
  auto all = kv.list({.prefix = KvKey{"ordered"}});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*all)), indexes);
  auto first = kv.list({.prefix = KvKey{"ordered"}}, {.limit = 3, .batchSize = 2});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*first)), vals(0, 1, 2));
  auto resumed = kv.list({.prefix = KvKey{"ordered"}}, {.cursor = first->cursor()});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*resumed)), std::vector<KvValue>(indexes.begin() + 3, indexes.end()));
  auto range = kv.list({.start = KvKey{"ordered", -2}, .end = KvKey{"ordered", 2}});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*range)), vals(8, 9));
  auto reverse = kv.list({.prefix = KvKey{"ordered"}}, {.limit = 2, .reverse = true});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*reverse)), vals(12, 11));
}

KV_TEST("prefix selectors exclude the exact prefix and native cursors retain cross-range behavior") {
  MemoryKv kv(testClock());
  co_await kv.set({"prefix"}, "root");
  co_await kv.set({"prefix", 1}, "child");
  co_await kv.set({"prefix", 2}, "next");
  auto first = kv.list({.prefix = KvKey{"prefix"}}, {.limit = 1});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*first)), vals("child"));
  auto other = kv.list({.prefix = KvKey{"other"}}, {.cursor = first->cursor()});
  EXPECT((co_await kvListToArray(*other)).empty());
  EXPECT_EQ(thrown([&] { (void)kv.list({.prefix = KvKey{"prefix"}, .end = KvKey{"outside", 1}}); }),
            std::string("TypeError: End key is not in the keyspace defined by prefix"));
  auto bounded = kv.list({.prefix = KvKey{"prefix"}, .end = KvKey{"prefix", 2}});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*bounded)), vals("child"));
}

KV_TEST("native cursor lifecycle clears empty and exhausted scans but preserves limited continuations") {
  MemoryKv kv(testClock());
  auto empty = kv.list({.prefix = KvKey{"empty-cursor"}});
  EXPECT_EQ(thrown([&] { (void)empty->cursor(); }), std::string("Error: Cannot get cursor before first iteration"));
  EXPECT((co_await kvListToArray(*empty)).empty());
  EXPECT_EQ(empty->cursor(), std::string(""));
  co_await kv.set({"cursor", 1}, "first");
  auto single = kv.list({.prefix = KvKey{"cursor"}});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*single)), vals("first"));
  EXPECT_EQ(single->cursor(), std::string(""));
  co_await kv.set({"cursor", 2}, "second");
  auto limited = kv.list({.prefix = KvKey{"cursor"}}, {.limit = 1});
  EXPECT_EQ(valuesOf(co_await kvListToArray(*limited)), vals("first"));
  const std::string continuation = limited->cursor();
  EXPECT(continuation != "");
  auto resumed = kv.list({.prefix = KvKey{"cursor"}}, {.cursor = continuation});
  EXPECT_EQ(resumed->cursor(), continuation);
  EXPECT_EQ(valuesOf(co_await kvListToArray(*resumed)), vals("second"));
  EXPECT_EQ(resumed->cursor(), std::string(""));
}

KV_TEST("unsigned sums wrap, retain real U64 values, and roll back wrong-type mutations") {
  MemoryKv kv(testClock());
  co_await kv.set({"sum"}, KvU64(std::numeric_limits<std::uint64_t>::max()));
  auto wrap = kv.atomic();
  wrap->sum({"sum"}, 1);
  EXPECT((co_await wrap->commit()).ok);
  EXPECT_EQ((co_await kv.get({"sum"})).value, KvValue(KvU64(0)));
  auto missing = kv.atomic();
  missing->sum({"missing-sum"}, 5);
  co_await missing->commit();
  EXPECT((co_await kv.get({"missing-sum"})).value.isU64());
  co_await kv.set({"wrong-type"}, BigInt(5));
  auto rollback = kv.atomic();
  rollback->set({"must-roll-back"}, "partial").sum({"wrong-type"}, 1);
  EXPECT_EQ(co_await outcome(rollback->commit()), std::string("TypeError: Atomic arithmetic requires an absent key or KvU64 value"));
  EXPECT(!(co_await kv.get({"must-roll-back"})).versionstamp);
  EXPECT_EQ((co_await kv.get({"wrong-type"})).value, KvValue(BigInt(5)));
}

KV_TEST("snapshot reads preserve absent values and clone caller-owned buffers") {
  MemoryKv kv(testClock());
  KvBytes owned{1, 2, 3};
  co_await kv.set({"bytes"}, owned);
  owned[0] = 99;
  auto entries = co_await kv.getMany(keys(KvKey{"bytes"}, KvKey{"missing"}));
  EXPECT_EQ(entries[0].value, KvValue(KvBytes{1, 2, 3}));
  EXPECT(entries[1] == (KvEntryMaybe<>{{"missing"}, nullptr, std::nullopt}));
  entries[0].value.as<KvBytes>()[1] = 99;
  EXPECT_EQ((co_await kv.get({"bytes"})).value, KvValue(bytes(1, 2, 3)));
}

KV_TEST("concurrent native arithmetic retains updates while caller CAS failure remains final") {
  MemoryKv kv(testClock());
  co_await kv.set({"guard-counter"}, KvU64(0));
  auto first = kv.atomic();
  first->sum({"guard-counter"}, 1);
  auto second = kv.atomic();
  second->sum({"guard-counter"}, 1);
  const auto committed = co_await (first->commit() && second->commit());
  const auto& one = std::get<0>(committed);
  const auto& two = std::get<1>(committed);
  EXPECT(one.ok && two.ok);
  EXPECT_EQ((co_await kv.get({"guard-counter"})).value, KvValue(KvU64(2)));
  const KvEntryMaybe<> stale = co_await kv.get({"caller-fence"});
  co_await kv.set({"caller-fence"}, true);
  auto fenced = kv.atomic();
  fenced->check(stale).sum({"guard-counter"}, 1);
  EXPECT_EQ(co_await fenced->commit(), conflict);
  EXPECT_EQ((co_await kv.get({"guard-counter"})).value, KvValue(KvU64(2)));
}

KV_TEST("numeric key order distinguishes signed zeros and retains infinity and canonical NaN") {
  MemoryKv kv(testClock());
  const std::vector<double> numbers{-INF, -2, -0.0, 0, 2, INF, NAN_};
  std::vector<KvValue> indexes;
  for (std::size_t index = 0; index < numbers.size(); index += 1) {
    co_await kv.set({"numeric", numbers[index]}, index);
    indexes.emplace_back(index);
  }
  auto iterator = kv.list({.prefix = KvKey{"numeric"}});
  const auto rows = co_await kvListToArray(*iterator);
  EXPECT_EQ(valuesOf(rows), indexes);
  EXPECT(std::signbit(rows[2].key[1].number()));
  EXPECT(!std::signbit(rows[3].key[1].number()));
  EXPECT(std::isnan(rows[6].key[1].number()));
}

KV_TEST("Error cause graphs fail before any atomic mutation can lose persisted fields") {
  MemoryKv kv(testClock());
  KvError inner;
  inner.message = "inner";
  KvError error;
  error.message = "outer";
  error.cause = std::make_shared<KvValue>(inner);
  const KvValue graph = KvValue::object({{"records", KvMap{{"failure", error}}}});
  auto operation = kv.atomic();
  operation->set({"cause-partial"}, "must not publish").set({"cause"}, graph);
  EXPECT_EQ(co_await outcome(operation->commit()), std::string("TypeError: Bun KV cannot preserve Error.cause; persist an explicit error record instead"));
  EXPECT(!(co_await kv.get({"cause-partial"})).versionstamp);
  EXPECT(!(co_await kv.get({"cause"})).versionstamp);
}

KV_TEST("two native handles share CAS fencing and monotonic persisted revisions") {
  auto database = std::make_shared<MemoryKvDatabase>(testClock());
  MemoryKv first(database);
  MemoryKv second(database);
  const KvEntryMaybe<> absent = co_await first.get({"claim"});
  auto a = first.atomic();
  a->check(absent).set({"claim"}, "first");
  auto b = second.atomic();
  b->check(absent).set({"claim"}, "second");
  const auto committed = co_await (a->commit() && b->commit());
  const auto& ra = std::get<0>(committed);
  const auto& rb = std::get<1>(committed);
  EXPECT_EQ(static_cast<int>(ra.ok) + static_cast<int>(rb.ok), 1);
  const KvEntryMaybe<> before = co_await first.get({"claim"});
  co_await second.delete_({"claim"});
  co_await second.set({"claim"}, "replacement");
  const KvEntryMaybe<> after = co_await first.get({"claim"});
  EXPECT(after.versionstamp != before.versionstamp);
  EXPECT(after.versionstamp.value_or("") > before.versionstamp.value_or(""));
  auto staleOwner = first.atomic();
  staleOwner->check(before).set({"stale-owner"}, true);
  EXPECT_EQ(co_await staleOwner->commit(), conflict);
}

KV_TEST("two native handles preserve ten caller checks under mixed counter-type updates") {
  auto database = std::make_shared<MemoryKvDatabase>(testClock());
  MemoryKv first(database);
  MemoryKv second(database);
  std::vector<KvKey> fenceKeys;
  for (int index = 0; index < 10; ++index) fenceKeys.push_back({"mixed-fence", index});
  const auto fences = co_await first.getMany(fenceKeys);
  co_await first.set({"mixed-counter"}, KvU64(0));
  auto writer = [&]() -> Task<void> {
    for (int index = 0; index < 40; index += 1) {
      co_await second.set({"mixed-counter"}, BigInt(3));
      co_await second.set({"mixed-counter"}, KvU64(0));
    }
  };
  std::vector<bool> outcomes;
  auto committer = [&]() -> Task<void> {
    for (int index = 0; index < 20; index += 1) {
      auto operation = first.atomic();
      operation->check(fences).set({"mixed-marker", index}, true).sum({"mixed-side", index}, 1).sum({"mixed-counter"}, 1);
      bool ok = false;
      try {
        const KvCommitResult result = co_await operation->commit();
        EXPECT(result.ok);
        ok = true;
      } catch (const TypeError& error) {
        if (error.message.find("KvU64") == std::string::npos) throw;
      }
      outcomes.push_back(ok);
    }
  };
  co_await (writer() && committer());
  EXPECT_EQ(outcomes.size(), 20u);
  for (std::size_t index = 0; index < outcomes.size(); index += 1) {
    EXPECT_EQ((co_await first.get({"mixed-marker", index})).value, outcomes[index] ? KvValue(true) : KvValue(nullptr));
    EXPECT_EQ((co_await first.get({"mixed-side", index})).value, outcomes[index] ? KvValue(KvU64(1)) : KvValue(nullptr));
  }
}

// ---- MemoryKv: oracle-pinned semantics ---------------------------------------------------------

KV_TEST("every commit takes the next version, empty and check-only commits included") {
  MemoryKv kv(testClock());
  std::vector<std::string> stamps;
  for (int index = 0; index < 12; index += 1) stamps.push_back((co_await kv.set({"v", index}, index)).versionstamp);
  EXPECT_EQ(stamps.front(), std::string("00000000000000010000"));
  EXPECT_EQ(stamps.back(), std::string("000000000000000c0000"));
  auto multi = kv.atomic();
  multi->set({"m", 1}, 1).set({"m", 2}, 2).delete_({"v", 0});
  const KvCommitResult result = co_await multi->commit();
  EXPECT_EQ(result.versionstamp, std::string("000000000000000d0000"));
  EXPECT_EQ((co_await kv.get({"m", 1})).versionstamp, std::optional<std::string>(result.versionstamp));
  EXPECT_EQ((co_await kv.get({"m", 2})).versionstamp, std::optional<std::string>(result.versionstamp));
  const KvCommitResult fourteenth{true, "000000000000000e0000"};
  EXPECT_EQ((co_await kv.atomic()->commit()), fourteenth);
  EXPECT_EQ((co_await kv.atomic()->commit()).versionstamp, std::string("000000000000000f0000"));
  co_await kv.delete_({"nope"});
  EXPECT_EQ((co_await kv.set({"after"}, 1)).versionstamp, std::string("00000000000000110000"));
  auto checkOnly = kv.atomic();
  checkOnly->check(co_await kv.get({"after"}));
  EXPECT_EQ((co_await checkOnly->commit()).versionstamp, std::string("00000000000000120000"));
  EXPECT_EQ(kv.database()->version(), 0x12u);
}

KV_TEST("versionIncrement models the native random step") {
  const std::vector<std::uint64_t> steps{2, 9, 5};
  std::size_t next = 0;
  MemoryKv kv(testClock(), MemoryKvOptions{[&] { return steps[next++ % steps.size()]; }});
  std::vector<std::string> stamps;
  for (int index = 0; index < 3; index += 1) stamps.push_back((co_await kv.set({"v", index}, index)).versionstamp);
  EXPECT_EQ(stamps, (std::vector<std::string>{"00000000000000020000", "000000000000000b0000", "00000000000000100000"}));
}

KV_TEST("atomic checks follow the native versionstamp rules") {
  MemoryKv kv(testClock());
  for (int index = 0; index < 12; index += 1) co_await kv.set({"warm"}, index);
  const auto commitChecks = [&](std::vector<AtomicCheck> checks, std::optional<KvValue> value) -> Task<KvCommitResult> {
    auto operation = kv.atomic();
    operation->check(std::move(checks));
    if (value) operation->set({"a"}, *value);
    co_return co_await operation->commit();
  };
  EXPECT((co_await commitChecks(checks(AtomicCheck{{"a"}, std::nullopt}), KvValue(1))).ok);
  EXPECT_EQ(co_await commitChecks(checks(AtomicCheck{{"a"}, std::nullopt}), KvValue(2)), conflict);
  const KvEntryMaybe<> a = co_await kv.get({"a"});
  EXPECT((co_await commitChecks(checks(a), KvValue(3))).ok);
  EXPECT_EQ(co_await commitChecks(checks(a), KvValue(4)), conflict);
  EXPECT_EQ(co_await commitChecks(checks(AtomicCheck{{"absent"}, "00000000000000010000"}), std::nullopt), conflict);
  EXPECT_EQ(co_await outcome(commitChecks(checks(AtomicCheck{{"a"}, "xyz"}), std::nullopt)), std::string("TypeError: Invalid byte 'x'"));
  EXPECT_EQ(co_await outcome(commitChecks(checks(AtomicCheck{{"a"}, "abc"}), std::nullopt)), std::string("RangeError: Odd length hex string"));
  EXPECT_EQ(co_await outcome(commitChecks(checks(AtomicCheck{{"a"}, "é0"}), std::nullopt)), std::string("TypeError: Invalid byte 'Ã'"));
  EXPECT_EQ(co_await outcome(commitChecks(checks(AtomicCheck{{"a"}, "0001"}), std::nullopt)), std::string("Error: InvalidVersionstamp"));
  EXPECT_EQ(co_await outcome(commitChecks(checks(AtomicCheck{{"a"}, "000000000000000000000000"}), std::nullopt)), std::string("Error: InvalidVersionstamp"));
  std::string current = *(co_await kv.get({"a"})).versionstamp;
  EXPECT(current.find_first_of("abcdef") != std::string::npos);
  std::string upper = current;
  std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
  EXPECT((co_await commitChecks(checks(AtomicCheck{{"a"}, upper}), std::nullopt)).ok);
  EXPECT((co_await commitChecks(checks(AtomicCheck{{"es"}, ""}), std::nullopt)).ok);
  EXPECT_EQ(co_await commitChecks(checks(AtomicCheck{{"a"}, ""}), std::nullopt), conflict);
  current = *(co_await kv.get({"a"})).versionstamp;
  EXPECT_EQ(co_await commitChecks(checks(AtomicCheck{{"a"}, current.substr(0, 16) + "0001"}), std::nullopt), conflict);
  EXPECT((co_await commitChecks(checks(AtomicCheck{KvKey{}, std::nullopt}), std::nullopt)).ok);
  std::vector<AtomicCheck> ten;
  for (int index = 0; index < 10; index += 1) ten.push_back(AtomicCheck{{"c", index}, std::nullopt});
  EXPECT((co_await commitChecks(ten, std::nullopt)).ok);
  std::vector<AtomicCheck> eleven = ten;
  eleven.push_back(AtomicCheck{{"c", 10}, std::nullopt});
  EXPECT_EQ(co_await outcome(commitChecks(eleven, std::nullopt)), std::string("Error: TooManyChecks"));
  eleven.back() = AtomicCheck{{"a"}, std::nullopt};
  EXPECT_EQ(co_await outcome(commitChecks(eleven, std::nullopt)), std::string("Error: TooManyChecks"));
}

KV_TEST("mutations apply in order inside one all-or-nothing commit") {
  MemoryKv kv(testClock());
  const auto commitOf = [&](const std::function<void(AtomicOperation&)>& build) -> Task<KvCommitResult> {
    auto operation = kv.atomic();
    build(*operation);
    co_return co_await operation->commit();
  };
  const auto many = [](int count) {
    return [count](AtomicOperation& operation) {
      for (int index = 0; index < count; index += 1) operation.set({"mm", index}, index);
    };
  };
  EXPECT((co_await commitOf(many(1000))).ok);
  EXPECT_EQ(co_await outcome(commitOf(many(1001))), std::string("Error: TooManyMutations"));
  co_await commitOf([](AtomicOperation& operation) { operation.set({"dup"}, 1).set({"dup"}, 2); });
  EXPECT_EQ((co_await kv.get({"dup"})).value, KvValue(2));
  co_await commitOf([](AtomicOperation& operation) { operation.set({"sd"}, 1).delete_({"sd"}); });
  EXPECT(!(co_await kv.get({"sd"})).versionstamp);
  co_await commitOf([](AtomicOperation& operation) { operation.delete_({"ds"}).set({"ds"}, 5); });
  EXPECT_EQ((co_await kv.get({"ds"})).value, KvValue(5));
  co_await commitOf([](AtomicOperation& operation) { operation.sum({"ss"}, 2).set({"ss"}, "x"); });
  EXPECT_EQ((co_await kv.get({"ss"})).value, KvValue("x"));
  try {
    co_await commitOf([](AtomicOperation& operation) { operation.set({"ts"}, "x").sum({"ts"}, 2); });
    EXPECT(false);
  } catch (const TypeError& error) {
    EXPECT_EQ(error.message, std::string("Atomic arithmetic requires an absent key or KvU64 value"));
    ASSERT(error.cause != nullptr);
    EXPECT_EQ(error.cause->toString(), std::string("Error: Failed to perform 'sum' mutation on a non-U64 value in the database"));
  }
  EXPECT(!(co_await kv.get({"ts"})).versionstamp);
  co_await commitOf([](AtomicOperation& operation) { operation.sum({"s2"}, 2).sum({"s2"}, 3); });
  EXPECT_EQ((co_await kv.get({"s2"})).value, KvValue(KvU64(5)));
  co_await commitOf([](AtomicOperation& operation) { operation.set({"s3"}, KvU64(10)).sum({"s3"}, 3); });
  EXPECT_EQ((co_await kv.get({"s3"})).value, KvValue(KvU64(13)));
  co_await commitOf([](AtomicOperation& operation) { operation.min({"minabs"}, 7); });
  EXPECT_EQ((co_await kv.get({"minabs"})).value, KvValue(KvU64(7)));
  co_await commitOf([](AtomicOperation& operation) { operation.max({"maxabs"}, 7); });
  EXPECT_EQ((co_await kv.get({"maxabs"})).value, KvValue(KvU64(7)));
  co_await kv.set({"u"}, KvU64(10));
  co_await commitOf([](AtomicOperation& operation) { operation.min({"u"}, 3); });
  EXPECT_EQ((co_await kv.get({"u"})).value, KvValue(KvU64(3)));
  co_await commitOf([](AtomicOperation& operation) { operation.max({"u"}, 30); });
  EXPECT_EQ((co_await kv.get({"u"})).value, KvValue(KvU64(30)));
  const KvCommitResult smaller = co_await commitOf([](AtomicOperation& operation) { operation.max({"u"}, 1); });
  const KvEntryMaybe<> unchanged = co_await kv.get({"u"});
  EXPECT_EQ(unchanged.value, KvValue(KvU64(30)));
  EXPECT_EQ(unchanged.versionstamp, std::optional<std::string>(smaller.versionstamp));
  co_await kv.set({"w"}, KvU64(std::numeric_limits<std::uint64_t>::max() - 1));
  co_await commitOf([](AtomicOperation& operation) { operation.sum({"w"}, 5); });
  EXPECT_EQ((co_await kv.get({"w"})).value, KvValue(KvU64(3)));
  co_await kv.set({"num"}, 5);
  co_await kv.set({"big"}, BigInt(5));
  co_await kv.set({"bytes"}, bytes(1, 2, 3, 4, 5, 6, 7, 8));
  const std::vector<const char*> wrongTypes{"num", "big", "bytes"};
  for (const char* key : wrongTypes)
    EXPECT_EQ(co_await outcome(commitOf([key](AtomicOperation& operation) { operation.sum({key}, 1); })),
              std::string("TypeError: Atomic arithmetic requires an absent key or KvU64 value"));
  EXPECT_EQ(co_await outcome(commitOf([](AtomicOperation& operation) { operation.min({"num"}, 1); })),
            std::string("TypeError: Atomic arithmetic requires an absent key or KvU64 value"));
  EXPECT_EQ(co_await outcome(commitOf([](AtomicOperation& operation) { operation.max({"num"}, 1); })),
            std::string("TypeError: Atomic arithmetic requires an absent key or KvU64 value"));
  EXPECT_EQ(co_await outcome(commitOf([](AtomicOperation& operation) { operation.mutate({KvMutation{{"ms"}, KvMutationType::sum, 5, std::nullopt}}); })),
            std::string("Error: Some of the parameters are not valid V8 values"));
  co_await commitOf([](AtomicOperation& operation) { operation.mutate({KvMutation{{"mm4"}, KvMutationType::min, KvU64(4), std::nullopt}}); });
  EXPECT_EQ((co_await kv.get({"mm4"})).value, KvValue(KvU64(4)));
}

KV_TEST("builder validation throws synchronously like the SDK") {
  MemoryKv kv(testClock());
  EXPECT_EQ(thrown([&] { kv.atomic()->set({"e"}, 1, {.expireIn = 0}); }), std::string("TypeError: Bad 'expireIn', expected optional positive integer, found 0"));
  EXPECT_EQ(thrown([&] { kv.atomic()->set({"e"}, 1, {.expireIn = 1.5}); }), std::string("TypeError: Bad 'expireIn', expected optional positive integer, found 1.5"));
  EXPECT_EQ(thrown([&] { kv.atomic()->set({"e"}, 1, {.expireIn = -5}); }), std::string("TypeError: Bad 'expireIn', expected optional positive integer, found -5"));
  EXPECT_EQ(thrown([&] { kv.atomic()->set({"e"}, 1, {.expireIn = NAN_}); }), std::string("TypeError: Bad 'expireIn', expected optional positive integer, found NaN"));
  EXPECT_EQ(co_await outcome(kv.set({"e"}, 1, {.expireIn = 0})), std::string("TypeError: Bad 'expireIn', expected optional positive integer, found 0"));
  EXPECT_EQ(co_await outcome(kv.set({}, 1)), std::string("TypeError: Key cannot be empty"));
  EXPECT_EQ(co_await outcome(kv.get({})), std::string("TypeError: Key cannot be empty"));
  EXPECT_EQ(co_await outcome(kv.delete_({})), std::string("TypeError: Key cannot be empty"));
  EXPECT_EQ(thrown([&] { kv.atomic()->delete_({}); }), std::string("TypeError: Key cannot be empty"));
  EXPECT_EQ(thrown([&] {
              kv.atomic()->mutate({KvMutation{{"me"}, KvMutationType::set, 1, 0.0}, KvMutation{{}, KvMutationType::delete_, KvValue(), std::nullopt}});
            }),
            std::string("TypeError: Key cannot be empty"));
  EXPECT_EQ(thrown([&] { kv.atomic()->mutate({KvMutation{{"me"}, KvMutationType::set, 1, 0.0}}); }),
            std::string("TypeError: Bad 'expireIn', expected optional positive integer, found 0"));
  EXPECT_EQ(thrown([&] { kv.atomic()->set({}, 1, {.expireIn = 0}); }), std::string("TypeError: Bad 'expireIn', expected optional positive integer, found 0"));
  EXPECT_EQ(thrown([&] { kv.atomic()->sum({}, -1); }), std::string("TypeError: Key cannot be empty"));
  EXPECT_EQ(thrown([&] { kv.atomic()->sum({"x"}, -1); }), std::string("Error: value must be a positive bigint"));
  EXPECT_EQ(thrown([&] { kv.atomic()->min({"x"}, BigInt(1) << BigInt(64)); }), std::string("Error: value must fit in a 64-bit unsigned integer"));
  EXPECT_EQ(thrown([&] { kv.atomic()->max({"x"}, std::numeric_limits<std::uint64_t>::max()); }), std::string("returned"));
}

KV_TEST("getMany follows the native read-range limit") {
  MemoryKv kv(testClock());
  co_await kv.set({"a"}, 3);
  EXPECT((co_await kv.getMany({})).empty());
  const auto keysOf = [](int count) {
    std::vector<KvKey> keys;
    for (int index = 0; index < count; index += 1) keys.push_back({"gm", index});
    return keys;
  };
  EXPECT_EQ((co_await kv.getMany(keysOf(10))).size(), 10u);
  EXPECT_EQ(co_await outcome(kv.getMany(keysOf(11))), std::string("Error: TooManyReadRanges"));
  const auto duplicates = co_await kv.getMany(keys(KvKey{"a"}, KvKey{"a"}));
  EXPECT_EQ(duplicates[0].value, KvValue(3));
  EXPECT_EQ(duplicates[1].value, KvValue(3));
  EXPECT_EQ(co_await outcome(kv.getMany(keys(KvKey{"a"}, KvKey{}))), std::string("TypeError: Key cannot be empty"));
}

KV_TEST("key and value sizes are limited like the native store") {
  MemoryKv kv(testClock());
  const auto keyOf = [](std::size_t bytes) { return KvKey{std::string(bytes - 2, 'k')}; };
  const std::string tooLong = "Error: KeyTooLong";
  const auto checkAbsent = [&](std::size_t bytes) -> Task<KvCommitResult> {
    auto operation = kv.atomic();
    operation->check(AtomicCheck{keyOf(bytes), std::nullopt});
    co_return co_await operation->commit();
  };
  const auto sumAt = [&](std::size_t bytes) -> Task<KvCommitResult> {
    auto operation = kv.atomic();
    operation->sum(keyOf(bytes), 1);
    co_return co_await operation->commit();
  };
  const auto listPrefix = [&](std::size_t bytes) -> Task<std::size_t> {
    auto iterator = kv.list({.prefix = keyOf(bytes)});
    co_return (co_await kvListToArray(*iterator)).size();
  };
  const auto listRange = [&](std::size_t bytes) -> Task<std::size_t> {
    auto iterator = kv.list({.start = keyOf(bytes), .end = KvKey{"z"}});
    co_return (co_await kvListToArray(*iterator)).size();
  };
  EXPECT((co_await kv.set(keyOf(2048), 1)).ok);
  EXPECT((co_await kv.get(keyOf(2048))).versionstamp.has_value());
  co_await kv.delete_(keyOf(2048));
  EXPECT((co_await checkAbsent(2048)).ok);
  EXPECT((co_await sumAt(2048)).ok);
  EXPECT_EQ(co_await outcome(listPrefix(2048)), tooLong);
  EXPECT_EQ(co_await listRange(2048), 1u);
  EXPECT_EQ((co_await kv.getMany(keys(keyOf(2048)))).size(), 1u);
  EXPECT_EQ(co_await outcome(kv.set(keyOf(2049), 1)), tooLong);
  EXPECT(!(co_await kv.get(keyOf(2049))).versionstamp);
  EXPECT_EQ(co_await outcome(kv.delete_(keyOf(2049))), tooLong);
  EXPECT((co_await checkAbsent(2049)).ok);
  EXPECT_EQ(co_await outcome(sumAt(2049)), tooLong);
  EXPECT_EQ(co_await outcome(listPrefix(2049)), tooLong);
  EXPECT_EQ(co_await listRange(2049), 0u);
  EXPECT_EQ((co_await kv.getMany(keys(keyOf(2049)))).size(), 1u);
  const std::vector<std::size_t> tooLongSizes{2050, 4096};
  for (const std::size_t size : tooLongSizes) {
    EXPECT_EQ(co_await outcome(kv.set(keyOf(size), 1)), tooLong);
    EXPECT_EQ(co_await outcome(kv.get(keyOf(size))), tooLong);
    EXPECT_EQ(co_await outcome(kv.delete_(keyOf(size))), tooLong);
    EXPECT_EQ(co_await outcome(checkAbsent(size)), tooLong);
    EXPECT_EQ(co_await outcome(sumAt(size)), tooLong);
    EXPECT_EQ(co_await outcome(listPrefix(size)), tooLong);
    EXPECT_EQ(co_await outcome(listRange(size)), tooLong);
    EXPECT_EQ(co_await outcome(kv.getMany(keys(keyOf(size)))), tooLong);
  }
  EXPECT((co_await kv.set({"bv"}, KvBytes(65536))).ok);
  EXPECT_EQ(co_await outcome(kv.set({"bv"}, KvBytes(65537))), std::string("Error: ValueTooLong"));
  // A string of n Latin-1 characters serializes to n + 9 bytes.
  EXPECT((co_await kv.set({"sv"}, std::string(65527, 's'))).ok);
  EXPECT_EQ(co_await outcome(kv.set({"sv"}, std::string(65528, 's'))), std::string("Error: ValueTooLong"));
}

KV_TEST("commit payloads are limited to 819200 bytes and validated in native order") {
  MemoryKv kv(testClock());
  const auto keyOf = [](std::size_t bytes) { return KvKey{std::string(bytes - 2, 'k')}; };
  // Twelve 65536-byte values under 12-byte keys, then ["q"] (3 bytes) with `extra` bytes.
  const auto payload = [&](std::size_t extra, std::size_t checkBytes, std::optional<KvMutationType> tail) -> Task<KvCommitResult> {
    auto operation = kv.atomic();
    if (checkBytes) operation->check(AtomicCheck{keyOf(checkBytes), std::nullopt});
    for (int index = 0; index < 12; index += 1) operation->set({"p", index}, KvBytes(65536));
    operation->set({"q"}, KvBytes(extra));
    if (tail == KvMutationType::delete_) operation->delete_({"dd"});
    if (tail == KvMutationType::sum) operation->sum({"ss"}, 1);
    co_return co_await operation->commit();
  };
  const std::size_t base = 12 * (12 + 65536) + 3;
  const std::string tooLarge = "Error: AtomicWriteTooLarge";
  EXPECT((co_await payload(819200 - base, 0, std::nullopt)).ok);
  EXPECT_EQ(co_await outcome(payload(819201 - base, 0, std::nullopt)), tooLarge);
  EXPECT((co_await payload(819200 - base - 2000, 2000, std::nullopt)).ok);
  EXPECT_EQ(co_await outcome(payload(819201 - base - 2000, 2000, std::nullopt)), tooLarge);
  EXPECT((co_await payload(819200 - base - 4, 0, KvMutationType::delete_)).ok);
  EXPECT_EQ(co_await outcome(payload(819201 - base - 4, 0, KvMutationType::delete_)), tooLarge);
  EXPECT((co_await payload(819200 - base - 4 - 8, 0, KvMutationType::sum)).ok);
  EXPECT_EQ(co_await outcome(payload(819201 - base - 4 - 8, 0, KvMutationType::sum)), tooLarge);

  const auto checksOf = [&](int count, std::size_t keyBytes) {
    std::vector<AtomicCheck> checks;
    for (int index = 0; index < count; index += 1) {
      std::string name = "c" + std::to_string(index);
      if (keyBytes > name.size() + 2) name.resize(keyBytes - 2, 'c');
      checks.push_back(AtomicCheck{{name}, std::nullopt});
    }
    return checks;
  };
  const auto attempt = [&](const std::function<void(AtomicOperation&)>& build) -> Task<std::string> {
    auto operation = kv.atomic();
    build(*operation);
    co_return co_await outcome(operation->commit());
  };
  const auto manySets = [](AtomicOperation& operation) {
    for (int index = 0; index < 1001; index += 1) operation.set({"m", index}, 1);
  };
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { manySets(operation.check(checksOf(11, 2))); }), std::string("Error: TooManyChecks"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { manySets(operation.check(checksOf(10, 2050))); }), std::string("Error: TooManyMutations"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.check(AtomicCheck{keyOf(2050), "0001"}); }), std::string("Error: KeyTooLong"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.check(AtomicCheck{{"a"}, "0001"}, AtomicCheck{keyOf(2050), std::nullopt}); }),
            std::string("Error: InvalidVersionstamp"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.check(AtomicCheck{keyOf(2050), std::nullopt}).set(keyOf(2049), 1); }),
            std::string("Error: KeyTooLong"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.check(AtomicCheck{{"a"}, "0001"}).set(keyOf(2049), 1); }),
            std::string("Error: InvalidVersionstamp"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.set({"v"}, KvBytes(65537)).set(keyOf(2049), 1); }), std::string("Error: ValueTooLong"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.set(keyOf(2049), 1).set({"v"}, KvBytes(65537)); }), std::string("Error: KeyTooLong"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.set(keyOf(2049), KvBytes(65537)); }), std::string("Error: KeyTooLong"));
  KvError caused;
  caused.message = "x";
  caused.cause = std::make_shared<KvValue>(1);
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.check(checksOf(11, 2)).set({"x"}, caused); }),
            std::string("TypeError: Bun KV cannot preserve Error.cause; persist an explicit error record instead"));
  EXPECT_EQ(co_await attempt([&](AtomicOperation& operation) { operation.check(AtomicCheck{{"a"}, "zz"}).set({"x"}, caused); }),
            std::string("TypeError: Invalid byte 'z'"));
}

KV_TEST("list pages, cursors and selectors follow the SDK") {
  MemoryKv kv(testClock());
  co_await kv.set({"p"}, "root");
  for (int index = 1; index <= 7; index += 1) co_await kv.set({"p", index}, "c" + std::to_string(index));
  co_await kv.set({"p", 3, "x"}, "deep");
  co_await kv.set({"q", 1}, "other");
  const auto page = [&](KvListSelector selector, KvListOptions options) -> Task<std::pair<std::vector<KvValue>, std::string>> {
    auto iterator = kv.list(std::move(selector), std::move(options));
    std::vector<KvValue> values = valuesOf(co_await kvListToArray(*iterator));
    co_return std::pair{std::move(values), iterator->cursor()};
  };
  const KvListSelector p{.prefix = KvKey{"p"}};
  const KvListOptions limit3{.limit = 3};
  const auto firstPage = co_await page(p, limit3);
  const auto& page1 = std::get<0>(firstPage);
  const auto& cursor1 = std::get<1>(firstPage);
  EXPECT_EQ(page1, (std::vector<KvValue>{"c1", "c2", "c3"}));
  EXPECT_EQ(cursor1, std::string("eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3MDAwMjFjMDA4MDAwMDAwMDAwMDAwIn0="));
  auto second = kv.list(p, {.limit = 3, .cursor = cursor1});
  EXPECT_EQ(second->cursor(), cursor1);
  EXPECT_EQ(valuesOf(co_await kvListToArray(*second)), vals("deep", "c4", "c5"));
  const std::string cursor2 = second->cursor();
  EXPECT_EQ(cursor2, std::string("eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3MDAwMjFjMDE0MDAwMDAwMDAwMDAwIn0="));
  const KvListOptions fromCursor2{.cursor = cursor2};
  const auto thirdPage = co_await page(p, fromCursor2);
  const auto& page3 = std::get<0>(thirdPage);
  const auto& cursor3 = std::get<1>(thirdPage);
  EXPECT_EQ(page3, (std::vector<KvValue>{"c6", "c7"}));
  EXPECT_EQ(cursor3, std::string(""));
  const KvListOptions reverse2{.limit = 2, .reverse = true};
  const auto reversePage = co_await page(p, reverse2);
  const auto& reversed = std::get<0>(reversePage);
  const auto& reverseCursor = std::get<1>(reversePage);
  EXPECT_EQ(reversed, (std::vector<KvValue>{"c7", "c6"}));
  EXPECT_EQ(reverseCursor, std::string("eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3MDAwMjFjMDE4MDAwMDAwMDAwMDAwIn0="));
  const KvListOptions reverseFromCursor{.cursor = reverseCursor, .reverse = true};
  const auto reverseRemainingPage = co_await page(p, reverseFromCursor);
  const auto& reversed2 = std::get<0>(reverseRemainingPage);
  const auto& reverseCursor2 = std::get<1>(reverseRemainingPage);
  EXPECT_EQ(reversed2, (std::vector<KvValue>{"c5", "c4", "deep", "c3", "c2", "c1"}));
  EXPECT_EQ(reverseCursor2, std::string(""));
  const std::vector<KvValue> children{"c1", "c2", "c3", "deep", "c4", "c5", "c6", "c7"};
  const KvListOptions none{};
  const KvListOptions reverse{.reverse = true};
  const KvListOptions batch1{.batchSize = 1};
  EXPECT_EQ((co_await page(p, batch1)).first, children);
  const KvListSelector fromP3{.prefix = KvKey{"p"}, .start = KvKey{"p", 3}};
  EXPECT_EQ((co_await page(fromP3, none)).first, vals("c3", "deep", "c4", "c5", "c6", "c7"));
  const KvListSelector toP3{.prefix = KvKey{"p"}, .end = KvKey{"p", 3}};
  EXPECT_EQ((co_await page(toP3, none)).first, vals("c1", "c2"));
  const KvListSelector pToP3{.start = KvKey{"p"}, .end = KvKey{"p", 3}};
  EXPECT_EQ((co_await page(pToP3, none)).first, vals("root", "c1", "c2"));
  EXPECT_EQ((co_await page(pToP3, reverse)).first, vals("c2", "c1", "root"));
  const KvListSelector p2ToP2{.start = KvKey{"p", 2}, .end = KvKey{"p", 2}};
  EXPECT((co_await page(p2ToP2, none)).first.empty());
  const KvListSelector everything{.prefix = KvKey{}};
  EXPECT_EQ((co_await page(everything, none)).first, vals("root", "c1", "c2", "c3", "deep", "c4", "c5", "c6", "c7", "other"));
  const KvListSelector fromP5{.prefix = KvKey{"p"}, .start = KvKey{"p", 5}};
  EXPECT_EQ((co_await page(fromP5, reverse)).first, vals("c7", "c6", "c5"));
  const KvListSelector toP5{.prefix = KvKey{"p"}, .end = KvKey{"p", 5}};
  EXPECT_EQ((co_await page(toP5, reverse2)).first, vals("c4", "deep"));
  const KvListSelector q{.prefix = KvKey{"q"}};
  const KvListOptions limit1{.limit = 1};
  const auto exactPage = co_await page(q, limit1);
  const auto& exact = std::get<0>(exactPage);
  const auto& exactCursor = std::get<1>(exactPage);
  EXPECT_EQ(exact, std::vector<KvValue>{"other"});
  EXPECT_EQ(exactCursor, std::string("eyJsYXN0WWllbGRlZEtleUJ5dGVzIjoiMDI3MTAwMjFiZmYwMDAwMDAwMDAwMDAwIn0="));
  const KvListOptions afterExact{.limit = 1, .cursor = exactCursor};
  const auto afterPage = co_await page(q, afterExact);
  const auto& after = std::get<0>(afterPage);
  const auto& afterCursor = std::get<1>(afterPage);
  EXPECT(after.empty());
  EXPECT_EQ(afterCursor, std::string(""));
  const KvListOptions crossCursor{.cursor = cursor1};
  EXPECT_EQ((co_await page(q, crossCursor)).first, vals("deep", "c4", "c5", "c6", "c7", "other"));
  const KvListOptions crossCursor2{.cursor = exactCursor};
  EXPECT((co_await page(p, crossCursor2)).first.empty());
  const auto listError = [&](KvListSelector selector, KvListOptions options) { return thrown([&] { (void)kv.list(std::move(selector), std::move(options)); }); };
  EXPECT_EQ(listError({.prefix = KvKey{"p"}, .start = KvKey{"p", 1}, .end = KvKey{"p", 2}}, {}),
            std::string("TypeError: Selector can not specify both 'start' and 'end' key when specifying 'prefix'"));
  EXPECT_EQ(listError({.prefix = KvKey{"p"}, .start = KvKey{"q"}}, {}), std::string("TypeError: Start key is not in the keyspace defined by prefix"));
  EXPECT_EQ(listError({.prefix = KvKey{"p"}, .start = KvKey{"p"}}, {}), std::string("TypeError: Start key is not in the keyspace defined by prefix"));
  EXPECT_EQ(listError({.prefix = KvKey{"p"}, .end = KvKey{"outside", 1}}, {}), std::string("TypeError: End key is not in the keyspace defined by prefix"));
  EXPECT_EQ(listError({.start = KvKey{"z"}, .end = KvKey{"a"}}, {}), std::string("TypeError: Start key is greater than end key"));
  EXPECT_EQ(listError({.start = KvKey{"z"}}, {}), std::string("TypeError: Selector must specify either 'prefix' or both 'start' and 'end' key"));
  EXPECT_EQ(listError({.end = KvKey{"z"}}, {}), std::string("TypeError: Selector must specify either 'prefix' or both 'start' and 'end' key"));
  EXPECT_EQ(listError(p, {.limit = 0}), std::string("TypeError: Bad 'limit': 0"));
  EXPECT_EQ(listError(p, {.limit = 1.5}), std::string("TypeError: Bad 'limit': 1.5"));
  EXPECT_EQ(listError(p, {.limit = -1}), std::string("TypeError: Bad 'limit': -1"));
  EXPECT_EQ(listError(p, {.batchSize = 1001}), std::string("TypeError: Bad 'batchSize': 1001"));
  EXPECT_EQ(listError(p, {.batchSize = 0}), std::string("TypeError: Bad 'batchSize': 0"));
  const std::vector<std::string> invalidCursors{"garbage", "", base64("{\"lastYieldedKeyBytes\":\"zz\"}")};
  for (const std::string& cursor : invalidCursors) {
    auto invalid = kv.list(p, {.cursor = cursor});
    EXPECT_EQ(invalid->cursor(), cursor);
    EXPECT_EQ(co_await outcome(invalid->next()), std::string("Error: Invalid cursor"));
    EXPECT(!(co_await invalid->next()));
  }
}

KV_TEST("list batches observe writes beyond the fetched batch only") {
  MemoryKv kv(testClock());
  co_await kv.set({"snap", 1}, 1);
  co_await kv.set({"snap", 3}, 3);
  auto small = kv.list({.prefix = KvKey{"snap"}}, {.batchSize = 1});
  EXPECT_EQ((co_await small->next())->key, (KvKey{"snap", 1}));
  co_await kv.set({"snap", 2}, 2);
  EXPECT_EQ(valuesOf(co_await kvListToArray(*small)), vals(2, 3));
  co_await kv.delete_({"snap", 2});
  auto large = kv.list({.prefix = KvKey{"snap"}});
  EXPECT_EQ((co_await large->next())->key, (KvKey{"snap", 1}));
  co_await kv.set({"snap", 2}, 2);
  EXPECT_EQ(valuesOf(co_await kvListToArray(*large)), vals(3));
}

KV_TEST("reverse pages, deleted cursor keys and forgiving cursors") {
  MemoryKv kv(testClock());
  co_await kv.set({"a"}, 0);
  for (int index = 1; index <= 5; index += 1) co_await kv.set({"rv", index}, index);
  std::optional<std::string> cursor;
  std::vector<std::pair<std::vector<KvValue>, std::string>> pages;
  for (int index = 0; index < 4; index += 1) {
    auto iterator = kv.list({.prefix = KvKey{"rv"}}, {.limit = 2, .cursor = cursor, .reverse = true, .batchSize = 2});
    std::vector<KvValue> values = valuesOf(co_await kvListToArray(*iterator));
    pages.emplace_back(std::move(values), iterator->cursor());
    cursor = iterator->cursor();
    if (cursor->empty()) break;
  }
  ASSERT_EQ(pages.size(), 3u);
  EXPECT_EQ(pages[0].first, (std::vector<KvValue>{5, 4}));
  EXPECT_EQ(unpackCursor(pages[0].second), packKey({"rv", 4}));
  EXPECT_EQ(hex(unpackCursor(pages[0].second)), std::string("0272760021c010000000000000"));
  EXPECT_EQ(pages[1].first, (std::vector<KvValue>{3, 2}));
  EXPECT_EQ(hex(unpackCursor(pages[1].second)), std::string("0272760021c000000000000000"));
  EXPECT_EQ(pages[2].first, (std::vector<KvValue>{1}));
  EXPECT_EQ(pages[2].second, std::string(""));
  auto limited = kv.list({.prefix = KvKey{"rv"}}, {.limit = 2});
  co_await kvListToArray(*limited);
  co_await kv.delete_({"rv", 2});
  const std::string resume = limited->cursor();
  const auto rest = [&](std::string text) -> Task<std::vector<KvValue>> {
    auto iterator = kv.list({.prefix = KvKey{"rv"}}, {.cursor = std::move(text)});
    co_return valuesOf(co_await kvListToArray(*iterator));
  };
  EXPECT_EQ(co_await rest(resume), vals(3, 4, 5));
  std::string unpadded = resume;
  while (!unpadded.empty() && unpadded.back() == '=') unpadded.pop_back();
  EXPECT_EQ(co_await rest(unpadded), vals(3, 4, 5));
  EXPECT_EQ(co_await rest(" " + resume + "\n"), vals(3, 4, 5));
  EXPECT_EQ(co_await outcome(rest(base64("5"))), std::string("Error: Invalid cursor"));
  EXPECT_EQ(co_await rest(base64("{\"lastYieldedKeyBytes\":\"0272760021BFF0000000000000\"}")), vals(3, 4, 5));
  // An empty key cursor starts the scan at the beginning of the whole keyspace.
  EXPECT_EQ(co_await rest(base64("{\"lastYieldedKeyBytes\":\"\"}")), vals(0, 1, 3, 4, 5));
}

KV_TEST("closing a handle follows the SDK's close checks") {
  MemoryKv kv(testClock());
  co_await kv.set({"a"}, 1);
  co_await kv.set({"b"}, 2);
  auto pending = kv.list({.prefix = KvKey{}});
  auto started = kv.list({.prefix = KvKey{}}, {.batchSize = 1});
  EXPECT_EQ((co_await started->next())->key, (KvKey{"a"}));
  auto pendingAtomic = kv.atomic();
  pendingAtomic->set({"c"}, 3);
  kv.close();
  EXPECT_EQ(co_await outcome(kv.get({"a"})), std::string("Error: Cannot call '.get' after '.close' is called"));
  EXPECT_EQ(co_await outcome(kv.getMany(keys(KvKey{"a"}))), std::string("Error: Cannot call '.getMany' after '.close' is called"));
  EXPECT_EQ(co_await outcome(kv.getMany({})), std::string("Error: Cannot call '.getMany' after '.close' is called"));
  EXPECT_EQ(co_await outcome(kv.set({"a"}, 1)), std::string("Error: Cannot call '.commit' after '.close' is called"));
  EXPECT_EQ(co_await outcome(kv.delete_({"a"})), std::string("Error: Cannot call '.commit' after '.close' is called"));
  EXPECT_EQ(thrown([&] { (void)kv.list({.prefix = KvKey{}}); }), std::string("Error: Cannot call '.list' after '.close' is called"));
  auto built = kv.atomic();
  built->set({"x"}, 1);
  EXPECT_EQ(co_await outcome(built->commit()), std::string("Error: Cannot call '.commit' after '.close' is called"));
  EXPECT_EQ(co_await outcome(pendingAtomic->commit()), std::string("Error: Cannot call '.commit' after '.close' is called"));
  EXPECT_EQ(co_await outcome(pending->next()), std::string("Error: db not found"));
  EXPECT_EQ(co_await outcome(started->next()), std::string("Error: db not found"));
  EXPECT_EQ(thrown([&] { kv.close(); }), std::string("Error: Cannot call '.close' after '.close' is called"));
  EXPECT_EQ(co_await outcome(kv.get({})), std::string("Error: Cannot call '.get' after '.close' is called"));
}

KV_TEST("values read back like a native round trip") {
  MemoryKv kv(testClock());
  const auto roundTrip = [&](KvValue value) -> Task<KvValue> {
    co_await kv.set({"value"}, std::move(value));
    co_return (co_await kv.get({"value"})).value;
  };
  EXPECT_EQ(co_await roundTrip(obj("c", KvU64(3), "arr", arr(KvU64(4)))), obj("c", u64Object(3), "arr", arr(u64Object(4))));
  KvMap map;
  map.set(1, "a");
  map.set("k", KvSet{BigInt(1), "x"});
  const KvValue rich = KvValue::object({{"m", map}, {"d", KvDate{1700000000123.0}}, {"r", KvRegExp{"x+", "gi"}}});
  EXPECT_EQ(co_await roundTrip(rich), rich);
  KvError boom;
  boom.name = "TypeError";
  boom.message = "boom";
  const KvValue error = co_await roundTrip(obj("e", boom));
  EXPECT_EQ(error["e"].as<KvError>().name, std::string("TypeError"));
  EXPECT_EQ(error["e"].as<KvError>().message, std::optional<std::string>("boom"));
  KvError custom;
  custom.name = "CustomError";
  custom.message = "x";
  EXPECT_EQ((co_await roundTrip(custom)).as<KvError>().name, std::string("Error"));
  const KvValue undefinedMembers = KvValue::object({{"a", KvValue()}, {"b", KvValue::array({KvValue(), 1})}, {"c", 1}});
  const KvValue members = co_await roundTrip(undefinedMembers);
  EXPECT_EQ(members, undefinedMembers);
  EXPECT(members.asObject().contains("a"));
  KvArray holes(4, KvHole{});
  holes[1] = 1;
  holes[3] = 2;
  EXPECT_EQ(co_await roundTrip(holes), KvValue(holes));
  const KvValue numbers = co_await roundTrip(obj("z", -0.0, "n", NAN_, "i", -INF));
  EXPECT(std::signbit(numbers["z"].asNumber()));
  EXPECT(std::isnan(numbers["n"].asNumber()));
  EXPECT_EQ(numbers["i"], KvValue(-INF));
  KvObject ordered;
  for (const auto& [key, value] : std::vector<std::pair<std::string, int>>{{"b", 1}, {"2", 2}, {"a", 3}, {"1", 4}, {"-1", 5}, {"01", 6}, {"4294967294", 7}, {"4294967295", 8}})
    ordered.set(key, value);
  std::vector<std::string> names;
  const KvValue orderedBack = co_await roundTrip(ordered);
  for (const auto& entry : orderedBack.asObject()) names.push_back(entry.first);
  EXPECT_EQ(names, (std::vector<std::string>{"1", "2", "4294967294", "b", "a", "-1", "01", "4294967295"}));
  const KvBytes backing{9, 1, 2, 9};
  const KvArrayBufferView middle{KvViewKind::Uint8Array, backing, 1, 2};
  EXPECT_EQ(co_await roundTrip(middle), KvValue(bytes(1, 2)));
  const KvValue nested = co_await roundTrip(obj("s", middle));
  EXPECT_EQ(nested["s"].as<KvArrayBufferView>().byteOffset, 1u);
  EXPECT_EQ(nested["s"].as<KvArrayBufferView>().buffer.size(), 4u);
  const KvArrayBuffer buffer{bytes(1, 2)};
  EXPECT_EQ(co_await roundTrip(buffer), KvValue(buffer));
  const KvArrayBufferView float32{KvViewKind::Float32Array, bytes(0, 0, 0xc0, 0x3f), 0, 4};
  const KvValue floats = co_await roundTrip(float32);
  EXPECT_EQ(floats.kind(), std::string_view("Float32Array"));
  EXPECT_EQ(co_await roundTrip(obj("v", -(BigInt(1) << BigInt(70)))), obj("v", -(BigInt(1) << BigInt(70))));
  EXPECT_EQ(co_await roundTrip("λ"), KvValue("λ"));
  EXPECT_EQ(co_await roundTrip("\xed\xa0\x80"), KvValue("\xed\xa0\x80"));
}

KV_TEST("expired rows stay readable until a native sweep") {
  fakeNow = 1'700'000'000'000.0;
  const double opened = fakeNow;
  MemoryKv kv(testClock());
  const KvCommitResult written = co_await kv.set({"e"}, "v", {.expireIn = 100});
  EXPECT_EQ(kv.database()->row({"e"})->expirationMs, opened + 100);
  fakeNow = opened + 500;
  EXPECT_EQ((co_await kv.get({"e"})).value, KvValue("v"));
  auto rows = kv.list({.prefix = KvKey{}});
  EXPECT_EQ((co_await kvListToArray(*rows)).size(), 1u);
  auto check = kv.atomic();
  check->check(AtomicCheck{{"probe-dummy"}, std::nullopt}).check(AtomicCheck{{"e"}, written.versionstamp});
  EXPECT((co_await check->commit()).ok);
  co_await kv.set({"o"}, 1, {.expireIn = 60'000});
  co_await kv.set({"o"}, 2);
  EXPECT_EQ(kv.database()->row({"o"})->expirationMs, -1.0);
  co_await kv.set({"s"}, KvU64(1), {.expireIn = 60'000});
  auto sum = kv.atomic();
  sum->sum({"s"}, 1);
  co_await sum->commit();
  EXPECT_EQ(kv.database()->row({"s"})->expirationMs, -1.0);
  fakeNow = opened + 59'999;
  EXPECT((co_await kv.get({"e"})).versionstamp.has_value());
  fakeNow = opened + 60'000;
  EXPECT(!(co_await kv.get({"e"})).versionstamp);
  EXPECT(!kv.database()->row({"e"}));
}

KV_TEST("a zero sweep interval expires rows as soon as they lapse") {
  MemoryKv kv(testClock(), {.sweepIntervalMs = 0});
  co_await kv.set({"e"}, "v", {.expireIn = 10});
  fakeNow += 9;
  EXPECT((co_await kv.get({"e"})).versionstamp.has_value());
  fakeNow += 1;
  EXPECT(!(co_await kv.get({"e"})).versionstamp);
}

KV_TEST("string key parts normalize through TextEncoder and TextDecoder") {
  MemoryKv kv(testClock());
  co_await kv.set({"bom", "﻿a"}, 1);
  co_await kv.set({"bom", "\xed\xa0\x80x"}, 2);
  co_await kv.set({"bom", "a﻿"}, 3);
  auto iterator = kv.list({.prefix = KvKey{"bom"}});
  const auto rows = co_await kvListToArray(*iterator);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].key, (KvKey{"bom", "a﻿"}));
  EXPECT_EQ(rows[1].key, (KvKey{"bom", "a"}));
  EXPECT_EQ(rows[2].key, (KvKey{"bom", "�x"}));
  EXPECT_EQ(valuesOf(rows), (std::vector<KvValue>{3, 1, 2}));
  EXPECT_EQ((co_await kv.get({"bom", "\xed\xa0\x80x"})).value, KvValue(2));
  EXPECT_EQ((co_await kv.get({"bom", "�x"})).value, KvValue(2));
  EXPECT_EQ((co_await kv.get({"bom", "﻿a"})).key, (KvKey{"bom", "﻿a"}));
  EXPECT_EQ(hex(packKey({"bom", "\uFEFFa"})), std::string("02626f6d0002efbbbf6100"));
  EXPECT_EQ(unhex("02626f6d00"), packKey({"bom"}));
}

KV_TEST("operations complete one macrotask later; SDK validation errors do not wait") {
  MemoryKv kv(testClock());
  co_await kv.set({"a"}, 1);
  co_await kv.set({"b"}, 2);
  auto executor = co_await boost::asio::this_coro::executor;
  bool ran = false;
  boost::asio::post(executor, [&] { ran = true; });
  co_await kv.get({"a"});
  EXPECT(ran);
  ran = false;
  boost::asio::post(executor, [&] { ran = true; });
  EXPECT_EQ(co_await outcome(kv.get({})), std::string("TypeError: Key cannot be empty"));
  EXPECT(!ran);
  co_await boost::asio::post(executor, boost::asio::use_awaitable);
  EXPECT(ran);
  ran = false;
  boost::asio::post(executor, [&] { ran = true; });
  EXPECT((co_await kv.getMany({})).empty());
  EXPECT(!ran);
  co_await boost::asio::post(executor, boost::asio::use_awaitable);
  auto iterator = kv.list({.prefix = KvKey{}});
  ran = false;
  boost::asio::post(executor, [&] { ran = true; });
  co_await iterator->next();
  EXPECT(ran);
  ran = false;
  boost::asio::post(executor, [&] { ran = true; });
  co_await iterator->next();
  EXPECT(!ran);
  co_await boost::asio::post(executor, boost::asio::use_awaitable);
  ran = false;
  boost::asio::post(executor, [&] { ran = true; });
  co_await kv.set({"c"}, 3);
  EXPECT(ran);
}

KV_TEST("typed get and getMany convert entries") {
  MemoryKv kv(testClock());
  co_await kv.set({"rec"}, obj("a", 1, "b", "x"));
  co_await kv.set({"counter"}, KvU64(5));
  const KvEntryMaybe<json::object> typed = co_await kv.get<json::object>({"rec"});
  EXPECT_EQ(typed.value.at("a").as_int64(), 1);
  EXPECT_EQ(typed.value.at("b").as_string(), json::string("x"));
  EXPECT(typed.versionstamp.has_value());
  const auto missing = co_await kv.get<std::optional<json::object>>({"missing"});
  EXPECT(!missing.value);
  EXPECT(!missing.versionstamp);
  EXPECT((co_await kv.get<json::value>({"missing"})).value.is_null());
  EXPECT_EQ(co_await outcome(kv.get<json::object>({"missing"})), std::string("TypeError: KV value is null, not Object"));
  EXPECT_EQ((co_await kv.get<KvU64>({"counter"})).value, KvU64(5));
  const auto both = co_await kv.getMany<json::value>(keys(KvKey{"rec"}, KvKey{"missing"}));
  EXPECT(both[0].value.is_object());
  EXPECT(both[1].value.is_null());
  const KvEntryMaybe<> raw = co_await kv.get({"rec"});
  EXPECT_EQ(raw.as<json::value>().value, json::parse(R"({"a":1,"b":"x"})"));
}

namespace {
  struct DedupeObservation {
    std::vector<AtomicCheck> checks;
    bool deletesDedupe = false;
    std::optional<KvCommitResult> result;
  };

  struct ObservedAtomic : ForwardingAtomicOperation {
    ObservedAtomic(std::unique_ptr<AtomicOperation> inner_, DedupeObservation& observation_, KvKey key_)
        : ForwardingAtomicOperation(std::move(inner_)), observation(observation_), key(std::move(key_)) {}
    using ForwardingAtomicOperation::check;
    AtomicOperation& check(std::vector<AtomicCheck> items) override {
      observation.checks.insert(observation.checks.end(), items.begin(), items.end());
      return ForwardingAtomicOperation::check(std::move(items));
    }
    AtomicOperation& delete_(KvKey target) override {
      if (target == key) observation.deletesDedupe = true;
      return ForwardingAtomicOperation::delete_(std::move(target));
    }
    Task<KvCommitResult> commit() override {
      const KvCommitResult result = co_await ForwardingAtomicOperation::commit();
      if (observation.deletesDedupe) observation.result = result;
      co_return result;
    }
    DedupeObservation& observation;
    KvKey key;
  };

  struct ObservingKv : ForwardingKv {
    ObservingKv(Kv& inner_, DedupeObservation& observation_, KvKey key_) : ForwardingKv(inner_), observation(observation_), key(std::move(key_)) {}
    std::unique_ptr<AtomicOperation> atomic() override { return std::make_unique<ObservedAtomic>(ForwardingKv::atomic(), observation, key); }
    DedupeObservation& observation;
    KvKey key;
  };
}

KV_TEST("forwarding wrappers observe builder calls and forward the commit") {
  MemoryKv native(testClock());
  co_await native.set({"dedupe", "f"}, "row");
  DedupeObservation observation;
  ObservingKv kv(native, observation, {"dedupe", "f"});
  const KvEntryMaybe<> row = co_await kv.get({"dedupe", "f"});
  auto operation = kv.atomic();
  operation->check(row).delete_({"dedupe", "f"});
  const KvCommitResult result = co_await operation->commit();
  EXPECT(result.ok);
  ASSERT_EQ(observation.checks.size(), 1u);
  EXPECT_EQ(observation.checks[0], AtomicCheck(row));
  EXPECT(observation.deletesDedupe);
  EXPECT_EQ(observation.result, std::optional<KvCommitResult>(result));
  EXPECT(!(co_await native.get({"dedupe", "f"})).versionstamp);
}

KV_TEST("return() ends a scan like breaking out of for await") {
  MemoryKv kv(testClock());
  for (int index = 1; index <= 3; index += 1) co_await kv.set({"r", index}, index);
  auto broken = kv.list({.prefix = KvKey{"r"}});
  EXPECT_EQ((co_await broken->next())->value, KvValue(1));
  co_await broken->return_();
  EXPECT_EQ(hex(unpackCursor(broken->cursor())), std::string("02720021bff0000000000000"));
  EXPECT(!(co_await broken->next()));
  EXPECT_EQ(broken->cursor(), std::string(""));
  auto unstarted = kv.list({.prefix = KvKey{"r"}}, {.limit = 2});
  co_await unstarted->return_();
  EXPECT(!(co_await unstarted->next()));
  EXPECT_EQ(unstarted->cursor(), std::string(""));
  auto limited = kv.list({.prefix = KvKey{"r"}}, {.limit = 1});
  co_await limited->next();
  co_await limited->return_();
  EXPECT(!(co_await limited->next()));
  EXPECT_EQ(hex(unpackCursor(limited->cursor())), std::string("02720021bff0000000000000"));
}

TEST("kvStructuredClone follows structuredClone on the value model") {
  EXPECT_EQ(kvStructuredClone(KvU64(5)), KvValue::object({{"value", BigInt(5)}}));
  const KvBytes backing{9, 1, 2, 9};
  const KvValue view = KvArrayBufferView{KvViewKind::Uint8Array, backing, 1, 2};
  EXPECT_EQ(kvStructuredClone(view), view);
  EXPECT_EQ(kvStructuredClone(KvArrayBufferView{KvViewKind::Uint8Array, backing, 0, 4}), KvValue(backing));
  KvError error;
  error.name = "AbortError";
  error.message = "stop";
  EXPECT_EQ(kvStructuredClone(error).as<KvError>().name, std::string("Error"));
  EXPECT_EQ(kvStructuredClone(KvValue::array({KvHole{}, KvValue(), -0.0})), KvValue::array({KvHole{}, KvValue(), -0.0}));
}

// ---- Identity (`WeakMap<Deno.Kv, T>`, `WeakSet`, `===`) ------------------------------------------
// JS facts measured with bun 1.4: a WeakMap key is the object itself (`set` on a
// `new Proxy(kv, ...)` leaves kv's entry alone and vice versa), `delete` is true once.

TEST("a Kv is a WeakMap and WeakSet key with JS object identity") {
  MemoryKv kv(testClock());
  WeakMap<Kv, int> generations;
  EXPECT(kv.identity() != nullptr);
  EXPECT(kv.identity() == kv.identity());
  EXPECT(!generations.has(kv));
  generations.set(kv, 1);
  EXPECT_EQ(generations.get(kv), std::optional<int>(1));
  // `new Proxy(kv, { get(...) })` is another object: no entry of its own, and its entry
  // leaves kv's alone.
  ForwardingKv proxy(kv);
  EXPECT(proxy.identity() != kv.identity());
  EXPECT(!generations.has(proxy));
  EXPECT_EQ(generations.get(proxy), std::nullopt);
  generations.set(proxy, 2);
  EXPECT_EQ(generations.get(kv), std::optional<int>(1));
  EXPECT_EQ(generations.get(proxy), std::optional<int>(2));
  EXPECT(generations.delete_(kv));
  EXPECT(!generations.delete_(kv));
  EXPECT(!generations.has(kv));
  EXPECT_EQ(generations.get(proxy), std::optional<int>(2));
  MemoryKv other(testClock());
  EXPECT(!generations.has(other));
  WeakSet<Kv> seen;
  seen.add(kv);
  EXPECT(seen.has(kv));
  EXPECT(!seen.has(proxy));
  EXPECT(seen.delete_(kv));
  EXPECT(!seen.delete_(kv));
}

TEST("WeakMap<Kv, T> entries die with the Kv and a later Kv at the same address is a new key") {
  WeakMap<Kv, std::string> tails;
  std::optional<MemoryKv> slot;
  slot.emplace(testClock());
  tails.set(*slot, "first");
  const std::weak_ptr<const void> first = slot->weakIdentity();
  const Kv* address = &*slot;
  EXPECT(!first.expired());
  slot.reset();
  EXPECT(first.expired());
  slot.emplace(testClock());
  // The same storage, a new object: `let cached = kv` compared with `===` later holds
  // weakIdentity() and compares while it lives, never a raw Kv*.
  EXPECT(&*slot == address);
  EXPECT(first.lock().get() != slot->identity());
  EXPECT(!tails.has(*slot));
  EXPECT_EQ(tails.get(*slot), std::nullopt);
  EXPECT(!tails.delete_(*slot));
  tails.set(*slot, "second");
  EXPECT_EQ(tails.get(*slot), std::optional<std::string>("second"));
  const std::weak_ptr<const void> second = slot->weakIdentity();
  EXPECT(second.lock().get() == slot->identity());
  // Every subclass carries its own: a hand-written fake as much as MemoryKv.
  CountingFacade facade(*slot);
  EXPECT(facade.identity() != slot->identity());
  EXPECT(!tails.has(facade));
}
