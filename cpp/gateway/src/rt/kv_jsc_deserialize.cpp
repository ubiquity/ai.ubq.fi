// The JavaScriptCore SerializedScriptValue reader (version 14) that Bun 1.4's node:v8
// `deserialize` uses: the inverse of kvJscSerialize for every form Bun writes. Bytes are
// parsed into a graph first (JSC's object table is what ObjectReferenceTag indexes), then
// materialized as a KvValue tree; shared references become copies and cycles are refused.
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
    // JavaScriptCore accepts 40001 nested containers. KvValue copies and destroys
    // recursively and overflows an 8 MiB stack (unoptimised) beyond about 3000 levels, so
    // the reader stops where kv-conversion's typed-value codec does.
    constexpr std::size_t MaxDepth = 2000;
    constexpr std::uint64_t MaxDenseLength = std::uint64_t{1} << 24;
    constexpr std::size_t MaxMaterializedValues = std::size_t{1} << 24;

    [[noreturn]] void malformed() { throw TypeError("Unable to deserialize data."); }
    [[noreturn]] void tooDeep() { throw RangeError("Maximum call stack size exceeded."); }
    [[noreturn]] void unrepresentable(const std::string& what) { throw TypeError("Serialized value " + what + " has no KvValue representation"); }

    using NodeId = std::uint32_t;

    // One JavaScript value of the graph. Primitives and leaf objects are finished
    // values; containers keep their children as node ids so references can share them.
    struct Node {
      enum class Kind : std::uint8_t { value, array, object, map, set };
      Kind kind = Kind::value;
      KvValue value{};
      // An array's length: its declared length, grown by elements stored past it.
      std::uint64_t length = 0;
      std::vector<std::pair<std::uint32_t, NodeId>> elements{};
      // Object members; extra properties of an array, Map or Set.
      std::vector<std::pair<std::string, NodeId>> properties{};
      std::vector<std::pair<NodeId, NodeId>> entries{};
      std::vector<NodeId> members{};
    };

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

    // The bytes-to-graph pass: CloneDeserializer's reads and validation. Containers are
    // parsed with an explicit stack (JSC's state machine), so nesting costs no C++ stack.
    class GraphReader {
    public:
      explicit GraphReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

      std::vector<Node> nodes;

      // The version header and the root value.
      NodeId read() {
        const std::uint32_t version = u32();
        if (version > CurrentVersion) malformed();
        const NodeId root = value();
        while (!frames_.empty()) step();
        return root;
      }

    private:
      // A container whose contents are being read: its next slot is a member, an array
      // index, a Map key or value, a Set member, or (after the data) a property.
      struct Frame {
        NodeId id = 0;
        Node::Kind kind = Node::Kind::object;
        bool properties = false;
        std::optional<NodeId> mapKey{};
      };

      std::span<const std::uint8_t> bytes_;
      std::size_t position_ = 0;
      std::vector<std::string> pool_;
      // JSC's object table, in the order ObjectReferenceTag indexes it.
      std::vector<NodeId> objects_;
      std::vector<Frame> frames_;

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

      NodeId add(Node node) {
        nodes.push_back(std::move(node));
        return static_cast<NodeId>(nodes.size() - 1);
      }
      NodeId add(KvValue value) {
        Node node;
        node.value = std::move(value);
        return add(std::move(node));
      }
      NodeId record(NodeId id) {
        objects_.push_back(id);
        return id;
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
      NodeId viewBuffer() {
        const std::uint8_t tag = u8();
        if (tag == ArrayBufferTag) return arrayBuffer();
        if (tag == ResizableArrayBufferTag) unrepresentable("resizable ArrayBuffer");
        if (tag != ObjectReferenceTag) malformed();
        const NodeId referenced = reference();
        if (nodes[referenced].kind != Node::Kind::value || !nodes[referenced].value.is<KvArrayBuffer>()) malformed();
        return referenced;
      }

      NodeId arrayBuffer() {
        const std::uint64_t length = u64();
        need(length);
        KvArrayBuffer buffer;
        buffer.bytes.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(position_), bytes_.begin() + static_cast<std::ptrdiff_t>(position_ + length));
        position_ += static_cast<std::size_t>(length);
        return record(add(KvValue(std::move(buffer))));
      }

      NodeId reference() {
        const std::uint32_t index = poolIndex(objects_.size());
        if (index >= objects_.size()) malformed();
        return objects_[index];
      }

      NodeId view() {
        const std::uint8_t subtag = u8();
        if (subtag > Float16ArraySubtag) malformed();
        const std::uint64_t byteOffset = u64();
        const std::uint64_t byteLength = u64();
        const NodeId bufferId = viewBuffer();
        if (byteLength == std::numeric_limits<std::uint64_t>::max()) unrepresentable("length-tracking typed array");
        const std::size_t size = elementSize(subtag);
        if (byteLength % size != 0) malformed();
        if (subtag == Float16ArraySubtag) unrepresentable("Float16Array");
        const KvBytes& buffer = nodes[bufferId].value.as<KvArrayBuffer>().bytes;
        KvValue result(nullptr);
        // A view outside its buffer, or misaligned, deserializes as null.
        if (byteOffset <= buffer.size() && byteLength <= buffer.size() - byteOffset && byteOffset % size == 0) {
          const auto kind = static_cast<KvViewKind>(subtag);
          if (kind == KvViewKind::Uint8Array && byteOffset == 0 && byteLength == buffer.size()) result = KvValue(buffer);
          else result = KvValue(KvArrayBufferView{kind, buffer, byteOffset, byteLength});
        }
        return record(add(std::move(result)));
      }

      // A new container: recorded, and its contents read by the frames loop.
      NodeId container(Node::Kind kind) {
        Node node;
        node.kind = kind;
        if (kind == Node::Kind::array) node.length = u32();
        const NodeId id = record(add(std::move(node)));
        if (frames_.size() >= MaxDepth) tooDeep();
        Frame frame;
        frame.id = id;
        frame.kind = kind;
        frames_.push_back(frame);
        return id;
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
        const Frame frame = frames_[top];
        if (frame.properties || frame.kind == Node::Kind::object) {
          std::optional<std::string> key = string(true);
          if (!key) {
            frames_.pop_back();
            return;
          }
          const NodeId child = value();
          nodes[frame.id].properties.emplace_back(std::move(*key), child);
          return;
        }
        switch (frame.kind) {
          case Node::Kind::array: {
            const std::uint32_t index = u32();
            if (index == TerminatorTag) {
              frames_.pop_back();
              return;
            }
            if (index == NonIndexPropertiesTag) {
              frames_[top].properties = true;
              return;
            }
            const NodeId child = value();
            Node& self = nodes[frame.id];
            self.elements.emplace_back(index, child);
            if (std::uint64_t{index} + 1 > self.length) self.length = std::uint64_t{index} + 1;
            return;
          }
          case Node::Kind::map: {
            if (frame.mapKey) {
              const NodeId entry = value();
              nodes[frame.id].entries.emplace_back(*frame.mapKey, entry);
              frames_[top].mapKey.reset();
              return;
            }
            if (collectionEnd(NonMapPropertiesTag)) {
              frames_[top].properties = true;
              return;
            }
            const NodeId key = value();
            frames_[top].mapKey = key;
            return;
          }
          default: {
            if (collectionEnd(NonSetPropertiesTag)) {
              frames_[top].properties = true;
              return;
            }
            const NodeId member = value();
            nodes[frame.id].members.push_back(member);
            return;
          }
        }
      }

      NodeId regexp() {
        const std::string source = requiredString();
        const std::string flags = requiredString();
        const std::optional<std::string> canonical = canonicalRegExpFlags(flags);
        if (!canonical) malformed();
        // rt::Regex has no unicodeSets mode; a v-flag pattern is taken as written.
        if (canonical->find('v') == std::string::npos) {
          try {
            const Regex compiled(source, *canonical);
          } catch (const SyntaxError&) {
            malformed();
          }
        }
        return record(add(KvValue(KvRegExp{source, *canonical})));
      }

      NodeId error() {
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
        return record(add(KvValue(std::move(error))));
      }

      // One value: a terminal is read whole; a container is opened (see step()).
      NodeId value() {
        const std::uint8_t tag = u8();
        switch (tag) {
          case ArrayTag:
            return container(Node::Kind::array);
          case ObjectTag:
            return container(Node::Kind::object);
          case MapObjectTag:
            return container(Node::Kind::map);
          case SetObjectTag:
            return container(Node::Kind::set);
          case UndefinedTag:
            return add(KvValue());
          case NullTag:
            return add(KvValue(nullptr));
          case IntTag:
            return add(KvValue(static_cast<double>(static_cast<std::int32_t>(u32()))));
          case ZeroTag:
            return add(KvValue(0.0));
          case OneTag:
            return add(KvValue(1.0));
          case FalseTag:
            return add(KvValue(false));
          case TrueTag:
            return add(KvValue(true));
          case DoubleTag:
            return add(KvValue(f64()));
          case DateTag:
            return record(add(KvValue(KvDate{timeClip(f64())})));
          case StringTag:
            return add(KvValue(requiredString()));
          case EmptyStringTag:
            return add(KvValue(std::string()));
          case RegExpTag:
            return regexp();
          case ObjectReferenceTag:
            return reference();
          case ArrayBufferTag:
            return arrayBuffer();
          case ArrayBufferViewTag:
            return view();
          case BigIntTag:
            return add(KvValue(bigint()));
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

    // The graph-to-tree pass, with an explicit stack of containers under construction.
    class Materializer {
    public:
      explicit Materializer(const std::vector<Node>& nodes) : nodes_(nodes), building_(nodes.size(), false) {}

      // The tree for `root`, which sits `depth` containers deep.
      KvValue tree(NodeId root, std::size_t depth) {
        depth_ = depth;
        enter(root);
        while (!stack_.empty()) {
          const std::optional<NodeId> child = nextChild();
          if (child) {
            enter(*child);
            continue;
          }
          Frame& done = stack_.back();
          building_[done.id] = false;
          KvValue built = std::move(done.built);
          stack_.pop_back();
          deliver(std::move(built));
        }
        return std::move(*result_);
      }

    private:
      struct Frame {
        NodeId id = 0;
        KvValue built{};
        // Children visited so far: elements, members, or Map keys and values in turn.
        std::size_t next = 0;
        std::optional<KvValue> mapKey{};
      };

      const std::vector<Node>& nodes_;
      std::vector<bool> building_;
      std::vector<Frame> stack_;
      std::optional<KvValue> result_;
      std::size_t count_ = 0;
      std::size_t depth_ = 0;

      void enter(NodeId id) {
        if (++count_ > MaxMaterializedValues) unrepresentable("whose shared references expand past " + std::to_string(MaxMaterializedValues) + " values");
        const Node& node = nodes_[id];
        if (node.kind == Node::Kind::value) {
          deliver(node.value);
          return;
        }
        if (depth_ + stack_.size() >= MaxDepth) tooDeep();
        if (building_[id]) unrepresentable("cycle");
        Frame frame;
        frame.id = id;
        switch (node.kind) {
          case Node::Kind::array:
            if (!node.properties.empty()) unrepresentable("array property " + jsonQuote(node.properties.front().first));
            if (node.length > MaxDenseLength) unrepresentable("sparse array length " + std::to_string(node.length));
            frame.built = KvValue(KvArray(static_cast<std::size_t>(node.length), KvValue(KvHole{})));
            break;
          case Node::Kind::object:
            frame.built = KvValue(KvObject());
            break;
          case Node::Kind::map:
            if (!node.properties.empty()) unrepresentable("Map property " + jsonQuote(node.properties.front().first));
            frame.built = KvValue(KvMap());
            break;
          default:
            if (!node.properties.empty()) unrepresentable("Set property " + jsonQuote(node.properties.front().first));
            frame.built = KvValue(KvSet());
            break;
        }
        building_[id] = true;
        stack_.push_back(std::move(frame));
      }

      std::optional<NodeId> nextChild() {
        Frame& frame = stack_.back();
        const Node& node = nodes_[frame.id];
        const std::size_t index = frame.next;
        std::optional<NodeId> child;
        switch (node.kind) {
          case Node::Kind::array:
            if (index < node.elements.size()) child = node.elements[index].second;
            break;
          case Node::Kind::object:
            if (index < node.properties.size()) child = node.properties[index].second;
            break;
          case Node::Kind::map:
            if (index < node.entries.size() * 2) child = index % 2 == 0 ? node.entries[index / 2].first : node.entries[index / 2].second;
            break;
          default:
            if (index < node.members.size()) child = node.members[index];
            break;
        }
        if (child) ++frame.next;
        return child;
      }

      // Puts a finished value in the slot the innermost container visited last.
      void deliver(KvValue value) {
        if (stack_.empty()) {
          result_ = std::move(value);
          return;
        }
        Frame& frame = stack_.back();
        const Node& node = nodes_[frame.id];
        const std::size_t index = frame.next - 1;
        switch (node.kind) {
          case Node::Kind::array:
            frame.built.asArray()[node.elements[index].first] = std::move(value);
            break;
          case Node::Kind::object:
            frame.built.asObject().set(node.properties[index].first, std::move(value));
            break;
          case Node::Kind::map:
            if (index % 2 == 0) {
              frame.mapKey = std::move(value);
            } else {
              std::get<KvMap>(frame.built.variant()).set(std::move(*frame.mapKey), std::move(value));
              frame.mapKey.reset();
            }
            break;
          default:
            std::get<KvSet>(frame.built.variant()).add(std::move(value));
            break;
        }
      }
    };

    // The element at `index` of an array node, or std::nullopt for a hole.
    std::optional<NodeId> element(const Node& array, std::uint32_t index) {
      std::optional<NodeId> found;
      for (const auto& [at, child] : array.elements)
        if (at == index) found = child;
      return found;
    }
  }

  KvValue kvJscDeserialize(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return KvValue(nullptr);
    const bool envelope = bytes.size() >= BufferEnvelope.size() && std::equal(BufferEnvelope.begin(), BufferEnvelope.end(), bytes.begin());
    if (!envelope) {
      GraphReader reader(bytes);
      const NodeId root = reader.read();
      Materializer materializer(reader.nodes);
      return materializer.tree(root, 0);
    }
    const std::span<const std::uint8_t> inner = bytes.subspan(BufferEnvelope.size());
    const auto invalidEnvelope = [] { return JsError("failed to parse serialized buffer envelope"); };
    // The inner bytes deserialize like any input: empty is null, which is not an envelope.
    if (inner.empty()) throw invalidEnvelope();
    GraphReader reader(inner);
    const NodeId root = reader.read();
    const Node& pair = reader.nodes[root];
    if (pair.kind != Node::Kind::array || pair.length != 2) throw invalidEnvelope();
    const std::optional<NodeId> buffers = element(pair, 1);
    if (!buffers || reader.nodes[*buffers].kind != Node::Kind::array) throw invalidEnvelope();
    // The second element lists the Uint8Arrays Bun revives as Buffers; a Buffer is the
    // Uint8Array it wraps, so only the first element is read.
    const std::optional<NodeId> value = element(pair, 0);
    if (!value) return KvValue();
    Materializer materializer(reader.nodes);
    return materializer.tree(*value, 1);
  }
}
