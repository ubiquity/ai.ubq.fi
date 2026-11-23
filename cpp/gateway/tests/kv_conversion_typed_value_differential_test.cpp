// Differential cases for src/kv-conversion/typed-value.ts beyond its ported TS tests: every
// expected value was recorded from Bun 1.4.0 running the TypeScript module
// (JSON.stringify of the interchange graphs, and the name/message of every error).
#include "uos/kv_conversion/typed_value.hpp"
#include "uos/rt/base64.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"
#include <cstdio>
#include <limits>
#include <map>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::kv_conversion::typed_value;
  using rt::KvValue;

  constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
  constexpr double Infinity = std::numeric_limits<double>::infinity();

  KvValue view(rt::KvViewKind kind, rt::KvBytes buffer, std::uint64_t offset, std::uint64_t length) { return rt::KvArrayBufferView{kind, std::move(buffer), offset, length}; }
  KvValue error(std::string name, std::optional<std::string> message, std::optional<std::string> stack, std::optional<KvValue> cause = std::nullopt) {
    rt::KvError out;
    out.name = std::move(name);
    out.message = std::move(message);
    out.stack = std::move(stack);
    if (cause) out.cause = std::make_shared<const KvValue>(*cause);
    return out;
  }

  // The JavaScript values of the recording script, as stored values.
  std::map<std::string, KvValue> values() {
    std::map<std::string, KvValue> out;
    out["null"] = KvValue(nullptr);
    out["undefined"] = KvValue();
    out["true"] = KvValue(true);
    out["false"] = KvValue(false);
    out["text"] = KvValue("text");
    out["lone"] = KvValue(std::string("\xED\xA0\xBD"));
    out["unicode"] = KvValue(std::string("é\U0001F600") + std::string(1, '\0'));
    out["zero"] = KvValue(0.0);
    out["negzero"] = KvValue(-0.0);
    out["fraction"] = KvValue(1.5);
    out["nan"] = KvValue(NaN);
    out["inf"] = KvValue(Infinity);
    out["neginf"] = KvValue(-Infinity);
    out["big"] = KvValue(1e21);
    out["pow53"] = KvValue(9007199254740992.0);
    out["tiny"] = KvValue(-1e-7);
    out["int"] = KvValue(123);
    out["bigzero"] = KvValue(rt::BigInt(0));
    out["bigneg"] = KvValue(-rt::BigInt(2).pow(rt::BigInt(120)));
    out["bigu64"] = KvValue(rt::BigInt::fromUint64(18446744073709551615ULL));
    out["sparse"] = KvValue(rt::KvArray{rt::KvHole{}, KvValue("x"), rt::KvHole{}, KvValue()});
    out["object"] = KvValue(rt::jsonParse(R"({"b":1,"10":"ten","2":"two","a":{"":null},"__proto__":5})"));
    out["map"] = KvValue(rt::KvMap{{true, "yes"}, {1, KvValue::array({2})}, {"k", rt::KvMap{}}, {-0.0, "zero"}});
    out["set"] = KvValue(rt::KvSet{rt::BigInt(2), "a", NaN, rt::KvObject{}});
    out["dates"] = KvValue::array({rt::KvDate{1700000000000}, rt::KvDate{NaN}, rt::KvDate{-1}});
    out["regexps"] = KvValue::array({rt::KvRegExp{"a\\/b", "giu"}, rt::KvRegExp{"x\\ny", "dmsy"}, rt::KvRegExp{"a", "v"}});
    out["errors"] = KvValue::array({error("TypeError", "typed", "trace", KvValue(rt::BigInt(7))), error("Error", std::nullopt, std::nullopt),
                                     error("Custom", "m", "s"), error("EvalError", "e", std::nullopt)});
    out["views"] = KvValue::object({
        {"whole", rt::KvBytes{1, 2, 3}},
        {"part", view(rt::KvViewKind::Uint8Array, {1, 2, 3, 4}, 1, 2)},
        {"i16", view(rt::KvViewKind::Int16Array, {1, 2, 3, 4, 5, 6}, 2, 4)},
        {"f64", view(rt::KvViewKind::Float64Array, {0, 0, 0, 0, 0, 0, 240, 63}, 0, 8)},
        {"bu64", view(rt::KvViewKind::BigUint64Array, {1, 0, 0, 0, 0, 0, 0, 0, 9}, 0, 8)},
        {"dv", view(rt::KvViewKind::DataView, {1, 2, 3, 4, 5, 6, 7, 8}, 3, 4)},
        {"clamped", view(rt::KvViewKind::Uint8ClampedArray, {1, 2, 3, 4, 5}, 4, 1)},
        {"ab", rt::KvArrayBuffer{{9, 8}}},
        {"empty", rt::KvBytes{}},
        {"i8", view(rt::KvViewKind::Int8Array, {255}, 0, 1)},
        {"u16", view(rt::KvViewKind::Uint16Array, {1, 2}, 0, 2)},
        {"i32", view(rt::KvViewKind::Int32Array, {1, 2, 3, 4}, 0, 4)},
        {"u32", view(rt::KvViewKind::Uint32Array, {1, 2, 3, 4}, 0, 4)},
        {"f32", view(rt::KvViewKind::Float32Array, {0, 0, 192, 63}, 0, 4)},
        {"bi64", view(rt::KvViewKind::BigInt64Array, {255, 255, 255, 255, 255, 255, 255, 255}, 0, 8)},
    });
    out["kvu64"] = KvValue(rt::KvU64(5));
    out["nested kvu64"] = KvValue::object({{"a", KvValue::array({rt::KvU64(1)})}});
    out["ordinary"] = KvValue::object({{"text", std::string("\xED\xA0\xBD")},
                                       {"values", KvValue::array({KvValue(), -0.0, rt::BigInt(7), rt::KvDate{NaN}, rt::KvRegExp{"x", "gi"}})}});
    out["nested"] = KvValue::object({{"list", KvValue::array({KvValue::object({{"m", rt::KvMap{{"k", rt::KvSet{1}}}}})})}, {"empty", rt::KvArray{}}, {"obj", rt::KvObject{}}});
    return out;
  }

  struct EncodeVector {
    const char* name;
    const char* graph;    // JSON.stringify(encodeTypedValue(value))
    const char* message;  // the TypeError message when encoding throws
  };
  const EncodeVector ENCODE_VECTORS[] = {
      {"null", R"JSON({"root":{"type":"null","value":null},"nodes":[]})JSON", ""},
      {"undefined", R"JSON({"root":{"type":"undefined"},"nodes":[]})JSON", ""},
      {"true", R"JSON({"root":{"type":"boolean","value":true},"nodes":[]})JSON", ""},
      {"false", R"JSON({"root":{"type":"boolean","value":false},"nodes":[]})JSON", ""},
      {"text", R"JSON({"root":{"type":"string","value":"text"},"nodes":[]})JSON", ""},
      {"lone", R"JSON({"root":{"type":"string","value":"\ud83d"},"nodes":[]})JSON", ""},
      {"unicode", R"JSON({"root":{"type":"string","value":"é😀\u0000"},"nodes":[]})JSON", ""},
      {"zero", R"JSON({"root":{"type":"number","value":0},"nodes":[]})JSON", ""},
      {"negzero", R"JSON({"root":{"type":"number","value":"-0"},"nodes":[]})JSON", ""},
      {"fraction", R"JSON({"root":{"type":"number","value":1.5},"nodes":[]})JSON", ""},
      {"nan", R"JSON({"root":{"type":"number","value":"NaN"},"nodes":[]})JSON", ""},
      {"inf", R"JSON({"root":{"type":"number","value":"Infinity"},"nodes":[]})JSON", ""},
      {"neginf", R"JSON({"root":{"type":"number","value":"-Infinity"},"nodes":[]})JSON", ""},
      {"big", R"JSON({"root":{"type":"number","value":1e+21},"nodes":[]})JSON", ""},
      {"pow53", R"JSON({"root":{"type":"number","value":9007199254740992},"nodes":[]})JSON", ""},
      {"tiny", R"JSON({"root":{"type":"number","value":-1e-7},"nodes":[]})JSON", ""},
      {"int", R"JSON({"root":{"type":"number","value":123},"nodes":[]})JSON", ""},
      {"bigzero", R"JSON({"root":{"type":"bigint","value":"0"},"nodes":[]})JSON", ""},
      {"bigneg", R"JSON({"root":{"type":"bigint","value":"-1329227995784915872903807060280344576"},"nodes":[]})JSON", ""},
      {"bigu64", R"JSON({"root":{"type":"bigint","value":"18446744073709551615"},"nodes":[]})JSON", ""},
      {"sparse", R"JSON({"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":4,"entries":[["1",{"type":"string","value":"x"}],["3",{"type":"undefined"}]]}]})JSON", ""},
      {"object", R"JSON({"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[["2",{"type":"string","value":"two"}],["10",{"type":"string","value":"ten"}],["b",{"type":"number","value":1}],["a",{"type":"ref","ref":1}],["__proto__",{"type":"number","value":5}]]},{"type":"object","entries":[["",{"type":"null","value":null}]]}]})JSON", ""},
      {"map", R"JSON({"root":{"type":"ref","ref":0},"nodes":[{"type":"Map","entries":[[{"type":"boolean","value":true},{"type":"string","value":"yes"}],[{"type":"number","value":1},{"type":"ref","ref":1}],[{"type":"string","value":"k"},{"type":"ref","ref":2}],[{"type":"number","value":0},{"type":"string","value":"zero"}]]},{"type":"Array","length":1,"entries":[["0",{"type":"number","value":2}]]},{"type":"Map","entries":[]}]})JSON", ""},
      {"set", R"JSON({"root":{"type":"ref","ref":0},"nodes":[{"type":"Set","items":[{"type":"bigint","value":"2"},{"type":"string","value":"a"},{"type":"number","value":"NaN"},{"type":"ref","ref":1}]},{"type":"object","entries":[]}]})JSON", ""},
      {"dates", R"JSON({"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":3,"entries":[["0",{"type":"ref","ref":1}],["1",{"type":"ref","ref":2}],["2",{"type":"ref","ref":3}]]},{"type":"Date","value":"1700000000000"},{"type":"Date","value":"NaN"},{"type":"Date","value":"-1"}]})JSON", ""},
      {"regexps", R"JSON({"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":3,"entries":[["0",{"type":"ref","ref":1}],["1",{"type":"ref","ref":2}],["2",{"type":"ref","ref":3}]]},{"type":"RegExp","value":"a\\/b","items":[{"type":"string","value":"giu"}]},{"type":"RegExp","value":"x\\ny","items":[{"type":"string","value":"dmsy"}]},{"type":"RegExp","value":"a","items":[{"type":"string","value":"v"}]}]})JSON", ""},
      {"errors", R"JSON({"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":4,"entries":[["0",{"type":"ref","ref":1}],["1",{"type":"ref","ref":2}],["2",{"type":"ref","ref":3}],["3",{"type":"ref","ref":4}]]},{"type":"TypeError","entries":[["message",{"type":"string","value":"typed"}],["stack",{"type":"string","value":"trace"}],["cause",{"type":"bigint","value":"7"}]]},{"type":"Error","entries":[["message",{"type":"string","value":""}],["stack",{"type":"undefined"}]]},{"type":"Error","entries":[["message",{"type":"string","value":"m"}],["stack",{"type":"string","value":"s"}]]},{"type":"EvalError","entries":[["message",{"type":"string","value":"e"}],["stack",{"type":"undefined"}]]}]})JSON", ""},
      {"views", R"JSON({"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[["whole",{"type":"ref","ref":1}],["part",{"type":"ref","ref":3}],["i16",{"type":"ref","ref":5}],["f64",{"type":"ref","ref":7}],["bu64",{"type":"ref","ref":9}],["dv",{"type":"ref","ref":11}],["clamped",{"type":"ref","ref":13}],["ab",{"type":"ref","ref":15}],["empty",{"type":"ref","ref":16}],["i8",{"type":"ref","ref":18}],["u16",{"type":"ref","ref":20}],["i32",{"type":"ref","ref":22}],["u32",{"type":"ref","ref":24}],["f32",{"type":"ref","ref":26}],["bi64",{"type":"ref","ref":28}]]},{"type":"Uint8Array","buffer":{"type":"ref","ref":2},"offset":0,"length":3},{"type":"ArrayBuffer","value":"AQID"},{"type":"Uint8Array","buffer":{"type":"ref","ref":4},"offset":1,"length":2},{"type":"ArrayBuffer","value":"AQIDBA=="},{"type":"Int16Array","buffer":{"type":"ref","ref":6},"offset":2,"length":4},{"type":"ArrayBuffer","value":"AQIDBAUG"},{"type":"Float64Array","buffer":{"type":"ref","ref":8},"offset":0,"length":8},{"type":"ArrayBuffer","value":"AAAAAAAA8D8="},{"type":"BigUint64Array","buffer":{"type":"ref","ref":10},"offset":0,"length":8},{"type":"ArrayBuffer","value":"AQAAAAAAAAAJ"},{"type":"DataView","buffer":{"type":"ref","ref":12},"offset":3,"length":4},{"type":"ArrayBuffer","value":"AQIDBAUGBwg="},{"type":"Uint8ClampedArray","buffer":{"type":"ref","ref":14},"offset":4,"length":1},{"type":"ArrayBuffer","value":"AQIDBAU="},{"type":"ArrayBuffer","value":"CQg="},{"type":"Uint8Array","buffer":{"type":"ref","ref":17},"offset":0,"length":0},{"type":"ArrayBuffer","value":""},{"type":"Int8Array","buffer":{"type":"ref","ref":19},"offset":0,"length":1},{"type":"ArrayBuffer","value":"/w=="},{"type":"Uint16Array","buffer":{"type":"ref","ref":21},"offset":0,"length":2},{"type":"ArrayBuffer","value":"AQI="},{"type":"Int32Array","buffer":{"type":"ref","ref":23},"offset":0,"length":4},{"type":"ArrayBuffer","value":"AQIDBA=="},{"type":"Uint32Array","buffer":{"type":"ref","ref":25},"offset":0,"length":4},{"type":"ArrayBuffer","value":"AQIDBA=="},{"type":"Float32Array","buffer":{"type":"ref","ref":27},"offset":0,"length":4},{"type":"ArrayBuffer","value":"AADAPw=="},{"type":"BigInt64Array","buffer":{"type":"ref","ref":29},"offset":0,"length":8},{"type":"ArrayBuffer","value":"//////////8="}]})JSON", ""},
      {"kvu64", "", "Unsupported runtime class: KvU64"},
      {"nested kvu64", "", "Unsupported runtime class: KvU64"},
      {"ordinary", R"JSON({"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[["text",{"type":"string","value":"\ud83d"}],["values",{"type":"ref","ref":1}]]},{"type":"Array","length":5,"entries":[["0",{"type":"undefined"}],["1",{"type":"number","value":"-0"}],["2",{"type":"bigint","value":"7"}],["3",{"type":"ref","ref":2}],["4",{"type":"ref","ref":3}]]},{"type":"Date","value":"NaN"},{"type":"RegExp","value":"x","items":[{"type":"string","value":"gi"}]}]})JSON", ""},
      {"nested", R"JSON({"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[["list",{"type":"ref","ref":1}],["empty",{"type":"ref","ref":5}],["obj",{"type":"ref","ref":6}]]},{"type":"Array","length":1,"entries":[["0",{"type":"ref","ref":2}]]},{"type":"object","entries":[["m",{"type":"ref","ref":3}]]},{"type":"Map","entries":[[{"type":"string","value":"k"},{"type":"ref","ref":4}]]},{"type":"Set","items":[{"type":"number","value":1}]},{"type":"Array","length":0,"entries":[]},{"type":"object","entries":[]}]})JSON", ""},
  };

  // [name, graph, nativeCapture, { ok: JSON.stringify(encode(decode(graph))) } | { error, message }]
  const char* const DECODE_VECTORS = R"JSON([["graph null",null,false,{"error":"TypeError","message":"null is not an object (evaluating 'graph.root')"}],["graph string","x",false,{"error":"TypeError","message":"undefined is not an object (evaluating 'atom.type')"}],["graph empty",{},false,{"error":"TypeError","message":"undefined is not an object (evaluating 'atom.type')"}],["root null",{"root":null},false,{"error":"TypeError","message":"null is not an object (evaluating 'atom.type')"}],["root number",{"root":5},false,{"error":"TypeError","message":"Unsupported interchange atom: undefined"}],["root type number",{"root":{"type":5}},false,{"error":"TypeError","message":"Unsupported interchange atom: 5"}],["root type object",{"root":{"type":{}}},false,{"error":"TypeError","message":"Unsupported interchange atom: [object Object]"}],["root type array",{"root":{"type":["x","y"]}},false,{"error":"TypeError","message":"Unsupported interchange atom: x,y"}],["ref missing",{"root":{"type":"ref"}},false,{"error":"TypeError","message":"Incomplete typed-value interchange"}],["nodes missing",{"root":{"type":"ref","ref":0}},false,{"error":"TypeError","message":"undefined is not an object (evaluating 'graph.nodes.at')"}],["nodes null",{"root":{"type":"ref","ref":0},"nodes":null},false,{"error":"TypeError","message":"null is not an object (evaluating 'graph.nodes.at')"}],["nodes object",{"root":{"type":"ref","ref":0},"nodes":{}},false,{"error":"TypeError","message":"graph.nodes.at is not a function. (In 'graph.nodes.at(id)', 'graph.nodes.at' is undefined)"}],["nodes number",{"root":{"type":"ref","ref":0},"nodes":5},false,{"error":"TypeError","message":"graph.nodes.at is not a function. (In 'graph.nodes.at(id)', 'graph.nodes.at' is undefined)"}],["nodes string",{"root":{"type":"ref","ref":0},"nodes":"abc"},false,{"error":"TypeError","message":"Unsupported interchange node: undefined"}],["nodes string out",{"root":{"type":"ref","ref":7},"nodes":"abc"},false,{"error":"TypeError","message":"Incomplete typed-value interchange"}],["nodes empty",{"root":{"type":"ref","ref":0},"nodes":[]},false,{"error":"TypeError","message":"Incomplete typed-value interchange"}],["node null",{"root":{"type":"ref","ref":0},"nodes":[null]},false,{"error":"TypeError","message":"null is not an object (evaluating 'node.type')"}],["node number",{"root":{"type":"ref","ref":0},"nodes":[5]},false,{"error":"TypeError","message":"Unsupported interchange node: undefined"}],["node unknown",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Weird"}]},false,{"error":"TypeError","message":"Unsupported interchange node: Weird"}],["node type number",{"root":{"type":"ref","ref":0},"nodes":[{"type":7}]},false,{"error":"TypeError","message":"Unsupported interchange node: 7"}],["node type missing",{"root":{"type":"ref","ref":0},"nodes":[{}]},false,{"error":"TypeError","message":"Unsupported interchange node: undefined"}],["ref null",{"root":{"type":"ref","ref":null},"nodes":[{"type":"object"}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[]}]}}],["ref string",{"root":{"type":"ref","ref":"0"},"nodes":[{"type":"object"}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[]}]}}],["ref negative",{"root":{"type":"ref","ref":-1},"nodes":[{"type":"Set"},{"type":"object"}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[]}]}}],["ref fraction",{"root":{"type":"ref","ref":1.7},"nodes":[{"type":"Set"},{"type":"Map"}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Map","entries":[]}]}}],["ref true",{"root":{"type":"ref","ref":true},"nodes":[{"type":"Set"},{"type":"Map"}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Map","entries":[]}]}}],["ref object",{"root":{"type":"ref","ref":{}},"nodes":[{"type":"Set"}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Set","items":[]}]}}],["bigint bad",{"root":{"type":"bigint","value":"x"},"nodes":[]},false,{"error":"SyntaxError","message":"Failed to parse String to BigInt"}],["bigint spaced",{"root":{"type":"bigint","value":" 12 "},"nodes":[]},false,{"ok":{"root":{"type":"bigint","value":"12"},"nodes":[]}}],["bigint missing",{"root":{"type":"bigint"},"nodes":[]},false,{"error":"SyntaxError","message":"Failed to parse String to BigInt"}],["bigint number",{"root":{"type":"bigint","value":5},"nodes":[]},false,{"ok":{"root":{"type":"bigint","value":"5"},"nodes":[]}}],["bigint hex",{"root":{"type":"bigint","value":"0x1f"},"nodes":[]},false,{"ok":{"root":{"type":"bigint","value":"31"},"nodes":[]}}],["number text",{"root":{"type":"number","value":"abc"},"nodes":[]},false,{"ok":{"root":{"type":"number","value":"NaN"},"nodes":[]}}],["number missing",{"root":{"type":"number"},"nodes":[]},false,{"ok":{"root":{"type":"number","value":"NaN"},"nodes":[]}}],["number -0",{"root":{"type":"number","value":"-0"},"nodes":[]},false,{"ok":{"root":{"type":"number","value":"-0"},"nodes":[]}}],["number Infinity",{"root":{"type":"number","value":"-Infinity"},"nodes":[]},false,{"ok":{"root":{"type":"number","value":"-Infinity"},"nodes":[]}}],["number spaced",{"root":{"type":"number","value":" 0x10 "},"nodes":[]},false,{"ok":{"root":{"type":"number","value":16},"nodes":[]}}],["string missing",{"root":{"type":"string"},"nodes":[]},false,{"ok":{"root":{"type":"undefined"},"nodes":[]}}],["string number",{"root":{"type":"string","value":5},"nodes":[]},false,{"ok":{"root":{"type":"number","value":5},"nodes":[]}}],["boolean object",{"root":{"type":"boolean","value":{"a":[1,null]}},"nodes":[]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[["a",{"type":"ref","ref":1}]]},{"type":"Array","length":2,"entries":[["0",{"type":"number","value":1}],["1",{"type":"null","value":null}]]}]}}],["null with value",{"root":{"type":"null","value":5},"nodes":[]},false,{"ok":{"root":{"type":"null","value":null},"nodes":[]}}],["ab missing",{"root":{"type":"ref","ref":0},"nodes":[{"type":"ArrayBuffer"}]},false,{"error":"TypeError","message":"Incomplete typed-value interchange"}],["ab invalid",{"root":{"type":"ref","ref":0},"nodes":[{"type":"ArrayBuffer","value":"!!!"}]},false,{"error":"InvalidCharacterError","message":"The string contains invalid characters."}],["ab number",{"root":{"type":"ref","ref":0},"nodes":[{"type":"ArrayBuffer","value":5}]},false,{"error":"InvalidCharacterError","message":"The string contains invalid characters."}],["ab null",{"root":{"type":"ref","ref":0},"nodes":[{"type":"ArrayBuffer","value":null}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"ArrayBuffer","value":"null"}]}}],["ab spaced",{"root":{"type":"ref","ref":0},"nodes":[{"type":"ArrayBuffer","value":" AQ ID "}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"ArrayBuffer","value":"AQID"}]}}],["date text",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Date","value":"abc"}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Date","value":"NaN"}]}}],["date missing",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Date"}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Date","value":"NaN"}]}}],["date fraction",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Date","value":"1.7"}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Date","value":"1"}]}}],["date neg fraction",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Date","value":-1.7}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Date","value":"-1"}]}}],["date huge",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Date","value":"8.64e15"}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Date","value":"8640000000000000"}]}}],["date too huge",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Date","value":8640000000000001}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Date","value":"NaN"}]}}],["regexp missing",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp"}]},false,{"error":"TypeError","message":"Incomplete typed-value interchange"}],["regexp no items",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a"}]},false,{"error":"TypeError","message":"Incomplete typed-value interchange"}],["regexp items null",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a","items":null}]},false,{"error":"TypeError","message":"null is not an object (evaluating 'required(node.items)[0]')"}],["regexp items empty",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a","items":[]}]},false,{"error":"TypeError","message":"undefined is not an object (evaluating 'atom.type')"}],["regexp items string",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a","items":"gi"}]},false,{"error":"TypeError","message":"Unsupported interchange atom: undefined"}],["regexp items number",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a","items":5}]},false,{"error":"TypeError","message":"undefined is not an object (evaluating 'atom.type')"}],["regexp dup flags",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a","items":[{"type":"string","value":"gg"}]}]},false,{"error":"SyntaxError","message":"Invalid flags supplied to RegExp constructor."}],["regexp flags number",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a","items":[{"type":"string","value":5}]}]},false,{"error":"SyntaxError","message":"Invalid flags supplied to RegExp constructor."}],["regexp flags undefined atom",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a","items":[{"type":"undefined"}]}]},false,{"error":"SyntaxError","message":"Invalid flags supplied to RegExp constructor."}],["regexp bad pattern",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"(","items":[{"type":"string","value":""}]}]},false,{"error":"SyntaxError","message":"Invalid regular expression: missing )"}],["regexp pattern null",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":null,"items":[{"type":"string","value":"i"}]}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"null","items":[{"type":"string","value":"i"}]}]}}],["regexp pattern slash",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a/b\n","items":[{"type":"string","value":"ysmi"}]}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a\\/b\\n","items":[{"type":"string","value":"imsy"}]}]}}],["regexp flags array",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a","items":[{"type":"ref","ref":1}]},{"type":"Array","length":1,"entries":[["0",{"type":"string","value":"gi"}]]}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a","items":[{"type":"string","value":"gi"}]}]}}],["regexp flags empty set",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a","items":[{"type":"ref","ref":1}]},{"type":"Set"}]},false,{"error":"SyntaxError","message":"Invalid flags supplied to RegExp constructor."}],["regexp flags empty u8",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a","items":[{"type":"ref","ref":1}]},{"type":"Uint8Array","buffer":{"type":"ref","ref":2},"offset":0,"length":0},{"type":"ArrayBuffer","value":""}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a","items":[{"type":"string","value":""}]}]}}],["regexp flags bigint",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a","items":[{"type":"bigint","value":"7"}]}]},false,{"error":"SyntaxError","message":"Invalid flags supplied to RegExp constructor."}],["view no buffer",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array"}]},false,{"error":"TypeError","message":"Incomplete typed-value interchange"}],["view no length",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array","buffer":{"type":"ref","ref":1},"offset":0},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"error":"TypeError","message":"Incomplete typed-value interchange"}],["view beyond",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array","buffer":{"type":"ref","ref":1},"offset":1,"length":10},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"error":"RangeError","message":"Length out of range of buffer"}],["view misaligned",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Int32Array","buffer":{"type":"ref","ref":1},"offset":2,"length":0},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"error":"RangeError","message":"byteOffset modulo TypedArray.BYTES_PER_ELEMENT must be 0"}],["view partial element",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Int32Array","buffer":{"type":"ref","ref":1},"offset":0,"length":6},{"type":"ArrayBuffer","value":"AQIDBAUGBwg="}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Int32Array","buffer":{"type":"ref","ref":1},"offset":0,"length":4},{"type":"ArrayBuffer","value":"AQIDBAUGBwg="}]}}],["view negative offset",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array","buffer":{"type":"ref","ref":1},"offset":-1,"length":1},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"error":"RangeError","message":"byteOffset cannot be negative"}],["view negative length",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array","buffer":{"type":"ref","ref":1},"offset":0,"length":-1},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"error":"RangeError","message":"length cannot be negative"}],["view offset beyond",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array","buffer":{"type":"ref","ref":1},"offset":5,"length":0},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"error":"RangeError","message":"Length out of range of buffer"}],["view no offset",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint16Array","buffer":{"type":"ref","ref":1},"length":4},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint16Array","buffer":{"type":"ref","ref":1},"offset":0,"length":4},{"type":"ArrayBuffer","value":"AQIDBA=="}]}}],["view string offset",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint16Array","buffer":{"type":"ref","ref":1},"offset":"2","length":"2"},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint16Array","buffer":{"type":"ref","ref":1},"offset":2,"length":2},{"type":"ArrayBuffer","value":"AQIDBA=="}]}}],["view fraction",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array","buffer":{"type":"ref","ref":1},"offset":1.9,"length":2.9},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array","buffer":{"type":"ref","ref":1},"offset":1,"length":2},{"type":"ArrayBuffer","value":"AQIDBA=="}]}}],["view nan length",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array","buffer":{"type":"ref","ref":1},"offset":0,"length":"x"},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array","buffer":{"type":"ref","ref":1},"offset":0,"length":0},{"type":"ArrayBuffer","value":"AQIDBA=="}]}}],["view big",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Float64Array","buffer":{"type":"ref","ref":1},"offset":0,"length":8},{"type":"ArrayBuffer","value":"AAAAAAAA8D8="}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Float64Array","buffer":{"type":"ref","ref":1},"offset":0,"length":8},{"type":"ArrayBuffer","value":"AAAAAAAA8D8="}]}}],["bigint64 view",{"root":{"type":"ref","ref":0},"nodes":[{"type":"BigInt64Array","buffer":{"type":"ref","ref":1},"offset":0,"length":8},{"type":"ArrayBuffer","value":"//////////8="}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"BigInt64Array","buffer":{"type":"ref","ref":1},"offset":0,"length":8},{"type":"ArrayBuffer","value":"//////////8="}]}}],["dataview whole",{"root":{"type":"ref","ref":0},"nodes":[{"type":"DataView","buffer":{"type":"ref","ref":1}},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"DataView","buffer":{"type":"ref","ref":1},"offset":0,"length":4},{"type":"ArrayBuffer","value":"AQIDBA=="}]}}],["dataview offset only",{"root":{"type":"ref","ref":0},"nodes":[{"type":"DataView","buffer":{"type":"ref","ref":1},"offset":1},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"DataView","buffer":{"type":"ref","ref":1},"offset":1,"length":3},{"type":"ArrayBuffer","value":"AQIDBA=="}]}}],["dataview beyond",{"root":{"type":"ref","ref":0},"nodes":[{"type":"DataView","buffer":{"type":"ref","ref":1},"offset":10},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"error":"RangeError","message":"byteOffset exceeds source ArrayBuffer byteLength"}],["dataview length beyond",{"root":{"type":"ref","ref":0},"nodes":[{"type":"DataView","buffer":{"type":"ref","ref":1},"offset":1,"length":4},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"error":"RangeError","message":"Length out of range of buffer"}],["dataview negative",{"root":{"type":"ref","ref":0},"nodes":[{"type":"DataView","buffer":{"type":"ref","ref":1},"offset":-1},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"error":"RangeError","message":"byteOffset cannot be negative"}],["dataview string buffer",{"root":{"type":"ref","ref":0},"nodes":[{"type":"DataView","buffer":{"type":"string","value":"abc"},"offset":0,"length":1}]},false,{"error":"TypeError","message":"Expected ArrayBuffer for the first argument."}],["dataview object buffer",{"root":{"type":"ref","ref":0},"nodes":[{"type":"DataView","buffer":{"type":"ref","ref":1},"offset":0},{"type":"object"}]},false,{"error":"TypeError","message":"Expected ArrayBuffer for the first argument."}],["view string buffer",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array","buffer":{"type":"string","value":"abc"},"offset":0,"length":2}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array","buffer":{"type":"ref","ref":1},"offset":0,"length":0},{"type":"ArrayBuffer","value":""}]}}],["view object buffer",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array","buffer":{"type":"ref","ref":1},"offset":0,"length":2},{"type":"object"}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array","buffer":{"type":"ref","ref":1},"offset":0,"length":0},{"type":"ArrayBuffer","value":""}]}}],["array type key",{"root":{"type":"ref","ref":0},"nodes":[{"type":["Uint8Array"],"buffer":{"type":"ref","ref":1},"offset":0,"length":2},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array","buffer":{"type":"ref","ref":1},"offset":0,"length":2},{"type":"ArrayBuffer","value":"AQIDBA=="}]}}],["dataview type key",{"root":{"type":"ref","ref":0},"nodes":[{"type":["DataView"],"buffer":{"type":"ref","ref":1},"offset":1,"length":2},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"DataView","buffer":{"type":"ref","ref":1},"offset":1,"length":2},{"type":"ArrayBuffer","value":"AQIDBA=="}]}}],["error type key",{"root":{"type":"ref","ref":0},"nodes":[{"type":["TypeError"],"entries":[["message",{"type":"string","value":"m"}]]}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"TypeError","entries":[["message",{"type":"string","value":"m"}],["stack",{"type":"string","value":"TypeError: m\n    at allocateNode (/home/codex/repos/ubiquity/ai.ubq.fi/.codex-worktrees/cpp-gateway-port/src/kv-conversion/typed-value.ts:138:52)\n    at readNode (/home/codex/repos/ubiquity/ai.ubq.fi/.codex-worktrees/cpp-gateway-port/src/kv-conversion/typed-value.ts:185:19)\n    at /tmp/claude-1002/wave/w01c7/scratch/typed-malformed.ts:73:56"}]]}]}}],["array length string",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":"3"}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":1,"entries":[["0",{"type":"string","value":"3"}]]}]}}],["array length negative",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":-1}]},false,{"error":"RangeError","message":"Array length must be a positive integer of safe magnitude."}],["array length fraction",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":1.5}]},false,{"error":"RangeError","message":"Array length must be a positive integer of safe magnitude."}],["array length missing",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array"}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":1,"entries":[["0",{"type":"undefined"}]]}]}}],["array length null",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":null}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":1,"entries":[["0",{"type":"null","value":null}]]}]}}],["array entry length",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":2,"entries":[["length",{"type":"number","value":1}]]}]},false,{"error":"TypeError","message":"Attempting to change configurable attribute of unconfigurable property."}],["array entry named",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":2,"entries":[["foo",{"type":"string","value":"x"}]]}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":2,"entries":[["foo",{"type":"string","value":"x"}]]}]}}],["array entry numeric key",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":2,"entries":[[0,{"type":"string","value":"x"}]]}]},false,{"error":"TypeError","message":"Invalid interchange property key"}],["array entry beyond",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":1,"entries":[["3",{"type":"string","value":"x"}],["01",{"type":"string","value":"y"}]]}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":4,"entries":[["3",{"type":"string","value":"x"}],["01",{"type":"string","value":"y"}]]}]}}],["array entries holes",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":4,"entries":[["2",{"type":"string","value":"x"}],["0",{"type":"undefined"}]]}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Array","length":4,"entries":[["0",{"type":"undefined"}],["2",{"type":"string","value":"x"}]]}]}}],["object proto key",{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[["__proto__",{"type":"string","value":"x"}],["10",{"type":"string","value":"a"}],["2",{"type":"string","value":"b"}],["z",{"type":"string","value":"c"}],["2",{"type":"string","value":"d"}]]}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[["2",{"type":"string","value":"d"}],["10",{"type":"string","value":"a"}],["__proto__",{"type":"string","value":"x"}],["z",{"type":"string","value":"c"}]]}]}}],["object entries null",{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":null}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[]}]}}],["object entries number",{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":5}]},false,{"error":"TypeError","message":"number is not iterable"}],["object entries object",{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":{}}]},false,{"error":"TypeError","message":"{} is not iterable"}],["object entry number",{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[5]}]},false,{"error":"TypeError","message":"number is not iterable"}],["object entry null",{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[null]}]},false,{"error":"TypeError","message":"null is not an object (evaluating '[key, entry]')"}],["object entry object",{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[{}]}]},false,{"error":"TypeError","message":"{} is not iterable"}],["object entry string",{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":["ab"]}]},false,{"error":"TypeError","message":"Unsupported interchange atom: undefined"}],["object entry short",{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[["k"]]}]},false,{"error":"TypeError","message":"undefined is not an object (evaluating 'atom.type')"}],["object entries string",{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":"ab"}]},false,{"error":"TypeError","message":"undefined is not an object (evaluating 'atom.type')"}],["map entries",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Map","entries":[[{"type":"string","value":"k"},{"type":"number","value":1}],[{"type":"number","value":"-0"},{"type":"string","value":"z"}],[{"type":"number","value":0},{"type":"string","value":"y"}],[{"type":"ref","ref":1},{"type":"ref","ref":1}]]},{"type":"object"}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Map","entries":[[{"type":"string","value":"k"},{"type":"number","value":1}],[{"type":"number","value":0},{"type":"string","value":"y"}],[{"type":"ref","ref":1},{"type":"ref","ref":1}]]},{"type":"object","entries":[]}]}}],["map entries string",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Map","entries":"ab"}]},false,{"error":"TypeError","message":"Unsupported interchange atom: undefined"}],["set items",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Set","items":[{"type":"string","value":"a"},{"type":"string","value":"a"},{"type":"number","value":"NaN"},{"type":"number","value":"NaN"}]}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Set","items":[{"type":"string","value":"a"},{"type":"number","value":"NaN"}]}]}}],["set items string",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Set","items":"ab"}]},false,{"error":"TypeError","message":"Unsupported interchange atom: undefined"}],["set items object",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Set","items":{}}]},false,{"error":"TypeError","message":"{} is not iterable"}],["error full",{"root":{"type":"ref","ref":0},"nodes":[{"type":"TypeError","entries":[["message",{"type":"string","value":"typed"}],["stack",{"type":"string","value":"trace"}],["cause",{"type":"bigint","value":"7"}]]}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"TypeError","entries":[["message",{"type":"string","value":"typed"}],["stack",{"type":"string","value":"trace"}],["cause",{"type":"bigint","value":"7"}]]}]}}],["error unknown type",{"root":{"type":"ref","ref":0},"nodes":[{"type":"AggregateError","entries":[]}]},false,{"error":"TypeError","message":"Unsupported interchange node: AggregateError"}],["error no entries",{"root":{"type":"ref","ref":0},"nodes":[{"type":"RangeError"}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"RangeError","entries":[["message",{"type":"string","value":""}],["stack",{"type":"string","value":"RangeError\n    at allocateNode (/home/codex/repos/ubiquity/ai.ubq.fi/.codex-worktrees/cpp-gateway-port/src/kv-conversion/typed-value.ts:138:52)\n    at readNode (/home/codex/repos/ubiquity/ai.ubq.fi/.codex-worktrees/cpp-gateway-port/src/kv-conversion/typed-value.ts:185:19)\n    at /tmp/claude-1002/wave/w01c7/scratch/typed-malformed.ts:73:56"}]]}]}}],["error entry key number",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Error","entries":[[1,{"type":"string","value":"x"}]]}]},false,{"error":"TypeError","message":"Invalid interchange property key"}],["dag",{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[["a",{"type":"ref","ref":1}],["b",{"type":"ref","ref":1}]]},{"type":"Array","length":1,"entries":[["0",{"type":"string","value":"shared"}]]}]},false,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[["a",{"type":"ref","ref":1}],["b",{"type":"ref","ref":1}]]},{"type":"Array","length":1,"entries":[["0",{"type":"string","value":"shared"}]]}]}}],["buffer ordinary",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":0,"length":2},{"type":"ArrayBuffer","value":"AQIDBA=="}]},false,{"error":"TypeError","message":"Unsupported interchange node: Buffer"}],["buffer native",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":1,"length":2},{"type":"ArrayBuffer","value":"AQIDBA=="}]},true,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":1,"length":2},{"type":"ArrayBuffer","value":"AQIDBA=="}]}}],["buffer native no offset",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"length":2},{"type":"ArrayBuffer","value":"AQIDBA=="}]},true,{"error":"TypeError","message":"Incomplete typed-value interchange"}],["buffer native no length",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":1},{"type":"ArrayBuffer","value":"AQIDBA=="}]},true,{"error":"TypeError","message":"Incomplete typed-value interchange"}],["buffer native string buffer",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"string","value":"x"},"offset":0,"length":1}]},true,{"error":"TypeError","message":"Native Buffer capture requires an ArrayBuffer backing"}],["buffer native beyond",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":1,"length":9},{"type":"ArrayBuffer","value":"AQIDBA=="}]},true,{"error":"RangeError","message":"\"length\" is outside of buffer bounds"}],["buffer native offset beyond",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":9,"length":0},{"type":"ArrayBuffer","value":"AQIDBA=="}]},true,{"error":"RangeError","message":"\"offset\" is outside of buffer bounds"}],["buffer native negative length",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":1,"length":-2},{"type":"ArrayBuffer","value":"AQIDBA=="}]},true,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":1,"length":0},{"type":"ArrayBuffer","value":"AQIDBA=="}]}}],["buffer native nan offset",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":"x","length":2},{"type":"ArrayBuffer","value":"AQIDBA=="}]},true,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":0,"length":2},{"type":"ArrayBuffer","value":"AQIDBA=="}]}}],["buffer native fraction",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":1.5,"length":1.5},{"type":"ArrayBuffer","value":"AQIDBA=="}]},true,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":1,"length":1},{"type":"ArrayBuffer","value":"AQIDBA=="}]}}],["buffer native whole",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":0,"length":4},{"type":"ArrayBuffer","value":"AQIDBA=="}]},true,{"ok":{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":0,"length":4},{"type":"ArrayBuffer","value":"AQIDBA=="}]}}],["buffer native negative offset",{"root":{"type":"ref","ref":0},"nodes":[{"type":"Buffer","buffer":{"type":"ref","ref":1},"offset":-1,"length":1},{"type":"ArrayBuffer","value":"AQIDBA=="}]},true,{"error":"RangeError","message":"\"offset\" is outside of buffer bounds"}]])JSON";

  std::string replaceAll(std::string text, std::string_view from, std::string_view to) { return rt::replaceAll(text, from, to); }

  // How the C++ port differs on the recorded inputs: KvValue is a tree (shared references
  // become copies), has no Buffer brand, no named array properties or array views over
  // non-ArrayBuffer values, and no decode-site Error stack.
  std::optional<json::value> divergence(const std::string& name, const json::value& recorded) {
    const auto reencoded = [&](std::string text) { return std::optional<json::value>(json::object{{"ok", rt::jsonParse(text)}}); };
    const auto failure = [](std::string message) { return std::optional<json::value>(json::object{{"error", "TypeError"}, {"message", json::string(message)}}); };
    if (name == "regexp bad pattern") return json::object{{"error", "SyntaxError"}, {"message", "expecting ')'"}};
    if (name == "regexp dup flags" || name == "regexp flags number" || name == "regexp flags undefined atom" ||
        name == "regexp flags empty set" || name == "regexp flags bigint")
      return json::object{{"error", "SyntaxError"}, {"message", "invalid regular expression flags"}};
    if (name == "error type key") return reencoded(R"({"root":{"type":"ref","ref":0},"nodes":[{"type":"TypeError","entries":[["message",{"type":"string","value":"m"}],["stack",{"type":"undefined"}]]}]})");
    if (name == "error no entries") return reencoded(R"({"root":{"type":"ref","ref":0},"nodes":[{"type":"RangeError","entries":[["message",{"type":"string","value":""}],["stack",{"type":"undefined"}]]}]})");
    if (name == "array entry named") return failure("Typed-value interchange array property \"foo\" has no KvValue representation");
    if (name == "array entry beyond") return failure("Typed-value interchange array property \"01\" has no KvValue representation");
    if (name == "view string buffer" || name == "view object buffer")
      return failure("Typed-value interchange typed array over a value that is not an ArrayBuffer has no KvValue representation");
    if (name == "dag")
      return reencoded(R"({"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[["a",{"type":"ref","ref":1}],["b",{"type":"ref","ref":2}]]},{"type":"Array","length":1,"entries":[["0",{"type":"string","value":"shared"}]]},{"type":"Array","length":1,"entries":[["0",{"type":"string","value":"shared"}]]}]})");
    if (name == "map entries")
      return reencoded(R"({"root":{"type":"ref","ref":0},"nodes":[{"type":"Map","entries":[[{"type":"string","value":"k"},{"type":"number","value":1}],[{"type":"number","value":0},{"type":"string","value":"y"}],[{"type":"ref","ref":1},{"type":"ref","ref":2}]]},{"type":"object","entries":[]},{"type":"object","entries":[]}]})");
    if (rt::startsWith(name, "buffer native") && rt::hasOwn(recorded.as_object(), "ok"))
      return reencoded(replaceAll(rt::jsonStringify(recorded.as_object().at("ok")), "\"Buffer\"", "\"Uint8Array\""));
    return std::nullopt;
  }

  json::value outcome(const json::value& graph, bool native) {
    try {
      const KvValue value = native ? decodeNativeCaptureValue(graph) : decodeTypedValue(graph);
      return json::object{{"ok", native ? encodeNativeCaptureValue(value) : encodeTypedValue(value)}};
    } catch (const rt::JsError& failure) {
      return json::object{{"error", json::string(failure.name)}, {"message", json::string(failure.message)}};
    }
  }
}

