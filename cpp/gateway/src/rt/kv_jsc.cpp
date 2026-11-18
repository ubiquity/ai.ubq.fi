// The JavaScriptCore SerializedScriptValue writer (version 14) that Bun 1.4's
// node:v8 `serialize` uses: the stored form, and the size the native store's
// value and commit limits measure.
#include "kv_internal.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <unordered_map>

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
      SetObjectTag = 29,
      MapObjectTag = 30,
      NonMapPropertiesTag = 31,
      NonSetPropertiesTag = 32,
      BigIntTag = 47,
      ErrorInstanceTag = 55,
    };
    constexpr std::uint32_t CurrentVersion = 14;
    constexpr std::uint32_t TerminatorTag = 0xFFFFFFFF;
    constexpr std::uint32_t StringPoolTag = 0xFFFFFFFE;
    constexpr std::uint32_t NonIndexPropertiesTag = 0xFFFFFFFD;
    constexpr std::uint32_t StringDataIs8BitFlag = 0x80000000;

    std::uint8_t errorType(std::string_view name) {
      static constexpr std::string_view names[] = {"Error", "EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", "URIError"};
      for (std::uint8_t index = 0; index < 7; ++index)
        if (names[index] == name) return index;
      return 0;
    }

    std::string canonicalFlags(std::string_view flags) {
      std::string out;
      for (const char flag : std::string_view("dgimsuvy"))
        if (flags.find(flag) != std::string_view::npos) out += flag;
      return out;
    }

    class Serializer {
    public:
      KvBytes out;

      void u8(std::uint8_t value) { out.push_back(value); }
      void u16(std::uint16_t value) {
        out.push_back(static_cast<std::uint8_t>(value));
        out.push_back(static_cast<std::uint8_t>(value >> 8));
      }
      void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
      }
      void u64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
      }
      void f64(double value) { u64(std::isnan(value) ? 0x7FF8000000000000ull : std::bit_cast<std::uint64_t>(value)); }

      void poolIndex(std::uint32_t index, std::size_t size) {
        if (size <= 0xFF) u8(static_cast<std::uint8_t>(index));
        else if (size <= 0xFFFF) u16(static_cast<std::uint16_t>(index));
        else u32(index);
      }

      // CloneSerializer::write(const String&): atomized into the constant pool.
      void identifier(std::string_view text) {
        std::u16string units = kv_internal::utf16Units(text);
        if (const auto found = pool_.find(units); found != pool_.end()) {
          u32(StringPoolTag);
          poolIndex(found->second, pool_.size());
          return;
        }
        const auto index = static_cast<std::uint32_t>(pool_.size());
        const auto length = static_cast<std::uint32_t>(units.size());
        const bool is8Bit = std::all_of(units.begin(), units.end(), [](char16_t unit) { return unit <= 0xFF; });
        u32(length | (is8Bit ? StringDataIs8BitFlag : 0));
        for (const char16_t unit : units) {
          if (is8Bit) u8(static_cast<std::uint8_t>(unit));
          else u16(static_cast<std::uint16_t>(unit));
        }
        pool_.emplace(std::move(units), index);
      }

      void nullableString(const std::optional<std::string>& text) {
        u32(text ? 0 : 1);
        if (text) identifier(*text);
      }

      void number(double value) {
        if (std::isfinite(value) && value == std::trunc(value) && value >= -2147483648.0 && value <= 2147483647.0 && !(value == 0 && std::signbit(value))) {
          const auto integer = static_cast<std::int32_t>(value);
          if (integer == 0) u8(ZeroTag);
          else if (integer == 1) u8(OneTag);
          else {
            u8(IntTag);
            u32(static_cast<std::uint32_t>(integer));
          }
          return;
        }
        u8(DoubleTag);
        f64(value);
      }

      void bigint(const BigInt& value) {
        const KvBytes magnitude = kv_internal::bigintMagnitude(value);
        const std::size_t words = (magnitude.size() + 7) / 8;
        u8(value.isNegative() ? 1 : 0);
        u32(static_cast<std::uint32_t>(words));
        for (std::size_t word = 0; word < words; ++word) {
          std::uint64_t bits = 0;
          for (std::size_t byte = 0; byte < 8; ++byte) {
            const std::size_t fromEnd = word * 8 + byte;
            if (fromEnd < magnitude.size()) bits |= static_cast<std::uint64_t>(magnitude[magnitude.size() - 1 - fromEnd]) << (8 * byte);
          }
          u64(bits);
        }
      }

      void arrayBuffer(std::span<const std::uint8_t> bytes) {
        u8(ArrayBufferTag);
        u64(bytes.size());
        out.insert(out.end(), bytes.begin(), bytes.end());
      }

      void view(const KvValue& input, const void* identity, KvViewKind kind, std::span<const std::uint8_t> buffer, std::uint64_t byteOffset,
                std::uint64_t byteLength) {
        if (byteOffset > buffer.size() || byteLength > buffer.size() - byteOffset) throw RangeError("KV value has an ArrayBuffer view outside its buffer");
        u8(ArrayBufferViewTag);
        u8(static_cast<std::uint8_t>(kind));
        u64(byteOffset);
        u64(byteLength);
        if (const KvValue* backing = identity ? input.backingBuffer() : nullptr) value(*backing);
        else {
          record(nullptr);
          arrayBuffer(buffer);
        }
        // The backing buffer is entered into the object table before its view.
        record(identity);
      }

      void value(const KvValue& input, std::size_t depth = 0) {
        const void* identity = input.identity();
        if (identity) {
          if (!objectIndices_) objectIndices_.emplace();
          const auto found = objectIndices_->find(identity);
          if (found != objectIndices_->end()) {
            u8(ObjectReferenceTag);
            poolIndex(found->second, objectCount_);
            return;
          }
        }
        std::visit(
          [&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, KvU64> || std::is_same_v<T, KvArray> || std::is_same_v<T, KvObject> ||
                          std::is_same_v<T, KvMap> || std::is_same_v<T, KvSet>)
              kv_detail::checkContainerDepth(depth);
            if constexpr (std::is_same_v<T, KvUndefined> || std::is_same_v<T, KvHole>) u8(UndefinedTag);
            else if constexpr (std::is_same_v<T, std::nullptr_t>) u8(NullTag);
            else if constexpr (std::is_same_v<T, bool>) u8(item ? TrueTag : FalseTag);
            else if constexpr (std::is_same_v<T, double>) number(item);
            else if constexpr (std::is_same_v<T, std::string>) {
              if (item.empty()) u8(EmptyStringTag);
              else {
                u8(StringTag);
                identifier(item);
              }
            } else if constexpr (std::is_same_v<T, BigInt>) {
              u8(BigIntTag);
              bigint(item);
            } else if constexpr (std::is_same_v<T, KvU64>) {
              record(identity);
              u8(ObjectTag);
              identifier("value");
              u8(BigIntTag);
              bigint(item.toBigInt());
              u32(TerminatorTag);
            } else if constexpr (std::is_same_v<T, KvBytes>) view(input, identity, KvViewKind::Uint8Array, item, 0, item.size());
            else if constexpr (std::is_same_v<T, KvArrayBuffer>) {
              record(identity);
              arrayBuffer(item.bytes);
            } else if constexpr (std::is_same_v<T, KvArrayBufferView>) view(input, identity, item.kind, item.buffer, item.byteOffset, item.byteLength);
            else if constexpr (std::is_same_v<T, KvDate>) {
              record(identity);
              u8(DateTag);
              f64(item.time);
            } else if constexpr (std::is_same_v<T, KvRegExp>) {
              record(identity);
              u8(RegExpTag);
              identifier(item.source);
              identifier(canonicalFlags(item.flags));
            } else if constexpr (std::is_same_v<T, KvError>) {
              record(identity);
              u8(ErrorInstanceTag);
              u8(errorType(item.name));
              nullableString(item.message);
              u32(item.line);
              u32(item.column);
              nullableString(item.sourceURL);
              nullableString(item.stack);
            } else if constexpr (std::is_same_v<T, KvArray>) {
              record(identity);
              u8(ArrayTag);
              u32(static_cast<std::uint32_t>(item.size()));
              std::size_t index = item.nextPresent(0);
              for (; index < item.size() && index != NonIndexPropertiesTag; index = item.nextPresent(index + 1)) {
                u32(static_cast<std::uint32_t>(index));
                value(item[index], depth + 1);
              }
              if (index < item.size()) {
                // This valid index collides with the marker when written as a raw u32.
                u32(NonIndexPropertiesTag);
                for (; index < item.size(); index = item.nextPresent(index + 1)) {
                  identifier(std::to_string(index));
                  value(item[index], depth + 1);
                }
              }
              u32(TerminatorTag);
            } else if constexpr (std::is_same_v<T, KvObject>) {
              record(identity);
              u8(ObjectTag);
              for (const auto& [key, member] : item) {
                identifier(key);
                value(member, depth + 1);
              }
              u32(TerminatorTag);
            } else if constexpr (std::is_same_v<T, KvMap>) {
              record(identity);
              u8(MapObjectTag);
              for (const auto& [key, member] : item) {
                value(key, depth + 1);
                value(member, depth + 1);
              }
              u8(NonMapPropertiesTag);
              u32(TerminatorTag);
            } else if constexpr (std::is_same_v<T, KvSet>) {
              record(identity);
              u8(SetObjectTag);
              for (const KvValue& member : item) value(member, depth + 1);
              u8(NonSetPropertiesTag);
              u32(TerminatorTag);
            } else std::terminate();
          },
          input.variant());
      }

    private:
      void record(const void* identity) {
        if (identity) objectIndices_->emplace(identity, objectCount_);
        ++objectCount_;
      }

      struct UnitsHash {
        std::size_t operator()(const std::u16string& units) const noexcept { return std::hash<std::u16string>{}(units); }
      };
      std::unordered_map<std::u16string, std::uint32_t, UnitsHash> pool_;
      std::optional<std::unordered_map<const void*, std::uint32_t>> objectIndices_;
      std::uint32_t objectCount_ = 0;
    };
  }

  KvBytes kvJscSerialize(const KvValue& value) {
    Serializer serializer;
    // The version and a fixed-width scalar fit without growing the buffer.
    serializer.out.reserve(16);
    serializer.u32(CurrentVersion);
    serializer.value(value);
    return std::move(serializer.out);
  }
}
