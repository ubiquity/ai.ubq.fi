// Port of tests/native-capture-operator.test.ts, the assertions src/kv-conversion/typed-value.ts
// can satisfy alone. Not ported: the Buffer brand, aliasing and cycle assertions of the first
// case (rt::KvValue has no Buffer brand and no shared or cyclic references), and the two
// producer cases (rust/crates/gateway-migration/examples/native-operator-capture.ts, bun:sqlite,
// node:v8 and src/kv-conversion/{native-schema,cursors}.ts are not ported).
#include "uos/kv_conversion/typed_value.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test.hpp"
#include <limits>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::kv_conversion::typed_value;

  // encodeNativeCaptureValue(sourceGraph()), recorded from Bun 1.4.0.
  const char* const SOURCE_GRAPH =
      R"({"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[["raw",{"type":"ref","ref":1}],["native",{"type":"ref","ref":2}],["again",{"type":"ref","ref":2}],["ordinary",{"type":"ref","ref":3}],["text",{"type":"string","value":"\ud83d"}],["self",{"type":"ref","ref":0}]]},{"type":"ArrayBuffer","value":"AAECAwQFBgcIAAAAAAAAAA=="},{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":3,"length":5},{"type":"Uint8Array","buffer":{"type":"ref","ref":1},"offset":3,"length":5}]})";

  json::value sourceGraph() { return rt::jsonParse(SOURCE_GRAPH); }

  // assert.throws(fn, pattern)
}

TEST("native capture Buffer retains brand, backing aliases, cycles and equal-but-distinct ordinary views without widening legacy") {
  const json::value graph = sourceGraph();
  static const rt::Regex BUFFER_NODE("Unsupported interchange node: Buffer");
  EXPECT_THROWS_MATCHING(decodeTypedValue(graph), BUFFER_NODE);
  const rt::KvValue ordinary = rt::KvValue::object(
      {{"text", std::string("\xED\xA0\xBD")},
       {"values", rt::KvValue::array({rt::KvValue(), -0.0, rt::BigInt(7), rt::KvDate{std::numeric_limits<double>::quiet_NaN()}, rt::KvRegExp{"x", "gi"}})}});
  EXPECT_EQ(rt::jsonStringify(encodeNativeCaptureValue(ordinary)), rt::jsonStringify(encodeTypedValue(ordinary)));
}

TEST("native capture Buffer rejects missing coordinates and a non-buffer backing") {
  static const rt::Regex PATTERN("Incomplete|ArrayBuffer backing");
  for (const std::string malformed : {"offset", "backing"}) {
    json::value graph = sourceGraph();
    json::object* view = nullptr;
    for (json::value& node : graph.as_object()["nodes"].as_array())
      if (node.as_object()["type"] == "Buffer") view = &node.as_object();
    ASSERT(view != nullptr);
    if (malformed == "offset") rt::objectDelete(*view, "offset");
    else (*view)["buffer"] = graph.as_object()["root"];
    EXPECT_THROWS_MATCHING(decodeNativeCaptureValue(graph), PATTERN);
  }
}