TEST("differential: encodeTypedValue graphs match Bun") {
  const std::map<std::string, KvValue> table = values();
  for (const EncodeVector& vector : ENCODE_VECTORS) {
    const KvValue& value = table.at(vector.name);
    if (*vector.message) {
      EXPECT_THROWS_MESSAGE(encodeTypedValue(value), rt::TypeError, vector.message);
      EXPECT_THROWS_MESSAGE(encodeNativeCaptureValue(value), rt::TypeError, vector.message);
      continue;
    }
    const std::string typed = rt::jsonStringify(encodeTypedValue(value));
    const std::string native = rt::jsonStringify(encodeNativeCaptureValue(value));
    if (typed != vector.graph || native != vector.graph) {
      EXPECT_EQ(typed, std::string(vector.graph));
      EXPECT_EQ(native, std::string(vector.graph));
      std::fprintf(stderr, "      value: %s\n", vector.name);
    }
  }
}

TEST("differential: decodeTypedValue round-trips every recorded graph") {
  const std::map<std::string, KvValue> table = values();
  for (const EncodeVector& vector : ENCODE_VECTORS) {
    if (*vector.message) continue;
    const json::value graph = rt::jsonParse(vector.graph);
    const KvValue decoded = decodeTypedValue(graph);
    if (std::string_view(vector.name) == "errors") {
      // As in JavaScript: the inherited message decodes to an own "", an unknown name to Error.
      const KvValue expected = KvValue::array({error("TypeError", "typed", "trace", KvValue(rt::BigInt(7))), error("Error", "", std::nullopt),
                                               error("Error", "m", "s"), error("EvalError", "e", std::nullopt)});
      EXPECT(decoded == expected);
      EXPECT_EQ(rt::jsonStringify(encodeTypedValue(decoded)), std::string(vector.graph));
      continue;
    }
    if (!(decoded == table.at(vector.name)) || rt::jsonStringify(encodeTypedValue(decoded)) != vector.graph) {
      EXPECT(decoded == table.at(vector.name));
      EXPECT_EQ(rt::jsonStringify(encodeTypedValue(decoded)), std::string(vector.graph));
      std::fprintf(stderr, "      value: %s\n", vector.name);
    }
    EXPECT(decodeNativeCaptureValue(graph) == decoded);
  }
}

