#include "uos/kv_conversion/typed_value.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "../rt/kv_internal.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uos::kv_conversion::typed_value {
  namespace {
    using rt::KvValue;
    using rt::kv_detail::ValueGraph;

    // `ARRAY_TYPES`: the constructors by name, with BYTES_PER_ELEMENT (Bun gives DataView 1).
    struct ArrayType {
      std::string_view name;
      rt::KvViewKind kind;
      std::uint64_t bytesPerElement;
    };
    constexpr ArrayType ARRAY_TYPES[] = {
        {"Int8Array", rt::KvViewKind::Int8Array, 1},       {"Uint8Array", rt::KvViewKind::Uint8Array, 1},
        {"Uint8ClampedArray", rt::KvViewKind::Uint8ClampedArray, 1}, {"Int16Array", rt::KvViewKind::Int16Array, 2},
        {"Uint16Array", rt::KvViewKind::Uint16Array, 2},   {"Int32Array", rt::KvViewKind::Int32Array, 4},
        {"Uint32Array", rt::KvViewKind::Uint32Array, 4},   {"Float32Array", rt::KvViewKind::Float32Array, 4},
        {"Float64Array", rt::KvViewKind::Float64Array, 8}, {"BigInt64Array", rt::KvViewKind::BigInt64Array, 8},
        {"BigUint64Array", rt::KvViewKind::BigUint64Array, 8}, {"DataView", rt::KvViewKind::DataView, 1},
    };
    constexpr std::string_view errorTypes[] = {"Error", "EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", "URIError"};

    // `Object.hasOwn(ARRAY_TYPES, key)` / `ARRAY_TYPES[key]`
    const ArrayType* arrayType(std::string_view key) {
      for (const ArrayType& type : ARRAY_TYPES)
        if (type.name == key) return &type;
      return nullptr;
    }
    std::uint64_t bytesPerElement(rt::KvViewKind kind) {
      for (const ArrayType& type : ARRAY_TYPES)
        if (type.kind == kind) return type.bytesPerElement;
      return 1;
    }
    // `Object.hasOwn(errorTypes, key)`
    bool isErrorType(std::string_view key) {
      for (const std::string_view name : errorTypes)
        if (name == key) return true;
      return false;
    }

    constexpr std::size_t kMaxNesting = rt::kv_detail::MaxContainerDepth;

    const json::value& required(const json::value* value) {
      if (!value) throw rt::TypeError("Incomplete typed-value interchange");
      return *value;
    }

    std::string encodeBytes(const rt::KvBytes& value) { return rt::toBase64(value); }
    rt::KvBytes decodeBytes(const std::string& value) { return rt::atobBytes(value); }

    json::value atom(std::string_view type) {
      json::object out;
      out["type"] = json::string(type);
      return out;
    }
    json::value atom(std::string_view type, json::value value) {
      json::object out;
      out["type"] = json::string(type);
      out["value"] = std::move(value);
      return out;
    }
    json::value refAtom(std::size_t id) {
      json::object out;
      out["type"] = "ref";
      out["ref"] = rt::makeNumber(static_cast<std::int64_t>(id));
      return out;
    }

    // A hole read as a value is `undefined`.
    std::optional<json::value> primitiveAtom(const KvValue& item) {
      if (item.isNull()) return atom("null", nullptr);
      if (item.isUndefined() || item.is<rt::KvHole>()) return atom("undefined");
      if (item.isString()) return atom("string", json::string(item.asString()));
      if (item.isBool()) return atom("boolean", item.asBool());
      if (item.isBigInt()) return atom("bigint", json::string(item.asBigInt().toString()));
      if (item.isNumber()) {
        const double number = item.asNumber();
        json::value value = rt::Number::isFinite(number) ? rt::makeNumber(number) : json::value(json::string(rt::numberToString(number)));
        if (rt::sameValue(number, -0.0)) value = "-0";
        return atom("number", std::move(value));
      }
      return std::nullopt;
    }

    // `Buffer.isBuffer(item)`: KvValue has no Buffer brand (docs/rt/kv.md), so no stored value is one.
    bool isNodeBuffer(const KvValue&) { return false; }

    // `visit` with an explicit stack instead of recursion, so the nesting depth is bounded by
    // kMaxNesting and never by the C++ stack.
    class Encoder {
    public:
      explicit Encoder(bool nativeCapture) : nativeCapture_(nativeCapture) {}

      json::value visitRoot(const KvValue& value) {
        std::optional<json::value> result = visit(value);
        while (!frames_.empty()) {
          Frame& frame = frames_.back();
          if (result) {
            accept(frame, std::move(*result));
            result.reset();
          }
          if (const KvValue* child = nextChild(frame)) {
            result = visit(*child);
            continue;
          }
          node(frame.id)[frame.item->is<rt::KvSet>() ? "items" : "entries"] = std::move(frame.entries);
          result = refAtom(frame.id);
          frames_.pop_back();
        }
        return std::move(*result);
      }

      json::array nodes;

    private:
      // A container whose children are being visited.
      struct Frame {
        const KvValue* item = nullptr;
        std::size_t id = 0;
        std::size_t next = 0;  // the child being visited (Map: key then value; Error: message, stack, cause)
        json::array entries;   // `node.entries`, or `node.items` for a Set
        json::value pendingKey;
        KvValue message;  // Error: item.message (an own message, else the inherited "")
        KvValue stack;    // Error: item.stack
      };

      json::object& node(std::size_t id) { return nodes[id].as_object(); }

      // The atom of a primitive or of a node without children; std::nullopt after starting a
      // container, whose atom follows its children.
      std::optional<json::value> visit(const KvValue& item) {
        if (std::optional<json::value> primitive = primitiveAtom(item)) return primitive;
        const std::size_t id = nodes.size();
        if (const void* identity = item.identity()) {
          const auto [previous, added] = seen_.try_emplace(identity, id);
          if (!added) return refAtom(previous->second);
        }
        nodes.push_back(json::object{{"type", "object"}});
        if (writeSpecialNode(item, id)) return refAtom(id);
        writeContainerNode(item, id);
        return std::nullopt;
      }

      bool writeSpecialNode(const KvValue& item, std::size_t id) {
        const KvValue::Variant& variant = item.variant();
        if (const auto* buffer = std::get_if<rt::KvArrayBuffer>(&variant)) {
          node(id)["type"] = "ArrayBuffer";
          node(id)["value"] = json::string(encodeBytes(buffer->bytes));
        } else if (item.isBytes() || item.is<rt::KvArrayBufferView>()) {
          const auto* view = std::get_if<rt::KvArrayBufferView>(&variant);
          const bool nativeBuffer = nativeCapture_ && isNodeBuffer(item);
          const std::string_view constructorName = view ? rt::kvViewKindName(view->kind) : std::string_view("Uint8Array");
          node(id)["type"] = json::string(nativeBuffer ? std::string_view("Buffer") : constructorName);
          if (!nativeBuffer && !arrayType(constructorName)) throw rt::TypeError("Unsupported typed array");
          const rt::KvBytes& bytes = view ? view->buffer : item.asBytes();
          const KvValue* bufferValue = item.backingBuffer();
          std::optional<json::value> backing = bufferValue ? visit(*bufferValue) : visit(KvValue(rt::KvArrayBuffer{bytes}));
          node(id)["buffer"] = std::move(*backing);
          node(id)["offset"] = rt::makeNumber(static_cast<double>(view ? view->byteOffset : 0));
          node(id)["length"] = rt::makeNumber(static_cast<double>(view ? view->byteLength : bytes.size()));
        } else if (const auto* date = std::get_if<rt::KvDate>(&variant)) {
          node(id)["type"] = "Date";
          node(id)["value"] = json::string(rt::numberToString(date->time));
        } else if (const auto* regexp = std::get_if<rt::KvRegExp>(&variant)) {
          node(id)["type"] = "RegExp";
          node(id)["value"] = json::string(regexp->source);
          node(id)["items"] = json::array{atom("string", json::string(regexp->flags))};
        } else {
          return false;
        }
        return true;
      }

      void writeContainerNode(const KvValue& item, std::size_t id) {
        Frame frame;
        frame.item = &item;
        frame.id = id;
        const KvValue::Variant& variant = item.variant();
        if (const auto* array = std::get_if<rt::KvArray>(&variant)) {
          node(id)["type"] = "Array";
          node(id)["length"] = rt::makeNumber(static_cast<double>(array->size()));
        } else if (item.is<rt::KvMap>()) {
          node(id)["type"] = "Map";
        } else if (item.is<rt::KvSet>()) {
          node(id)["type"] = "Set";
        } else if (const auto* error = std::get_if<rt::KvError>(&variant)) {
          node(id)["type"] = json::string(isErrorType(error->name) ? std::string_view(error->name) : std::string_view("Error"));
          frame.message = KvValue(error->message.value_or(""));
          frame.stack = error->stack ? KvValue(*error->stack) : KvValue();
        } else if (!item.isObject()) {
          // KvU64, the remaining stored kind, is a class instance.
          throw rt::TypeError("Unsupported runtime class: KvU64");
        }
        if (frames_.size() >= kMaxNesting) throw rt::RangeError("Maximum call stack size exceeded.");
        frames_.push_back(std::move(frame));
      }

      // The next child to visit, holes skipped; nullptr when every child was visited.
      const KvValue* nextChild(Frame& frame) {
        const KvValue::Variant& variant = frame.item->variant();
        const auto position = [&frame] { return static_cast<std::ptrdiff_t>(frame.next); };
        if (const auto* array = std::get_if<rt::KvArray>(&variant)) {
          frame.next = array->nextPresent(frame.next);
          return frame.next < array->size() ? &(*array)[frame.next] : nullptr;
        }
        if (const auto* map = std::get_if<rt::KvMap>(&variant)) {
          if (frame.next >= 2 * map->size()) return nullptr;
          const auto& entry = *std::next(map->begin(), static_cast<std::ptrdiff_t>(frame.next / 2));
          return frame.next % 2 == 0 ? &entry.first : &entry.second;
        }
        if (const auto* set = std::get_if<rt::KvSet>(&variant)) return frame.next < set->size() ? &*std::next(set->begin(), position()) : nullptr;
        if (const auto* error = std::get_if<rt::KvError>(&variant)) {
          if (frame.next == 0) return &frame.message;
          if (frame.next == 1) return &frame.stack;
          return frame.next == 2 && error->cause ? error->cause.get() : nullptr;
        }
        const rt::KvObject& object = frame.item->asObject();
        return frame.next < object.size() ? &std::next(object.begin(), position())->second : nullptr;
      }

      // Stores the atom of the child `nextChild` returned.
      void accept(Frame& frame, json::value result) {
        const KvValue::Variant& variant = frame.item->variant();
        if (std::holds_alternative<rt::KvArray>(variant)) {
          frame.entries.push_back(json::array{json::string(std::to_string(frame.next)), std::move(result)});
        } else if (std::holds_alternative<rt::KvMap>(variant)) {
          if (frame.next % 2 == 0) frame.pendingKey = std::move(result);
          else frame.entries.push_back(json::array{std::move(frame.pendingKey), std::move(result)});
        } else if (std::holds_alternative<rt::KvSet>(variant)) {
          frame.entries.push_back(std::move(result));
        } else if (std::holds_alternative<rt::KvError>(variant)) {
          const char* const name = frame.next == 0 ? "message" : frame.next == 1 ? "stack" : "cause";
          frame.entries.push_back(json::array{name, std::move(result)});
        } else {
          const std::string& key = std::next(frame.item->asObject().begin(), static_cast<std::ptrdiff_t>(frame.next))->first;
          frame.entries.push_back(json::array{json::string(key), std::move(result)});
        }
        ++frame.next;
      }

      bool nativeCapture_;
      std::deque<Frame> frames_;
      std::unordered_map<const void*, std::size_t> seen_;
    };

    TypedValue encodeValueGraph(const KvValue& value, bool nativeCapture) {
      Encoder encoder(nativeCapture);
      json::value root = encoder.visitRoot(value);
      json::object graph;
      graph["root"] = std::move(root);
      graph["nodes"] = std::move(encoder.nodes);
      return graph;
    }

    // `String(value)` for a decoded value (what `String(read(...))` builds RegExp flags from).
    std::string viewElementsText(rt::KvViewKind kind, const rt::KvBytes& buffer, std::uint64_t byteOffset, std::uint64_t byteLength) {
      const std::uint64_t size = bytesPerElement(kind);
      std::string out;
      for (std::uint64_t position = 0; position + size <= byteLength; position += size) {
        const std::uint8_t* data = buffer.data() + byteOffset + position;
        std::uint64_t bits = 0;
        for (std::uint64_t index = 0; index < size; ++index) bits |= static_cast<std::uint64_t>(data[index]) << (8 * index);
        std::string element;
        switch (kind) {
          case rt::KvViewKind::Int8Array: element = rt::numberToString(static_cast<std::int8_t>(bits)); break;
          case rt::KvViewKind::Int16Array: element = rt::numberToString(static_cast<std::int16_t>(bits)); break;
          case rt::KvViewKind::Int32Array: element = rt::numberToString(static_cast<std::int32_t>(bits)); break;
          case rt::KvViewKind::Float32Array: {
            float number = 0;
            const auto raw = static_cast<std::uint32_t>(bits);
            std::memcpy(&number, &raw, sizeof number);
            element = rt::numberToString(static_cast<double>(number));
            break;
          }
          case rt::KvViewKind::Float64Array: {
            double number = 0;
            std::memcpy(&number, &bits, sizeof number);
            element = rt::numberToString(number);
            break;
          }
          case rt::KvViewKind::BigInt64Array: element = rt::BigInt(static_cast<std::int64_t>(bits)).toString(); break;
          case rt::KvViewKind::BigUint64Array: element = rt::BigInt::fromUint64(bits).toString(); break;
          default: element = rt::numberToString(static_cast<double>(bits)); break;
        }
        if (position) out += ",";
        out += element;
      }
      return out;
    }

    std::string jsString(const KvValue& value, std::size_t depth = 0) {
      if (depth >= kMaxNesting) throw rt::RangeError("Maximum call stack size exceeded.");
      const KvValue::Variant& variant = value.variant();
      if (value.isUndefined() || value.is<rt::KvHole>()) return "undefined";
      if (value.isNull()) return "null";
      if (value.isBool()) return value.asBool() ? "true" : "false";
      if (value.isNumber()) return rt::numberToString(value.asNumber());
      if (value.isString()) return value.asString();
      if (value.isBigInt()) return value.asBigInt().toString();
      if (value.isBytes()) return viewElementsText(rt::KvViewKind::Uint8Array, value.asBytes(), 0, value.asBytes().size());
      if (const auto* view = std::get_if<rt::KvArrayBufferView>(&variant)) {
        if (view->kind == rt::KvViewKind::DataView) return "[object DataView]";
        return viewElementsText(view->kind, view->buffer, view->byteOffset, view->byteLength);
      }
      if (value.is<rt::KvArrayBuffer>()) return "[object ArrayBuffer]";
      if (const auto* date = std::get_if<rt::KvDate>(&variant)) return rt::Date(date->time).toString();
      if (const auto* regexp = std::get_if<rt::KvRegExp>(&variant)) return "/" + regexp->source + "/" + regexp->flags;
      if (const auto* error = std::get_if<rt::KvError>(&variant)) {
        const std::string message = error->message.value_or("");
        return message.empty() ? error->name : error->name + ": " + message;
      }
      if (const auto* array = std::get_if<rt::KvArray>(&variant)) {
        std::string out;
        for (std::size_t index = 0; index < array->size(); ++index) {
          if (index) out += ",";
          const KvValue& element = (*array)[index];
          if (!element.isNullish() && !element.is<rt::KvHole>()) rt::append(out, jsString(element, depth + 1));
        }
        return out;
      }
      if (value.is<rt::KvMap>()) return "[object Map]";
      if (value.is<rt::KvSet>()) return "[object Set]";
      return "[object Object]";
    }

    // `Number(value)` for a decoded value.
    double jsNumber(const KvValue& value) {
      if (value.isUndefined() || value.is<rt::KvHole>()) return rt::Number::NaN;
      if (value.isNull()) return 0;
      if (value.isBool()) return value.asBool() ? 1 : 0;
      if (value.isNumber()) return value.asNumber();
      if (value.isBigInt()) throw rt::TypeError("Conversion from 'BigInt' to 'number' is not allowed.");
      if (const auto* date = std::get_if<rt::KvDate>(&value.variant())) return date->time;
      return rt::stringToNumber(jsString(value));
    }

    rt::TypeError noKvValue(const std::string& what) { return rt::TypeError("Typed-value interchange " + what + " has no KvValue representation"); }

    // ToIndex with JavaScriptCore's RangeError texts for the argument named `what`.
    double toIndex(double value, std::string_view what) {
      const double integer = std::isnan(value) ? 0 : std::trunc(value) + 0.0;
      if (integer < 0) throw rt::RangeError(std::string(what) + " cannot be negative");
      if (integer > rt::Number::MAX_SAFE_INTEGER) throw rt::RangeError(std::string(what) + " larger than (2 ** 53) - 1");
      return integer;
    }

    // The typed array or DataView over (a copy of) its whole buffer; a Uint8Array spanning
    // the whole buffer is KvBytes.
    KvValue viewValue(rt::KvViewKind kind, const rt::KvBytes& buffer, double byteOffset, double byteLength) {
      const auto offset = static_cast<std::uint64_t>(byteOffset);
      const auto length = static_cast<std::uint64_t>(byteLength);
      if (kind == rt::KvViewKind::Uint8Array && offset == 0 && length == buffer.size()) return KvValue(rt::KvBytes(buffer));
      return KvValue(rt::KvArrayBufferView{kind, buffer, offset, length});
    }

    // `new DataView(buffer, byteOffset, byteLength)`; std::nullopt is an undefined byteLength.
    KvValue dataView(const rt::KvArrayBuffer* buffer, double byteOffset, std::optional<double> byteLength) {
      if (!buffer) throw rt::TypeError("Expected ArrayBuffer for the first argument.");
      const double offset = toIndex(byteOffset, "byteOffset");
      const auto bufferByteLength = static_cast<double>(buffer->bytes.size());
      if (offset > bufferByteLength) throw rt::RangeError("byteOffset exceeds source ArrayBuffer byteLength");
      double viewByteLength = bufferByteLength - offset;
      if (byteLength) {
        viewByteLength = toIndex(*byteLength, "byteLength");
        if (offset + viewByteLength > bufferByteLength) throw rt::RangeError("Length out of range of buffer");
      }
      return viewValue(rt::KvViewKind::DataView, buffer->bytes, offset, viewByteLength);
    }

    // `new ARRAY_TYPE(buffer, byteOffset, length)` over an ArrayBuffer.
    KvValue typedArray(const ArrayType& type, const rt::KvArrayBuffer& buffer, double byteOffset, double length) {
      const double offset = toIndex(byteOffset, "byteOffset");
      const auto elementSize = static_cast<double>(type.bytesPerElement);
      if (rt::mod(offset, elementSize) != 0) throw rt::RangeError("byteOffset modulo TypedArray.BYTES_PER_ELEMENT must be 0");
      const double newLength = toIndex(length, "length");
      const double newByteLength = newLength * elementSize;
      if (offset + newByteLength > static_cast<double>(buffer.bytes.size())) throw rt::RangeError("Length out of range of buffer");
      return viewValue(type.kind, buffer.bytes, offset, newByteLength);
    }

    // Bun's `Buffer.from(arrayBuffer, byteOffset, length)`, as the Uint8Array view it is.
    KvValue bufferFrom(const rt::KvArrayBuffer& buffer, double byteOffset, double length) {
      if (std::isnan(byteOffset)) byteOffset = 0;
      const double maxLength = static_cast<double>(buffer.bytes.size()) - byteOffset;
      if (maxLength < 0 || std::trunc(byteOffset) < 0) throw rt::RangeError("\"offset\" is outside of buffer bounds");
      if (length > 0) {
        if (length > maxLength) throw rt::RangeError("\"length\" is outside of buffer bounds");
      } else {
        length = 0;
      }
      return viewValue(rt::KvViewKind::Uint8Array, buffer.bytes, std::trunc(byteOffset) + 0.0, std::trunc(length));
    }

    // The `objects` Map keys: the `ref` values with SameValueZero (object refs by identity).
    struct NodeKey {
      int kind = 0;
      double number = 0;
      std::string text;
      const void* identity = nullptr;
      friend bool operator<(const NodeKey& left, const NodeKey& right) {
        return std::tie(left.kind, left.number, left.text, left.identity) < std::tie(right.kind, right.number, right.text, right.identity);
      }
    };
    NodeKey nodeKey(const json::value& id) {
      NodeKey key;
      if (id.is_null()) {
        key.kind = 1;
      } else if (id.is_bool()) {
        key.kind = 2;
        key.number = id.get_bool() ? 1 : 0;
      } else if (const std::optional<double> number = rt::getNumber(id)) {
        key.kind = std::isnan(*number) ? 4 : 3;
        key.number = std::isnan(*number) || *number == 0 ? 0 : *number;
      } else if (id.is_string()) {
        key.kind = 5;
        key.text = std::string(id.get_string());
      } else {
        key.kind = 6;
        key.identity = &id;
      }
      return key;
    }

    std::string describeNotIterable(const json::value& value) {
      if (value.is_bool()) return value.get_bool() ? "true" : "false";
      if (rt::isNumber(value)) return "number";
      return "{}";
    }

    // `read`/`readNode` with an explicit stack of the nodes being allocated or populated, so the
    // nesting depth is bounded by kMaxNesting and never by the C++ stack. Each node runs the TypeScript's steps in order and suspends
    // where a nested `read` reaches a node that is not decoded yet.
    class Decoder {
    public:
      Decoder(const json::value& graph, bool nativeCapture) : graph_(graph), nativeCapture_(nativeCapture) {}

      KvValue decode() {
        if (graph_.is_null()) throw rt::TypeError("null is not an object (evaluating 'graph.root')");
        std::optional<KvValue> incoming = beginRead(rt::get(graph_, "root"));
        while (!frames_.empty()) {
          Frame& frame = frames_.back();
          if (!advance(frame, incoming)) continue;
          incoming = std::move(frame.value);
          frames_.pop_back();
        }
        return ValueGraph::finish(std::move(*incoming));
      }

    private:
      enum class Stage { allocate, flags, buffer, populate, mapKey, mapValue, setItem, property };
      // One `readNode(id)` in progress.
      struct Frame {
        NodeKey key;
        const json::value* node = nullptr;
        Stage stage = Stage::allocate;
        const json::value* type = nullptr;
        const json::value* pattern = nullptr;
        KvValue value;
        std::vector<const json::value*> elements;
        std::size_t index = 0;
        const json::value* entryAtom = nullptr;
        std::string propertyKey;
        KvValue keyRead;
      };

      // `base.key` where base is an atom or node: TypeError for undefined and null.
      static const json::value* property(const json::value* base, std::string_view key, std::string_view expression) {
        if (!base) throw rt::TypeError("undefined is not an object (evaluating '" + std::string(expression) + "')");
        if (base->is_null()) throw rt::TypeError("null is not an object (evaluating '" + std::string(expression) + "')");
        return rt::get(*base, key);
      }

      static KvValue take(std::optional<KvValue>& incoming) {
        KvValue value = std::move(*incoming);
        incoming.reset();
        return value;
      }

      const json::value* keep(json::value value) {
        scratch_.push_back(std::move(value));
        return &scratch_.back();
      }

      // `read(atom)`: the value, or std::nullopt after starting the node it names.
      std::optional<KvValue> beginRead(const json::value* atom) {
        const json::value* type = property(atom, "type", "atom.type");
        const std::optional<std::string_view> name = rt::getString(type);
        if (name == "undefined") return KvValue();
        if (name == "null") return KvValue(nullptr);
        if (name == "string" || name == "boolean") {
          const json::value* value = rt::get(*atom, "value");
          return value ? KvValue(*value) : KvValue();
        }
        if (name == "bigint") return KvValue(rt::BigInt::fromString(rt::toString(rt::get(*atom, "value"))));
        if (name == "number") {
          const json::value* value = rt::get(*atom, "value");
          const std::optional<std::string_view> text = rt::getString(value);
          return KvValue(text == "-0" ? -0.0 : rt::toNumber(value));
        }
        if (name == "ref") {
          const json::value& id = required(rt::get(*atom, "ref"));
          NodeKey key = nodeKey(id);
          if (const auto found = objects_.find(key); found != objects_.end()) return ValueGraph::borrow(found->second);
          if (frames_.size() >= kMaxNesting) throw rt::RangeError("Maximum call stack size exceeded.");
          const json::value& node = required(nodeAt(id));
          Frame frame;
          frame.key = std::move(key);
          frame.node = &node;
          frames_.push_back(std::move(frame));
          return std::nullopt;
        }
        throw rt::TypeError("Unsupported interchange atom: " + rt::toString(type));
      }

      // `graph.nodes.at(id)`
      const json::value* nodeAt(const json::value& id) {
        const json::value* nodes = rt::get(graph_, "nodes");
        if (!nodes) throw rt::TypeError("undefined is not an object (evaluating 'graph.nodes.at')");
        if (nodes->is_null()) throw rt::TypeError("null is not an object (evaluating 'graph.nodes.at')");
        if (const json::array* list = rt::getArray(*nodes)) return rt::at(*list, rt::toNumber(id));
        if (const std::optional<std::string_view> text = rt::getString(*nodes)) {
          const std::optional<std::string> character = rt::at(*text, rt::toNumber(id));
          return character ? keep(json::string(*character)) : nullptr;
        }
        throw rt::TypeError("graph.nodes.at is not a function. (In 'graph.nodes.at(id)', 'graph.nodes.at' is undefined)");
      }

      // Runs `frame` until it waits for a node read (false) or its node is complete (true).
      bool advance(Frame& frame, std::optional<KvValue>& incoming) {
        for (;;) {
          switch (frame.stage) {
            case Stage::allocate:
              if (!allocateNode(frame, incoming)) return false;
              continue;
            case Stage::flags: {
              const KvValue flags = take(incoming);
              const rt::Regex regexp(rt::toString(frame.pattern), jsString(flags));
              frame.value = rt::KvRegExp{regexp.source(), regexp.flags()};
              startPopulation(frame);
              continue;
            }
            case Stage::buffer: {
              const KvValue buffer = take(incoming);
              frame.value = allocateView(frame, buffer);
              startPopulation(frame);
              ValueGraph::node(frame.value).backing = std::make_unique<KvValue>(ValueGraph::borrow(buffer));
              continue;
            }
            case Stage::populate: {
              if (frame.index >= frame.elements.size()) return true;
              const json::value& element = *frame.elements[frame.index];
              if (frame.value.is<rt::KvSet>()) {
                frame.stage = Stage::setItem;
                incoming = beginRead(&element);
              } else if (frame.value.is<rt::KvMap>()) {
                const auto [key, entry] = destructure(element);
                frame.entryAtom = entry;
                frame.stage = Stage::mapKey;
                incoming = beginRead(key);
              } else {
                const auto [key, entry] = destructure(element);
                if (!key || !key->is_string()) throw rt::TypeError("Invalid interchange property key");
                frame.propertyKey = std::string(key->get_string());
                frame.stage = Stage::property;
                incoming = beginRead(entry);
              }
              if (!incoming) return false;
              continue;
            }
            case Stage::mapKey:
              frame.keyRead = take(incoming);
              frame.stage = Stage::mapValue;
              incoming = beginRead(frame.entryAtom);
              if (!incoming) return false;
              continue;
            case Stage::mapValue: {
              ValueGraph::node(frame.value).value.as<rt::KvMap>().set(std::move(frame.keyRead), take(incoming));
              nextElement(frame);
              continue;
            }
            case Stage::setItem:
              ValueGraph::node(frame.value).value.as<rt::KvSet>().add(take(incoming));
              nextElement(frame);
              continue;
            case Stage::property:
              defineProperty(ValueGraph::node(frame.value).value, frame.propertyKey, take(incoming));
              nextElement(frame);
              continue;
          }
        }
      }

      static void nextElement(Frame& frame) {
        frame.index += 1;
        frame.stage = Stage::populate;
      }

      // allocateNode; false while a view's buffer or a RegExp's flags wait for a node read.
      bool allocateNode(Frame& frame, std::optional<KvValue>& incoming) {
        const json::value& node = *frame.node;
        frame.type = property(&node, "type", "node.type");
        const std::optional<std::string_view> name = rt::getString(frame.type);
        if (name == "ArrayBuffer") {
          frame.value = rt::KvArrayBuffer{decodeBytes(rt::toString(&required(rt::get(node, "value"))))};
          startPopulation(frame);
          return true;
        }
        if (name == "Date") {
          frame.value = rt::KvDate{rt::Date(rt::toNumber(rt::get(node, "value"))).getTime()};
          startPopulation(frame);
          return true;
        }
        if (name == "RegExp") {
          frame.pattern = &required(rt::get(node, "value"));
          const json::value* first = flagsAtom(required(rt::get(node, "items")));
          frame.stage = Stage::flags;
          incoming = beginRead(first);
          return incoming.has_value();
        }
        const std::string typeKey = rt::toString(frame.type);
        if (arrayType(typeKey) || (nativeCapture_ && name == "Buffer")) {
          frame.stage = Stage::buffer;
          incoming = beginRead(&required(rt::get(node, "buffer")));
          return incoming.has_value();
        }
        if (name == "Array") {
          frame.value = newArray(rt::get(node, "length"));
        } else if (name == "Map") {
          frame.value = rt::KvMap{};
        } else if (name == "Set") {
          frame.value = rt::KvSet{};
        } else if (isErrorType(typeKey)) {
          rt::KvError error;
          error.name = typeKey;
          frame.value = std::move(error);
        } else if (name == "object") {
          frame.value = rt::KvObject{};
        } else {
          throw rt::TypeError("Unsupported interchange node: " + typeKey);
        }
        startPopulation(frame);
        return true;
      }

      // `required(node.items)[0]`
      const json::value* flagsAtom(const json::value& items) {
        if (items.is_null()) throw rt::TypeError("null is not an object (evaluating 'required(node.items)[0]')");
        if (const json::array* list = rt::getArray(items)) return rt::get(*list, 0);
        if (const std::optional<std::string_view> text = rt::getString(items)) {
          const std::optional<std::string> character = rt::at(*text, 0);
          return character ? keep(json::string(*character)) : nullptr;
        }
        return rt::get(items, "0");
      }

      // allocateView, once `read(required(node.buffer))` produced `buffer`.
      KvValue allocateView(const Frame& frame, const KvValue& buffer) {
        const json::value& node = *frame.node;
        const auto* backing = std::get_if<rt::KvArrayBuffer>(&buffer.variant());
        const std::optional<std::string_view> name = rt::getString(frame.type);
        if (nativeCapture_ && name == "Buffer") {
          if (!backing) throw rt::TypeError("Native Buffer capture requires an ArrayBuffer backing");
          const double offset = rt::toNumber(&required(rt::get(node, "offset")));
          const double length = rt::toNumber(&required(rt::get(node, "length")));
          return bufferFrom(*backing, offset, length);
        }
        if (name == "DataView") {
          const json::value* length = rt::get(node, "length");
          return dataView(backing, rt::toNumber(rt::get(node, "offset")), length ? std::optional<double>(rt::toNumber(length)) : std::nullopt);
        }
        const ArrayType& type = *arrayType(rt::toString(frame.type));
        const double length = rt::toNumber(&required(rt::get(node, "length"))) / static_cast<double>(type.bytesPerElement);
        if (type.kind == rt::KvViewKind::DataView) return dataView(backing, rt::toNumber(rt::get(node, "offset")), length);
        // JavaScript builds the view from any other value as an array-like or iterable.
        if (!backing) throw noKvValue("typed array over a value that is not an ArrayBuffer");
        return typedArray(type, *backing, rt::toNumber(rt::get(node, "offset")), length);
      }

      // `objects.set(id, value)`, then populateNode's iteration of entries or items.
      void startPopulation(Frame& frame) {
        if (!valueGraph_) valueGraph_ = std::make_shared<ValueGraph>();
        frame.value = valueGraph_->add(std::move(frame.value));
        objects_.emplace(frame.key, ValueGraph::borrow(frame.value));
        frame.stage = Stage::populate;
        frame.index = 0;
        const json::value& node = *frame.node;
        if (frame.value.is<rt::KvMap>()) {
          frame.elements = forOf(rt::get(node, "entries"));
        } else if (frame.value.is<rt::KvSet>()) {
          frame.elements = forOf(rt::get(node, "items"));
        } else {
          const std::optional<std::string_view> type = rt::getString(rt::get(node, "type"));
          if (type == "Array" || type == "object" || frame.value.is<rt::KvError>()) frame.elements = forOf(rt::get(node, "entries"));
        }
      }

      // `new Array(length)`
      static KvValue newArray(const json::value* length) {
        if (!length) return rt::KvArray{KvValue()};
        const std::optional<double> number = rt::getNumber(*length);
        if (!number) return rt::KvArray{KvValue(*length)};
        if (static_cast<double>(rt::toUint32(*number)) != *number) throw rt::RangeError("Array length must be a positive integer of safe magnitude.");
        return rt::KvArray(static_cast<std::size_t>(*number), KvValue(rt::KvHole{}));
      }

      // The elements `for (... of iterable ?? [])` visits: an array's, or a string's code points.
      std::vector<const json::value*> forOf(const json::value* iterable) {
        std::vector<const json::value*> elements;
        if (!iterable || iterable->is_null()) return elements;
        if (const json::array* list = rt::getArray(*iterable)) {
          for (const json::value& element : *list) elements.push_back(&element);
          return elements;
        }
        if (const std::optional<std::string_view> text = rt::getString(*iterable)) {
          for (const std::string& character : rt::codePointStrings(*text)) elements.push_back(keep(json::string(character)));
          return elements;
        }
        throw rt::TypeError(describeNotIterable(*iterable) + " is not iterable");
      }

      // `const [key, entry] = element`
      std::pair<const json::value*, const json::value*> destructure(const json::value& element) {
        if (element.is_null()) throw rt::TypeError("null is not an object (evaluating '[key, entry]')");
        if (const json::array* list = rt::getArray(element)) return {rt::get(*list, 0), rt::get(*list, 1)};
        if (const std::optional<std::string_view> text = rt::getString(element)) {
          const std::vector<std::string> characters = rt::codePointStrings(*text);
          const json::value* key = characters.size() > 0 ? keep(json::string(characters[0])) : nullptr;
          const json::value* entry = characters.size() > 1 ? keep(json::string(characters[1])) : nullptr;
          return {key, entry};
        }
        throw rt::TypeError(describeNotIterable(element) + " is not iterable");
      }

      // Object.defineProperty(value, key, { value: read(entry), writable: true, enumerable, configurable: true })
      static void defineProperty(KvValue& target, const std::string& key, KvValue property) {
        if (auto* array = std::get_if<rt::KvArray>(&target.variant())) {
          if (key == "length") {
            const double number = jsNumber(property);
            if (static_cast<double>(rt::toUint32(number)) != number) throw rt::RangeError("Invalid array length");
            throw rt::TypeError("Attempting to change configurable attribute of unconfigurable property.");
          }
          if (!rt::isArrayIndex(key)) throw noKvValue("array property " + rt::jsonQuote(key));
          const double index = rt::stringToNumber(key);
          array->set(static_cast<std::size_t>(index), std::move(property));
          return;
        }
        if (auto* error = std::get_if<rt::KvError>(&target.variant())) {
          if (key == "message" && property.isString()) error->message = property.asString();
          else if (key == "stack" && (property.isString() || property.isUndefined())) error->stack = property.isString() ? std::optional<std::string>(property.asString()) : std::nullopt;
          else if (key == "cause") error->cause = std::make_shared<const KvValue>(std::move(property));
          else throw noKvValue("Error property " + rt::jsonQuote(key) + " holding " + std::string(property.kind()));
          return;
        }
        target.asObject().set(key, std::move(property));
      }

      const json::value& graph_;
      bool nativeCapture_;
      std::shared_ptr<ValueGraph> valueGraph_;
      std::map<NodeKey, KvValue> objects_;
      std::deque<Frame> frames_;
      std::deque<json::value> scratch_;
    };
  }

  TypedValue encodeTypedValue(const rt::KvValue& value) { return encodeValueGraph(value, false); }
  TypedValue encodeNativeCaptureValue(const rt::KvValue& value) { return encodeValueGraph(value, true); }
  rt::KvValue decodeTypedValue(const TypedValue& graph) { return Decoder(graph, false).decode(); }
  rt::KvValue decodeNativeCaptureValue(const TypedValue& graph) { return Decoder(graph, true).decode(); }
}
