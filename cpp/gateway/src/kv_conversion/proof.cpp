#include "uos/kv_conversion/proof.hpp"

#include "uos/kv_conversion/legacy_codec.hpp"
#include "uos/runtime/native_kv_bootstrap.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/storage/kv.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace uos::kv_conversion::proof {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    // ---- node:assert/strict as Bun 1.4 fails (rt has no node:assert) ------------------------------
    // Bun's lib/internal/assert/assertion_error.ts: createErrDiff over util.inspect(value,
    // { compact: false, sorted: true, depth: 1000 }) and its Myers line diff.

    struct AssertionError : rt::JsError {
      explicit AssertionError(std::string message_) : rt::JsError(std::move(message_), "AssertionError") {}
    };

    std::string upperHex(unsigned value, int digits) {
      static const char* const DIGITS = "0123456789ABCDEF";
      std::string out(static_cast<std::size_t>(digits), '0');
      for (int index = digits - 1; index >= 0; --index) {
        out[static_cast<std::size_t>(index)] = DIGITS[value & 0xF];
        value >>= 4;
      }
      return out;
    }

    // util.inspect's `meta` escape of one code unit.
    std::string meta(std::uint16_t point) {
      const std::string backslash(1, '\\');
      switch (point) {
        case 8: return backslash + "b";
        case 9: return backslash + "t";
        case 10: return backslash + "n";
        case 12: return backslash + "f";
        case 13: return backslash + "r";
        case 39: return backslash + "'";
        case 92: return backslash + backslash;
        default: return backslash + "x" + upperHex(point, 2);
      }
    }

    // util.inspect's strEscape: single quotes, else double quotes when the text has a single
    // quote and no double quote, else backticks when it has neither a backtick nor "${".
    std::string escapeInspectedString(std::string_view text) {
      int quote = 39;
      if (rt::includes(text, "'")) {
        if (!rt::includes(text, "\"")) quote = -1;
        else if (!rt::includes(text, "`") && !rt::includes(text, "${")) quote = -2;
      }
      const std::u16string units = rt::toUtf16(text);
      std::string body;
      std::u16string pending;
      const auto flush = [&] {
        body += rt::fromUtf16(pending);
        pending.clear();
      };
      for (std::size_t index = 0; index < units.size(); ++index) {
        const std::uint16_t point = units[index];
        if (point == quote || point == 92 || point < 32 || (point > 126 && point < 160)) {
          flush();
          body += meta(point);
        } else if (point >= 0xD800 && point <= 0xDFFF) {
          if (point <= 0xDBFF && index + 1 < units.size() && units[index + 1] >= 0xDC00 && units[index + 1] <= 0xDFFF) {
            pending.push_back(units[index]);
            pending.push_back(units[index + 1]);
            ++index;
            continue;
          }
          flush();
          body += std::string(1, '\\') + "u" + rt::toLowerCase(upperHex(point, 4));
        } else {
          pending.push_back(units[index]);
        }
      }
      flush();
      if (quote == -1) return "\"" + body + "\"";
      if (quote == -2) return "`" + body + "`";
      return "'" + body + "'";
    }

    // `typeof value === "object" && value !== null` for a stored value.
    bool isObject(const rt::KvValue& value) {
      return !(value.isUndefined() || value.isNull() || value.isBool() || value.isNumber() || value.isString() || value.isBigInt() || value.is<rt::KvHole>());
    }

    std::string formatNumber(double number) { return number == 0 && std::signbit(number) ? "-0" : rt::numberToString(number); }

    // reduceToSingleString with `compact: false`: every entry on its own line.
    std::string block(const std::string& open, std::vector<std::string> entries, const std::string& close, std::size_t indentation, bool sorted) {
      if (sorted) rt::sort(entries);
      const std::string newline = "\n" + std::string(indentation, ' ');
      std::string out = open + newline + "  ";
      for (std::size_t index = 0; index < entries.size(); ++index) {
        if (index > 0) out += "," + newline + "  ";
        out += entries[index];
      }
      return out + newline + close;
    }

    std::string propertyName(const std::string& key) {
      static const rt::Regex IDENTIFIER("^[a-zA-Z_][a-zA-Z_0-9]*$", "");
      return IDENTIFIER.test(key) ? key : escapeInspectedString(key);
    }

    template <class T> std::vector<std::string> typedElements(const rt::KvArrayBufferView& view) {
      std::vector<std::string> out;
      for (std::uint64_t offset = 0; offset + sizeof(T) <= view.byteLength; offset += sizeof(T)) {
        T element;
        std::memcpy(&element, view.buffer.data() + view.byteOffset + offset, sizeof(T));
        if constexpr (std::is_same_v<T, std::int64_t>) out.push_back(rt::BigInt(element).toString() + "n");
        else if constexpr (std::is_same_v<T, std::uint64_t>) out.push_back(rt::BigInt::fromUint64(element).toString() + "n");
        else out.push_back(formatNumber(static_cast<double>(element)));
      }
      return out;
    }

    std::string inspect(const rt::KvValue& value, std::size_t indentation);

    std::string arrayBufferContents(const rt::KvBytes& bytes, std::size_t indentation) {
      std::string contents = "[Uint8Contents]: <";
      for (std::size_t index = 0; index < bytes.size(); ++index) contents += (index ? " " : "") + rt::toLowerCase(upperHex(bytes[index], 2));
      contents += ">";
      return block("ArrayBuffer {", {contents, "byteLength: " + std::to_string(bytes.size())}, "}", indentation, true);
    }

    // util.inspect(value, { compact: false, sorted: true, depth: 1000, maxArrayLength: Infinity })
    // over the stored value model.
    std::string inspect(const rt::KvValue& value, std::size_t indentation) {
      const std::size_t inner = indentation + 2;
      if (value.isUndefined() || value.is<rt::KvHole>()) return "undefined";
      if (value.isNull()) return "null";
      if (value.isBool()) return value.asBool() ? "true" : "false";
      if (value.isNumber()) return formatNumber(value.asNumber());
      if (value.isString()) return escapeInspectedString(value.asString());
      if (value.isBigInt()) return value.asBigInt().toString() + "n";
      if (value.isU64()) return block("_KvU64 {", {"value: " + value.asU64().toBigInt().toString() + "n"}, "}", indentation, true);
      if (value.isBytes()) {
        const rt::KvBytes& bytes = value.asBytes();
        const std::string prefix = "Uint8Array(" + std::to_string(bytes.size()) + ") [";
        if (bytes.empty()) return prefix + "]";
        std::vector<std::string> elements;
        for (const std::uint8_t byte : bytes) elements.push_back(std::to_string(byte));
        return block(prefix, elements, "]", indentation, false);
      }
      if (value.is<rt::KvArrayBuffer>()) return arrayBufferContents(value.as<rt::KvArrayBuffer>().bytes, indentation);
      if (value.is<rt::KvArrayBufferView>()) {
        const rt::KvArrayBufferView& view = value.as<rt::KvArrayBufferView>();
        if (view.kind == rt::KvViewKind::DataView) {
          return block("DataView {",
                       {"byteLength: " + std::to_string(view.byteLength), "byteOffset: " + std::to_string(view.byteOffset), "buffer: " + arrayBufferContents(view.buffer, inner)},
                       "}", indentation, true);
        }
        std::vector<std::string> elements;
        switch (view.kind) {
          case rt::KvViewKind::Int8Array: elements = typedElements<std::int8_t>(view); break;
          case rt::KvViewKind::Uint8Array:
          case rt::KvViewKind::Uint8ClampedArray: elements = typedElements<std::uint8_t>(view); break;
          case rt::KvViewKind::Int16Array: elements = typedElements<std::int16_t>(view); break;
          case rt::KvViewKind::Uint16Array: elements = typedElements<std::uint16_t>(view); break;
          case rt::KvViewKind::Int32Array: elements = typedElements<std::int32_t>(view); break;
          case rt::KvViewKind::Uint32Array: elements = typedElements<std::uint32_t>(view); break;
          case rt::KvViewKind::Float32Array: elements = typedElements<float>(view); break;
          case rt::KvViewKind::Float64Array: elements = typedElements<double>(view); break;
          case rt::KvViewKind::BigInt64Array: elements = typedElements<std::int64_t>(view); break;
          case rt::KvViewKind::BigUint64Array: elements = typedElements<std::uint64_t>(view); break;
          case rt::KvViewKind::DataView: break;
        }
        const std::string prefix = std::string(rt::kvViewKindName(view.kind)) + "(" + std::to_string(elements.size()) + ") [";
        if (elements.empty()) return prefix + "]";
        return block(prefix, elements, "]", indentation, false);
      }
      if (value.is<rt::KvDate>()) {
        const rt::Date date(value.as<rt::KvDate>().time);
        return std::isnan(date.getTime()) ? std::string("Invalid Date") : date.toISOString();
      }
      if (value.is<rt::KvRegExp>()) {
        const rt::KvRegExp& regexp = value.as<rt::KvRegExp>();
        return "/" + regexp.source + "/" + regexp.flags;
      }
      if (value.is<rt::KvError>()) {
        const rt::KvError& error = value.as<rt::KvError>();
        if (error.stack && !error.stack->empty()) return *error.stack;
        const std::string message = error.message.value_or("");
        return "[" + (message.empty() ? error.name : error.name + ": " + message) + "]";
      }
      if (value.is<rt::KvMap>()) {
        const rt::KvMap& map = value.as<rt::KvMap>();
        const std::string prefix = "Map(" + std::to_string(map.size()) + ") {";
        if (map.empty()) return prefix + "}";
        std::vector<std::string> entries;
        for (const auto& [key, member] : map) entries.push_back(inspect(key, inner) + " => " + inspect(member, inner));
        return block(prefix, entries, "}", indentation, true);
      }
      if (value.is<rt::KvSet>()) {
        const rt::KvSet& set = value.as<rt::KvSet>();
        const std::string prefix = "Set(" + std::to_string(set.size()) + ") {";
        if (set.empty()) return prefix + "}";
        std::vector<std::string> entries;
        for (const rt::KvValue& member : set) entries.push_back(inspect(member, inner));
        return block(prefix, entries, "}", indentation, true);
      }
      if (value.isArray()) {
        const rt::KvArray& items = value.asArray();
        if (items.empty()) return "[]";
        std::vector<std::string> elements;
        std::size_t holes = 0;
        const auto flushHoles = [&] {
          if (holes > 0) elements.push_back("<" + std::to_string(holes) + " empty item" + (holes > 1 ? "s" : "") + ">");
          holes = 0;
        };
        for (const rt::KvValue& item : items) {
          if (item.is<rt::KvHole>()) {
            ++holes;
            continue;
          }
          flushHoles();
          elements.push_back(inspect(item, inner));
        }
        flushHoles();
        return block("[", elements, "]", indentation, false);
      }
      const rt::KvObject& object = value.asObject();
      if (object.empty()) return "{}";
      std::vector<std::string> entries;
      for (const auto& [key, member] : object) entries.push_back(propertyName(key) + ": " + inspect(member, inner));
      return block("{", entries, "}", indentation, true);
    }

    std::string inspectValue(const rt::KvValue& value) { return inspect(value, 0); }

    // Bun's myersDiff over lines, as the list its printMyersDiff walks from the end.
    struct DiffEntry {
      // 0 insert (actual), 1 delete (expected), 2 equal.
      int kind;
      std::string value;
    };

    bool linesEqual(const std::string& actual, const std::string& expected, bool checkCommaDisparity) {
      if (actual == expected) return true;
      if (checkCommaDisparity) return actual + "," == expected || actual == expected + ",";
      return false;
    }

    std::vector<DiffEntry> myersDiff(const std::vector<std::string>& actual, const std::vector<std::string>& expected, bool checkCommaDisparity) {
      const long actualLength = static_cast<long>(actual.size());
      const long expectedLength = static_cast<long>(expected.size());
      const long max = actualLength + expectedLength;
      const auto at = [max](const std::vector<long>& v, long index) { return index >= 0 && index <= 2 * max ? v[static_cast<std::size_t>(index)] : 0; };
      std::vector<long> v(static_cast<std::size_t>(2 * max + 1), 0);
      std::vector<std::vector<long>> trace;
      for (long diffLevel = 0; diffLevel <= max; ++diffLevel) {
        trace.push_back(v);
        for (long diagonalIndex = -diffLevel; diagonalIndex <= diffLevel; diagonalIndex += 2) {
          const long offset = diagonalIndex + max;
          long x = (diagonalIndex == -diffLevel || (diagonalIndex != diffLevel && at(v, offset - 1) < at(v, offset + 1))) ? at(v, offset + 1) : at(v, offset - 1) + 1;
          long y = x - diagonalIndex;
          while (x < actualLength && y < expectedLength && linesEqual(actual[static_cast<std::size_t>(x)], expected[static_cast<std::size_t>(y)], checkCommaDisparity)) {
            ++x;
            ++y;
          }
          v[static_cast<std::size_t>(offset)] = x;
          if (x >= actualLength && y >= expectedLength) {
            std::vector<DiffEntry> result;
            long bx = actualLength;
            long by = expectedLength;
            for (long level = static_cast<long>(trace.size()) - 1; level >= 0; --level) {
              const std::vector<long>& row = trace[static_cast<std::size_t>(level)];
              const long diagonal = bx - by;
              const long rowOffset = diagonal + max;
              const long previousDiagonal =
                (diagonal == -level || (diagonal != level && at(row, rowOffset - 1) < at(row, rowOffset + 1))) ? diagonal + 1 : diagonal - 1;
              const long previousX = at(row, previousDiagonal + max);
              const long previousY = previousX - previousDiagonal;
              while (bx > previousX && by > previousY) {
                const std::string& actualItem = actual[static_cast<std::size_t>(bx - 1)];
                result.push_back({2, checkCommaDisparity && !rt::endsWith(actualItem, ",") ? expected[static_cast<std::size_t>(by - 1)] : actualItem});
                --bx;
                --by;
              }
              if (level > 0) {
                if (bx > previousX) {
                  result.push_back({0, actual[static_cast<std::size_t>(bx - 1)]});
                  --bx;
                } else {
                  result.push_back({1, expected[static_cast<std::size_t>(by - 1)]});
                  --by;
                }
              }
            }
            return result;
          }
        }
      }
      return {};
    }

    constexpr int kNopLinesToCollapse = 5;

    // printMyersDiff: `message` starts with a newline; `skipped` when equal lines were elided.
    std::pair<std::string, bool> printMyersDiff(const std::vector<DiffEntry>& diff) {
      std::string message;
      bool skipped = false;
      int nopCount = 0;
      for (long index = static_cast<long>(diff.size()) - 1; index >= 0; --index) {
        const DiffEntry& entry = diff[static_cast<std::size_t>(index)];
        const bool hasPrevious = index < static_cast<long>(diff.size()) - 1;
        const int previousType = hasPrevious ? diff[static_cast<std::size_t>(index + 1)].kind : -1;
        if (hasPrevious && entry.kind != previousType && previousType == 2) {
          if (nopCount == kNopLinesToCollapse + 1) {
            message += "  " + diff[static_cast<std::size_t>(index + 1)].value + "\n";
          } else if (nopCount == kNopLinesToCollapse + 2) {
            message += "  " + diff[static_cast<std::size_t>(index + 2)].value + "\n";
            message += "  " + diff[static_cast<std::size_t>(index + 1)].value + "\n";
          }
          if (nopCount >= kNopLinesToCollapse + 3) {
            message += "...\n";
            message += "  " + diff[static_cast<std::size_t>(index + 1)].value + "\n";
            skipped = true;
          }
          nopCount = 0;
        }
        if (entry.kind == 0) message += "+ " + entry.value + "\n";
        else if (entry.kind == 1) message += "- " + entry.value + "\n";
        else {
          if (nopCount < kNopLinesToCollapse) message += "  " + entry.value + "\n";
          ++nopCount;
        }
      }
      return {"\n" + rt::trimEnd(message), skipped};
    }

    std::string readableOperator(const std::string& operatorName) {
      if (operatorName == "deepStrictEqual") return "Expected values to be strictly deep-equal:";
      if (operatorName == "strictEqualObject") return "Expected \"actual\" to be reference-equal to \"expected\":";
      if (operatorName == "notIdentical") return "Values have same structure but are not reference-equal:";
      return "Expected values to be strictly equal:";
    }

    // createErrDiff (no colors; stderr is not a terminal, so 80 columns).
    std::string createErrDiff(const rt::KvValue& actual, const rt::KvValue& expected, std::string operatorName, const std::optional<std::string>& customMessage) {
      if (operatorName == "strictEqual" && isObject(actual) && isObject(expected)) operatorName = "strictEqualObject";
      bool skipped = false;
      std::string message;
      const std::string inspectedActual = inspectValue(actual);
      const std::string inspectedExpected = inspectValue(expected);
      const std::vector<std::string> splitActual = rt::split(inspectedActual, "\n");
      const std::vector<std::string> splitExpected = rt::split(inspectedExpected, "\n");
      const bool simple = !(splitActual.size() > 1 || splitExpected.size() > 1) && (!isObject(actual) || !isObject(expected));
      std::string header = "+ actual - expected";
      if (simple) {
        const std::string& left = splitActual[0];
        const std::string& right = splitExpected[0];
        double stringsLength = static_cast<double>(rt::length(left) + rt::length(right));
        if (actual.isString()) stringsLength -= 2;
        if (expected.isString()) stringsLength -= 2;
        const auto isZero = [](const rt::KvValue& value) { return value.isNumber() && value.asNumber() == 0; };
        if (stringsLength <= 12 && !(isZero(actual) && isZero(expected))) {
          message = left + " !== " + right;
          header = "";
        } else {
          // getStackedDiff: both arguments are the inspected strings.
          message = "\n+ " + left + "\n- " + right;
          if (static_cast<double>(rt::length(left) + rt::length(right)) <= 80) {
            const std::u16string a = rt::toUtf16(left);
            const std::u16string b = rt::toUtf16(right);
            for (std::size_t index = 0; index < a.size(); ++index) {
              if (index >= b.size() || a[index] != b[index]) {
                if (index >= 3) message += "\n" + std::string(index + 2, ' ') + "^";
                break;
              }
            }
          }
        }
      } else if (inspectedActual == inspectedExpected) {
        operatorName = "notIdentical";
        if (splitActual.size() > 50) {
          std::string first;
          for (std::size_t index = 0; index < 50; ++index) first += (index ? "\n" : "") + splitActual[index];
          message = first + "\n...}";
          skipped = true;
        } else {
          message = inspectedActual;
        }
        header = "";
      } else {
        const auto [printed, elided] = printMyersDiff(myersDiff(splitActual, splitExpected, isObject(actual)));
        message = printed;
        skipped = elided;
      }
      const std::string title = customMessage && !customMessage->empty() ? *customMessage : readableOperator(operatorName);
      return title + "\n" + header + (skipped ? "\n... Skipped lines" : "") + "\n" + message + "\n";
    }

    // Object.is on stored values; objects compare by identity.
    bool objectIs(const rt::KvValue& actual, const rt::KvValue& expected) {
      if (isObject(actual) || isObject(expected)) return &actual == &expected;
      if (actual.variant().index() != expected.variant().index()) return false;
      if (actual.isNumber()) return rt::sameValue(actual.asNumber(), expected.asNumber());
      return actual == expected;
    }

    // assert.equal (strictEqual) of node:assert/strict.
    void strictEqual(const rt::KvValue& actual, const rt::KvValue& expected, std::optional<std::string> message = std::nullopt) {
      if (!objectIs(actual, expected)) throw AssertionError(createErrDiff(actual, expected, "strictEqual", message));
    }

    // assert.deepEqual (deepStrictEqual) of node:assert/strict.
    void deepStrictEqual(const rt::KvValue& actual, const rt::KvValue& expected) {
      if (!(actual == expected)) throw AssertionError(createErrDiff(actual, expected, "deepStrictEqual", std::nullopt));
    }

    // assert(value, message): Bun reads no source text, so a missing message is `<value> == true`.
    void ok(const rt::KvValue& value, std::optional<std::string> message = std::nullopt) {
      if (value.truthy()) return;
      throw AssertionError(message ? *message : inspectValue(value) + " == true");
    }

    // await assert.rejects(task[, regexp])
    template <class T> rt::Task<void> rejects(rt::Task<T> task, std::optional<rt::Regex> expected) {
      rt::Reason failure;
      try {
        co_await std::move(task);
      } catch (...) {
        failure = std::current_exception();
      }
      if (!failure) throw AssertionError("Missing expected rejection.");
      if (!expected) co_return;
      const std::string input = rt::reasonToString(failure);
      if (expected->test(input)) co_return;
      throw AssertionError("The input did not match the regular expression " + expected->toString() + ". Input:\n\n" + escapeInspectedString(input) + "\n");
    }

    // ---- the proof --------------------------------------------------------------------------------

    // The fixture.mjs and verify.mjs texts around their JSON.stringify interpolations.
    constexpr std::string_view FIXTURE_TEXT_0 = R"JS(const kv = await Deno.openKv()JS";
    constexpr std::string_view FIXTURE_TEXT_1 = R"JS();