TEST("differential: decoding malformed and unusual graphs matches Bun") {
  const json::value vectors = rt::jsonParse(DECODE_VECTORS);
  for (const json::value& row : vectors.as_array()) {
    const json::array& vector = row.as_array();
    const std::string name(vector[0].as_string());
    if (name == "dag" || name == "map entries") {
      EXPECT_EQ(outcome(vector[1], vector[2].as_bool()), vector[3]);
      continue;
    }
    const json::value expected = divergence(name, vector[3]).value_or(vector[3]);
    const json::value actual = outcome(vector[1], vector[2].as_bool());
    if (!rt::deepEqual(actual, expected)) {
      EXPECT_EQ(actual, expected);
      std::fprintf(stderr, "      case: %s\n", name.c_str());
    }
  }
}

TEST("differential: deep nesting encodes and round-trips like Bun") {
  KvValue deep("leaf");
  for (int level = 0; level < 2000; ++level) deep = KvValue(rt::KvArray{deep});
  const json::value graph = encodeTypedValue(deep);
  EXPECT_EQ(rt::jsonStringify(graph).size(), std::size_t(142941));
  EXPECT(decodeTypedValue(graph) == deep);
}

// JavaScriptCore stops at its stack limit (Bun: decode between 8000 and 10000 levels, encode
// between 4000 and 8000); the port stops at 2000 levels, below where rt::KvValue's recursive
// copy overflows the C++ stack.
TEST("nesting up to 2000 levels encodes and decodes") {
  KvValue deep("leaf");
  for (int level = 0; level < 2000; ++level) {
    rt::KvArray array;
    array.push_back(std::move(deep));
    deep = KvValue(std::move(array));
  }
  const json::value graph = encodeTypedValue(deep);
  EXPECT(decodeTypedValue(graph) == deep);
  rt::KvArray array;
  array.push_back(std::move(deep));
  const KvValue deeper(std::move(array));
  EXPECT_THROWS_MESSAGE(encodeTypedValue(deeper), rt::RangeError, "Maximum call stack size exceeded.");
  json::value graphTooDeep = graph;
  json::array& nodes = graphTooDeep.as_object()["nodes"].as_array();
  nodes.insert(nodes.begin(), json::object{{"type", "Array"}, {"length", 1}, {"entries", json::array{json::array{"0", json::object{{"type", "ref"}, {"ref", 1}}}}}});
  for (std::size_t index = 1; index < nodes.size(); ++index) {
    json::object& node = nodes[index].as_object();
    for (json::value& entry : node["entries"].as_array()) {
      json::object& atom = entry.as_array()[1].as_object();
      if (atom["type"] == "ref") atom["ref"] = atom["ref"].as_int64() + 1;
    }
  }
  EXPECT_THROWS_MESSAGE(decodeTypedValue(graphTooDeep), rt::RangeError, "Maximum call stack size exceeded.");
}

