// The JavaScriptCore SerializedScriptValue reader (version 14) that Bun 1.4's node:v8
// `deserialize` uses. The object table indexes nodes in the stored-value graph,
// allowing shared references and cycles to point to the original node.
#include "kv_internal.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace uos::rt {
  namespace {
    enum Tag : std::uint8_t {
      ArrayTag = 1,
      ObjectTag = 2,
      UndefinedTag = 3,
      NullTag = 4,
      IntTag = 5,
      ZeroTag = 6,
      OneTag = 7,
      FalseTag = 8,
      TrueTag = 9,
      DoubleTag = 10,
      DateTag = 11,
      StringTag = 16,
      EmptyStringTag = 17,
      RegExpTag = 18,
      ObjectReferenceTag = 19,
      ArrayBufferTag = 21,
      ArrayBufferViewTag = 22,
      TrueObjectTag = 24,
      FalseObjectTag = 25,
      StringObjectTag = 26,
      EmptyStringObjectTag = 27,
      NumberObjectTag = 28,
      SetObjectTag = 29,
      MapObjectTag = 30,
      NonMapPropertiesTag = 31,
      NonSetPropertiesTag = 32,
      BigIntTag = 47,
      BigIntObjectTag = 48,
      DOMExceptionTag = 51,
      ResizableArrayBufferTag = 54,
      ErrorInstanceTag = 55,
      // Bun's own tag for Blob and File.
      BunBlobTag = 254,
    };
    constexpr std::uint32_t CurrentVersion = 14;
    constexpr std::uint32_t TerminatorTag = 0xFFFFFFFF;
    constexpr std::uint32_t StringPoolTag = 0xFFFFFFFE;
    constexpr std::uint32_t NonIndexPropertiesTag = 0xFFFFFFFD;
    constexpr std::uint32_t StringDataIs8BitFlag = 0x80000000;
    constexpr std::uint8_t Float16ArraySubtag = 12;
    // JavaScriptCore's JSBigInt::maxLength in 64-bit digits (2^20 bits).
    constexpr std::uint32_t MaxBigIntWords = 1u << 14;
    // Bun wraps a graph that holds Buffers as `[value, buffers]` behind this prefix.
    constexpr std::array<std::uint8_t, 5> BufferEnvelope = {0xFF, 0x42, 0x55, 0x4E, 0x01};
    [[noreturn]] void malformed() { throw TypeError("Unable to deserialize data."); }
    [[noreturn]] void tooDeep() { throw RangeError("Maximum call stack size exceeded."); }
    [[noreturn]] void unrepresentable(const std::string& what) { throw TypeError("Serialized value " + what + " has no KvValue representation"); }

    using kv_detail::GraphNode;
    using kv_detail::ValueGraph;

    std::size_t elementSize(std::uint8_t subtag) {
      switch (subtag) {
        case 0:
        case 1:
        case 2:
        case 3:
          return 1;
        case 4:
        case 5:
        case Float16ArraySubtag:
          return 2;
        case 6:
        case 7:
        case 8:
          return 4;
        default:
          return 8;
      }
    }

    // TimeClip, which a deserialized Date applies to its time value.
    double timeClip(double time) {
      if (!std::isfinite(time) || std::fabs(time) > 8.64e15) return std::numeric_limits<double>::quiet_NaN();
      return std::trunc(time) + 0.0;
    }

    // RegExp flags in canonical order; std::nullopt for flags `new RegExp` refuses.
    std::optional<std::string> canonicalRegExpFlags(std::string_view flags) {
      static constexpr std::string_view order = "dgimsuvy";
      std::array<bool, 8> seen{};
      for (const char flag : flags) {
        const std::size_t position = order.find(flag);
        if (position == std::string_view::npos || seen[position]) return std::nullopt;
        seen[position] = true;
      }
      if (seen[5] && seen[6]) return std::nullopt;
      std::string out;
      for (std::size_t index = 0; index < order.size(); ++index)
        if (seen[index]) out += order[index];
      return out;
    }

    // CloneDeserializer's reads and validation, with an explicit stack of open containers.
    class GraphReader {
    public:
      explicit GraphReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

      KvValue read() {
        const std::uint32_t version = u32();
        if (version > CurrentVersion) malformed();
        KvValue root = value();
        while (!frames_.empty()) step();
        if (unsupportedProperty_) unrepresentable(*unsupportedProperty_);
        return ValueGraph::finish(std::move(root));
      }

    private:
      enum class Kind { array, object, map, set };

      struct Frame {
        GraphNode* node;
        Kind kind;
        bool properties;
        std::size_t childHeight = 0;
        std::optional<KvValue> mapKey{};
      };

      std::span<const std::uint8_t> bytes_;
      std::size_t position_ = 0;
      std::vector<std::string> pool_;
      // JSC's object table, in the order ObjectReferenceTag indexes it.
      std::vector<GraphNode*> objects_;
      std::vector<Frame> frames_;
      std::shared_ptr<ValueGraph> graph_;
      std::optional<std::string> unsupportedProperty_;

      std::size_t remaining() const noexcept { return bytes_.size() - position_; }
      void need(std::uint64_t count) const {
        if (count > remaining()) malformed();
      }
      std::uint8_t u8() {
        need(1);
        return bytes_[position_++];
      }
      std::uint16_t u16() {
        need(2);
        const auto value = static_cast<std::uint16_t>(bytes_[position_] | (bytes_[position_ + 1] << 8));
        position_ += 2;
        return value;
      }
      std::uint32_t u32() {
        need(4);
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(bytes_[position_++]) << shift;
        return value;
      }
      std::uint64_t u64() {
        need(8);
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 8) value |= static_cast<std::uint64_t>(bytes_[position_++]) << shift;
        return value;
      }
      double f64() { return std::bit_cast<double>(u64()); }
      // JSC's read(bool): one byte, 0 or 1.
      bool boolean8() {
        const std::uint8_t value = u8();
        if (value > 1) malformed();
        return value == 1;
      }
      // A u32 that must be 0 or 1 (the null flag of a nullable string).
      bool boolean32() {
        const std::uint32_t value = u32();
        if (value > 1) malformed();
        return value == 1;
      }

      // readConstantPoolIndex: the width follows the table's size at the time of reading.
      std::uint32_t poolIndex(std::size_t size) {
        if (size <= 0xFF) return u8();
        if (size <= 0xFFFF) return u16();
        return u32();
      }

      KvValue record(KvValue value) {
        if (value.isNull()) {
          objects_.push_back(nullptr);
          return value;
        }
        if (!graph_) graph_ = std::make_shared<ValueGraph>();
        KvValue link = graph_->add(std::move(value));
        objects_.push_back(&ValueGraph::node(link));
        return link;
      }

      // readStringData: a new string (appended to the constant pool), a pool reference, or
      // the terminator (std::nullopt) where the caller accepts one.
      std::optional<std::string> string(bool terminatorAllowed) {
        const std::uint32_t header = u32();
        if (header == TerminatorTag) {
          if (!terminatorAllowed) malformed();
          return std::nullopt;
        }
        if (header == StringPoolTag) {
          const std::uint32_t index = poolIndex(pool_.size());
          if (index >= pool_.size()) malformed();
          return pool_[index];
        }
        const bool is8Bit = (header & StringDataIs8BitFlag) != 0;
        const std::uint64_t length = header & ~StringDataIs8BitFlag;
        need(is8Bit ? length : length * 2);
        std::u16string units;
        units.reserve(static_cast<std::size_t>(length));
        for (std::uint64_t index = 0; index < length; ++index) units.push_back(is8Bit ? static_cast<char16_t>(u8()) : static_cast<char16_t>(u16()));
        pool_.push_back(fromUtf16(units));
        return pool_.back();
      }
      std::string requiredString() { return *string(false); }

      std::optional<std::string> nullableString() {
        if (boolean32()) return std::nullopt;
        return requiredString();
      }

      BigInt bigint() {
        const bool negative = boolean8();
        const std::uint32_t words = u32();
        if (words == 0) return BigInt(0);
        if (words > MaxBigIntWords) malformed();
        need(std::uint64_t{words} * 8);
        KvBytes magnitude(std::size_t{words} * 8);
        for (std::size_t word = 0; word < words; ++word) {
          const std::uint64_t bits = u64();
          for (std::size_t byte = 0; byte < 8; ++byte) magnitude[magnitude.size() - 1 - (word * 8 + byte)] = static_cast<std::uint8_t>(bits >> (8 * byte));
        }
        std::size_t first = 0;
        while (first < magnitude.size() && magnitude[first] == 0) ++first;
        return kv_internal::bigintFromMagnitude(std::span<const std::uint8_t>(magnitude).subspan(first), negative);
      }

      // The ArrayBuffer a view reads (readTerminal): a new buffer or a reference to one.
      KvValue viewBuffer() {
        const std::uint8_t tag = u8();
        if (tag == ArrayBufferTag) return arrayBuffer();
        if (tag == ResizableArrayBufferTag) unrepresentable("resizable ArrayBuffer");
        if (tag != ObjectReferenceTag) malformed();
        KvValue backing = reference();
        if (!backing.is<KvArrayBuffer>()) malformed();
        return backing;
      }

      KvValue arrayBuffer() {
        const std::uint64_t length = u64();
        need(length);
        KvArrayBuffer buffer;
        buffer.bytes.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(position_), bytes_.begin() + static_cast<std::ptrdiff_t>(position_ + length));
        position_ += static_cast<std::size_t>(length);
        return record(KvValue(std::move(buffer)));
      }

      KvValue reference() {
        const std::uint32_t index = poolIndex(objects_.size());
        if (index >= objects_.size()) malformed();
        GraphNode* target = objects_[index];
        if (!target) return nullptr;
        if (!frames_.empty()) {
          // An already closed subtree still counts toward the depth limit. An
          // open node is a back-reference and needs no additional traversal.
          const std::size_t height = target->height;
          if (frames_.size() + height > kv_detail::MaxContainerDepth) tooDeep();
          frames_.back().childHeight = std::max(frames_.back().childHeight, height);
        }
        return graph_->link(*target);
      }

      KvValue view() {
        const std::uint8_t subtag = u8();
        if (subtag > Float16ArraySubtag) malformed();
        const std::uint64_t byteOffset = u64();
        const std::uint64_t byteLength = u64();
        const KvValue backing = viewBuffer();
        if (byteLength == std::numeric_limits<std::uint64_t>::max()) unrepresentable("length-tracking typed array");
        const std::size_t size = elementSize(subtag);
        if (byteLength % size != 0) malformed();
        if (subtag == Float16ArraySubtag) unrepresentable("Float16Array");
        const KvBytes& buffer = backing.as<KvArrayBuffer>().bytes;
        // A view outside its buffer, or misaligned, deserializes as null.
        if (byteOffset > buffer.size() || byteLength > buffer.size() - byteOffset || byteOffset % size != 0) return record(nullptr);
        const auto kind = static_cast<KvViewKind>(subtag);
        KvValue result = kind == KvViewKind::Uint8Array && byteOffset == 0 && byteLength == buffer.size()
                           ? KvValue(buffer)
                           : KvValue(KvArrayBufferView{kind, buffer, byteOffset, byteLength});
        KvValue link = record(std::move(result));
        ValueGraph::node(link).backing = std::make_unique<KvValue>(ValueGraph::borrow(backing));
        return link;
      }

      KvValue container(Kind kind, KvValue containerValue) {
        kv_detail::checkContainerDepth(frames_.size());
        KvValue link = record(std::move(containerValue));
        frames_.push_back(Frame{&ValueGraph::node(link), kind, kind == Kind::object});
        return link;
      }

      void closeContainer() {
        const Frame& frame = frames_.back();
        const std::size_t height = frame.childHeight + 1;
        frame.node->height = height;
        frames_.pop_back();
        if (!frames_.empty()) frames_.back().childHeight = std::max(frames_.back().childHeight, height);
      }

      // consumeCollectionDataTerminationIfPossible: the tag that ends a collection's data.
      bool collectionEnd(std::uint8_t tag) {
        if (remaining() > 0 && bytes_[position_] == tag) {
          ++position_;
          return true;
        }
        return false;
      }

      // Reads the next slot of the innermost open container, or closes it.
      void step() {
        const std::size_t top = frames_.size() - 1;
        const Kind kind = frames_[top].kind;
        KvValue& self = frames_[top].node->value;
        if (frames_[top].properties) {
          std::optional<std::string> key = string(true);
          if (!key) {
            closeContainer();
            return;
          }
          KvValue child = value();
          if (kind == Kind::object) {
            self.asObject().set(std::move(*key), std::move(child));
          } else if (kind == Kind::array && isArrayIndex(*key)) {
            self.asArray().set(static_cast<std::size_t>(stringToNumber(*key)), std::move(child));
          } else if (!unsupportedProperty_) {
            const char* name = kind == Kind::array ? "array" : kind == Kind::map ? "Map" : "Set";
            unsupportedProperty_ = std::string(name) + " property " + jsonQuote(*key);
          }
          return;
        }
        switch (kind) {
          case Kind::array: {
            const std::uint32_t index = u32();
            if (index == TerminatorTag) {
              closeContainer();
              return;
            }
            if (index == NonIndexPropertiesTag) {
              frames_[top].properties = true;
              return;
            }
            self.asArray().set(index, value());
            return;
          }
          case Kind::map: {
            if (frames_[top].mapKey) {
              KvValue entry = value();
              self.as<KvMap>().set(std::move(*frames_[top].mapKey), std::move(entry));
              frames_[top].mapKey.reset();
              return;
            }
            if (collectionEnd(NonMapPropertiesTag)) {
              frames_[top].properties = true;
              return;
            }
            KvValue key = value();
            frames_[top].mapKey = std::move(key);
            return;
          }
          default:
            if (collectionEnd(NonSetPropertiesTag)) frames_[top].properties = true;
            else self.as<KvSet>().add(value());
        }
      }

      KvValue regexp() {
        const std::string source = requiredString();
        const std::string flags = requiredString();
        const std::optional<std::string> canonical = canonicalRegExpFlags(flags);
        if (!canonical) malformed();
        // Keep the existing storage path that preserves v-flag patterns verbatim.
        if (canonical->find('v') == std::string::npos) {
          try {
            const Regex compiled(source, *canonical);
          } catch (const SyntaxError&) {
            malformed();
          } catch (const RangeError&) {
            malformed();
          }
        }
        return record(KvValue(KvRegExp{source, *canonical}));
      }

      KvValue error() {
        static constexpr std::array<std::string_view, 7> names = {"Error", "EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", "URIError"};
        const std::uint8_t type = u8();
        if (type >= names.size()) malformed();
        KvError error;
        error.name = std::string(names[type]);
        error.message = nullableString();
        error.line = u32();
        error.column = u32();
        error.sourceURL = nullableString();
        error.stack = nullableString();
        return record(KvValue(std::move(error)));
      }

      // One value: a terminal is read whole; a container is opened (see step()).
      KvValue value() {
        const std::uint8_t tag = u8();
        switch (tag) {
          case ArrayTag:
            return container(Kind::array, KvArray(u32(), KvHole{}));
          case ObjectTag:
            return container(Kind::object, KvObject());
          case MapObjectTag:
            return container(Kind::map, KvMap());
          case SetObjectTag:
            return container(Kind::set, KvSet());
          case UndefinedTag:
            return KvValue();
          case NullTag:
            return KvValue(nullptr);
          case IntTag:
            return KvValue(static_cast<double>(static_cast<std::int32_t>(u32())));
          case ZeroTag:
            return KvValue(0.0);
          case OneTag:
            return KvValue(1.0);
          case FalseTag:
            return KvValue(false);
          case TrueTag:
            return KvValue(true);
          case DoubleTag:
            return KvValue(f64());
          case DateTag:
            return record(KvValue(KvDate{timeClip(f64())}));
          case StringTag:
            return KvValue(requiredString());
          case EmptyStringTag:
            return KvValue(std::string());
          case RegExpTag:
            return regexp();
          case ObjectReferenceTag:
            return reference();
          case ArrayBufferTag:
            return arrayBuffer();
          case ArrayBufferViewTag:
            return view();
          case BigIntTag:
            return KvValue(bigint());
          case ErrorInstanceTag:
            return error();
          case TrueObjectTag:
          case FalseObjectTag:
            unrepresentable("Boolean object");
          case StringObjectTag:
          case EmptyStringObjectTag:
            unrepresentable("String object");
          case NumberObjectTag:
            unrepresentable("Number object");
          case BigIntObjectTag:
            unrepresentable("BigInt object");
          case ResizableArrayBufferTag:
            unrepresentable("resizable ArrayBuffer");
          case DOMExceptionTag:
            unrepresentable("DOMException");
          case BunBlobTag:
            unrepresentable("Blob");
          default:
            malformed();
        }
      }
    };

  }

  KvValue kvJscDeserialize(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return KvValue(nullptr);
    const bool envelope = bytes.size() >= BufferEnvelope.size() && std::equal(BufferEnvelope.begin(), BufferEnvelope.end(), bytes.begin());
    if (!envelope) return GraphReader(bytes).read();

    const std::span<const std::uint8_t> inner = bytes.subspan(BufferEnvelope.size());
    const KvValue pair = inner.empty() ? KvValue(nullptr) : GraphReader(inner).read();
    if (!pair.isArray() || pair.asArray().size() != 2 || !pair[1].isArray()) throw JsError("failed to parse serialized buffer envelope");
    // The second element lists the Uint8Arrays Bun revives as Buffers. The
    // reader already represents those Buffers as their underlying Uint8Arrays.
    return pair[0];
  }
}