const shared = new Uint8Array([0,1,2,255]).buffer;
const cyclic = { missing: undefined, big: -(2n ** 120n), bytes: new Uint8Array(shared,1,2), view: new DataView(shared,2,1), shared, when: new Date(1700000000000), map: new Map([[2n,new Set([undefined,7n])]]), regexp: /a\/b/giu, error: new TypeError('synthetic') }; cyclic.self=cyclic;
await kv.set(['typed',new Uint8Array([0,255]),-2n,-0,false],cyclic,{expireIn:3600000});
await kv.set(['typed',new Uint8Array([0,255]),-2n,0,false],{separate_zero:true});
await kv.set(['counter'],new Deno.KvU64(42n));
await kv.set(['raw'],new Uint8Array([8,0,255]));
const prefix=['uos_ai','sentinel_replay','v1','accounting'];
for(let i=1;i<=3;i++) await kv.set([...prefix,i,'capture-'+i],{state:'reserved',bytes:i});
const iterator=kv.list({prefix},{limit:1}); await iterator.next();
await kv.set(['uos_ai','sentinel_replay','v1','reap_cursor'],iterator.cursor);
await kv.set(['uos_ai','sentinel_replay','v1','budget'],{bootstrap_cursor:'0|'+iterator.cursor,stored_bytes:123,records:7});
await kv.set(['uos_ai','paid_fallback','v3','rollup_backfill_cursor'],{request_key:['paid',9n,new Uint8Array([4])]});
kv.close();
const unsupported = await Deno.openKv()JS";
    constexpr std::string_view FIXTURE_TEXT_2 = R"JS(); await unsupported.set(['synthetic-error'],new Error('unsupported',{cause:7n})); unsupported.close();