TEST("object cycles are preserved and recursive allocation fails closed") {
  const json::value cycle = rt::jsonParse(R"({"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[["self",{"type":"ref","ref":0}]]}]})");
  const KvValue decoded = decodeTypedValue(cycle);
  EXPECT(decoded.identity() && decoded["self"].identity() == decoded.identity());
  EXPECT_EQ(encodeTypedValue(decoded), cycle);
  // A view whose backing is itself recurses without end in JavaScript as well.
  const json::value selfBacked = rt::jsonParse(R"({"root":{"type":"ref","ref":0},"nodes":[{"type":"Uint8Array","buffer":{"type":"ref","ref":0},"offset":0,"length":0}]})");
  EXPECT_THROWS_MESSAGE(decodeTypedValue(selfBacked), rt::RangeError, "Maximum call stack size exceeded.");
  json::array nodes;
  for (int index = 0; index < 20000; ++index)
    nodes.push_back(json::object{{"type", "Array"}, {"length", 1}, {"entries", json::array{json::array{"0", json::object{{"type", "ref"}, {"ref", index + 1}}}}}});
  nodes.push_back(json::object{{"type", "object"}});
  const json::value chain = json::object{{"root", json::object{{"type", "ref"}, {"ref", 0}}}, {"nodes", std::move(nodes)}};
  EXPECT_THROWS_MESSAGE(decodeTypedValue(chain), rt::RangeError, "Maximum call stack size exceeded.");
}

TEST("a node named by two keys decodes twice and its shared children stay readable") {
  // `1`/"1" make JavaScript allocate node 1 twice; its inner `ref 2` is then read a second time
  // and must still resolve to the finished node. Bun: {"a":{"c":{"d":4}},"b":{"c":{"d":4}}},
  // with a !== b and a.c === b.c (identity a KvValue cannot hold).
  const json::value graph = rt::jsonParse(
      R"({"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[["a",{"type":"ref","ref":1}],["b",{"type":"ref","ref":"1"}]]},)"
      R"({"type":"object","entries":[["c",{"type":"ref","ref":2}]]},{"type":"object","entries":[["d",{"type":"number","value":4}]]}]})");
  const KvValue expected = KvValue::object({{"a", KvValue::object({{"c", KvValue::object({{"d", 4}})}})}, {"b", KvValue::object({{"c", KvValue::object({{"d", 4}})}})}});
  EXPECT(decodeTypedValue(graph) == expected);
  const KvValue decoded = decodeTypedValue(graph);
  EXPECT(decoded["a"].identity() != decoded["b"].identity());
  EXPECT(decoded["a"]["c"].identity() && decoded["a"]["c"].identity() == decoded["b"]["c"].identity());
}

TEST("JSC and interchange preserve Bun's cyclic collections, sparse indices and shared buffers") {
  // Bun 1.4.0 node:v8 serialize and the TypeScript encodeTypedValue on its deserialized result.
  const std::string expected = R"JSON({"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":[["array",{"type":"ref","ref":1}],["map",{"type":"ref","ref":2}],["set",{"type":"ref","ref":3}],["view",{"type":"ref","ref":4}],["full",{"type":"ref","ref":6}],["repeated",{"type":"ref","ref":6}],["backing",{"type":"ref","ref":5}],["self",{"type":"ref","ref":0}]]},{"type":"Array","length":16777217,"entries":[["16777216",{"type":"ref","ref":0}]]},{"type":"Map","entries":[[{"type":"ref","ref":0},{"type":"ref","ref":1}],[{"type":"ref","ref":1},{"type":"ref","ref":3}]]},{"type":"Set","items":[{"type":"ref","ref":3},{"type":"ref","ref":2}]},{"type":"DataView","buffer":{"type":"ref","ref":5},"offset":1,"length":2},{"type":"ArrayBuffer","value":"AAEC/w=="},{"type":"Uint8Array","buffer":{"type":"ref","ref":5},"offset":0,"length":4}]})JSON";
  const rt::KvBytes bytes = rt::fromHex("0e000000020500008061727261790101000001000000011300ffffffff030000806d61701e1300130113011d1303130220ffffffff1fffffffff0300008073657413030400008076696577160001000000000000000200000000000000150400000000000000000102ff0400008066756c6c16020000000000000000040000000000000013040800008072657065617465641306070000806261636b696e6713040400008073656c661300ffffffff");
  const auto verify = [&](const KvValue& value) {
    const KvValue& array = value["array"];
    EXPECT_EQ(array.asArray().size(), std::size_t{16777217});
    EXPECT(array[16777216].identity() == value.identity());
    EXPECT(value["repeated"].identity() == value["full"].identity());
    ASSERT(value["view"].backingBuffer() && value["full"].backingBuffer());
    EXPECT(value["view"].backingBuffer()->identity() == value["backing"].identity());
    EXPECT(value["full"].backingBuffer()->identity() == value["backing"].identity());
    EXPECT_EQ(rt::jsonStringify(encodeTypedValue(value)), expected);
    EXPECT_EQ(rt::kvJscSerialize(value), bytes);
  };
  verify(rt::kvJscDeserialize(bytes));
  verify(decodeTypedValue(rt::jsonParse(expected)));

  // A copy of an interior handle owns the graph after the root goes out of scope.
  KvValue array;
  {
    const KvValue root = rt::kvJscDeserialize(bytes);
    array = root["array"];
  }
  const KvValue& owned = array;
  verify(owned[16777216]);
}