const negative = await Deno.openKv()JS";
    constexpr std::string_view FIXTURE_TEXT_3 = R"JS(); const bits=new DataView(new ArrayBuffer(8)); bits.setBigUint64(0,0xfff8000000000000n,false); await negative.set(['nan',bits.getFloat64(0,false)],'blocked'); negative.close();)JS";
    constexpr std::string_view VERIFY_TEXT_0 = R"JS(const kv=await Deno.openKv()JS";
    constexpr std::string_view VERIFY_TEXT_1 = R"JS();
const latest=await kv.get(['bun-latest']); if(latest.versionstamp!==)JS";
    constexpr std::string_view VERIFY_TEXT_2 = R"JS( || latest.value.big!==77n || latest.value.missing!==undefined || latest.value.bytes.join(',')!=='3,2,1') throw new Error('latest state lost');
const count=await kv.get(['counter']); if(!(count.value instanceof Deno.KvU64)||count.value.value!==49n) throw new Error('counter lost');
const ledger=await kv.get(['uos_ai','sentinel_replay','v1','budget']); if(ledger.value.stored_bytes!==456 || ledger.value.records!==9) throw new Error('ledger changed');
const cursor=await kv.get(['uos_ai','sentinel_replay','v1','reap_cursor']); const iterator=kv.list({prefix:['uos_ai','sentinel_replay','v1','accounting']},{cursor:cursor.value}); const remaining=[]; for await(const entry of iterator) remaining.push(entry.key[4]); if(remaining.length!==1 || remaining[0]!==3) throw new Error('cursor continuation changed');
const boot=kv.list({prefix:['uos_ai','sentinel_replay','v1','accounting']},{cursor:ledger.value.bootstrap_cursor.split('|')[1]}); if((await boot.next()).value.key[4]!==3) throw new Error('bootstrap continuation changed');
const typed=await kv.get(['typed',new Uint8Array([0,255]),-2n,-0,false]); if(typed.value.self!==typed.value || typed.value.big!==-(2n**120n)) throw new Error('typed state lost');
const paid=await kv.get(['uos_ai','paid_fallback','v3','rollup_backfill_cursor']); if(paid.value.request_key[1]!==9n) throw new Error('paid key cursor changed'); kv.close(); console.log('Deno latest-state runtime acceptance passed');)JS";

    rt::Task<std::string> run(std::vector<std::string> args, std::string directory) {
      rt::process::SpawnOptions options;
      options.cwd = directory;
      options.env = std::vector<std::pair<std::string, std::string>>{};
      options.stdout = rt::process::Stdio::pipe;
      options.stderr = rt::process::Stdio::pipe;
      const rt::process::Subprocess child = rt::process::spawn(args, options);
      rt::Response stdoutResponse(rt::BodyInit(child.stdout()));
      rt::Response stderrResponse(rt::BodyInit(child.stderr()));
      rt::Promise<std::string> stdoutText(stdoutResponse.text());
      rt::Promise<std::string> stderrText(stderrResponse.text());
      rt::Promise<int> exited = child.exited();
      auto outputs = rt::all(stdoutText, stderrText, exited);
      const auto outputValues = co_await outputs.wait();
      const auto& output = std::get<0>(outputValues);
      const auto& errors = std::get<1>(outputValues);
      const auto& code = std::get<2>(outputValues);
      strictEqual(rt::KvValue(code), rt::KvValue(0), errors);
      co_return output;
    }

    // Buffer.from(value) for a column value.
    std::vector<std::uint8_t> bufferFrom(const rt::sqlite::Value* value) {
      if (const rt::sqlite::Bytes* bytes = rt::sqlite::getBytes(value)) return *bytes;
      if (const std::string* text = rt::sqlite::getString(value)) return rt::encodeUtf8(*text);
      std::string received;
      if (value == nullptr) received = "undefined";
      else if (std::holds_alternative<std::nullptr_t>(*value)) received = "null";
      else if (const std::int64_t* integer = std::get_if<std::int64_t>(value)) received = "type bigint (" + std::to_string(*integer) + "n)";
      else received = "type number (" + rt::numberToString(std::get<double>(*value)) + ")";
      throw rt::CodedTypeError("The first argument must be of type string or an instance of Buffer, ArrayBuffer, or Array or an Array-like Object. Received " + received,
                               "ERR_INVALID_ARG_TYPE");
    }

    // A column value as the JavaScript value bun:sqlite returns.
    rt::KvValue columnValue(const rt::sqlite::Value& value) {
      if (std::holds_alternative<std::nullptr_t>(value)) return rt::KvValue(nullptr);
      if (const std::int64_t* integer = std::get_if<std::int64_t>(&value)) return rt::KvValue(rt::BigInt(*integer));
      if (const double* number = std::get_if<double>(&value)) return rt::KvValue(*number);
      if (const std::string* text = std::get_if<std::string>(&value)) return rt::KvValue(*text);
      return rt::KvValue(std::get<rt::sqlite::Bytes>(value));
    }

    rt::KvValue rawState(const std::string& path) {
      rt::sqlite::DatabaseOptions options;
      options.readonly = true;
      options.safeIntegers = true;
      const rt::sqlite::Database db(path, options);
      rt::KvArray rows;
      rt::Reason failure;
      try {
        for (const rt::sqlite::Row& row : db.query("SELECT k,v_encoding,version,seq,expiration_ms FROM kv ORDER BY k").all()) {
          rt::KvObject object;
          for (const auto& [name, value] : row) object.set(name, columnValue(value));
          object.set("k", rt::KvValue(rt::toHex(bufferFrom(row.get(std::string_view("k"))))));
          rows.emplace_back(std::move(object));
        }
      } catch (...) {
        failure = std::current_exception();
      }
      db.close(true);
      if (failure) std::rethrow_exception(failure);
      return rt::KvValue(std::move(rows));
    }

    rt::Task<std::string> digest(std::string path) {
      rt::Task<std::string> digesting = legacy_codec::fileDigest(std::move(path));
      std::string sha256 = co_await std::move(digesting);
      co_return sha256;
    }

    // `{ ...base, direction, source, destination, receipt[, signal] }`
    convert::ConvertOptions conversion(const std::string& deno, const std::string& pin, std::string direction, std::string source, std::string destination,
                                       std::string receipt) {
      return convert::ConvertOptions{
        .direction = std::move(direction),
        .source = std::move(source),
        .destination = std::move(destination),
        .receipt = std::move(receipt),
        .deno = deno,
        .denoSha256 = pin,
      };
    }

    // `.value?.value` of a KvU64 read: the counter's bigint, a plain object's member, else undefined.
    rt::KvValue counterValue(const rt::KvValue& value) {
      if (value.isU64()) return rt::KvValue(value.asU64().toBigInt());
      return value["value"];
    }

    rt::Task<void> writeLatestState(std::shared_ptr<rt::Kv> kv, std::shared_ptr<rt::KvValue> latestTyped, std::shared_ptr<std::string> latestVersion) {
      const rt::KvKey signedZero{"typed", rt::KvBytes{0, 255}, rt::BigInt(-2), -0.0, false};
      rt::Task<rt::KvEntryMaybe<>> gettingTyped = kv->get(signedZero);
      const rt::KvEntryMaybe<> entry = co_await std::move(gettingTyped);
      const rt::KvValue& typed = entry.value;
      ok(typed, "Patched native codec must reach the imported signed-zero key");
      strictEqual(typed["big"], rt::KvValue(-rt::BigInt(2).pow(rt::BigInt(120))));
      strictEqual(typed["self"], typed);
      const rt::KvKey positiveZero{"typed", rt::KvBytes{0, 255}, rt::BigInt(-2), 0.0, false};
      rt::Task<rt::KvEntryMaybe<>> gettingZero = kv->get(positiveZero);
      const rt::KvEntryMaybe<> zero = co_await std::move(gettingZero);
      strictEqual(zero.value["separate_zero"], rt::KvValue(true));
      const rt::KvKey counter{"counter"};
      rt::Task<rt::KvEntryMaybe<>> gettingCounter = kv->get(counter);
      const rt::KvEntryMaybe<> counted = co_await std::move(gettingCounter);
      strictEqual(counterValue(counted.value), rt::KvValue(rt::BigInt(42)));
      const std::unique_ptr<rt::AtomicOperation> summing = kv->atomic();
      summing->sum(counter, rt::BigInt(7));
      rt::Task<rt::KvCommitResult> committingSum = summing->commit();
      const rt::KvCommitResult summed = co_await std::move(committingSum);
      strictEqual(rt::KvValue(summed.ok), rt::KvValue(true));
      const std::unique_ptr<rt::AtomicOperation> stale = kv->atomic();
      stale->check(rt::AtomicCheck(counter, std::string("00000000000000000000"))).set(counter, rt::KvValue(rt::KvU64(0)));
      rt::Task<rt::KvCommitResult> committingStale = stale->commit();
      const rt::KvCommitResult refused = co_await std::move(committingStale);
      strictEqual(rt::KvValue(refused.ok), rt::KvValue(false));
      const rt::KvValue latestValue =
        rt::KvValue::object({{"after_cutover", true}, {"big", rt::BigInt(77)}, {"bytes", rt::KvBytes{3, 2, 1}}, {"missing", rt::KvValue::undefined()}});
      const rt::KvKey latestKey{"bun-latest"};
      rt::Task<rt::KvCommitResult> settingLatest = kv->set(latestKey, latestValue);
      co_await std::move(settingLatest);
      rt::KvListSelector selector;
      selector.prefix = rt::KvKey{"uos_ai", "sentinel_replay", "v1", "accounting"};
      rt::KvListOptions listOptions;
      listOptions.limit = 2;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector, listOptions);
      rt::Task<std::optional<rt::KvEntry<>>> first = iterator->next();
      co_await std::move(first);
      rt::Task<std::optional<rt::KvEntry<>>> second = iterator->next();
      co_await std::move(second);
      const rt::KvKey reapCursorKey{"uos_ai", "sentinel_replay", "v1", "reap_cursor"};
      const rt::KvValue reapCursor(iterator->cursor());
      rt::Task<rt::KvCommitResult> settingCursor = kv->set(reapCursorKey, reapCursor);
      co_await std::move(settingCursor);
      const rt::KvKey budgetKey{"uos_ai", "sentinel_replay", "v1", "budget"};
      const rt::KvValue budget = rt::KvValue::object({{"bootstrap_cursor", "0|" + iterator->cursor()}, {"stored_bytes", 456}, {"records", 9}});
      rt::Task<rt::KvCommitResult> settingBudget = kv->set(budgetKey, budget);
      co_await std::move(settingBudget);
      rt::Task<rt::KvEntryMaybe<>> gettingLatest = kv->get(latestKey);
      const rt::KvEntryMaybe<> latest = co_await std::move(gettingLatest);
      *latestTyped = latest.value;
      ok(rt::KvValue(latest.versionstamp.has_value()));
      *latestVersion = *latest.versionstamp;
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const LegacyRoundtripProof& value) {
    out = Writer()
            .required("forward", value.forward)
            .required("reverse", value.reverse)
            .required("deno_acceptance", value.deno_acceptance)
            .required("latest_typed", value.latest_typed)
            .required("interruption", value.interruption)
            .required("retained_fixture", value.retained_fixture)
            .done();
  }

  LegacyRoundtripProof tag_invoke(const json::value_to_tag<LegacyRoundtripProof>&, const json::value& value) {
    const Reader in(value, "LegacyRoundtripProof");
    return {
      .forward = in.required<convert::ConversionReceipt>("forward"),
      .reverse = in.required<convert::ConversionReceipt>("reverse"),
      .deno_acceptance = in.required<std::string>("deno_acceptance"),
      .latest_typed = in.required<json::value>("latest_typed"),
      .interruption = in.required<std::string>("interruption"),
      .retained_fixture = in.required<std::string>("retained_fixture"),
    };
  }

  rt::Task<LegacyRoundtripProof> proveLegacyRoundtrip(std::string directory, std::string deno, std::string denoSha256) {
    const std::vector<std::uint8_t> expectedPin = rt::fromHex(denoSha256);
    strictEqual(rt::KvValue(static_cast<double>(expectedPin.size())), rt::KvValue(32), "The explicit legacy codec pin must be SHA-256");
    rt::Promise<void> preparing = runtime::native_kv_bootstrap::prepareNativeKvForDevelopment();
    co_await preparing.wait();
    const std::string pin = rt::toHex(expectedPin);
    rt::Task<std::string> digestingDeno = digest(deno);
    const std::string denoDigest = co_await std::move(digestingDeno);
    strictEqual(rt::KvValue(denoDigest), rt::KvValue(pin), "Legacy codec executable does not match the explicit SHA-256 pin");
    const std::string root = rt::path::resolve(directory);
    if (rt::fs::existsSync(root)) throw rt::JsError("Proof directory must be NEW and disposable");
    rt::fs::MkdirOptions privateDirectory;
    privateDirectory.recursive = true;
    privateDirectory.mode = 0700;
    rt::fs::mkdirSync(root, privateDirectory);
    const std::string legacy = rt::path::join(root, "legacy.sqlite3");
    const std::string candidate = rt::path::join(root, "candidate.sqlite3");
    const std::string rollback = rt::path::join(root, "rollback.sqlite3");
    const std::string fixture = rt::path::join(root, "fixture.mjs");
    const std::string sourceText = std::string(FIXTURE_TEXT_0) + rt::jsonQuote(legacy) + std::string(FIXTURE_TEXT_1) +
                                   rt::jsonQuote(rt::path::join(root, "unsupported.sqlite3")) + std::string(FIXTURE_TEXT_2) +
                                   rt::jsonQuote(rt::path::join(root, "negative-nan.sqlite3")) + std::string(FIXTURE_TEXT_3);
    rt::fs::WriteFileOptions ownerOnly;
    ownerOnly.mode = 0600;
    rt::fs::writeFileSync(fixture, sourceText, ownerOnly);
    const std::vector<std::string> permissions{"--allow-read=" + root, "--allow-write=" + root};
    std::vector<std::string> fixtureCommand{deno, "run", "--no-config", "--no-lock", "--no-prompt", "--unstable-kv"};
    fixtureCommand.insert(fixtureCommand.end(), permissions.begin(), permissions.end());
    fixtureCommand.push_back(fixture);
    rt::Task<std::string> writingFixture = run(fixtureCommand, root);
    co_await std::move(writingFixture);
    rt::Task<std::string> digestingLegacy = digest(legacy);
    const std::string originalHash = co_await std::move(digestingLegacy);
    const rt::KvValue originalRows = rawState(legacy);

    rt::Task<convert::ConversionReceipt> converting =
      convert::convertKvDatabase(conversion(deno, pin, "deno-to-bun", legacy, candidate, rt::path::join(root, "forward.json")));
    const convert::ConversionReceipt forward = co_await std::move(converting);
    rt::Task<std::string> digestingLegacyAfter = digest(legacy);
    const std::string legacyAfter = co_await std::move(digestingLegacyAfter);
    strictEqual(rt::KvValue(legacyAfter), rt::KvValue(originalHash));
    deepStrictEqual(rawState(candidate), originalRows);
    strictEqual(rt::KvValue(forward.transcoded_cursors), rt::KvValue(2));

    rt::Task<std::shared_ptr<rt::Kv>> opening = storage::kv::openKv(candidate);
    const std::shared_ptr<rt::Kv> kv = co_await std::move(opening);
    const auto latestTyped = std::make_shared<rt::KvValue>();
    const auto latestVersion = std::make_shared<std::string>();
    rt::Reason failure;
    try {
      rt::Task<void> writing = writeLatestState(kv, latestTyped, latestVersion);
      co_await std::move(writing);
    } catch (...) {
      failure = std::current_exception();
    }
    kv->close();
    if (failure) std::rethrow_exception(failure);

    const rt::KvValue latestRows = rawState(candidate);
    rt::Task<std::string> digestingCandidate = digest(candidate);
    const std::string candidateHash = co_await std::move(digestingCandidate);
    rt::Task<convert::ConversionReceipt> reversing =
      convert::convertKvDatabase(conversion(deno, pin, "bun-to-deno", candidate, rollback, rt::path::join(root, "reverse.json")));
    const convert::ConversionReceipt reverse = co_await std::move(reversing);
    rt::Task<std::string> digestingCandidateAfter = digest(candidate);
    const std::string candidateAfter = co_await std::move(digestingCandidateAfter);
    strictEqual(rt::KvValue(candidateAfter), rt::KvValue(candidateHash));
    deepStrictEqual(rawState(rollback), latestRows);
    const std::string verify = rt::path::join(root, "verify.mjs");
    rt::fs::writeFileSync(verify, std::string(VERIFY_TEXT_0) + rt::jsonQuote(rollback) + std::string(VERIFY_TEXT_1) + rt::jsonQuote(*latestVersion) + std::string(VERIFY_TEXT_2),
                          ownerOnly);
    std::vector<std::string> verifyCommand{deno, "run", "--no-config", "--no-lock", "--no-prompt", "--unstable-kv"};
    verifyCommand.insert(verifyCommand.end(), permissions.begin(), permissions.end());
    verifyCommand.push_back(verify);
    rt::Task<std::string> verifying = run(verifyCommand, root);
    const std::string denoAcceptance = co_await std::move(verifying);

    const std::string interruptedDestination = rt::path::join(root, "interrupted.sqlite3");
    const rt::AbortController abort;
    const rt::Timeout interruptSoon = rt::setTimeout([abort] { abort.abort(rt::makeReason(rt::JsError("synthetic interruption"))); }, 1);
    convert::ConvertOptions interruptedOptions =
      conversion(deno, pin, "bun-to-deno", candidate, interruptedDestination, rt::path::join(root, "interrupted.json"));
    interruptedOptions.signal = abort.signal();
    rt::Task<void> interrupted = rejects(convert::convertKvDatabase(std::move(interruptedOptions)), std::nullopt);
    co_await std::move(interrupted);
    strictEqual(rt::KvValue(rt::fs::existsSync(interruptedDestination)), rt::KvValue(false));
    strictEqual(rt::KvValue(rt::fs::existsSync(rt::path::join(root, "interrupted.json"))), rt::KvValue(false));
    rt::Task<std::string> digestingCandidateFinal = digest(candidate);
    const std::string candidateFinal = co_await std::move(digestingCandidateFinal);
    strictEqual(rt::KvValue(candidateFinal), rt::KvValue(candidateHash));
    const std::vector<std::pair<std::string, std::string>> refusals{{"unsupported", "Error.cause"}, {"negative-nan", "Negative NaN"}};
    for (const auto& [name, reason] : refusals) {
      const std::string source = rt::path::join(root, name + ".sqlite3");
      rt::Task<std::string> digestingSource = digest(source);
      const std::string hash = co_await std::move(digestingSource);
      const std::string destination = rt::path::join(root, name + "-rejected.sqlite3");
      const std::string receipt = rt::path::join(root, name + "-rejected.json");
      std::optional<rt::Regex> expected;
      expected.emplace(reason, "");
      rt::Task<void> refusing = rejects(convert::convertKvDatabase(conversion(deno, pin, "deno-to-bun", source, destination, receipt)), std::move(expected));
      co_await std::move(refusing);
      rt::Task<std::string> digestingSourceAfter = digest(source);
      const std::string sourceAfter = co_await std::move(digestingSourceAfter);
      strictEqual(rt::KvValue(sourceAfter), rt::KvValue(hash));
      strictEqual(rt::KvValue(rt::fs::existsSync(destination)), rt::KvValue(false));
      strictEqual(rt::KvValue(rt::fs::existsSync(receipt)), rt::KvValue(false));
    }
    LegacyRoundtripProof result{
      .forward = forward,
      .reverse = reverse,
      .deno_acceptance = rt::trim(denoAcceptance),
      .latest_typed = typed_value::encodeTypedValue(*latestTyped),
      .interruption = "no publication; source unchanged",
      .retained_fixture = root,
    };
    rt::fs::writeFileSync(rt::path::join(root, "proof.json"), rt::jsonStringify(json::value_from(result), 2) + "\n", ownerOnly);
    co_return result;
  }

  rt::Task<void> importMetaMain() {
    const std::vector<std::string> argv = rt::process::argv();
    const auto argument = [&argv](std::size_t index) { return index < argv.size() ? argv[index] : std::string(); };
    const std::string directory = argument(2);
    const std::string deno = argument(3);
    const std::string sha = argument(4);
    if (directory.empty() || deno.empty() || sha.empty())
      throw rt::JsError("Usage: uos-kv-proof --runtime-manifest <absolute-path> <NEW-disposable-directory> <pinned-deno> <sha256>");
    rt::Task<LegacyRoundtripProof> proving = proveLegacyRoundtrip(directory, deno, sha);
    const LegacyRoundtripProof result = co_await std::move(proving);
    json::object summary;
    summary["retained_fixture"] = result.retained_fixture;
    summary["deno_acceptance"] = result.deno_acceptance;
    summary["rows"] = rt::makeNumber(result.reverse.destination.row_count);
    summary["interruption"] = result.interruption;
    rt::console::log(rt::jsonStringify(json::value(std::move(summary))));
  }
}