TEST("an interchange Error cause can reference its own node") {
  const auto graph = rt::jsonParse(R"JSON({"root":{"type":"ref","ref":0},"nodes":[{"type":"Error","entries":[["message",{"type":"string","value":"e"}],["stack",{"type":"undefined"}],["cause",{"type":"ref","ref":0}]]}]})JSON");
  rt::KvError error;
  {
    const KvValue value = decodeTypedValue(graph);
    EXPECT_EQ(encodeTypedValue(value), graph);
    EXPECT_THROWS_MESSAGE(rt::assertPortableKvValue(value), rt::TypeError, "Bun KV cannot preserve Error.cause; persist an explicit error record instead");
    error = value.as<rt::KvError>();
  }
  ASSERT(error.cause);
  EXPECT_EQ(encodeTypedValue(*error.cause), graph);
}

TEST("coercing a cyclic array while allocating a RegExp throws Bun's RangeError") {
  const auto graph = rt::jsonParse(
      R"JSON({"root":{"type":"ref","ref":0},"nodes":[{"type":"RegExp","value":"a","items":[{"type":"ref","ref":1}]},{"type":"Array","length":1,"entries":[["0",{"type":"ref","ref":1}]]}]})JSON");
  EXPECT_THROWS_MESSAGE(decodeTypedValue(graph), rt::RangeError, "Maximum call stack size exceeded.");
}
