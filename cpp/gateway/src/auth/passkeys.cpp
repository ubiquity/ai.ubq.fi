#include "uos/auth/passkeys.hpp"

#include "uos/auth/relay.hpp"
#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/request.hpp"
#include "uos/utils.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <qcbor/qcbor_decode.h>
#include <qcbor/qcbor_encode.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <variant>

namespace uos::auth::passkeys {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    using Bytes = std::vector<std::uint8_t>;
    using BytesView = std::span<const std::uint8_t>;

    // ===================================================================================
    // @simplewebauthn/server 13.2.2 and the libraries used here (@hexagon/base64 1.1.28,
    // QCBOR 1.6.1, asn1js 3.0.10 under @peculiar/asn1-schema 2.11.0,
    // @peculiar/x509) as far as passkeys.ts reaches them.
    // ===================================================================================

    [[noreturn]] void fail(std::string message) { throw rt::JsError(std::move(message)); }

    // A named Error subclass of the library (`class X extends Error { name = "X" }`).
    [[noreturn]] void failNamed(std::string name, std::string message) { throw rt::JsError(std::move(message), std::move(name)); }

    Bytes concatBytes(std::initializer_list<BytesView> parts) {
      Bytes out;
      for (BytesView part : parts) out.insert(out.end(), part.begin(), part.end());
      return out;
    }

    bool bytesEqual(BytesView a, BytesView b) { return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin()); }

    // isoUint8Array.toHex
    std::string toHex(BytesView bytes) { return rt::toHex(bytes); }

    // isoUint8Array.fromASCIIString: `Uint8Array.from(value.split("").map((x) => x.charCodeAt(0)))`
    // keeps the low byte of each UTF-16 code unit.
    Bytes fromASCIIString(std::string_view value) {
      Bytes out;
      for (char16_t unit : rt::toUtf16(value)) out.push_back(static_cast<std::uint8_t>(unit & 0xFF));
      return out;
    }

    // ---- @hexagon/base64 -------------------------------------------------------------

    namespace hexagon {
      constexpr std::string_view CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      constexpr std::string_view CHARS_URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

      std::array<std::uint8_t, 256> genLookup(std::string_view target) {
        std::array<std::uint8_t, 256> table{};
        for (std::size_t i = 0; i < CHARS.size(); ++i) table[static_cast<unsigned char>(target[i])] = static_cast<std::uint8_t>(i);
        return table;
      }

      // `base64.toArrayBuffer(data, urlMode)`: a character outside the table (or past the end)
      // reads as 0, the buffer length is `data.length * 0.75` minus the padding, truncated, and
      // writes past it are dropped.
      Bytes toArrayBuffer(std::string_view data, bool urlMode) {
        static const std::array<std::uint8_t, 256> lookup = genLookup(CHARS);
        static const std::array<std::uint8_t, 256> lookupUrl = genLookup(CHARS_URL);
        const std::array<std::uint8_t, 256>& target = urlMode ? lookupUrl : lookup;
        const std::u16string units = rt::toUtf16(data);
        const std::size_t len = units.size();
        double bufferLength = static_cast<double>(len) * 0.75;
        if (len >= 1 && units[len - 1] == u'=') {
          bufferLength--;
          if (len >= 2 && units[len - 2] == u'=') bufferLength--;
        }
        const std::size_t size = bufferLength <= 0 ? 0 : static_cast<std::size_t>(std::floor(bufferLength));
        Bytes bytes(size);
        const auto at = [&](std::size_t index) -> std::uint32_t {
          if (index >= len) return 0;
          const char16_t unit = units[index];
          return unit < 256 ? target[unit] : 0;
        };
        std::size_t p = 0;
        const auto put = [&](std::uint32_t value) {
          if (p < size) bytes[p] = static_cast<std::uint8_t>(value & 0xFF);
          ++p;
        };
        for (std::size_t i = 0; i < len; i += 4) {
          const std::uint32_t encoded1 = at(i);
          const std::uint32_t encoded2 = at(i + 1);
          const std::uint32_t encoded3 = at(i + 2);
          const std::uint32_t encoded4 = at(i + 3);
          put((encoded1 << 2) | (encoded2 >> 4));
          put(((encoded2 & 15) << 4) | (encoded3 >> 2));
          put(((encoded3 & 3) << 6) | (encoded4 & 63));
        }
        return bytes;
      }

      // `base64.fromArrayBuffer(bytes, urlMode)`: padded base64, or unpadded base64url.
      std::string fromArrayBuffer(BytesView bytes, bool urlMode) {
        const std::string_view target = urlMode ? CHARS_URL : CHARS;
        std::string result;
        const std::size_t len = bytes.size();
        const auto at = [&](std::size_t index) -> std::uint32_t { return index < len ? bytes[index] : 0; };
        for (std::size_t i = 0; i < len; i += 3) {
          result += target[at(i) >> 2];
          result += target[((at(i) & 3) << 4) | (at(i + 1) >> 4)];
          result += target[((at(i + 1) & 15) << 2) | (at(i + 2) >> 6)];
          result += target[at(i + 2) & 63];
        }
        const std::size_t remainder = len % 3;
        if (remainder == 2) result = result.substr(0, result.size() - 1) + (urlMode ? "" : "=");
        else if (remainder == 1) result = result.substr(0, result.size() - 2) + (urlMode ? "" : "==");
        return result;
      }

      // `/^[-A-Za-z0-9\-_]*$/` (url) and `/^[-A-Za-z0-9+/]*={0,3}$/`.
      bool validate(std::string_view encoded, bool urlMode) {
        const auto alnum = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; };
        if (urlMode) return std::all_of(encoded.begin(), encoded.end(), [&](char c) { return alnum(c) || c == '_'; });
        std::size_t end = encoded.size();
        std::size_t padding = 0;
        while (end > 0 && encoded[end - 1] == '=' && padding < 3) {
          --end;
          ++padding;
        }
        return std::all_of(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(end), [&](char c) { return alnum(c) || c == '+' || c == '/'; });
      }
    }

    // ---- isoBase64URL ------------------------------------------------------------------

    namespace isoBase64URL {
      Bytes toBuffer(std::string_view base64urlString, bool fromBase64Url = true) { return hexagon::toArrayBuffer(base64urlString, fromBase64Url); }
      std::string fromBuffer(BytesView buffer, bool toBase64Url = true) { return hexagon::fromArrayBuffer(buffer, toBase64Url); }
      std::string toBase64(std::string_view base64urlString) { return hexagon::fromArrayBuffer(hexagon::toArrayBuffer(base64urlString, true), false); }
      std::string toUTF8String(std::string_view base64urlString) { return rt::decodeUtf8(BytesView(hexagon::toArrayBuffer(base64urlString, true))); }
      // `input.replace(/=/g, "")`
      std::string trimPadding(std::string_view input) {
        std::string out;
        for (char c : input)
          if (c != '=') out += c;
        return out;
      }
      bool isBase64(std::string_view input) { return hexagon::validate(input, false); }
      bool isBase64URL(std::string_view input) { return hexagon::validate(trimPadding(input), true); }
    }

    // `isoBase64URL.isBase64URL(value)` on an unknown JSON member: `trimPadding` calls
    // `input.replace`, so anything but a string throws JavaScriptCore's TypeError.
    bool isBase64URLValue(const json::value* value) {
      if (value == nullptr) throw rt::TypeError("undefined is not an object (evaluating 'input.replace')");
      if (value->is_null()) throw rt::TypeError("null is not an object (evaluating 'input.replace')");
      if (!value->is_string()) throw rt::TypeError("input.replace is not a function. (In 'input.replace(/=/g, \"\")', 'input.replace' is undefined)");
      return isoBase64URL::isBase64URL(value->as_string());
    }

    // ---- WebAuthn CBOR (QCBOR) ---------------------------------------------------------

    using CborKey = std::variant<std::int64_t, std::string>;
    struct CborEntry;

    // QCBOR streams items. The verifier needs owning values across asynchronous calls.
    struct Cbor {
      using Array = std::vector<Cbor>;
      using Map = std::vector<CborEntry>;
      enum class Kind { undefined_, null_, boolean, number, string, bytes, array, map };
      std::variant<std::monostate, std::nullptr_t, bool, double, std::string, Bytes, Array, Map> data;

      Kind kind() const noexcept { return static_cast<Kind>(data.index()); }
      bool boolean() const { return std::get<bool>(data); }
      double number() const { return std::get<double>(data); }
      const std::string& text() const { return std::get<std::string>(data); }
      const Bytes& bytes() const { return std::get<Bytes>(data); }
      const Array& items() const { return std::get<Array>(data); }
      const Map& entries() const { return std::get<Map>(data); }
      const Cbor* get(std::int64_t key) const;
      const Cbor* get(std::string_view key) const;
    };

    struct CborEntry {
      CborKey key;
      Cbor value;
    };

    template <class Label, class Key> const Cbor* getCborMember(const Cbor& map, Key key) {
      if (map.kind() != Cbor::Kind::map) return nullptr;
      for (const CborEntry& entry : map.entries()) {
        const Label* label = std::get_if<Label>(&entry.key);
        if (label != nullptr && *label == key) return &entry.value;
      }
      return nullptr;
    }

    const Cbor* Cbor::get(std::int64_t key) const { return getCborMember<std::int64_t>(*this, key); }
    const Cbor* Cbor::get(std::string_view key) const { return getCborMember<std::string>(*this, key); }

    bool truthy(const Cbor* value) {
      if (value == nullptr) return false;
      switch (value->kind()) {
        case Cbor::Kind::undefined_:
        case Cbor::Kind::null_: return false;
        case Cbor::Kind::boolean: return value->boolean();
        case Cbor::Kind::number: return value->number() != 0 && !std::isnan(value->number());
        case Cbor::Kind::string: return !value->text().empty();
        default: return true;
      }
    }

    // Diagnostic and coercion helpers used by the attestation verifier.
    std::string jsString(const Cbor& value) {
      switch (value.kind()) {
        case Cbor::Kind::undefined_: return "undefined";
        case Cbor::Kind::null_: return "null";
        case Cbor::Kind::boolean: return value.boolean() ? "true" : "false";
        case Cbor::Kind::number: return rt::numberToString(value.number());
        case Cbor::Kind::string: return value.text();
        case Cbor::Kind::bytes: {
          std::string out;
          for (std::uint8_t byte : value.bytes()) out += (out.empty() ? "" : ",") + std::to_string(byte);
          return out;
        }
        case Cbor::Kind::array: {
          std::string out;
          for (std::size_t i = 0; i < value.items().size(); ++i) {
            if (i != 0) out += ",";
            const Cbor& item = value.items()[i];
            if (item.kind() != Cbor::Kind::undefined_ && item.kind() != Cbor::Kind::null_) out += jsString(item);
          }
          return out;
        }
        case Cbor::Kind::map: return "[object Map]";
      }
      return "";
    }

    std::string jsString(const Cbor* value) { return value == nullptr ? "undefined" : jsString(*value); }

    namespace isoCBOR {
      constexpr std::size_t maxInput = 1024 * 1024;
      constexpr int maxDepth = 8;
      constexpr std::int64_t maxInteger = 9007199254740991;
      constexpr std::int64_t minInteger = -maxInteger - 1;

      struct Decoded {
        Cbor value;
        std::size_t consumed;
      };

      [[noreturn]] void invalid(std::string_view reason) { fail("Invalid passkey CBOR (" + std::string(reason) + ")"); }

      void checkWidth(unsigned additional, std::uint64_t argument) {
        constexpr std::array<std::uint64_t, 4> minimum{24, 256, 65536, 4294967296};
        if (additional >= 24 && argument < minimum[additional - 24]) invalid("non-minimal argument");
      }

      Cbor readValue(QCBORDecodeContext& decoder, BytesView input, bool mapKey = false) {
        const std::size_t start = QCBORDecode_Tell(&decoder);
        if (start >= input.size()) invalid("truncated item");
        const unsigned head = input[start];
        const unsigned major = head >> 5;
        const unsigned additional = head & 31;
        if (additional >= 28 || major == 6 || (major == 7 && head != 0xf4 && head != 0xf5)) invalid("unsupported type");
        if (mapKey && major != 0 && major != 1 && major != 3) invalid("unsupported map key");

        QCBORItem item;
        if (QCBORDecode_GetNext(&decoder, &item) != QCBOR_SUCCESS) invalid("malformed item");
        switch (item.uDataType) {
          case QCBOR_TYPE_INT64: {
            const std::int64_t n = item.val.int64;
            if (n < minInteger || n > maxInteger) invalid("integer outside the supported range");
            checkWidth(additional, static_cast<std::uint64_t>(n < 0 ? -1 - n : n));
            return Cbor{static_cast<double>(n)};
          }
          case QCBOR_TYPE_FALSE: return Cbor{false};
          case QCBOR_TYPE_TRUE: return Cbor{true};
          case QCBOR_TYPE_BYTE_STRING: {
            const auto* bytes = static_cast<const std::uint8_t*>(item.val.string.ptr);
            checkWidth(additional, item.val.string.len);
            BytesView view(bytes, item.val.string.len);
            return Cbor{Bytes(view.begin(), view.end())};
          }
          case QCBOR_TYPE_TEXT_STRING: {
            const auto* chars = static_cast<const char*>(item.val.string.ptr);
            const std::string_view text(chars, item.val.string.len);
            checkWidth(additional, text.size());
            if (!rt::isUtf8(text)) invalid("invalid UTF-8");
            // The previous decoder stripped a leading BOM before comparing keys.
            if (mapKey && text.starts_with("\xef\xbb\xbf")) invalid("BOM in map key");
            return Cbor{std::string(text)};
          }
          case QCBOR_TYPE_ARRAY:
          case QCBOR_TYPE_MAP_AS_ARRAY: {
            if (item.uNestingLevel >= maxDepth) invalid("nesting limit");
            const bool isMap = item.uDataType == QCBOR_TYPE_MAP_AS_ARRAY;
            const std::size_t count = item.val.uCount;
            checkWidth(additional, isMap ? count / 2 : count);
            if (count > input.size() - QCBORDecode_Tell(&decoder)) invalid("truncated container");
            if (!isMap) {
              Cbor::Array values;
              values.reserve(count);
              for (std::size_t i = 0; i < count; ++i) values.push_back(readValue(decoder, input));
              return Cbor{std::move(values)};
            }

            Cbor::Map entries;
            entries.reserve(count / 2);
            BytesView previous;
            for (std::size_t i = 0; i < count / 2; ++i) {
              const std::size_t keyStart = QCBORDecode_Tell(&decoder);
              Cbor key = readValue(decoder, input, true);
              const BytesView encodedKey = input.subspan(keyStart, QCBORDecode_Tell(&decoder) - keyStart);
              // For the admitted key types, CTAP2 order is lexicographic over canonical encodings.
              if (!previous.empty() && !std::lexicographical_compare(previous.begin(), previous.end(), encodedKey.begin(), encodedKey.end()))
                invalid("duplicate or out-of-order map key");
              previous = encodedKey;
              CborKey label = key.kind() == Cbor::Kind::number ? CborKey{static_cast<std::int64_t>(key.number())} :
                                                               CborKey{std::get<std::string>(std::move(key.data))};
              entries.push_back(CborEntry{std::move(label), readValue(decoder, input)});
            }
            return Cbor{std::move(entries)};
          }
          default: invalid("unsupported item");
        }
      }

      Decoded decodeItem(BytesView input, bool requireComplete = false) {
        if (input.size() > maxInput) invalid("input limit");
        QCBORDecodeContext decoder;
        QCBORDecode_Init(&decoder, {input.data(), input.size()}, QCBOR_DECODE_MODE_MAP_AS_ARRAY);
        Cbor value = readValue(decoder, input);
        std::size_t consumed = 0;
        const QCBORError result = QCBORDecode_PartialFinish(&decoder, &consumed);
        if (result != QCBOR_SUCCESS && result != QCBOR_ERR_EXTRA_BYTES) invalid("incomplete item");
        if (requireComplete && result == QCBOR_ERR_EXTRA_BYTES) invalid("trailing bytes");
        return {std::move(value), consumed};
      }

      Cbor decodeFirst(BytesView input) { return decodeItem(input, true).value; }

      void addText(QCBOREncodeContext& encoder, std::string_view text) { QCBOREncode_AddText(&encoder, {text.data(), text.size()}); }

      void addValue(QCBOREncodeContext& encoder, const Cbor& value) {
        switch (value.kind()) {
          case Cbor::Kind::boolean: QCBOREncode_AddBool(&encoder, value.boolean()); return;
          case Cbor::Kind::number: {
            const double number = value.number();
            if (!(number >= static_cast<double>(minInteger) && number <= static_cast<double>(maxInteger)) || std::trunc(number) != number)
              invalid("unsupported number for encoding");
            QCBOREncode_AddInt64(&encoder, static_cast<std::int64_t>(number));
            return;
          }
          case Cbor::Kind::string: addText(encoder, value.text()); return;
          case Cbor::Kind::bytes: QCBOREncode_AddBytes(&encoder, {value.bytes().data(), value.bytes().size()}); return;
          case Cbor::Kind::array:
            QCBOREncode_OpenArray(&encoder);
            for (const Cbor& item : value.items()) addValue(encoder, item);
            QCBOREncode_CloseArray(&encoder);
            return;
          case Cbor::Kind::map:
            QCBOREncode_OpenMap(&encoder);
            for (const CborEntry& entry : value.entries()) {
              if (const auto* label = std::get_if<std::int64_t>(&entry.key)) QCBOREncode_AddInt64(&encoder, *label);
              else addText(encoder, std::get<std::string>(entry.key));
              addValue(encoder, entry.value);
            }
            QCBOREncode_CloseMap(&encoder);
            return;
          default: invalid("unsupported value for encoding");
        }
      }

      // A validated item already has minimal widths and sorted labels. The encoded size is exact.
      Bytes encode(const Decoded& decoded) {
        Bytes output(decoded.consumed);
        QCBOREncodeContext encoder;
        QCBOREncode_Init(&encoder, {output.data(), output.size()});
        addValue(encoder, decoded.value);
        UsefulBufC encoded;
        if (QCBOREncode_Finish(&encoder, &encoded) != QCBOR_SUCCESS || encoded.len != output.size()) invalid("encoding failed");
        return output;
      }
    }
  }

  namespace cbor {
    std::pair<Bytes, std::size_t> reencode(BytesView input, bool requireComplete) {
      const auto decoded = isoCBOR::decodeItem(input, requireComplete);
      return {isoCBOR::encode(decoded), decoded.consumed};
    }
  }

  namespace {
    // ---- asn1js (BER decoding as @peculiar/asn1-schema's AsnParser.parse drives it) -----

    namespace asn1 {
      // The asn1js block class a tag decodes to; it picks the value decoder.
      enum class Type {
        EndOfContent, Boolean, Integer, BitString, OctetString, Null, ObjectIdentifier, Enumerated, Utf8String, RelativeObjectIdentifier, TIME,
        Sequence, Set, NumericString, PrintableString, TeletexString, VideotexString, IA5String, UTCTime, GeneralizedTime, GraphicString,
        VisibleString, GeneralString, UniversalString, CharacterString, BmpString, DATE, TimeOfDay, DateTime, Duration, Primitive, Constructed
      };

      struct Node {
        Type type = Type::Primitive;
        int tagClass = -1;  // 1 universal, 2 application, 3 context-specific, 4 private
        double tagNumber = -1;
        bool isHexOnly = false;
        bool isConstructed = false;
        bool isIndefiniteForm = false;
        double length = 0;
        double blockLength = 0;
        std::string error;
        Bytes valueHex;           // primitive content (valueBlock.valueHexView)
        std::vector<Node> value;  // constructed content
        int unusedBits = 0;       // BIT STRING
        std::string text;         // string types: the decoded value
        double year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, millisecond = 0;  // UTCTime / GeneralizedTime
        Bytes raw;                // valueBeforeDecode: the whole encoding
      };

      struct Context {
        int depth = 0;
        int maxDepth = 100;
        int nodesCount = 0;
        int maxNodes = 10000;
        double maxContentLength = 16 * 1024 * 1024;
      };

      struct Result {
        double offset;
        Node result;
      };

      bool checkBufferParams(std::string& error, BytesView inputBuffer, double inputOffset, double inputLength) {
        if (inputBuffer.empty()) {
          error = "Wrong parameter: inputBuffer has zero length";
          return false;
        }
        if (inputOffset < 0) {
          error = "Wrong parameter: inputOffset less than zero";
          return false;
        }
        if (inputLength < 0) {
          error = "Wrong parameter: inputLength less than zero";
          return false;
        }
        if (static_cast<double>(inputBuffer.size()) - inputOffset - inputLength < 0) {
          error = "End of input reached before message was fully decoded (inconsistent offset and length values)";
          return false;
        }
        return true;
      }

      BytesView sub(BytesView buffer, double offset, double length) {
        return buffer.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
      }

      // `pvutils.utilFromBase(bytes, bits)`
      double fromBase(BytesView bytes, int bits) {
        double out = 0;
        for (std::uint8_t byte : bytes) out = out * std::pow(2.0, bits) + byte;
        return out;
      }

      Result localFromBER(BytesView inputBuffer, double inputOffset, double inputLength, Context& context);

      Result localFromBERWithChildContext(BytesView inputBuffer, double inputOffset, double inputLength, Context& context) {
        const int childDepth = context.depth + 1;
        if (childDepth > context.maxDepth) {
          Node failed;
          failed.error = "Maximum ASN.1 nesting depth exceeded";
          return {-1, std::move(failed)};
        }
        context.depth = childDepth;
        struct Restore {
          Context& context;
          ~Restore() { context.depth -= 1; }
        } restore{context};
        return localFromBER(inputBuffer, inputOffset, inputLength, context);
      }

      // LocalConstructedValueBlock.fromBER; returns the offset or -1 with node.error set.
      double constructedValueFromBER(Node& node, BytesView view, double inputOffset, double inputLength, Context& context, double& valueBlockLength) {
        if (!checkBufferParams(node.error, view, inputOffset, inputLength)) return -1;
        if (inputLength == 0) return inputOffset;  // "Zero buffer length" warning
        double currentOffset = inputOffset;
        while ((node.isIndefiniteForm ? 1 : inputLength) > 0) {
          Result returnObject = localFromBERWithChildContext(view, currentOffset, inputLength, context);
          if (returnObject.offset == -1) {
            node.error = returnObject.result.error;
            return -1;
          }
          currentOffset = returnObject.offset;
          valueBlockLength += returnObject.result.blockLength;
          inputLength -= returnObject.result.blockLength;
          const bool endOfContent = returnObject.result.type == Type::EndOfContent;
          node.value.push_back(std::move(returnObject.result));
          if (node.isIndefiniteForm && endOfContent) break;
        }
        if (node.isIndefiniteForm && !node.value.empty() && node.value.back().type == Type::EndOfContent) node.value.pop_back();
        return currentOffset;
      }

      // HexBlock.fromBER
      double hexValueFromBER(Node& node, BytesView view, double inputOffset, double inputLength, double& valueBlockLength) {
        if (!checkBufferParams(node.error, view, inputOffset, inputLength)) return -1;
        const BytesView content = sub(view, inputOffset, inputLength);
        node.valueHex.assign(content.begin(), content.end());
        if (content.empty()) return inputOffset;  // "Zero buffer length" warning
        valueBlockLength = inputLength;
        return inputOffset + inputLength;
      }

      // `String.fromCharCode.apply(null, bytes)`
      std::string latin1(BytesView bytes) { return rt::binaryString(bytes); }

      void decodeUtcTime(Node& node) {
        // `/(\d{2})(\d{2})(\d{2})(\d{2})(\d{2})(\d{2})Z/ig.exec(input)`
        static const rt::Regex parser("(\\d{2})(\\d{2})(\\d{2})(\\d{2})(\\d{2})(\\d{2})Z", "i");
        const std::optional<rt::RegexMatch> parserArray = parser.exec(node.text);
        if (!parserArray) {
          node.error = "Wrong input string for conversion";
          return;
        }
        const double year = rt::parseInt(*(*parserArray)[1], 10);
        node.year = year >= 50 ? 1900 + year : 2000 + year;
        node.month = rt::parseInt(*(*parserArray)[2], 10);
        node.day = rt::parseInt(*(*parserArray)[3], 10);
        node.hour = rt::parseInt(*(*parserArray)[4], 10);
        node.minute = rt::parseInt(*(*parserArray)[5], 10);
        node.second = rt::parseInt(*(*parserArray)[6], 10);
      }

      // GeneralizedTime.fromString: throws Error("Wrong input string for conversion").
      void decodeGeneralizedTime(Node& node) {
        const std::string& inputString = node.text;
        const auto wrong = [] { fail("Wrong input string for conversion"); };
        bool isUTC = false;
        std::string timeString;
        std::string dateTimeString;
        double fractionPart = 0;
        double hourDifference = 0;
        double minuteDifference = 0;
        const double inputLength = static_cast<double>(rt::length(inputString));
        const std::string last = inputLength > 0 ? rt::charAt(inputString, inputLength - 1) : std::string();
        if (last == "Z") {
          timeString = rt::substring(inputString, 0, inputLength - 1);
          isUTC = true;
        } else {
          // `new Number(inputString[inputString.length - 1])`: undefined is NaN.
          if (inputLength == 0 || std::isnan(rt::stringToNumber(last))) wrong();
          timeString = inputString;
        }
        if (isUTC) {
          if (rt::indexOf(timeString, "+") != -1) wrong();
          if (rt::indexOf(timeString, "-") != -1) wrong();
        } else {
          double multiplier = 1;
          double differencePosition = static_cast<double>(rt::indexOf(timeString, "+"));
          std::string differenceString;
          if (differencePosition == -1) {
            differencePosition = static_cast<double>(rt::indexOf(timeString, "-"));
            multiplier = -1;
          }
          if (differencePosition != -1) {
            differenceString = rt::substring(timeString, differencePosition + 1);
            timeString = rt::substring(timeString, 0, differencePosition);
            const auto differenceLength = rt::length(differenceString);
            if (differenceLength != 2 && differenceLength != 4) wrong();
            double number = rt::parseInt(rt::substring(differenceString, 0, 2), 10);
            if (std::isnan(number)) wrong();
            hourDifference = multiplier * number;
            if (differenceLength == 4) {
              number = rt::parseInt(rt::substring(differenceString, 2, 4), 10);
              if (std::isnan(number)) wrong();
              minuteDifference = multiplier * number;
            }
          }
        }
        double fractionPointPosition = static_cast<double>(rt::indexOf(timeString, "."));
        if (fractionPointPosition == -1) fractionPointPosition = static_cast<double>(rt::indexOf(timeString, ","));
        if (fractionPointPosition != -1) {
          const double fractionPartCheck = rt::stringToNumber("0" + rt::substring(timeString, fractionPointPosition));
          if (std::isnan(fractionPartCheck)) wrong();
          fractionPart = fractionPartCheck;
          dateTimeString = rt::substring(timeString, 0, fractionPointPosition);
        } else {
          dateTimeString = timeString;
        }
        const char* pattern = nullptr;
        switch (rt::length(dateTimeString)) {
          case 8:
            pattern = "(\\d{4})(\\d{2})(\\d{2})";
            if (fractionPointPosition != -1) wrong();
            break;
          case 10: {
            pattern = "(\\d{4})(\\d{2})(\\d{2})(\\d{2})";
            if (fractionPointPosition != -1) {
              double fractionResult = 60 * fractionPart;
              node.minute = std::floor(fractionResult);
              fractionResult = 60 * (fractionResult - node.minute);
              node.second = std::floor(fractionResult);
              fractionResult = 1000 * (fractionResult - node.second);
              node.millisecond = std::floor(fractionResult);
            }
            break;
          }
          case 12: {
            pattern = "(\\d{4})(\\d{2})(\\d{2})(\\d{2})(\\d{2})";
            if (fractionPointPosition != -1) {
              double fractionResult = 60 * fractionPart;
              node.second = std::floor(fractionResult);
              fractionResult = 1000 * (fractionResult - node.second);
              node.millisecond = std::floor(fractionResult);
            }
            break;
          }
          case 14: {
            pattern = "(\\d{4})(\\d{2})(\\d{2})(\\d{2})(\\d{2})(\\d{2})";
            if (fractionPointPosition != -1) node.millisecond = std::floor(1000 * fractionPart);
            break;
          }
          default: wrong();
        }
        const rt::Regex parser(pattern, "i");
        const std::optional<rt::RegexMatch> parserArray = parser.exec(dateTimeString);
        if (!parserArray) wrong();
        for (std::size_t j = 1; j < parserArray->captures.size(); j++) {
          const double parsed = rt::parseInt(*(*parserArray)[j], 10);
          switch (j) {
            case 1: node.year = parsed; break;
            case 2: node.month = parsed; break;
            case 3: node.day = parsed; break;
            case 4: node.hour = parsed + hourDifference; break;
            case 5: node.minute = parsed + minuteDifference; break;
            case 6: node.second = parsed; break;
            default: wrong();
          }
        }
        if (!isUTC) {
          // `new Date(year, month, day, ...)` in local time (UTC on the host), then read back with
          // the UTC getters (getUTCDay is the weekday: the library's own quirk).
          const rt::Date tempDate(rt::Date::UTC(node.year, node.month, node.day, node.hour, node.minute, node.second, node.millisecond));
          node.year = tempDate.getUTCFullYear();
          node.month = tempDate.getUTCMonth();
          node.day = tempDate.getUTCDay();
          node.hour = tempDate.getUTCHours();
          node.minute = tempDate.getUTCMinutes();
          node.second = tempDate.getUTCSeconds();
          node.millisecond = tempDate.getUTCMilliseconds();
        }
      }

      // BaseStringBlock.fromBER's `fromBuffer` for each string class.
      void decodeString(Node& node) {
        const BytesView bytes(node.valueHex);
        switch (node.type) {
          case Type::Utf8String:
          case Type::DATE:
          case Type::TimeOfDay:
          case Type::DateTime:
          case Type::Duration:
          case Type::TIME:
            // `decodeURIComponent(escape(binary))`, the raw binary string when that throws.
            if (rt::isUtf8(bytes)) node.text = rt::decodeUtf8(bytes, {.ignoreBOM = true});
            else node.text = latin1(bytes);
            return;
          case Type::BmpString: {
            if (bytes.size() % 2 != 0) throw rt::RangeError("Out of bounds access");
            std::u16string units;
            for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) units.push_back(static_cast<char16_t>((bytes[i] << 8) | bytes[i + 1]));
            node.text = rt::fromUtf16(units);
            return;
          }
          case Type::UniversalString: {
            if (bytes.size() % 4 != 0) throw rt::RangeError("byte length of Uint32Array should be a multiple of 4");
            std::u16string units;
            for (std::size_t i = 0; i < bytes.size(); i += 4) units.push_back(static_cast<char16_t>(bytes[i + 3] | (bytes[i + 2] << 8)));
            node.text = rt::fromUtf16(units);
            return;
          }
          case Type::UTCTime:
            node.text = latin1(bytes);
            decodeUtcTime(node);
            return;
          case Type::GeneralizedTime:
            node.text = latin1(bytes);
            decodeGeneralizedTime(node);
            return;
          default: node.text = latin1(bytes); return;
        }
      }

      // `localFromBERWithChildContext(buf, 0, buf.byteLength, context)` inside try/catch: the
      // OCTET STRING and BIT STRING decoders probe their content for an embedded encoding. Only
      // its side effects on the shared context (node count) are observable.
      void probeEmbedded(BytesView content, Context& context) {
        if (content.empty()) return;
        try {
          (void)localFromBERWithChildContext(content, 0, static_cast<double>(content.size()), context);
        } catch (...) {
        }
      }

      // The class-specific fromBER after the identification and length blocks.
      double valueFromBER(Node& node, BytesView view, double inputOffset, double valueLength, Context& context) {
        const double headerLength = node.blockLength;  // id + length blocks
        double valueBlockLength = 0;
        double resultOffset = -1;
        switch (node.type) {
          case Type::EndOfContent: resultOffset = inputOffset; break;
          case Type::Null: {
            node.blockLength = headerLength + valueLength;
            if (inputOffset + valueLength > static_cast<double>(view.size())) {
              node.error = "End of input reached before message was fully decoded (inconsistent offset and length values)";
              return -1;
            }
            return inputOffset + valueLength;
          }
          case Type::Boolean: {
            if (!checkBufferParams(node.error, view, inputOffset, valueLength)) return -1;
            const BytesView content = sub(view, inputOffset, valueLength);
            node.valueHex.assign(content.begin(), content.end());
            valueBlockLength = valueLength;
            resultOffset = inputOffset + valueLength;
            break;
          }
          case Type::Integer:
          case Type::Enumerated:
          case Type::Primitive: resultOffset = hexValueFromBER(node, view, inputOffset, valueLength, valueBlockLength); break;
          case Type::OctetString: {
            if (valueLength == 0) {
              node.blockLength = headerLength;
              return inputOffset;
            }
            if (!node.isConstructed) probeEmbedded(sub(view, inputOffset, std::min(valueLength, static_cast<double>(view.size()) - inputOffset)), context);
            if (node.isConstructed) {
              resultOffset = constructedValueFromBER(node, view, inputOffset, valueLength, context, valueBlockLength);
              if (resultOffset == -1) return -1;
              for (const Node& child : node.value) {
                if (child.type == Type::EndOfContent) {
                  if (node.isIndefiniteForm) break;
                  node.error = "EndOfContent is unexpected, OCTET STRING may consists of OCTET STRINGs only";
                  return -1;
                }
                if (child.type != Type::OctetString) {
                  node.error = "OCTET STRING may consists of OCTET STRINGs only";
                  return -1;
                }
              }
            } else {
              resultOffset = hexValueFromBER(node, view, inputOffset, valueLength, valueBlockLength);
              valueBlockLength = valueLength;
            }
            break;
          }
          case Type::BitString: {
            if (valueLength == 0) {
              resultOffset = inputOffset;
              break;
            }
            if (node.isConstructed) {
              resultOffset = constructedValueFromBER(node, view, inputOffset, valueLength, context, valueBlockLength);
              if (resultOffset == -1) return -1;
              int unusedBits = 0;
              for (const Node& child : node.value) {
                if (child.type == Type::EndOfContent) {
                  if (node.isIndefiniteForm) break;
                  node.error = "EndOfContent is unexpected, BIT STRING may consists of BIT STRINGs only";
                  return -1;
                }
                if (child.type != Type::BitString) {
                  node.error = "BIT STRING may consists of BIT STRINGs only";
                  return -1;
                }
                if (unusedBits > 0 && child.unusedBits > 0) {
                  node.error = "Using of \"unused bits\" inside constructive BIT STRING allowed for least one only";
                  return -1;
                }
                unusedBits = child.unusedBits;
              }
              node.unusedBits = unusedBits;
              break;
            }
            if (!checkBufferParams(node.error, view, inputOffset, valueLength)) return -1;
            const BytesView content = sub(view, inputOffset, valueLength);
            node.unusedBits = content[0];
            if (node.unusedBits > 7) {
              node.error = "Unused bits for BitString must be in range 0-7";
              return -1;
            }
            if (node.unusedBits == 0) probeEmbedded(content.subspan(1), context);
            node.valueHex.assign(content.begin() + 1, content.end());
            valueBlockLength = valueLength;
            resultOffset = inputOffset + valueLength;
            break;
          }
          case Type::ObjectIdentifier:
          case Type::RelativeObjectIdentifier: {
            resultOffset = inputOffset;
            double remaining = valueLength;
            while (remaining > 0) {
              if (!checkBufferParams(node.error, view, resultOffset, remaining)) return -1;
              const BytesView content = sub(view, resultOffset, remaining);
              std::size_t sidLength = 0;
              for (std::size_t i = 0; i < content.size(); ++i) {
                ++sidLength;
                if ((content[i] & 0x80) == 0) break;
              }
              if ((content[sidLength - 1] & 0x80) != 0) {
                node.error = "End of input reached before message was fully decoded";
                return -1;
              }
              node.valueHex.insert(node.valueHex.end(), content.begin(), content.begin() + static_cast<std::ptrdiff_t>(sidLength));
              valueBlockLength += static_cast<double>(sidLength);
              remaining -= static_cast<double>(sidLength);
              resultOffset += static_cast<double>(sidLength);
            }
            break;
          }
          case Type::Sequence:
          case Type::Set:
          case Type::Constructed: resultOffset = constructedValueFromBER(node, view, inputOffset, valueLength, context, valueBlockLength); break;
          default: {
            // BaseStringBlock.fromBER
            resultOffset = hexValueFromBER(node, view, inputOffset, valueLength, valueBlockLength);
            if (resultOffset == -1) return -1;
            decodeString(node);
            break;
          }
        }
        if (resultOffset == -1) return -1;
        node.blockLength = headerLength + valueBlockLength;
        return resultOffset;
      }

      Result localFromBER(BytesView inputBuffer, double inputOffset, double inputLength, Context& context) {
        const double incomingOffset = inputOffset;
        Node node;
        const auto failed = [&](std::string error) {
          node.error = std::move(error);
          return Result{-1, std::move(node)};
        };
        if (!checkBufferParams(node.error, inputBuffer, inputOffset, inputLength)) return Result{-1, std::move(node)};
        if (inputLength == 0) return failed("Zero buffer length");
        if (++context.nodesCount > context.maxNodes) return failed("Maximum ASN.1 node count exceeded");

        // Identification block
        const BytesView idBuffer = sub(inputBuffer, inputOffset, inputLength);
        const std::uint8_t first = idBuffer[0];
        node.tagClass = (first >> 6) + 1;
        node.isConstructed = (first & 0x20) == 0x20;
        double idBlockLength = 1;
        if ((first & 0x1F) != 0x1F) {
          node.tagNumber = first & 0x1F;
        } else {
          std::size_t count = 0;
          while (true) {
            const std::size_t tagByteIndex = count + 1;
            if (tagByteIndex >= idBuffer.size()) return failed("End of input reached before message was fully decoded");
            count++;
            if ((idBuffer[tagByteIndex] & 0x80) == 0x00) break;
          }
          idBlockLength = static_cast<double>(count + 1);
          if (idBlockLength <= 9) {
            double tagNumber = 0;
            for (std::size_t i = 0; i < count; i++) tagNumber = tagNumber * 128 + (idBuffer[i + 1] & 0x7F);
            node.tagNumber = tagNumber;
          } else {
            node.isHexOnly = true;
          }
        }
        if (node.tagClass == 1 && node.isConstructed && !node.isHexOnly) {
          const double t = node.tagNumber;
          if (t == 1 || t == 2 || t == 5 || t == 6 || t == 9 || t == 13 || t == 14 || t == 23 || t == 24 || t == 31 || t == 32 || t == 33 || t == 34)
            return failed("Constructed encoding used for primitive type");
        }
        inputOffset += idBlockLength;
        inputLength -= idBlockLength;

        // Length block
        if (!checkBufferParams(node.error, inputBuffer, inputOffset, inputLength)) return Result{-1, std::move(node)};
        const BytesView lenBuffer = sub(inputBuffer, inputOffset, inputLength);
        if (lenBuffer.empty()) return failed("Zero buffer length");
        if (lenBuffer[0] == 0xFF) return failed("Length block 0xFF is reserved by standard");
        double lenBlockLength = 1;
        node.isIndefiniteForm = lenBuffer[0] == 0x80;
        if (!node.isIndefiniteForm) {
          if ((lenBuffer[0] & 0x80) == 0) {
            node.length = lenBuffer[0];
          } else {
            const std::size_t count = lenBuffer[0] & 0x7F;
            if (count > 8) return failed("Too big integer");
            if (count + 1 > lenBuffer.size()) return failed("End of input reached before message was fully decoded");
            node.length = fromBase(lenBuffer.subspan(1, count), 8);
            lenBlockLength = static_cast<double>(count + 1);
          }
        }
        inputOffset += lenBlockLength;
        inputLength -= lenBlockLength;
        node.blockLength = idBlockLength + lenBlockLength;

        const double valueLength = node.isIndefiniteForm ? inputLength : node.length;
        if (valueLength > context.maxContentLength) return failed("Maximum ASN.1 content length exceeded");
        if (!node.isConstructed && node.isIndefiniteForm) return failed("Indefinite length form used for primitive encoding form");

        if (node.tagClass == 1) {
          if (node.tagNumber >= 37 && !node.isHexOnly) return failed("UNIVERSAL 37 and upper tags are reserved by ASN.1 standard");
          switch (node.isHexOnly ? -1 : static_cast<int>(node.tagNumber)) {
            case 0:
              if (node.isConstructed && node.length > 0) return failed("Type [UNIVERSAL 0] is reserved");
              node.type = Type::EndOfContent;
              break;
            case 1: node.type = Type::Boolean; break;
            case 2: node.type = Type::Integer; break;
            case 3: node.type = Type::BitString; break;
            case 4: node.type = Type::OctetString; break;
            case 5: node.type = Type::Null; break;
            case 6: node.type = Type::ObjectIdentifier; break;
            case 10: node.type = Type::Enumerated; break;
            case 12: node.type = Type::Utf8String; break;
            case 13: node.type = Type::RelativeObjectIdentifier; break;
            case 14: node.type = Type::TIME; break;
            case 15: return failed("[UNIVERSAL 15] is reserved by ASN.1 standard");
            case 16: node.type = Type::Sequence; break;
            case 17: node.type = Type::Set; break;
            case 18: node.type = Type::NumericString; break;
            case 19: node.type = Type::PrintableString; break;
            case 20: node.type = Type::TeletexString; break;
            case 21: node.type = Type::VideotexString; break;
            case 22: node.type = Type::IA5String; break;
            case 23: node.type = Type::UTCTime; break;
            case 24: node.type = Type::GeneralizedTime; break;
            case 25: node.type = Type::GraphicString; break;
            case 26: node.type = Type::VisibleString; break;
            case 27: node.type = Type::GeneralString; break;
            case 28: node.type = Type::UniversalString; break;
            case 29: node.type = Type::CharacterString; break;
            case 30: node.type = Type::BmpString; break;
            case 31: node.type = Type::DATE; break;
            case 32: node.type = Type::TimeOfDay; break;
            case 33: node.type = Type::DateTime; break;
            case 34: node.type = Type::Duration; break;
            default: node.type = node.isConstructed ? Type::Constructed : Type::Primitive; break;
          }
        } else {
          node.type = node.isConstructed ? Type::Constructed : Type::Primitive;
        }
        const double resultOffset = valueFromBER(node, inputBuffer, inputOffset, valueLength, context);
        if (resultOffset == -1) return Result{-1, std::move(node)};
        const double rawLength = std::min(node.blockLength, static_cast<double>(inputBuffer.size()) - incomingOffset);
        const BytesView raw = sub(inputBuffer, incomingOffset, rawLength);
        node.raw.assign(raw.begin(), raw.end());
        return Result{resultOffset, std::move(node)};
      }

      // `asn1js.fromBER(buffer)`: one top-level element; trailing bytes are ignored.
      Result fromBER(BytesView inputBuffer) {
        if (inputBuffer.empty()) {
          Node node;
          node.error = "Input buffer has zero length";
          return {-1, std::move(node)};
        }
        Context context;
        return localFromBER(inputBuffer, 0, static_cast<double>(inputBuffer.size()), context);
      }

      // AsnParser.parse's first step: throws Error(result.error) when the top element failed.
      Node parse(BytesView data) {
        Result parsed = fromBER(data);
        if (!parsed.result.error.empty()) fail(parsed.result.error);
        return std::move(parsed.result);
      }

      bool is(const Node& node, int tagClass, double tagNumber, bool constructed) {
        return node.tagClass == tagClass && !node.isHexOnly && node.tagNumber == tagNumber && node.isConstructed == constructed;
      }
      bool isUniversal(const Node& node, Type type) { return node.tagClass == 1 && node.type == type; }

      // The ObjectIdentifier converter's dotted string.
      std::string oidString(const Node& node) {
        std::string out;
        std::uint64_t sid = 0;
        std::size_t sidBytes = 0;
        bool firstSid = true;
        const auto flush = [&] {
          if (firstSid && node.type == Type::ObjectIdentifier) {
            if (sid < 40) out += "0." + std::to_string(sid);
            else if (sid < 80) out += "1." + std::to_string(sid - 40);
            else out += "2." + std::to_string(sid - 80);
          } else {
            if (!out.empty()) out += ".";
            out += std::to_string(sid);
          }
          firstSid = false;
          sid = 0;
          sidBytes = 0;
        };
        for (std::uint8_t byte : node.valueHex) {
          sid = (sid << 7) | (byte & 0x7F);
          ++sidBytes;
          if ((byte & 0x80) == 0) flush();
        }
        return out;
      }

      // `Date.UTC(...)` of a parsed UTCTime / GeneralizedTime (`toDate().getTime()`).
      double timeValue(const Node& node) {
        if (node.type == Type::UTCTime) return rt::Date::UTC(node.year, node.month - 1, node.day, node.hour, node.minute, node.second);
        return rt::Date::UTC(node.year, node.month - 1, node.day, node.hour, node.minute, node.second, node.millisecond);
      }
    }

    // AsnParser.parse(signature, ECDSASigValue) with its exact errors; {r, s} as stored.
    struct EcdsaSigValue {
      Bytes r;
      Bytes s;
    };

    EcdsaSigValue parseEcdsaSigValue(BytesView signature) {
      asn1::Node top = asn1::parse(signature);
      // asn1-schema compares a generic constructed (context-class) element against a
      // context-class schema with the element's own tag number.
      int schemaClass = 1;
      double schemaNumber = 16;
      if (top.type == asn1::Type::Constructed) {
        schemaClass = 3;
        schemaNumber = top.tagNumber;
        if (top.isHexOnly) fail("Data does not match to ECDSASigValue ASN1 schema. Error decoding idBlock for ASN.1 schema");
      }
      const std::string mismatch = "Data does not match to ECDSASigValue ASN1 schema.";
      if (top.tagClass != schemaClass || top.tagNumber != schemaNumber || !top.isConstructed || top.isHexOnly) fail(mismatch);
      if (top.value.empty()) fail(mismatch + " Inconsistent object length");
      if (!asn1::is(top.value[0], 1, 2, false)) fail(mismatch);
      if (top.value.size() < 2) fail(mismatch + " Inconsistent length between ASN.1 data and schema");
      if (!asn1::is(top.value[1], 1, 2, false)) fail(mismatch);
      return {.r = top.value[0].valueHex, .s = top.value[1].valueHex};
    }

    // ---- X.509 structures as @peculiar/asn1-x509 parses them ---------------------------

    // One AttributeTypeAndValue: the type OID and the AttributeValue's `toString()`.
    struct NameAttribute {
      std::string type;
      std::string value;
    };
    using RelativeDistinguishedName = std::vector<NameAttribute>;

    struct Extension {
      std::string extnID;
      bool critical = false;
      Bytes extnValue;
    };

    struct Certificate {
      Bytes der;
      Bytes tbsRaw;
      double version = 0;
      Bytes serialNumber;
      std::string signature;  // tbsCertificate.signature.algorithm
      std::vector<RelativeDistinguishedName> issuer;
      Bytes issuerRaw;
      std::vector<RelativeDistinguishedName> subject;
      Bytes subjectRaw;
      double notBefore = 0;
      double notAfter = 0;
      std::string publicKeyAlgorithm;
      std::optional<Bytes> publicKeyParameters;  // the raw encoding of `parameters`
      Bytes subjectPublicKey;
      Bytes subjectPublicKeyInfoRaw;
      std::optional<std::vector<Extension>> extensions;
      std::string signatureAlgorithm;
      Bytes signatureAlgorithmRaw;
      Bytes signatureValue;
    };

    [[noreturn]] void schemaMismatch(std::string_view schema) { fail("Data does not match to " + std::string(schema) + " ASN1 schema."); }

    const asn1::Node& requireChild(const asn1::Node& node, std::size_t index, std::string_view schema) {
      if (index >= node.value.size()) schemaMismatch(schema);
      return node.value[index];
    }

    // AttributeValue.toString(): ia5String, else the hex of anyValue, else the DirectoryString.
    std::string attributeValueString(const asn1::Node& value) {
      if (value.tagClass == 1) {
        switch (value.type) {
          case asn1::Type::IA5String:
          case asn1::Type::BmpString:
          case asn1::Type::PrintableString:
          case asn1::Type::TeletexString:
          case asn1::Type::UniversalString:
          case asn1::Type::Utf8String: return value.text;
          default: break;
        }
      }
      return toHex(value.raw);
    }

    std::vector<RelativeDistinguishedName> parseName(const asn1::Node& name, std::string_view schema) {
      if (!asn1::isUniversal(name, asn1::Type::Sequence)) schemaMismatch(schema);
      std::vector<RelativeDistinguishedName> out;
      for (const asn1::Node& set : name.value) {
        if (!asn1::isUniversal(set, asn1::Type::Set)) schemaMismatch("RelativeDistinguishedName");
        RelativeDistinguishedName rdn;
        for (const asn1::Node& attribute : set.value) {
          if (!asn1::isUniversal(attribute, asn1::Type::Sequence) || attribute.value.size() < 2 ||
              !asn1::isUniversal(attribute.value[0], asn1::Type::ObjectIdentifier))
            schemaMismatch("AttributeTypeAndValue");
          rdn.push_back(NameAttribute{.type = asn1::oidString(attribute.value[0]), .value = attributeValueString(attribute.value[1])});
        }
        out.push_back(std::move(rdn));
      }
      return out;
    }

    double parseTime(const asn1::Node& time) {
      if (!(asn1::isUniversal(time, asn1::Type::UTCTime) || asn1::isUniversal(time, asn1::Type::GeneralizedTime))) schemaMismatch("Time");
      return asn1::timeValue(time);
    }

    std::pair<std::string, std::optional<Bytes>> parseAlgorithmIdentifier(const asn1::Node& node) {
      if (!asn1::isUniversal(node, asn1::Type::Sequence) || node.value.empty() || !asn1::isUniversal(node.value[0], asn1::Type::ObjectIdentifier))
        schemaMismatch("AlgorithmIdentifier");
      std::optional<Bytes> parameters;
      if (node.value.size() > 1) parameters = node.value[1].raw;
      return {asn1::oidString(node.value[0]), std::move(parameters)};
    }

    std::vector<Extension> parseExtensions(const asn1::Node& sequence) {
      if (!asn1::isUniversal(sequence, asn1::Type::Sequence)) schemaMismatch("Extensions");
      std::vector<Extension> out;
      for (const asn1::Node& item : sequence.value) {
        if (!asn1::isUniversal(item, asn1::Type::Sequence) || item.value.empty() || !asn1::isUniversal(item.value[0], asn1::Type::ObjectIdentifier))
          schemaMismatch("Extension");
        Extension extension;
        extension.extnID = asn1::oidString(item.value[0]);
        std::size_t index = 1;
        if (index < item.value.size() && asn1::isUniversal(item.value[index], asn1::Type::Boolean)) {
          extension.critical = std::any_of(item.value[index].valueHex.begin(), item.value[index].valueHex.end(), [](std::uint8_t b) { return b != 0; });
          ++index;
        }
        if (index >= item.value.size() || !asn1::isUniversal(item.value[index], asn1::Type::OctetString)) schemaMismatch("Extension");
        extension.extnValue = item.value[index].valueHex;
        out.push_back(std::move(extension));
      }
      return out;
    }

    // `AsnParser.parse(bytes, Certificate)`
    Certificate parseCertificate(BytesView bytes) {
      const asn1::Node top = asn1::parse(bytes);
      if (!asn1::isUniversal(top, asn1::Type::Sequence) || top.value.size() < 3) schemaMismatch("Certificate");
      const asn1::Node& tbs = top.value[0];
      if (!asn1::isUniversal(tbs, asn1::Type::Sequence)) schemaMismatch("Certificate");
      Certificate certificate;
      certificate.der = top.raw;
      certificate.tbsRaw = tbs.raw;
      std::size_t index = 0;
      if (index < tbs.value.size() && asn1::is(tbs.value[index], 3, 0, true)) {
        const asn1::Node& wrapper = tbs.value[index];
        if (wrapper.value.empty() || !asn1::isUniversal(wrapper.value[0], asn1::Type::Integer)) schemaMismatch("TBSCertificate");
        double version = 0;
        for (std::uint8_t byte : wrapper.value[0].valueHex) version = version * 256 + byte;
        certificate.version = version;
        ++index;
      }
      const asn1::Node& serial = requireChild(tbs, index++, "TBSCertificate");
      if (!asn1::isUniversal(serial, asn1::Type::Integer)) schemaMismatch("TBSCertificate");
      certificate.serialNumber = serial.valueHex;
      certificate.signature = parseAlgorithmIdentifier(requireChild(tbs, index++, "TBSCertificate")).first;
      const asn1::Node& issuer = requireChild(tbs, index++, "TBSCertificate");
      certificate.issuer = parseName(issuer, "Name");
      certificate.issuerRaw = issuer.raw;
      const asn1::Node& validity = requireChild(tbs, index++, "TBSCertificate");
      if (!asn1::isUniversal(validity, asn1::Type::Sequence) || validity.value.size() < 2) schemaMismatch("Validity");
      certificate.notBefore = parseTime(validity.value[0]);
      certificate.notAfter = parseTime(validity.value[1]);
      const asn1::Node& subject = requireChild(tbs, index++, "TBSCertificate");
      certificate.subject = parseName(subject, "Name");
      certificate.subjectRaw = subject.raw;
      const asn1::Node& spki = requireChild(tbs, index++, "TBSCertificate");
      if (!asn1::isUniversal(spki, asn1::Type::Sequence) || spki.value.size() < 2 || !asn1::isUniversal(spki.value[1], asn1::Type::BitString))
        schemaMismatch("SubjectPublicKeyInfo");
      auto [algorithm, parameters] = parseAlgorithmIdentifier(spki.value[0]);
      certificate.publicKeyAlgorithm = std::move(algorithm);
      certificate.publicKeyParameters = std::move(parameters);
      certificate.subjectPublicKey = spki.value[1].valueHex;
      certificate.subjectPublicKeyInfoRaw = spki.raw;
      for (; index < tbs.value.size(); ++index) {
        const asn1::Node& optional = tbs.value[index];
        if (asn1::is(optional, 3, 3, true)) {
          if (optional.value.empty()) schemaMismatch("TBSCertificate");
          certificate.extensions = parseExtensions(optional.value[0]);
          break;
        }
        if (!(optional.tagClass == 3 && (optional.tagNumber == 1 || optional.tagNumber == 2))) break;
      }
      const auto [signatureAlgorithm, signatureParameters] = parseAlgorithmIdentifier(top.value[1]);
      certificate.signatureAlgorithm = signatureAlgorithm;
      certificate.signatureAlgorithmRaw = top.value[1].raw;
      if (!asn1::isUniversal(top.value[2], asn1::Type::BitString)) schemaMismatch("Certificate");
      certificate.signatureValue = top.value[2].valueHex;
      return certificate;
    }

    // `parsedCert.tbsCertificate.extensions?.find((ext) => ext.extnID === id)`
    const Extension* findExtension(const Certificate& certificate, std::string_view id) {
      if (!certificate.extensions) return nullptr;
      for (const Extension& extension : *certificate.extensions)
        if (extension.extnID == id) return &extension;
      return nullptr;
    }

    // `AsnParser.parse(extnValue, OctetString)`: the content of a primitive OCTET STRING.
    Bytes parseOctetString(BytesView data) {
      const asn1::Node node = asn1::parse(data);
      if (node.type != asn1::Type::OctetString) throw rt::TypeError("Argument 'asn' is not instance of ASN.1 OctetString");
      return node.isConstructed ? Bytes() : node.valueHex;
    }

    // ---- WebCrypto as Bun runs the library's calls (OpenSSL 3) ---------------------------

    namespace ossl {
      struct PkeyFree {
        void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
      };
      struct PkeyCtxFree {
        void operator()(EVP_PKEY_CTX* context) const noexcept { EVP_PKEY_CTX_free(context); }
      };
      struct MdCtxFree {
        void operator()(EVP_MD_CTX* context) const noexcept { EVP_MD_CTX_free(context); }
      };
      struct ParamBldFree {
        void operator()(OSSL_PARAM_BLD* builder) const noexcept { OSSL_PARAM_BLD_free(builder); }
      };
      struct ParamFree {
        void operator()(OSSL_PARAM* params) const noexcept { OSSL_PARAM_free(params); }
      };
      struct BnFree {
        void operator()(BIGNUM* number) const noexcept { BN_free(number); }
      };
      struct EcdsaSigFree {
        void operator()(ECDSA_SIG* signature) const noexcept { ECDSA_SIG_free(signature); }
      };
      using Pkey = std::unique_ptr<EVP_PKEY, PkeyFree>;
      using PkeyCtx = std::unique_ptr<EVP_PKEY_CTX, PkeyCtxFree>;
      using MdCtx = std::unique_ptr<EVP_MD_CTX, MdCtxFree>;
      using Bn = std::unique_ptr<BIGNUM, BnFree>;

      const EVP_MD* digestOf(std::string_view name) {
        if (name == "SHA-1") return EVP_sha1();
        if (name == "SHA-256") return EVP_sha256();
        if (name == "SHA-384") return EVP_sha384();
        if (name == "SHA-512") return EVP_sha512();
        return nullptr;
      }

      // An empty span's data() may be null; OpenSSL wants a pointer.
      const unsigned char* dataOf(BytesView bytes) {
        static const unsigned char empty = 0;
        return bytes.empty() ? &empty : bytes.data();
      }

      Pkey fromData(const char* type, OSSL_PARAM_BLD* builder) {
        std::unique_ptr<OSSL_PARAM, ParamFree> params(OSSL_PARAM_BLD_to_param(builder));
        PkeyCtx context(EVP_PKEY_CTX_new_from_name(nullptr, type, nullptr));
        if (!params || !context || EVP_PKEY_fromdata_init(context.get()) != 1) return nullptr;
        EVP_PKEY* raw = nullptr;
        if (EVP_PKEY_fromdata(context.get(), &raw, EVP_PKEY_PUBLIC_KEY, params.get()) != 1) return nullptr;
        return Pkey(raw);
      }

      // EVP_DigestVerify with optional RSA-PSS padding; any OpenSSL failure is a false result.
      bool digestVerify(EVP_PKEY* key, const EVP_MD* md, BytesView signature, BytesView data, std::optional<int> pssSaltLength = std::nullopt) {
        MdCtx context(EVP_MD_CTX_new());
        EVP_PKEY_CTX* keyContext = nullptr;
        if (!context || EVP_DigestVerifyInit(context.get(), &keyContext, md, nullptr, key) != 1) return false;
        if (pssSaltLength) {
          if (EVP_PKEY_CTX_set_rsa_padding(keyContext, RSA_PKCS1_PSS_PADDING) != 1) return false;
          if (EVP_PKEY_CTX_set_rsa_pss_saltlen(keyContext, *pssSaltLength) != 1) return false;
          if (EVP_PKEY_CTX_set_rsa_mgf1_md(keyContext, md) != 1) return false;
        }
        return EVP_DigestVerify(context.get(), dataOf(signature), signature.size(), dataOf(data), data.size()) == 1;
      }
    }

    [[noreturn]] void invalidKeyData() { throw rt::DOMException("Invalid keyData", "DataError"); }

    // `crypto.subtle.digest(name, data)`
    Bytes subtleDigest(std::string_view name, BytesView data) { return rt::crypto::subtle::digest(name, data); }

    struct EcCurve {
      const char* jwkName;
      const char* groupName;
      std::size_t fieldBytes;
    };

    // importKey("jwk", { kty: "EC", crv, x, y }, { name: "ECDSA", namedCurve }, false, ["verify"])
    ossl::Pkey importEcJwk(const EcCurve& curve, std::string_view xBase64Url, std::string_view yBase64Url) {
      const Bytes x = rt::fromBase64(xBase64Url);
      const Bytes y = rt::fromBase64(yBase64Url);
      if (x.size() != curve.fieldBytes || y.size() != curve.fieldBytes) invalidKeyData();
      const Bytes point = concatBytes({Bytes{0x04}, x, y});
      std::unique_ptr<OSSL_PARAM_BLD, ossl::ParamBldFree> builder(OSSL_PARAM_BLD_new());
      if (!builder || OSSL_PARAM_BLD_push_utf8_string(builder.get(), OSSL_PKEY_PARAM_GROUP_NAME, curve.groupName, 0) != 1 ||
          OSSL_PARAM_BLD_push_octet_string(builder.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()) != 1)
        invalidKeyData();
      ossl::Pkey key = ossl::fromData("EC", builder.get());
      if (!key) invalidKeyData();
      return key;
    }

    // subtle.verify({ name: "ECDSA", hash }, key, r || s, data)
    bool ecdsaVerify(EVP_PKEY* key, const EcCurve& curve, std::string_view hash, BytesView signature, BytesView data) {
      if (signature.size() != 2 * curve.fieldBytes) return false;
      std::unique_ptr<ECDSA_SIG, ossl::EcdsaSigFree> sig(ECDSA_SIG_new());
      BIGNUM* r = BN_bin2bn(signature.data(), static_cast<int>(curve.fieldBytes), nullptr);
      BIGNUM* s = BN_bin2bn(signature.data() + curve.fieldBytes, static_cast<int>(curve.fieldBytes), nullptr);
      if (!sig || !r || !s || ECDSA_SIG_set0(sig.get(), r, s) != 1) {
        BN_free(r);
        BN_free(s);
        return false;
      }
      unsigned char* der = nullptr;
      const int derLength = i2d_ECDSA_SIG(sig.get(), &der);
      if (derLength <= 0) return false;
      const Bytes encoded(der, der + derLength);
      OPENSSL_free(der);
      return ossl::digestVerify(key, ossl::digestOf(hash), encoded, data);
    }

    // importKey("jwk", { kty: "OKP", crv: "Ed25519", x }, { name: "Ed25519" }, false, ["verify"])
    ossl::Pkey importEd25519Jwk(std::string_view xBase64Url) {
      const Bytes x = rt::fromBase64(xBase64Url);
      if (x.size() != 32) invalidKeyData();
      ossl::Pkey key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, x.data(), x.size()));
      if (!key) invalidKeyData();
      return key;
    }

    bool ed25519Verify(EVP_PKEY* key, BytesView signature, BytesView data) {
      if (signature.size() != 64) return false;
      return ossl::digestVerify(key, nullptr, signature, data);
    }

    // The RSA public key of a JWK `{ kty: "RSA", n, e }` already accepted by Bun's import.
    ossl::Pkey rsaKeyFromJwk(std::string_view nBase64Url, std::string_view eBase64Url) {
      const Bytes n = rt::fromBase64(nBase64Url);
      const Bytes e = rt::fromBase64(eBase64Url);
      ossl::Bn modulus(BN_bin2bn(n.data(), static_cast<int>(n.size()), nullptr));
      ossl::Bn exponent(BN_bin2bn(e.data(), static_cast<int>(e.size()), nullptr));
      std::unique_ptr<OSSL_PARAM_BLD, ossl::ParamBldFree> builder(OSSL_PARAM_BLD_new());
      if (!modulus || !exponent || !builder || OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_N, modulus.get()) != 1 ||
          OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_E, exponent.get()) != 1)
        return nullptr;
      return ossl::fromData("RSA", builder.get());
    }

    // ---- COSE (helpers/cose.ts) -------------------------------------------------------

    namespace COSEKEYS {
      constexpr std::int64_t kty = 1;
      constexpr std::int64_t alg = 3;
      constexpr std::int64_t crv = -1;
      constexpr std::int64_t x = -2;
      constexpr std::int64_t y = -3;
      constexpr std::int64_t n = -1;
      constexpr std::int64_t e = -2;
    }

    // `Object.values(Enum).indexOf(value) >= 0` over a TS numeric enum: its member names and
    // its numbers both match.
    bool inEnumValues(const Cbor* value, std::initializer_list<std::string_view> names, std::initializer_list<double> numbers) {
      if (value == nullptr) return false;
      if (value->kind() == Cbor::Kind::string) return std::find(names.begin(), names.end(), value->text()) != names.end();
      if (value->kind() == Cbor::Kind::number) return std::find(numbers.begin(), numbers.end(), value->number()) != numbers.end();
      return false;
    }
    bool isCOSEKty(const Cbor* kty) { return inEnumValues(kty, {"OKP", "EC2", "RSA"}, {1, 2, 3}); }
    bool isCOSECrv(const Cbor* crv) { return inEnumValues(crv, {"P256", "P384", "P521", "ED25519", "SECP256K1"}, {1, 2, 3, 6, 8}); }
    bool isCOSEAlg(const Cbor* alg) {
      return inEnumValues(alg, {"ES256", "EdDSA", "ES384", "ES512", "PS256", "PS384", "PS512", "ES256K", "RS256", "RS384", "RS512", "RS1"},
                          {-7, -8, -35, -36, -37, -38, -39, -47, -257, -258, -259, -65535});
    }
    bool numberIs(const Cbor* value, double number) { return value != nullptr && value->kind() == Cbor::Kind::number && value->number() == number; }
    bool isCOSEPublicKeyOKP(const Cbor& key) { return isCOSEKty(key.get(COSEKEYS::kty)) && numberIs(key.get(COSEKEYS::kty), 1); }
    bool isCOSEPublicKeyEC2(const Cbor& key) { return isCOSEKty(key.get(COSEKEYS::kty)) && numberIs(key.get(COSEKEYS::kty), 2); }
    bool isCOSEPublicKeyRSA(const Cbor& key) { return isCOSEKty(key.get(COSEKEYS::kty)) && numberIs(key.get(COSEKEYS::kty), 3); }

    // `name.get(...)` on a decoded value that may not be a Map (JavaScriptCore's TypeErrors).
    const Cbor& requireMap(const Cbor& value, std::string_view name, std::string_view key) {
      if (value.kind() == Cbor::Kind::map) return value;
      const std::string variable(name);
      if (value.kind() == Cbor::Kind::null_ || value.kind() == Cbor::Kind::undefined_)
        throw rt::TypeError(std::string(value.kind() == Cbor::Kind::null_ ? "null" : "undefined") + " is not an object (evaluating '" + variable + ".get')");
      throw rt::TypeError(variable + ".get is not a function. (In '" + variable + ".get(" + std::string(key) + ")', '" + variable + ".get' is undefined)");
    }

    // `mapCoseAlgToWebCryptoAlg(alg)`
    std::string mapCoseAlgToWebCryptoAlg(const Cbor* alg) {
      const auto in = [&](std::initializer_list<double> values) { return alg != nullptr && alg->kind() == Cbor::Kind::number && std::find(values.begin(), values.end(), alg->number()) != values.end(); };
      if (in({-65535})) return "SHA-1";
      if (in({-7, -37, -257})) return "SHA-256";
      if (in({-35, -38, -258})) return "SHA-384";
      if (in({-36, -39, -259, -8})) return "SHA-512";
      fail("Could not map COSE alg value of " + jsString(alg) + " to a WebCrypto alg");
    }

    // `mapCoseAlgToWebCryptoKeyAlgName(alg)`
    std::string mapCoseAlgToWebCryptoKeyAlgName(const Cbor* alg) {
      const auto in = [&](std::initializer_list<double> values) { return alg != nullptr && alg->kind() == Cbor::Kind::number && std::find(values.begin(), values.end(), alg->number()) != values.end(); };
      if (in({-8})) return "Ed25519";
      if (in({-7, -35, -36, -47})) return "ECDSA";
      if (in({-257, -258, -259, -65535})) return "RSASSA-PKCS1-v1_5";
      if (in({-37, -38, -39})) return "RSA-PSS";
      fail("Could not map COSE alg value of " + jsString(alg) + " to a WebCrypto key alg name");
    }

    // `Number(value)` of a decoded item, for `new Uint8Array(array)` elements.
    double cborToNumber(const Cbor& value) {
      switch (value.kind()) {
        case Cbor::Kind::undefined_: return std::numeric_limits<double>::quiet_NaN();
        case Cbor::Kind::null_: return 0;
        case Cbor::Kind::boolean: return value.boolean() ? 1 : 0;
        case Cbor::Kind::number: return value.number();
        case Cbor::Kind::string: return rt::stringToNumber(value.text());
        case Cbor::Kind::bytes:
        case Cbor::Kind::array: return rt::stringToNumber(jsString(value));
        case Cbor::Kind::map: return std::numeric_limits<double>::quiet_NaN();
      }
      return 0;
    }

    // `new Uint8Array(length)` for a primitive argument (ToIndex). Lengths a JS engine could
    // only serve lazily are refused as out of memory.
    std::size_t typedArrayLength(double number) {
      if (std::isnan(number)) return 0;
      const double length = std::trunc(number);
      if (length < 0) throw rt::RangeError("length cannot be negative");
      if (length > 9007199254740991.0) throw rt::RangeError("length larger than (2 ** 53) - 1");
      if (length > 64.0 * 1024 * 1024) throw rt::RangeError("Out of memory");
      return static_cast<std::size_t>(length);
    }

    // `new Uint8Array(value)` of a decoded item (isoBase64URL.fromBuffer's first step).
    Bytes cborToUint8Array(const Cbor& value) {
      switch (value.kind()) {
        case Cbor::Kind::bytes: return value.bytes();
        case Cbor::Kind::array: {
          Bytes out;
          for (const Cbor& item : value.items()) {
            const double number = cborToNumber(item);
            out.push_back(std::isfinite(number) ? static_cast<std::uint8_t>(static_cast<std::int64_t>(std::fmod(std::trunc(number), 256.0) + 256) % 256) : 0);
          }
          return out;
        }
        case Cbor::Kind::map: return Bytes(value.entries().size(), 0);  // each entry is a [key, value] array: NaN
        case Cbor::Kind::null_: return {};
        case Cbor::Kind::undefined_: return {};
        case Cbor::Kind::boolean: return Bytes(value.boolean() ? 1 : 0, 0);
        case Cbor::Kind::number: return Bytes(typedArrayLength(value.number()), 0);
        case Cbor::Kind::string: return Bytes(typedArrayLength(rt::stringToNumber(value.text())), 0);
      }
      return {};
    }

    std::string cborBase64Url(const Cbor& value) { return isoBase64URL::fromBuffer(cborToUint8Array(value)); }

    // ---- isoCrypto.verify ----------------------------------------------------------------

    // `unwrapEC2Signature`'s component length; throws for any other curve.
    std::size_t getSignatureComponentLength(const Cbor* crv) {
      if (numberIs(crv, 1)) return 32;
      if (numberIs(crv, 2)) return 48;
      if (numberIs(crv, 3)) return 66;
      fail("Unexpected COSE crv value of " + jsString(crv) + " (EC2)");
    }

    Bytes toNormalizedBytes(const Bytes& bytes, std::size_t componentLength) {
      if (bytes.size() < componentLength) {
        // In case the bytes are shorter than expected, we need to pad it with leading `0`s.
        Bytes normalized(componentLength, 0);
        std::copy(bytes.begin(), bytes.end(), normalized.begin() + static_cast<std::ptrdiff_t>(componentLength - bytes.size()));
        return normalized;
      }
      if (bytes.size() == componentLength) return bytes;
      if (bytes.size() == componentLength + 1 && bytes[0] == 0 && (bytes[1] & 0x80) == 0x80) {
        // The bytes contain a leading `0` to encode that the integer is positive. This leading `0`
        // needs to be removed for compatibility with the SubtleCrypto Web Crypto API.
        return Bytes(bytes.begin() + 1, bytes.end());
      }
      fail("Invalid signature component length " + std::to_string(bytes.size()) + ", expected " + std::to_string(componentLength));
    }

    Bytes unwrapEC2Signature(BytesView signature, const Cbor* crv) {
      const EcdsaSigValue parsedSignature = parseEcdsaSigValue(signature);
      const std::size_t componentLength = getSignatureComponentLength(crv);
      return concatBytes({toNormalizedBytes(parsedSignature.r, componentLength), toNormalizedBytes(parsedSignature.s, componentLength)});
    }

    // A hash override: a COSE alg number (or absent).
    using ShaHashOverride = std::optional<Cbor>;

    bool verifyEC2(const Cbor& cosePublicKey, BytesView signature, BytesView data, const ShaHashOverride& shaHashOverride) {
      const Cbor* alg = cosePublicKey.get(COSEKEYS::alg);
      const Cbor* crv = cosePublicKey.get(COSEKEYS::crv);
      const Cbor* x = cosePublicKey.get(COSEKEYS::x);
      const Cbor* y = cosePublicKey.get(COSEKEYS::y);
      if (!truthy(alg)) fail("Public key was missing alg (EC2)");
      if (!truthy(crv)) fail("Public key was missing crv (EC2)");
      if (!truthy(x)) fail("Public key was missing x (EC2)");
      if (!truthy(y)) fail("Public key was missing y (EC2)");
      EcCurve curve{};
      if (numberIs(crv, 1)) curve = {"P-256", "prime256v1", 32};
      else if (numberIs(crv, 2)) curve = {"P-384", "secp384r1", 48};
      else if (numberIs(crv, 3)) curve = {"P-521", "secp521r1", 66};
      else fail("Unexpected COSE crv value of " + jsString(crv) + " (EC2)");
      const std::string keyX = cborBase64Url(*x);
      const std::string keyY = cborBase64Url(*y);
      const ossl::Pkey key = importEcJwk(curve, keyX, keyY);
      // Determine which SHA algorithm to use for signature verification
      std::string subtleAlg = mapCoseAlgToWebCryptoAlg(alg);
      if (shaHashOverride && truthy(&*shaHashOverride)) subtleAlg = mapCoseAlgToWebCryptoAlg(&*shaHashOverride);
      return ecdsaVerify(key.get(), curve, subtleAlg, signature, data);
    }

    bool verifyRSA(const Cbor& cosePublicKey, BytesView signature, BytesView data, const ShaHashOverride& shaHashOverride) {
      const Cbor* alg = cosePublicKey.get(COSEKEYS::alg);
      const Cbor* n = cosePublicKey.get(COSEKEYS::n);
      const Cbor* e = cosePublicKey.get(COSEKEYS::e);
      if (!truthy(alg)) fail("Public key was missing alg (RSA)");
      if (!isCOSEAlg(alg)) fail("Public key had invalid alg " + jsString(alg) + " (RSA)");
      if (!truthy(n)) fail("Public key was missing n (RSA)");
      if (!truthy(e)) fail("Public key was missing e (RSA)");
      const std::string keyN = cborBase64Url(*n);
      const std::string keyE = cborBase64Url(*e);
      const std::string keyAlgorithmName = mapCoseAlgToWebCryptoKeyAlgName(alg);
      std::string hashName = mapCoseAlgToWebCryptoAlg(alg);
      if (shaHashOverride && truthy(&*shaHashOverride)) hashName = mapCoseAlgToWebCryptoAlg(&*shaHashOverride);
      std::string jwkAlg;
      std::optional<int> saltLength;
      if (keyAlgorithmName == "RSASSA-PKCS1-v1_5") {
        if (hashName == "SHA-256") jwkAlg = "RS256";
        else if (hashName == "SHA-384") jwkAlg = "RS384";
        else if (hashName == "SHA-512") jwkAlg = "RS512";
        else if (hashName == "SHA-1") jwkAlg = "RS1";
      } else if (keyAlgorithmName == "RSA-PSS") {
        /**
         * salt length. The default value is 20 but the convention is to use hLen, the length of the
         * output of the hash function in bytes. A salt length of zero is permitted and will result in
         * a deterministic signature value. The actual salt length used can be determined from the
         * signature value.
         *
         * From https://www.cryptosys.net/pki/manpki/pki_rsaschemes.html
         */
        saltLength = 0;
        if (hashName == "SHA-256") {
          jwkAlg = "PS256";
          saltLength = 32;  // 256 bits => 32 bytes
        } else if (hashName == "SHA-384") {
          jwkAlg = "PS384";
          saltLength = 48;  // 384 bits => 48 bytes
        } else if (hashName == "SHA-512") {
          jwkAlg = "PS512";
          saltLength = 64;  // 512 bits => 64 bytes
        }
      } else {
        fail("Unexpected RSA key algorithm " + jsString(alg) + " (" + keyAlgorithmName + ")");
      }
      // Bun validates RSA JWKs the same way for both RSA schemes: rt's RSASSA-PKCS1-v1_5 import
      // carries its checks and messages (the matching "RS*" alg stands in for "PS*").
      json::object jwk;
      jwk["kty"] = "RSA";
      if (saltLength) {
        jwk["alg"] = hashName == "SHA-256" ? "RS256" : hashName == "SHA-384" ? "RS384" : hashName == "SHA-512" ? "RS512" : "";
      } else {
        jwk["alg"] = jwkAlg;
      }
      jwk["n"] = keyN;
      jwk["e"] = keyE;
      jwk["ext"] = false;
      rt::crypto::Algorithm importAlgorithm;
      importAlgorithm.name = "RSASSA-PKCS1-v1_5";
      importAlgorithm.hash = hashName;
      const std::vector<std::string> usages{"verify"};
      const rt::crypto::CryptoKey imported = rt::crypto::subtle::importKey("jwk", jwk, importAlgorithm, false, usages);
      if (!saltLength) return rt::crypto::subtle::verify("RSASSA-PKCS1-v1_5", imported, signature, data);
      const ossl::Pkey key = rsaKeyFromJwk(keyN, keyE);
      if (!key) return false;
      return ossl::digestVerify(key.get(), ossl::digestOf(hashName), signature, data, *saltLength);
    }

    bool verifyOKP(const Cbor& cosePublicKey, BytesView signature, BytesView data) {
      const Cbor* alg = cosePublicKey.get(COSEKEYS::alg);
      const Cbor* crv = cosePublicKey.get(COSEKEYS::crv);
      const Cbor* x = cosePublicKey.get(COSEKEYS::x);
      if (!truthy(alg)) fail("Public key was missing alg (OKP)");
      if (!isCOSEAlg(alg)) fail("Public key had invalid alg " + jsString(alg) + " (OKP)");
      if (!truthy(crv)) fail("Public key was missing crv (OKP)");
      if (!truthy(x)) fail("Public key was missing x (OKP)");
      // Pulled key import steps from here:
      // https://wicg.github.io/webcrypto-secure-curves/#ed25519-operations
      if (!numberIs(crv, 6)) fail("Unexpected COSE crv value of " + jsString(crv) + " (OKP)");
      const ossl::Pkey key = importEd25519Jwk(cborBase64Url(*x));
      return ed25519Verify(key.get(), signature, data);
    }

    /**
     * Verify signatures with their public key. Supports EC2 and RSA public keys.
     */
    bool isoCryptoVerify(const Cbor& cosePublicKey, BytesView signature, BytesView data, const ShaHashOverride& shaHashOverride) {
      const Cbor& key = requireMap(cosePublicKey, "cosePublicKey", "COSEKEYS.kty");
      if (isCOSEPublicKeyEC2(key)) {
        const Cbor* crv = key.get(COSEKEYS::crv);
        if (!isCOSECrv(crv)) fail("unknown COSE curve " + jsString(crv));
        const Bytes unwrappedSignature = unwrapEC2Signature(signature, crv);
        return verifyEC2(key, unwrappedSignature, data, shaHashOverride);
      }
      if (isCOSEPublicKeyRSA(key)) return verifyRSA(key, signature, data, shaHashOverride);
      if (isCOSEPublicKeyOKP(key)) return verifyOKP(key, signature, data);
      fail("Signature verification with public key of kty " + jsString(key.get(COSEKEYS::kty)) + " is not supported by this method");
    }

    Cbor numberCbor(double value) { return Cbor{value}; }

    // `mapX509SignatureAlgToCOSEAlg(oid)`
    double mapX509SignatureAlgToCOSEAlg(std::string_view signatureAlgorithm) {
      if (signatureAlgorithm == "1.2.840.10045.4.3.2") return -7;
      if (signatureAlgorithm == "1.2.840.10045.4.3.3") return -35;
      if (signatureAlgorithm == "1.2.840.10045.4.3.4") return -36;
      if (signatureAlgorithm == "1.2.840.113549.1.1.11") return -257;
      if (signatureAlgorithm == "1.2.840.113549.1.1.12") return -258;
      if (signatureAlgorithm == "1.2.840.113549.1.1.13") return -259;
      if (signatureAlgorithm == "1.2.840.113549.1.1.5") return -65535;
      fail("Unable to map X.509 signature algorithm " + std::string(signatureAlgorithm) + " to a COSE algorithm");
    }

    Cbor coseMap(std::vector<std::pair<std::int64_t, Cbor>> entries) {
      Cbor::Map map;
      map.reserve(entries.size());
      for (auto& [key, value] : entries) map.push_back(CborEntry{CborKey{key}, std::move(value)});
      return Cbor{std::move(map)};
    }

    Cbor bytesCbor(Bytes bytes) { return Cbor{std::move(bytes)}; }

    // `convertX509PublicKeyToCOSE(x509Certificate)`
    Cbor convertX509PublicKeyToCOSE(BytesView x509Certificate) {
      const Certificate x509 = parseCertificate(x509Certificate);
      const std::string& publicKeyAlgorithmID = x509.publicKeyAlgorithm;
      if (publicKeyAlgorithmID == "1.2.840.10045.2.1") {
        /**
         * EC2 Public Key
         */
        if (!x509.publicKeyParameters) fail("Certificate public key was missing parameters (EC2)");
        // ECParameters ::= CHOICE { namedCurve OBJECT IDENTIFIER, ... }
        const asn1::Node ecParameters = asn1::parse(*x509.publicKeyParameters);
        if (!asn1::isUniversal(ecParameters, asn1::Type::ObjectIdentifier)) fail("Data does not match to ECParameters ASN1 schema.");
        const std::string namedCurve = asn1::oidString(ecParameters);
        double crv = -999;
        if (namedCurve == "1.2.840.10045.3.1.7") crv = 1;
        else if (namedCurve == "1.3.132.0.34") crv = 2;
        else fail("Certificate public key contained unexpected namedCurve " + namedCurve + " (EC2)");
        const Bytes& subjectPublicKey = x509.subjectPublicKey;
        if (subjectPublicKey.empty() || subjectPublicKey[0] != 0x04) fail("TODO: Figure out how to handle public keys in \"compressed form\"");
        // Public key is in "uncompressed form", so we can split the remaining bytes in half
        const double halfLength = static_cast<double>(subjectPublicKey.size() - 1) / 2;
        const std::size_t xEnd = std::min(subjectPublicKey.size(), 1 + static_cast<std::size_t>(std::trunc(halfLength)));
        Bytes x(subjectPublicKey.begin() + 1, subjectPublicKey.begin() + static_cast<std::ptrdiff_t>(xEnd));
        Bytes y(subjectPublicKey.begin() + static_cast<std::ptrdiff_t>(xEnd), subjectPublicKey.end());
        std::vector<std::pair<std::int64_t, Cbor>> entries;
        entries.emplace_back(COSEKEYS::kty, numberCbor(2));
        entries.emplace_back(COSEKEYS::alg, numberCbor(mapX509SignatureAlgToCOSEAlg(x509.signature)));
        entries.emplace_back(COSEKEYS::crv, numberCbor(crv));
        entries.emplace_back(COSEKEYS::x, bytesCbor(std::move(x)));
        entries.emplace_back(COSEKEYS::y, bytesCbor(std::move(y)));
        return coseMap(std::move(entries));
      }
      if (publicKeyAlgorithmID == "1.2.840.113549.1.1.1") {
        /**
         * RSA public key
         */
        const asn1::Node rsaPublicKey = asn1::parse(x509.subjectPublicKey);
        if (!asn1::isUniversal(rsaPublicKey, asn1::Type::Sequence) || !rsaPublicKey.isConstructed || rsaPublicKey.value.size() < 2 ||
            !asn1::is(rsaPublicKey.value[0], 1, 2, false) || !asn1::is(rsaPublicKey.value[1], 1, 2, false))
          fail("Data does not match to RSAPublicKey ASN1 schema.");
        std::vector<std::pair<std::int64_t, Cbor>> entries;
        entries.emplace_back(COSEKEYS::kty, numberCbor(3));
        entries.emplace_back(COSEKEYS::alg, numberCbor(mapX509SignatureAlgToCOSEAlg(x509.signature)));
        entries.emplace_back(COSEKEYS::n, bytesCbor(rsaPublicKey.value[0].valueHex));
        entries.emplace_back(COSEKEYS::e, bytesCbor(rsaPublicKey.value[1].valueHex));
        return coseMap(std::move(entries));
      }
      fail("Certificate public key contained unexpected algorithm ID " + publicKeyAlgorithmID);
    }

    // `verifySignature({ signature, data, credentialPublicKey | x509Certificate, hashAlgorithm })`
    struct VerifySignatureOptions {
      Bytes signature;
      Bytes data;
      std::optional<Bytes> credentialPublicKey;
      std::optional<Bytes> x509Certificate;
      ShaHashOverride hashAlgorithm;
    };

    bool verifySignature(const VerifySignatureOptions& opts) {
      Cbor cosePublicKey;
      if (opts.credentialPublicKey) cosePublicKey = isoCBOR::decodeFirst(*opts.credentialPublicKey);
      else if (opts.x509Certificate) cosePublicKey = convertX509PublicKeyToCOSE(*opts.x509Certificate);
      return isoCryptoVerify(cosePublicKey, opts.signature, opts.data, opts.hashAlgorithm);
    }

    // `toHash(data, algorithm = -7)`
    Bytes toHash(BytesView data, const Cbor* algorithm = nullptr) {
      const Cbor es256 = numberCbor(-7);
      return subtleDigest(mapCoseAlgToWebCryptoAlg(algorithm ? algorithm : &es256), data);
    }

    // ---- authenticator data, client data -----------------------------------------------

    struct AuthenticatorFlags {
      bool up = false;  // User Presence
      bool uv = false;  // User Verified
      bool be = false;  // Backup Eligibility
      bool bs = false;  // Backup State
      bool at = false;  // Attested Credential Data Present
      bool ed = false;  // Extension Data Present
      std::uint8_t flagsInt = 0;
    };

    struct ParsedAuthenticatorData {
      Bytes rpIdHash;
      AuthenticatorFlags flags;
      double counter = 0;
      std::optional<Bytes> aaguid;
      std::optional<Bytes> credentialID;
      std::optional<Bytes> credentialPublicKey;
    };

    Bytes sliceBytes(BytesView bytes, std::size_t start, std::size_t end) {
      start = std::min(start, bytes.size());
      end = std::min(std::max(end, start), bytes.size());
      return Bytes(bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.begin() + static_cast<std::ptrdiff_t>(end));
    }

    // DataView getters: RangeError past the end.
    std::uint32_t getUint(BytesView bytes, std::size_t offset, std::size_t width) {
      if (offset + width > bytes.size()) throw rt::RangeError("Out of bounds access");
      std::uint32_t out = 0;
      for (std::size_t i = 0; i < width; ++i) out = (out << 8) | bytes[offset + i];
      return out;
    }

    // `convertMapToObjectDeep(toCBOR)`: only its `for (const [key, value] of input)` destructuring
    // can fail (Maps are the only values it recurses into); the object it builds is never read.
    void convertMapToObjectDeep(const Cbor& input) {
      const auto notIterable = [](const Cbor& value, std::string_view expression) {
        switch (value.kind()) {
          case Cbor::Kind::null_: throw rt::TypeError("null is not an object (evaluating '" + std::string(expression) + "')");
          case Cbor::Kind::undefined_: throw rt::TypeError("undefined is not an object (evaluating '" + std::string(expression) + "')");
          case Cbor::Kind::boolean: throw rt::TypeError(std::string(value.boolean() ? "true" : "false") + " is not iterable");
          case Cbor::Kind::number: throw rt::TypeError("number is not iterable");
          default: break;
        }
      };
      notIterable(input, "const [key, value] of input");
      if (input.kind() == Cbor::Kind::bytes) {
        if (!input.bytes().empty()) throw rt::TypeError("number is not iterable");
      } else if (input.kind() == Cbor::Kind::array) {
        for (const Cbor& element : input.items()) notIterable(element, "[key, value]");
      }
    }

    /**
     * Make sense of the authData buffer contained in an Attestation
     */
    ParsedAuthenticatorData parseAuthenticatorData(Bytes authData) {
      if (authData.size() < 37) fail("Authenticator data was " + std::to_string(authData.size()) + " bytes, expected at least 37 bytes");
      ParsedAuthenticatorData parsed;
      std::size_t pointer = 0;
      parsed.rpIdHash = sliceBytes(authData, pointer, pointer + 32);
      pointer += 32;
      const std::uint8_t flagsInt = authData[pointer];
      pointer += 1;
      // Bit positions can be referenced here:
      // https://www.w3.org/TR/webauthn-2/#flags
      parsed.flags = AuthenticatorFlags{
        .up = (flagsInt & (1 << 0)) != 0,
        .uv = (flagsInt & (1 << 2)) != 0,
        .be = (flagsInt & (1 << 3)) != 0,
        .bs = (flagsInt & (1 << 4)) != 0,
        .at = (flagsInt & (1 << 6)) != 0,
        .ed = (flagsInt & (1 << 7)) != 0,
        .flagsInt = flagsInt,
      };
      parsed.counter = getUint(authData, pointer, 4);
      pointer += 4;
      const BytesView encodedAuthData(authData);
      if (parsed.flags.at) {
        parsed.aaguid = sliceBytes(authData, pointer, pointer + 16);
        pointer += 16;
        const std::size_t credIDLen = getUint(authData, pointer, 2);
        pointer += 2;
        parsed.credentialID = sliceBytes(authData, pointer, pointer + credIDLen);
        pointer += credIDLen;
        /**
         * Firefox 117 incorrectly CBOR-encodes authData when EdDSA (-8) is used for the public key.
         * A CBOR "Map of 3 items" (0xa3) should be "Map of 4 items" (0xa4), and if we manually adjust
         * the single byte there's a good chance the authData can be correctly parsed.
         *
         * This browser release also incorrectly uses the string labels "OKP" and "Ed25519" instead of
         * their integer representations for kty and crv respectively. That's why the COSE public key
         * in the hex below looks so odd.
         */
        // Bytes decode to `{ 1: "OKP", 3: -8, -1: "Ed25519" }` (it's missing key -2 a.k.a. COSEKEYS.x)
        static const Bytes badEdDSACBOR = rt::fromHex("a301634f4b500327206745643235353139");
        if (pointer > authData.size()) fail("Truncated credential ID");
        const BytesView remaining = encodedAuthData.subspan(pointer);
        bool foundBadCBOR = false;
        if (remaining.size() >= badEdDSACBOR.size() && bytesEqual(badEdDSACBOR, remaining.first(badEdDSACBOR.size()))) {
          // Change the bad CBOR 0xa3 to 0xa4 so that the credential public key can be recognized
          foundBadCBOR = true;
          authData[pointer] = 0xa4;
        }
        auto [publicKey, consumed] = cbor::reencode(remaining, false);
        if (foundBadCBOR) {
          // Restore the bit we changed so that `authData` is the same as it came in and won't break
          // signature verification.
          authData[pointer] = 0xa3;
        }
        pointer += consumed;
        parsed.credentialPublicKey = std::move(publicKey);
      }
      if (parsed.flags.ed) {
        const isoCBOR::Decoded extensions = isoCBOR::decodeItem(encodedAuthData.subspan(pointer));
        convertMapToObjectDeep(extensions.value);
        pointer += extensions.consumed;
      }
      // Pointer should be at the end of the authenticator data, otherwise too much data was sent
      if (authData.size() > pointer) fail("Leftover bytes detected while parsing authenticator data");
      return parsed;
    }

    /**
     * Decode an authenticator's base64url-encoded clientDataJSON to JSON
     */
    json::value decodeClientDataJSON(std::string_view data) { return rt::jsonParse(isoBase64URL::toUTF8String(data)); }

    // `const { type, origin, challenge, tokenBinding } = clientDataJSON;`
    struct ClientData {
      json::value parsed;
      const json::value* member(std::string_view key) const { return rt::get(parsed, key); }
    };

    ClientData destructureClientData(json::value parsed) {
      if (parsed.is_null()) throw rt::TypeError("Cannot destructure property 'type' from null or undefined value");
      return ClientData{std::move(parsed)};
    }

    // `String(value)` / `${value}` of an unknown JSON member (undefined for nullptr).
    std::string jsonString(const json::value* value) { return rt::toString(value); }

    // `value === "text"`
    bool jsonIs(const json::value* value, std::string_view text) { return value != nullptr && value->is_string() && value->as_string() == text; }

    // `tokenBinding.status` membership for the two verifiers' lists.
    bool statusIn(const json::value* tokenBinding, std::initializer_list<std::string_view> statuses) {
      const json::value* status = rt::get(tokenBinding, "status");
      if (status == nullptr || !status->is_string()) return false;
      return std::find(statuses.begin(), statuses.end(), std::string_view(status->as_string())) != statuses.end();
    }

    /**
     * Go through each expected RP ID and try to find one that matches. Returns the unhashed RP ID
     * that matched the hash in the response.
     *
     * Raises an `UnexpectedRPIDHash` error if no match is found
     */
    std::string matchExpectedRPID(BytesView rpIDHash, const std::vector<std::string>& expectedRPIDs) {
      for (const std::string& expected : expectedRPIDs) {
        const Bytes expectedRPIDHash = toHash(fromASCIIString(expected));
        if (bytesEqual(rpIDHash, expectedRPIDHash)) return expected;
      }
      failNamed("UnexpectedRPIDHash", "Unexpected RP ID hash");
    }

    /**
     * Make sense of Bits 3 and 4 in authenticator indicating:
     *
     * - Whether the credential can be used on multiple devices
     * - Whether the credential is backed up or not
     *
     * Invalid configurations will raise an `Error`
     */
    void parseBackupFlags(const AuthenticatorFlags& flags) {
      const bool credentialBackedUp = flags.bs;
      const bool singleDevice = !flags.be;
      if (singleDevice && credentialBackedUp)
        failNamed("InvalidBackupFlags", "Single-device credential indicated that it was backed up, which should be impossible.");
    }

    // ---- JS values the attestation verifiers read loosely --------------------------------

    // `value.length` of a decoded item: Uint8Array, array and string have one.
    std::optional<double> cborLength(const Cbor& value) {
      switch (value.kind()) {
        case Cbor::Kind::bytes: return static_cast<double>(value.bytes().size());
        case Cbor::Kind::array: return static_cast<double>(value.items().size());
        case Cbor::Kind::string: return static_cast<double>(rt::length(value.text()));
        default: return std::nullopt;
      }
    }

    // `value[index]` of a decoded item as the strict-equality operand isoUint8Array.areEqual reads.
    std::optional<double> cborElementNumber(const Cbor& value, std::size_t index) {
      if (value.kind() == Cbor::Kind::bytes) return index < value.bytes().size() ? std::optional<double>(value.bytes()[index]) : std::nullopt;
      if (value.kind() == Cbor::Kind::array && index < value.items().size() && value.items()[index].kind() == Cbor::Kind::number) return value.items()[index].number();
      return std::nullopt;  // undefined, a string character or another type: never === a byte
    }

    // `isoUint8Array.areEqual(bytes, value)`
    bool areEqual(BytesView array1, const Cbor& array2) {
      const std::optional<double> length = cborLength(array2);
      if (!length || static_cast<double>(array1.size()) != *length) return false;
      for (std::size_t i = 0; i < array1.size(); ++i) {
        const std::optional<double> element = cborElementNumber(array2, i);
        if (!element || *element != array1[i]) return false;
      }
      return true;
    }

    // `isoUint8Array.concat(arrays)`: `new Uint8Array(sum of .length)` then `set(arr, pointer)`.
    Bytes jsConcat(const std::vector<const Cbor*>& arrays) {
      double totalLength = 0;
      for (const Cbor* array : arrays) {
        const std::optional<double> length = cborLength(*array);
        totalLength += length ? *length : std::numeric_limits<double>::quiet_NaN();
      }
      Bytes toReturn(std::isnan(totalLength) ? 0 : static_cast<std::size_t>(totalLength));
      double pointer = 0;
      for (const Cbor* array : arrays) {
        const double offset = std::isnan(pointer) ? 0 : pointer;
        if (array->kind() == Cbor::Kind::null_ || array->kind() == Cbor::Kind::undefined_) throw rt::TypeError("Argument is not an object");
        const std::optional<double> length = cborLength(*array);
        const double srcLength = length ? *length : 0;
        if (srcLength + offset > static_cast<double>(toReturn.size())) throw rt::RangeError("Source is too large");
        for (std::size_t i = 0; i < static_cast<std::size_t>(srcLength); ++i) {
          double number = 0;
          if (array->kind() == Cbor::Kind::bytes) number = array->bytes()[i];
          else if (array->kind() == Cbor::Kind::array) number = cborToNumber(array->items()[i]);
          else number = rt::stringToNumber(rt::charAt(array->text(), static_cast<double>(i)));
          toReturn[static_cast<std::size_t>(offset) + i] =
            std::isfinite(number) ? static_cast<std::uint8_t>(static_cast<std::int64_t>(std::fmod(std::trunc(number), 256.0) + 256) % 256) : 0;
        }
        pointer += length ? *length : std::numeric_limits<double>::quiet_NaN();
      }
      return toReturn;
    }

    // A decoded item that the library hands to a byte API (AsnParser, DataView, digest, verify):
    // anything but a Uint8Array makes that API throw.
    const Bytes& requireBytes(const Cbor* value, std::string_view what) {
      if (value == nullptr || value->kind() != Cbor::Kind::bytes)
        throw rt::TypeError("The provided value is not of type '(ArrayBuffer or ArrayBufferView)' (" + std::string(what) + ")");
      return value->bytes();
    }

    // `value[0]` of an attestation statement's x5c, which must be a Uint8Array for the parsers.
    const Bytes& firstCertificate(const Cbor* x5c) {
      if (x5c == nullptr || x5c->kind() != Cbor::Kind::array || x5c->items().empty()) throw rt::TypeError("x5c[0] is not a certificate");
      return requireBytes(&x5c->items().front(), "x5c[0]");
    }

    /**
     * Takes COSE-encoded public key and converts it to PKCS key
     */
    Bytes convertCOSEtoPKCS(BytesView cosePublicKey) {
      // This is a little sloppy, I'm using COSEPublicKeyEC2 since it could have both x and y, but when
      // there's no y it means it's probably better typed as COSEPublicKeyOKP. I'll leave this for now
      // and revisit it later if it ever becomes an actual problem.
      const Cbor decoded = isoCBOR::decodeFirst(cosePublicKey);
      const Cbor& structure = requireMap(decoded, "struct", "COSEKEYS.x");
      const Cbor tag = bytesCbor(Bytes{0x04});
      const Cbor* x = structure.get(COSEKEYS::x);
      const Cbor* y = structure.get(COSEKEYS::y);
      if (!truthy(x)) fail("COSE public key was missing x");
      if (truthy(y)) return jsConcat({&tag, x, y});
      return jsConcat({&tag, x});
    }

    /**
     * Convert buffer to an OpenSSL-compatible PEM text format.
     */
    std::string convertCertBufferToPEM(const Cbor& certBuffer) {
      std::string b64cert;
      /**
       * Get certBuffer to a base64 representation
       */
      if (certBuffer.kind() == Cbor::Kind::string) {
        if (isoBase64URL::isBase64URL(certBuffer.text())) b64cert = isoBase64URL::toBase64(certBuffer.text());
        else if (isoBase64URL::isBase64(certBuffer.text())) b64cert = certBuffer.text();
        else fail("Certificate is not a valid base64 or base64url string");
      } else {
        b64cert = isoBase64URL::fromBuffer(cborToUint8Array(certBuffer), false);
      }
      std::string PEMKey;
      const double length = static_cast<double>(rt::length(b64cert));
      for (double i = 0; i < std::ceil(length / 64); i += 1) PEMKey += rt::substr(b64cert, 64 * i, 64) + "\n";
      return "-----BEGIN CERTIFICATE-----\n" + PEMKey + "-----END CERTIFICATE-----\n";
    }

    // `x5c.map(convertCertBufferToPEM)` on a decoded attestation statement member.
    std::vector<std::string> mapCertsToPEM(const Cbor* x5c) {
      if (x5c == nullptr || x5c->kind() != Cbor::Kind::array) throw rt::TypeError("x5c.map is not a function");
      std::vector<std::string> out;
      for (const Cbor& certBuffer : x5c->items()) out.push_back(convertCertBufferToPEM(certBuffer));
      return out;
    }

    // ---- getCertificateInfo ------------------------------------------------------------

    struct CertificateParty {
      std::optional<std::string> C;
      std::optional<std::string> O;
      std::optional<std::string> OU;
      std::optional<std::string> CN;
      std::string combined;
    };

    struct CertificateInfo {
      CertificateParty issuer;
      CertificateParty subject;
      double version = 0;
      bool basicConstraintsCA = false;
      double notBefore = 0;
      double notAfter = 0;
      Certificate parsedCertificate;
    };

    CertificateParty certificateParty(const std::vector<RelativeDistinguishedName>& name) {
      CertificateParty party;
      for (const RelativeDistinguishedName& rdn : name) {
        // `forEach(([iss]) => ...)`: the first attribute of each RDN.
        if (rdn.empty()) throw rt::TypeError("undefined is not an object (evaluating 'iss.type')");
        const NameAttribute& iss = rdn.front();
        if (iss.type == "2.5.4.6") party.C = iss.value;
        else if (iss.type == "2.5.4.10") party.O = iss.value;
        else if (iss.type == "2.5.4.11") party.OU = iss.value;
        else if (iss.type == "2.5.4.3") party.CN = iss.value;
      }
      std::vector<std::string> parts;
      if (rt::truthy(party.C)) parts.push_back(*party.C);
      if (rt::truthy(party.O)) parts.push_back(*party.O);
      if (rt::truthy(party.OU)) parts.push_back(*party.OU);
      if (rt::truthy(party.CN)) parts.push_back(*party.CN);
      party.combined = rt::join(parts, " : ");
      return party;
    }

    // `AsnParser.parse(extnValue, BasicConstraints).cA`
    bool basicConstraintsCA(BytesView extnValue) {
      const asn1::Node node = asn1::parse(extnValue);
      if (!asn1::isUniversal(node, asn1::Type::Sequence) || !node.isConstructed) schemaMismatch("BasicConstraints");
      if (!node.value.empty() && asn1::isUniversal(node.value[0], asn1::Type::Boolean))
        return std::any_of(node.value[0].valueHex.begin(), node.value[0].valueHex.end(), [](std::uint8_t b) { return b != 0; });
      return false;
    }

    /**
     * Extract PEM certificate info
     */
    CertificateInfo getCertificateInfo(BytesView leafCertBuffer) {
      CertificateInfo info;
      info.parsedCertificate = parseCertificate(leafCertBuffer);
      const Certificate& parsedCert = info.parsedCertificate;
      info.issuer = certificateParty(parsedCert.issuer);
      info.subject = certificateParty(parsedCert.subject);
      if (parsedCert.extensions) {
        for (const Extension& ext : *parsedCert.extensions)
          if (ext.extnID == "2.5.29.19") info.basicConstraintsCA = basicConstraintsCA(ext.extnValue);
      }
      info.version = parsedCert.version;
      info.notBefore = parsedCert.notBefore;
      info.notAfter = parsedCert.notAfter;
      return info;
    }

    /**
     * Look for the id-fido-gen-ce-aaguid certificate extension. If it's present then check it against
     * the attestation statement AAGUID.
     */
    void validateExtFIDOGenCEAAGUID(const Certificate& certificate, BytesView aaguid) {
      // The certificate had no extensions so there's nothing to validate
      if (!certificate.extensions) return;
      const Extension* extFIDOGenCEAAGUID = findExtension(certificate, "1.3.6.1.4.1.45724.1.1.4");
      // The extension isn't present so there's nothing to validate
      if (extFIDOGenCEAAGUID == nullptr) return;
      // Parse the extension value
      const Bytes extValue = parseOctetString(extFIDOGenCEAAGUID->extnValue);
      // Compare the two values
      if (!bytesEqual(aaguid, extValue))
        fail("Certificate extension id-fido-gen-ce-aaguid (1.3.6.1.4.1.45724.1.1.4) value of \"" + toHex(extValue) +
             "\" was present but not equal to attestation statement AAGUID value of \"" + toHex(aaguid) + "\"");
    }

    // ---- SettingsService default root certificates (services/defaultRootCerts) ----------

    // The PEM text exactly as the library stores it: roots 3 and 4 start with a newline, so
    // the android-key comparison against a PEM built from x5c can never match them.
    const char* const Google_Hardware_Attestation_Root_1 = R"PEM(-----BEGIN CERTIFICATE-----
MIIFYDCCA0igAwIBAgIJAOj6GWMU0voYMA0GCSqGSIb3DQEBCwUAMBsxGTAXBgNV
BAUTEGY5MjAwOWU4NTNiNmIwNDUwHhcNMTYwNTI2MTYyODUyWhcNMjYwNTI0MTYy
ODUyWjAbMRkwFwYDVQQFExBmOTIwMDllODUzYjZiMDQ1MIICIjANBgkqhkiG9w0B
AQEFAAOCAg8AMIICCgKCAgEAr7bHgiuxpwHsK7Qui8xUFmOr75gvMsd/dTEDDJdS
Sxtf6An7xyqpRR90PL2abxM1dEqlXnf2tqw1Ne4Xwl5jlRfdnJLmN0pTy/4lj4/7
tv0Sk3iiKkypnEUtR6WfMgH0QZfKHM1+di+y9TFRtv6y//0rb+T+W8a9nsNL/ggj
nar86461qO0rOs2cXjp3kOG1FEJ5MVmFmBGtnrKpa73XpXyTqRxB/M0n1n/W9nGq
C4FSYa04T6N5RIZGBN2z2MT5IKGbFlbC8UrW0DxW7AYImQQcHtGl/m00QLVWutHQ
oVJYnFPlXTcHYvASLu+RhhsbDmxMgJJ0mcDpvsC4PjvB+TxywElgS70vE0XmLD+O
JtvsBslHZvPBKCOdT0MS+tgSOIfga+z1Z1g7+DVagf7quvmag8jfPioyKvxnK/Eg
sTUVi2ghzq8wm27ud/mIM7AY2qEORR8Go3TVB4HzWQgpZrt3i5MIlCaY504LzSRi
igHCzAPlHws+W0rB5N+er5/2pJKnfBSDiCiFAVtCLOZ7gLiMm0jhO2B6tUXHI/+M
RPjy02i59lINMRRev56GKtcd9qO/0kUJWdZTdA2XoS82ixPvZtXQpUpuL12ab+9E
aDK8Z4RHJYYfCT3Q5vNAXaiWQ+8PTWm2QgBR/bkwSWc+NpUFgNPN9PvQi8WEg5Um
AGMCAwEAAaOBpjCBozAdBgNVHQ4EFgQUNmHhAHyIBQlRi0RsR/8aTMnqTxIwHwYD
VR0jBBgwFoAUNmHhAHyIBQlRi0RsR/8aTMnqTxIwDwYDVR0TAQH/BAUwAwEB/zAO
BgNVHQ8BAf8EBAMCAYYwQAYDVR0fBDkwNzA1oDOgMYYvaHR0cHM6Ly9hbmRyb2lk
Lmdvb2dsZWFwaXMuY29tL2F0dGVzdGF0aW9uL2NybC8wDQYJKoZIhvcNAQELBQAD
ggIBACDIw41L3KlXG0aMiS//cqrG+EShHUGo8HNsw30W1kJtjn6UBwRM6jnmiwfB
Pb8VA91chb2vssAtX2zbTvqBJ9+LBPGCdw/E53Rbf86qhxKaiAHOjpvAy5Y3m00m
qC0w/Zwvju1twb4vhLaJ5NkUJYsUS7rmJKHHBnETLi8GFqiEsqTWpG/6ibYCv7rY
DBJDcR9W62BW9jfIoBQcxUCUJouMPH25lLNcDc1ssqvC2v7iUgI9LeoM1sNovqPm
QUiG9rHli1vXxzCyaMTjwftkJLkf6724DFhuKug2jITV0QkXvaJWF4nUaHOTNA4u
JU9WDvZLI1j83A+/xnAJUucIv/zGJ1AMH2boHqF8CY16LpsYgBt6tKxxWH00XcyD
CdW2KlBCeqbQPcsFmWyWugxdcekhYsAWyoSf818NUsZdBWBaR/OukXrNLfkQ79Iy
ZohZbvabO/X+MVT3rriAoKc8oE2Uws6DF+60PV7/WIPjNvXySdqspImSN78mflxD
qwLqRBYkA3I75qppLGG9rp7UCdRjxMl8ZDBld+7yvHVgt1cVzJx9xnyGCC23Uaic
MDSXYrB4I4WHXPGjxhZuCuPBLTdOLU8YRvMYdEvYebWHMpvwGCF6bAx3JBpIeOQ1
wDB5y0USicV3YgYGmi+NZfhA4URSh77Yd6uuJOJENRaNVTzk
-----END CERTIFICATE-----
)PEM";
    const char* const Google_Hardware_Attestation_Root_2 = R"PEM(-----BEGIN CERTIFICATE-----
MIIFHDCCAwSgAwIBAgIJANUP8luj8tazMA0GCSqGSIb3DQEBCwUAMBsxGTAXBgNV
BAUTEGY5MjAwOWU4NTNiNmIwNDUwHhcNMTkxMTIyMjAzNzU4WhcNMzQxMTE4MjAz
NzU4WjAbMRkwFwYDVQQFExBmOTIwMDllODUzYjZiMDQ1MIICIjANBgkqhkiG9w0B
AQEFAAOCAg8AMIICCgKCAgEAr7bHgiuxpwHsK7Qui8xUFmOr75gvMsd/dTEDDJdS
Sxtf6An7xyqpRR90PL2abxM1dEqlXnf2tqw1Ne4Xwl5jlRfdnJLmN0pTy/4lj4/7
tv0Sk3iiKkypnEUtR6WfMgH0QZfKHM1+di+y9TFRtv6y//0rb+T+W8a9nsNL/ggj
nar86461qO0rOs2cXjp3kOG1FEJ5MVmFmBGtnrKpa73XpXyTqRxB/M0n1n/W9nGq
C4FSYa04T6N5RIZGBN2z2MT5IKGbFlbC8UrW0DxW7AYImQQcHtGl/m00QLVWutHQ
oVJYnFPlXTcHYvASLu+RhhsbDmxMgJJ0mcDpvsC4PjvB+TxywElgS70vE0XmLD+O
JtvsBslHZvPBKCOdT0MS+tgSOIfga+z1Z1g7+DVagf7quvmag8jfPioyKvxnK/Eg
sTUVi2ghzq8wm27ud/mIM7AY2qEORR8Go3TVB4HzWQgpZrt3i5MIlCaY504LzSRi
igHCzAPlHws+W0rB5N+er5/2pJKnfBSDiCiFAVtCLOZ7gLiMm0jhO2B6tUXHI/+M
RPjy02i59lINMRRev56GKtcd9qO/0kUJWdZTdA2XoS82ixPvZtXQpUpuL12ab+9E
aDK8Z4RHJYYfCT3Q5vNAXaiWQ+8PTWm2QgBR/bkwSWc+NpUFgNPN9PvQi8WEg5Um
AGMCAwEAAaNjMGEwHQYDVR0OBBYEFDZh4QB8iAUJUYtEbEf/GkzJ6k8SMB8GA1Ud
IwQYMBaAFDZh4QB8iAUJUYtEbEf/GkzJ6k8SMA8GA1UdEwEB/wQFMAMBAf8wDgYD
VR0PAQH/BAQDAgIEMA0GCSqGSIb3DQEBCwUAA4ICAQBOMaBc8oumXb2voc7XCWnu
XKhBBK3e2KMGz39t7lA3XXRe2ZLLAkLM5y3J7tURkf5a1SutfdOyXAmeE6SRo83U
h6WszodmMkxK5GM4JGrnt4pBisu5igXEydaW7qq2CdC6DOGjG+mEkN8/TA6p3cno
L/sPyz6evdjLlSeJ8rFBH6xWyIZCbrcpYEJzXaUOEaxxXxgYz5/cTiVKN2M1G2ok
QBUIYSY6bjEL4aUN5cfo7ogP3UvliEo3Eo0YgwuzR2v0KR6C1cZqZJSTnghIC/vA
D32KdNQ+c3N+vl2OTsUVMC1GiWkngNx1OO1+kXW+YTnnTUOtOIswUP/Vqd5SYgAI
mMAfY8U9/iIgkQj6T2W6FsScy94IN9fFhE1UtzmLoBIuUFsVXJMTz+Jucth+IqoW
Fua9v1R93/k98p41pjtFX+H8DslVgfP097vju4KDlqN64xV1grw3ZLl4CiOe/A91
oeLm2UHOq6wn3esB4r2EIQKb6jTVGu5sYCcdWpXr0AUVqcABPdgL+H7qJguBw09o
jm6xNIrw2OocrDKsudk/okr/AwqEyPKw9WnMlQgLIKw1rODG2NvU9oR3GVGdMkUB
ZutL8VuFkERQGt6vQ2OCw0sV47VMkuYbacK/xyZFiRcrPJPb41zgbQj9XAEyLKCH
ex0SdDrx+tWUDqG8At2JHA==
-----END CERTIFICATE-----
)PEM";
    const char* const Google_Hardware_Attestation_Root_3 = R"PEM(
-----BEGIN CERTIFICATE-----
MIIFHDCCAwSgAwIBAgIJAMNrfES5rhgxMA0GCSqGSIb3DQEBCwUAMBsxGTAXBgNV
BAUTEGY5MjAwOWU4NTNiNmIwNDUwHhcNMjExMTE3MjMxMDQyWhcNMzYxMTEzMjMx
MDQyWjAbMRkwFwYDVQQFExBmOTIwMDllODUzYjZiMDQ1MIICIjANBgkqhkiG9w0B
AQEFAAOCAg8AMIICCgKCAgEAr7bHgiuxpwHsK7Qui8xUFmOr75gvMsd/dTEDDJdS
Sxtf6An7xyqpRR90PL2abxM1dEqlXnf2tqw1Ne4Xwl5jlRfdnJLmN0pTy/4lj4/7
tv0Sk3iiKkypnEUtR6WfMgH0QZfKHM1+di+y9TFRtv6y//0rb+T+W8a9nsNL/ggj
nar86461qO0rOs2cXjp3kOG1FEJ5MVmFmBGtnrKpa73XpXyTqRxB/M0n1n/W9nGq
C4FSYa04T6N5RIZGBN2z2MT5IKGbFlbC8UrW0DxW7AYImQQcHtGl/m00QLVWutHQ
oVJYnFPlXTcHYvASLu+RhhsbDmxMgJJ0mcDpvsC4PjvB+TxywElgS70vE0XmLD+O
JtvsBslHZvPBKCOdT0MS+tgSOIfga+z1Z1g7+DVagf7quvmag8jfPioyKvxnK/Eg
sTUVi2ghzq8wm27ud/mIM7AY2qEORR8Go3TVB4HzWQgpZrt3i5MIlCaY504LzSRi
igHCzAPlHws+W0rB5N+er5/2pJKnfBSDiCiFAVtCLOZ7gLiMm0jhO2B6tUXHI/+M
RPjy02i59lINMRRev56GKtcd9qO/0kUJWdZTdA2XoS82ixPvZtXQpUpuL12ab+9E
aDK8Z4RHJYYfCT3Q5vNAXaiWQ+8PTWm2QgBR/bkwSWc+NpUFgNPN9PvQi8WEg5Um
AGMCAwEAAaNjMGEwHQYDVR0OBBYEFDZh4QB8iAUJUYtEbEf/GkzJ6k8SMB8GA1Ud
IwQYMBaAFDZh4QB8iAUJUYtEbEf/GkzJ6k8SMA8GA1UdEwEB/wQFMAMBAf8wDgYD
VR0PAQH/BAQDAgIEMA0GCSqGSIb3DQEBCwUAA4ICAQBTNNZe5cuf8oiq+jV0itTG
zWVhSTjOBEk2FQvh11J3o3lna0o7rd8RFHnN00q4hi6TapFhh4qaw/iG6Xg+xOan
63niLWIC5GOPFgPeYXM9+nBb3zZzC8ABypYuCusWCmt6Tn3+Pjbz3MTVhRGXuT/T
QH4KGFY4PhvzAyXwdjTOCXID+aHud4RLcSySr0Fq/L+R8TWalvM1wJJPhyRjqRCJ
erGtfBagiALzvhnmY7U1qFcS0NCnKjoO7oFedKdWlZz0YAfu3aGCJd4KHT0MsGiL
Zez9WP81xYSrKMNEsDK+zK5fVzw6jA7cxmpXcARTnmAuGUeI7VVDhDzKeVOctf3a
0qQLwC+d0+xrETZ4r2fRGNw2YEs2W8Qj6oDcfPvq9JySe7pJ6wcHnl5EZ0lwc4xH
7Y4Dx9RA1JlfooLMw3tOdJZH0enxPXaydfAD3YifeZpFaUzicHeLzVJLt9dvGB0b
HQLE4+EqKFgOZv2EoP686DQqbVS1u+9k0p2xbMA105TBIk7npraa8VM0fnrRKi7w
lZKwdH+aNAyhbXRW9xsnODJ+g8eF452zvbiKKngEKirK5LGieoXBX7tZ9D1GNBH2
Ob3bKOwwIWdEFle/YF/h6zWgdeoaNGDqVBrLr2+0DtWoiB1aDEjLWl9FmyIUyUm7
mD/vFDkzF+wm7cyWpQpCVQ==
-----END CERTIFICATE-----
)PEM";
    const char* const Google_Hardware_Attestation_Root_4 = R"PEM(
-----BEGIN CERTIFICATE-----
MIIFHDCCAwSgAwIBAgIJAPHBcqaZ6vUdMA0GCSqGSIb3DQEBCwUAMBsxGTAXBgNV
BAUTEGY5MjAwOWU4NTNiNmIwNDUwHhcNMjIwMzIwMTgwNzQ4WhcNNDIwMzE1MTgw
NzQ4WjAbMRkwFwYDVQQFExBmOTIwMDllODUzYjZiMDQ1MIICIjANBgkqhkiG9w0B
AQEFAAOCAg8AMIICCgKCAgEAr7bHgiuxpwHsK7Qui8xUFmOr75gvMsd/dTEDDJdS
Sxtf6An7xyqpRR90PL2abxM1dEqlXnf2tqw1Ne4Xwl5jlRfdnJLmN0pTy/4lj4/7
tv0Sk3iiKkypnEUtR6WfMgH0QZfKHM1+di+y9TFRtv6y//0rb+T+W8a9nsNL/ggj
nar86461qO0rOs2cXjp3kOG1FEJ5MVmFmBGtnrKpa73XpXyTqRxB/M0n1n/W9nGq
C4FSYa04T6N5RIZGBN2z2MT5IKGbFlbC8UrW0DxW7AYImQQcHtGl/m00QLVWutHQ
oVJYnFPlXTcHYvASLu+RhhsbDmxMgJJ0mcDpvsC4PjvB+TxywElgS70vE0XmLD+O
JtvsBslHZvPBKCOdT0MS+tgSOIfga+z1Z1g7+DVagf7quvmag8jfPioyKvxnK/Eg
sTUVi2ghzq8wm27ud/mIM7AY2qEORR8Go3TVB4HzWQgpZrt3i5MIlCaY504LzSRi
igHCzAPlHws+W0rB5N+er5/2pJKnfBSDiCiFAVtCLOZ7gLiMm0jhO2B6tUXHI/+M
RPjy02i59lINMRRev56GKtcd9qO/0kUJWdZTdA2XoS82ixPvZtXQpUpuL12ab+9E
aDK8Z4RHJYYfCT3Q5vNAXaiWQ+8PTWm2QgBR/bkwSWc+NpUFgNPN9PvQi8WEg5Um
AGMCAwEAAaNjMGEwHQYDVR0OBBYEFDZh4QB8iAUJUYtEbEf/GkzJ6k8SMB8GA1Ud
IwQYMBaAFDZh4QB8iAUJUYtEbEf/GkzJ6k8SMA8GA1UdEwEB/wQFMAMBAf8wDgYD
VR0PAQH/BAQDAgIEMA0GCSqGSIb3DQEBCwUAA4ICAQB8cMqTllHc8U+qCrOlg3H7
174lmaCsbo/bJ0C17JEgMLb4kvrqsXZs01U3mB/qABg/1t5Pd5AORHARs1hhqGIC
W/nKMav574f9rZN4PC2ZlufGXb7sIdJpGiO9ctRhiLuYuly10JccUZGEHpHSYM2G
tkgYbZba6lsCPYAAP83cyDV+1aOkTf1RCp/lM0PKvmxYN10RYsK631jrleGdcdkx
oSK//mSQbgcWnmAEZrzHoF1/0gso1HZgIn0YLzVhLSA/iXCX4QT2h3J5z3znluKG
1nv8NQdxei2DIIhASWfu804CA96cQKTTlaae2fweqXjdN1/v2nqOhngNyz1361mF
mr4XmaKH/ItTwOe72NI9ZcwS1lVaCvsIkTDCEXdm9rCNPAY10iTunIHFXRh+7KPz
lHGewCq/8TOohBRn0/NNfh7uRslOSZ/xKbN9tMBtw37Z8d2vvnXq/YWdsm1+JLVw
n6yYD/yacNJBlwpddla8eaVMjsF6nBnIgQOf9zKSe06nSTqvgwUHosgOECZJZ1Eu
zbH4yswbt02tKtKEFhx+v+OTge/06V+jGsqTWLsfrOCNLuA8H++z+pUENmpqnnHo
vaI47gC+TNpkgYGkkBT6B/m/U01BuOBBTzhIlMEZq9qkDWuM2cA5kW5V3FJUcfHn
w1IdYIg2Wxg7yHcQZemFQg==
-----END CERTIFICATE-----
)PEM";
    const char* const GlobalSign_Root_CA = R"PEM(-----BEGIN CERTIFICATE-----
MIIDdTCCAl2gAwIBAgILBAAAAAABFUtaw5QwDQYJKoZIhvcNAQEFBQAwVzELMAkG
A1UEBhMCQkUxGTAXBgNVBAoTEEdsb2JhbFNpZ24gbnYtc2ExEDAOBgNVBAsTB1Jv
b3QgQ0ExGzAZBgNVBAMTEkdsb2JhbFNpZ24gUm9vdCBDQTAeFw05ODA5MDExMjAw
MDBaFw0yODAxMjgxMjAwMDBaMFcxCzAJBgNVBAYTAkJFMRkwFwYDVQQKExBHbG9i
YWxTaWduIG52LXNhMRAwDgYDVQQLEwdSb290IENBMRswGQYDVQQDExJHbG9iYWxT
aWduIFJvb3QgQ0EwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDaDuaZ
jc6j40+Kfvvxi4Mla+pIH/EqsLmVEQS98GPR4mdmzxzdzxtIK+6NiY6arymAZavp
xy0Sy6scTHAHoT0KMM0VjU/43dSMUBUc71DuxC73/OlS8pF94G3VNTCOXkNz8kHp
1Wrjsok6Vjk4bwY8iGlbKk3Fp1S4bInMm/k8yuX9ifUSPJJ4ltbcdG6TRGHRjcdG
snUOhugZitVtbNV4FpWi6cgKOOvyJBNPc1STE4U6G7weNLWLBYy5d4ux2x8gkasJ
U26Qzns3dLlwR5EiUWMWea6xrkEmCMgZK9FGqkjWZCrXgzT/LCrBbBlDSgeF59N8
9iFo7+ryUp9/k5DPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8E
BTADAQH/MB0GA1UdDgQWBBRge2YaRQ2XyolQL30EzTSo//z9SzANBgkqhkiG9w0B
AQUFAAOCAQEA1nPnfE920I2/7LqivjTFKDK1fPxsnCwrvQmeU79rXqoRSLblCKOz
yj1hTdNGCbM+w6DjY1Ub8rrvrTnhQ7k4o+YviiY776BQVvnGCv04zcQLcFGUl5gE
38NflNUVyRRBnMRddWQVDf9VMOyGj/8N7yy5Y0b2qvzfvGn9LhJIZJrglfCm7ymP
AbEVtQwdpf5pLGkkeB6zpxxxYu7KyJesF12KwvhHhm4qxFYxldBniYUr+WymXUad
DKqC5JlR3XC321Y9YeRq4VzW9v493kHMB65jUr9TU/Qr6cf9tveCX4XSQRjbgbME
HMUfpIBvFSDJ3gyICh3WZlXi/EjJKSZp4A==
-----END CERTIFICATE-----
)PEM";
    const char* const Apple_WebAuthn_Root_CA = R"PEM(-----BEGIN CERTIFICATE-----
MIICEjCCAZmgAwIBAgIQaB0BbHo84wIlpQGUKEdXcTAKBggqhkjOPQQDAzBLMR8w
HQYDVQQDDBZBcHBsZSBXZWJBdXRobiBSb290IENBMRMwEQYDVQQKDApBcHBsZSBJ
bmMuMRMwEQYDVQQIDApDYWxpZm9ybmlhMB4XDTIwMDMxODE4MjEzMloXDTQ1MDMx
NTAwMDAwMFowSzEfMB0GA1UEAwwWQXBwbGUgV2ViQXV0aG4gUm9vdCBDQTETMBEG
A1UECgwKQXBwbGUgSW5jLjETMBEGA1UECAwKQ2FsaWZvcm5pYTB2MBAGByqGSM49
AgEGBSuBBAAiA2IABCJCQ2pTVhzjl4Wo6IhHtMSAzO2cv+H9DQKev3//fG59G11k
xu9eI0/7o6V5uShBpe1u6l6mS19S1FEh6yGljnZAJ+2GNP1mi/YK2kSXIuTHjxA/
pcoRf7XkOtO4o1qlcaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUJtdk
2cV4wlpn0afeaxLQG2PxxtcwDgYDVR0PAQH/BAQDAgEGMAoGCCqGSM49BAMDA2cA
MGQCMFrZ+9DsJ1PW9hfNdBywZDsWDbWFp28it1d/5w2RPkRX3Bbn/UbDTNLx7Jr3
jAGGiQIwHFj+dJZYUJR786osByBelJYsVZd2GbHQu209b5RCmGQ21gpSAk9QZW4B
1bWeT0vT
-----END CERTIFICATE-----
)PEM";

    // `SettingsService.getRootCertificates({ identifier })`
    std::vector<std::string> getRootCertificates(std::string_view identifier) {
      if (identifier == "android-key")
        return {Google_Hardware_Attestation_Root_1, Google_Hardware_Attestation_Root_2, Google_Hardware_Attestation_Root_3, Google_Hardware_Attestation_Root_4};
      if (identifier == "android-safetynet") return {GlobalSign_Root_CA};
      if (identifier == "apple") return {Apple_WebAuthn_Root_CA};
      return {};
    }

    // ---- @peculiar/x509 X509Certificate / X509Crl as validateCertificatePath reads them -----

    // `new X509Certificate(pem)`
    Certificate parsePemCertificate(std::string_view pem) {
      std::string body(pem);
      for (const std::string_view marker : {std::string_view("-----BEGIN CERTIFICATE-----"), std::string_view("-----END CERTIFICATE-----")}) {
        const std::size_t at = body.find(marker);
        if (at != std::string::npos) body.erase(at, marker.size());
      }
      std::string base64;
      for (char c : body)
        if (!rt::isJsWhitespace(static_cast<unsigned char>(c))) base64 += c;
      return parseCertificate(rt::fromBase64(base64));
    }

    // `cert.subject === cert.issuer` (the Name strings).
    bool sameName(const std::vector<RelativeDistinguishedName>& a, const std::vector<RelativeDistinguishedName>& b) {
      if (a.size() != b.size()) return false;
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].size() != b[i].size()) return false;
        for (std::size_t j = 0; j < a[i].size(); ++j)
          if (a[i][j].type != b[i][j].type || a[i][j].value != b[i][j].value) return false;
      }
      return true;
    }

    // `subject.verify({ publicKey: issuer.publicKey, signatureOnly: true }, crypto)`: a key that
    // does not import for the signature algorithm verifies false; an algorithm @peculiar/x509
    // cannot map throws.
    bool verifyCertificateSignature(const Certificate& subject, const Certificate& issuer) {
      const std::string& algorithm = subject.signatureAlgorithm;
      enum class Scheme { rsaPkcs1, rsaPss, ecdsa, ed25519 };
      Scheme scheme = Scheme::rsaPkcs1;
      const EVP_MD* md = nullptr;
      int saltLength = 20;
      if (algorithm == "1.2.840.113549.1.1.5") md = EVP_sha1();
      else if (algorithm == "1.2.840.113549.1.1.11") md = EVP_sha256();
      else if (algorithm == "1.2.840.113549.1.1.12") md = EVP_sha384();
      else if (algorithm == "1.2.840.113549.1.1.13") md = EVP_sha512();
      else if (algorithm == "1.2.840.10045.4.1") {
        scheme = Scheme::ecdsa;
        md = EVP_sha1();
      } else if (algorithm == "1.2.840.10045.4.3.2") {
        scheme = Scheme::ecdsa;
        md = EVP_sha256();
      } else if (algorithm == "1.2.840.10045.4.3.3") {
        scheme = Scheme::ecdsa;
        md = EVP_sha384();
      } else if (algorithm == "1.2.840.10045.4.3.4") {
        scheme = Scheme::ecdsa;
        md = EVP_sha512();
      } else if (algorithm == "1.3.101.112") {
        scheme = Scheme::ed25519;
      } else if (algorithm == "1.2.840.113549.1.1.10") {
        // RSASSA-PSS-params: hashAlgorithm [0] (default SHA-1), saltLength [2] (default 20).
        scheme = Scheme::rsaPss;
        md = EVP_sha1();
        const asn1::Node top = asn1::parse(subject.signatureAlgorithmRaw);
        if (top.value.size() > 1 && asn1::isUniversal(top.value[1], asn1::Type::Sequence)) {
          for (const asn1::Node& field : top.value[1].value) {
            if (asn1::is(field, 3, 0, true) && !field.value.empty() && !field.value[0].value.empty()) {
              const std::string hash = asn1::oidString(field.value[0].value[0]);
              if (hash == "2.16.840.1.101.3.4.2.1") md = EVP_sha256();
              else if (hash == "2.16.840.1.101.3.4.2.2") md = EVP_sha384();
              else if (hash == "2.16.840.1.101.3.4.2.3") md = EVP_sha512();
            } else if (asn1::is(field, 3, 2, true) && !field.value.empty()) {
              double value = 0;
              for (std::uint8_t byte : field.value[0].valueHex) value = value * 256 + byte;
              saltLength = static_cast<int>(value);
            }
          }
        }
      } else {
        fail("Cannot get WebCrypto algorithm for '" + algorithm + "'");
      }
      const unsigned char* cursor = issuer.subjectPublicKeyInfoRaw.data();
      ossl::Pkey key(d2i_PUBKEY(nullptr, &cursor, static_cast<long>(issuer.subjectPublicKeyInfoRaw.size())));
      if (!key) return false;
      const int type = EVP_PKEY_get_base_id(key.get());
      if ((scheme == Scheme::rsaPkcs1 || scheme == Scheme::rsaPss) && type != EVP_PKEY_RSA) return false;
      if (scheme == Scheme::ecdsa && type != EVP_PKEY_EC) return false;
      if (scheme == Scheme::ed25519 && type != EVP_PKEY_ED25519) return false;
      return ossl::digestVerify(key.get(), md, subject.signatureValue, subject.tbsRaw,
                                scheme == Scheme::rsaPss ? std::optional<int>(saltLength) : std::nullopt);
    }

    // X509Certificate.serialNumber: hex, without a leading 00 before a byte above 0x7F.
    std::string certificateSerialHex(const Certificate& certificate) {
      BytesView serial(certificate.serialNumber);
      if (serial.size() > 1 && serial[0] == 0x00 && serial[1] > 0x7F) serial = serial.subspan(1);
      return toHex(serial);
    }

    struct CachedCrl {
      std::vector<std::string> revokedCerts;
      std::optional<double> nextUpdate;
    };

    // The module's `cacheRevokedCerts` (keyed by the CA key identifier).
    std::map<std::string, CachedCrl>& cacheRevokedCerts() {
      static std::map<std::string, CachedCrl> cache;
      return cache;
    }

    // `new X509Crl(bytes)`: nextUpdate (undefined when absent) and the entries' serial hex.
    CachedCrl parseCrl(BytesView bytes) {
      const asn1::Node top = asn1::parse(bytes);
      if (!asn1::isUniversal(top, asn1::Type::Sequence) || top.value.size() < 3 || !asn1::isUniversal(top.value[0], asn1::Type::Sequence))
        schemaMismatch("CertificateList");
      const asn1::Node& tbs = top.value[0];
      std::size_t index = 0;
      if (index < tbs.value.size() && asn1::isUniversal(tbs.value[index], asn1::Type::Integer)) ++index;
      (void)parseAlgorithmIdentifier(requireChild(tbs, index++, "TBSCertList"));
      (void)parseName(requireChild(tbs, index++, "TBSCertList"), "Name");
      (void)parseTime(requireChild(tbs, index++, "TBSCertList"));
      CachedCrl crl;
      if (index < tbs.value.size() && (asn1::isUniversal(tbs.value[index], asn1::Type::UTCTime) || asn1::isUniversal(tbs.value[index], asn1::Type::GeneralizedTime))) {
        const double nextUpdate = parseTime(tbs.value[index]);
        // `nextUpdate?.getTime() || undefined`
        if (nextUpdate != 0 && !std::isnan(nextUpdate)) crl.nextUpdate = nextUpdate;
        ++index;
      }
      if (index < tbs.value.size() && asn1::isUniversal(tbs.value[index], asn1::Type::Sequence)) {
        for (const asn1::Node& entry : tbs.value[index].value) {
          if (!asn1::isUniversal(entry, asn1::Type::Sequence) || entry.value.size() < 2 || !asn1::isUniversal(entry.value[0], asn1::Type::Integer))
            schemaMismatch("RevokedCertificate");
          crl.revokedCerts.push_back(toHex(entry.value[0].valueHex));
        }
      }
      return crl;
    }

    // The keyIdentifier of an AuthorityKeyIdentifier extension ([0] IMPLICIT OCTET STRING).
    std::string authorityKeyId(BytesView extnValue) {
      const asn1::Node node = asn1::parse(extnValue);
      if (!asn1::isUniversal(node, asn1::Type::Sequence)) schemaMismatch("AuthorityKeyIdentifier");
      for (const asn1::Node& field : node.value)
        if (field.tagClass == 3 && field.tagNumber == 0 && !field.isConstructed) return toHex(field.valueHex);
      return "";
    }

    // `extCRLDistributionPoints?.distributionPoints?.[0].distributionPoint?.fullName?.[0].uniformResourceIdentifier`
    std::optional<std::string> crlDistributionPointUrl(BytesView extnValue) {
      const asn1::Node points = asn1::parse(extnValue);
      if (!asn1::isUniversal(points, asn1::Type::Sequence)) schemaMismatch("CRLDistributionPoints");
      if (points.value.empty()) throw rt::TypeError("undefined is not an object (evaluating 'distributionPoints?.[0].distributionPoint')");
      const asn1::Node& point = points.value[0];
      for (const asn1::Node& field : point.value) {
        if (!(field.tagClass == 3 && field.tagNumber == 0 && field.isConstructed)) continue;
        // DistributionPointName: fullName [0] GeneralNames
        for (const asn1::Node& name : field.value) {
          if (!(name.tagClass == 3 && name.tagNumber == 0 && name.isConstructed)) continue;
          if (name.value.empty()) throw rt::TypeError("undefined is not an object (evaluating 'fullName?.[0].uniformResourceIdentifier')");
          const asn1::Node& general = name.value[0];
          if (general.tagClass == 3 && general.tagNumber == 6 && !general.isConstructed) return asn1::latin1(general.valueHex);
          return std::nullopt;
        }
        return std::nullopt;
      }
      return std::nullopt;
    }

    /**
     * A method to pull a CRL from a certificate and compare its serial number to the list of revoked
     * certificate serial numbers within the CRL.
     *
     * CRL certificate structure referenced from https://tools.ietf.org/html/rfc5280#page-117
     */
    rt::Task<bool> isCertRevoked(Certificate cert) {
      if (!cert.extensions) co_return false;
      const Extension* extAuthorityKeyID = nullptr;
      const Extension* extSubjectKeyID = nullptr;
      const Extension* extCRLDistributionPoints = nullptr;
      for (const Extension& ext : *cert.extensions) {
        if (ext.extnID == "2.5.29.35") extAuthorityKeyID = &ext;
        else if (ext.extnID == "2.5.29.14") extSubjectKeyID = &ext;
        else if (ext.extnID == "2.5.29.31") extCRLDistributionPoints = &ext;
      }
      // Check to see if we've got cached info for the cert's CA
      std::string keyIdentifier;
      const std::string authorityKeyIdentifier = extAuthorityKeyID != nullptr ? authorityKeyId(extAuthorityKeyID->extnValue) : std::string();
      if (extAuthorityKeyID != nullptr && !authorityKeyIdentifier.empty()) {
        keyIdentifier = authorityKeyIdentifier;
      } else if (extSubjectKeyID != nullptr) {
        /**
         * We might be dealing with a self-signed root certificate. Check the
         * Subject key Identifier extension next.
         */
        keyIdentifier = toHex(parseOctetString(extSubjectKeyID->extnValue));
      }
      const std::string serialNumber = certificateSerialHex(cert);
      if (!keyIdentifier.empty()) {
        const auto cached = cacheRevokedCerts().find(keyIdentifier);
        if (cached != cacheRevokedCerts().end()) {
          const double now = rt::dateNow();
          // If there's a nextUpdate then make sure we're before it
          if (!cached->second.nextUpdate || *cached->second.nextUpdate > now)
            co_return rt::includes(cached->second.revokedCerts, serialNumber);
        }
      }
      const std::optional<std::string> crlURL = extCRLDistributionPoints != nullptr ? crlDistributionPointUrl(extCRLDistributionPoints->extnValue) : std::nullopt;
      // If no URL is provided then we have nothing to check
      if (!crlURL || crlURL->empty()) co_return false;
      // Download and read the CRL
      Bytes certListBytes;
      bool downloaded = false;
      try {
        auto fetching = rt::fetch(rt::RequestInfo(*crlURL), rt::RequestInit());
        const rt::Response respCRL = co_await std::move(fetching);
        auto reading = respCRL.arrayBuffer();
        certListBytes = co_await std::move(reading);
        downloaded = true;
      } catch (...) {
      }
      if (!downloaded) co_return false;
      CachedCrl newCached;
      try {
        newCached = parseCrl(certListBytes);
      } catch (...) {
        // Something was malformed with the CRL, so pass
        co_return false;
      }
      // Cache the results
      if (!keyIdentifier.empty()) cacheRevokedCerts()[keyIdentifier] = newCached;
      co_return rt::includes(newCached.revokedCerts, serialNumber);
    }

    // A custom error that tells validateCertificatePath to try the next trust anchor.
    struct InvalidSubjectAndIssuer : rt::JsError {
      InvalidSubjectAndIssuer() : rt::JsError("Subject issuer did not match issuer subject", "InvalidSubjectAndIssuer") {}
    };

    /**
     * Traverse an array of PEM certificates and ensure they form a proper chain
     * @param x5cCertsPEM Typically the result of `x5c.map(convertASN1toPEM)`
     * @param trustAnchorsPEM PEM-formatted certs that an attestation statement x5c may chain back to
     */
    rt::Task<bool> validateCertificatePath(std::vector<std::string> x5cCertsPEM, std::vector<std::string> trustAnchorsPEM) {
      if (trustAnchorsPEM.empty()) {
        // We have no trust anchors to chain back to, so skip path validation
        co_return true;
      }
      // Prepare to work with x5c certs
      std::vector<Certificate> x5cCertsParsed;
      for (const std::string& certPEM : x5cCertsPEM) x5cCertsParsed.push_back(parsePemCertificate(certPEM));
      // Check for any expired or temporally invalid certs in x5c
      for (std::size_t i = 0; i < x5cCertsParsed.size(); i++) {
        const Certificate& cert = x5cCertsParsed[i];
        bool revoked = false;
        try {
          rt::Task<bool> checking = isCertRevoked(cert);
          revoked = co_await std::move(checking);
        } catch (...) {
          revoked = true;
        }
        if (revoked) fail("Found revoked certificate in x5c:\n" + x5cCertsPEM[i]);
        const double now = rt::dateNow();
        if (cert.notBefore > now || cert.notAfter < now) fail("Found certificate out of validity period in x5c:\n" + x5cCertsPEM[i]);
      }
      // Prepare to work with trust anchor certs
      std::vector<Certificate> trustAnchorsParsed;
      for (const std::string& certPEM : trustAnchorsPEM) {
        try {
          trustAnchorsParsed.push_back(parsePemCertificate(certPEM));
        } catch (const rt::JsError& err) {
          throw rt::JsError("Could not parse trust anchor certificate:\n" + certPEM, "Error", std::make_shared<rt::JsError>(err));
        }
      }
      // Filter out any expired or temporally invalid trust anchors certs
      std::size_t validTrustAnchors = 0;
      for (const Certificate& cert : trustAnchorsParsed) {
        bool revoked = false;
        try {
          rt::Task<bool> checking = isCertRevoked(cert);
          revoked = co_await std::move(checking);
        } catch (...) {
          revoked = true;
        }
        // Continue processing the other certs
        if (revoked) continue;
        const double now = rt::dateNow();
        if (cert.notBefore > now || cert.notAfter < now) continue;
        validTrustAnchors += 1;
      }
      if (validTrustAnchors == 0) fail("No specified trust anchor was valid for verifying x5c");
      // Try to verify x5c with each trust anchor
      bool invalidSubjectAndIssuerError = false;
      for (const Certificate& anchor : trustAnchorsParsed) {
        try {
          std::vector<const Certificate*> x5cWithTrustAnchor;
          for (const Certificate& cert : x5cCertsParsed) x5cWithTrustAnchor.push_back(&cert);
          x5cWithTrustAnchor.push_back(&anchor);
          // Check signatures, and notBefore and notAfter
          for (std::size_t i = 0; i + 1 < x5cWithTrustAnchor.size(); i++) {
            const Certificate& subject = *x5cWithTrustAnchor[i];
            const Certificate& issuer = *x5cWithTrustAnchor[i + 1];
            // Leaf or intermediate cert, make sure the next cert in the chain signed it
            if (!verifyCertificateSignature(subject, issuer)) throw InvalidSubjectAndIssuer();
            if (sameName(issuer.subject, issuer.issuer)) {
              // Root cert detected, make sure it signed itself
              if (!verifyCertificateSignature(issuer, issuer)) throw InvalidSubjectAndIssuer();
              // Don't process anything else after a root cert
              break;
            }
          }
          // If we successfully validated a path then there's no need to continue. Reset any existing
          // errors that were thrown by earlier trust anchors
          invalidSubjectAndIssuerError = false;
          break;
        } catch (const InvalidSubjectAndIssuer&) {
          invalidSubjectAndIssuerError = true;
        } catch (const rt::JsError& err) {
          throw rt::JsError("Unexpected error while validating certificate path", "Error", std::make_shared<rt::JsError>(err));
        }
      }
      // We tried multiple trust anchors and none of them worked
      if (invalidSubjectAndIssuerError) throw InvalidSubjectAndIssuer();
      co_return true;
    }

    // ---- attestation statement formats (registration/verifications) ----------------------

    struct AttestationFormatVerifierOpts {
      Bytes aaguid;
      Cbor attStmt;
      Bytes authData;
      Bytes clientDataHash;
      Bytes credentialID;
      Bytes credentialPublicKey;
      std::vector<std::string> rootCertificates;
      Bytes rpIdHash;
      bool attestationSafetyNetEnforceCTSCheck = true;
    };

    // `attStmt.get(key)`: the statement must be a Map.
    const Cbor* statementMember(const Cbor& attStmt, std::string_view key) {
      if (attStmt.kind() == Cbor::Kind::null_ || attStmt.kind() == Cbor::Kind::undefined_)
        throw rt::TypeError(std::string(attStmt.kind() == Cbor::Kind::null_ ? "null" : "undefined") + " is not an object (evaluating 'attStmt.get')");
      if (attStmt.kind() != Cbor::Kind::map) throw rt::TypeError("attStmt.get is not a function. (In 'attStmt.get(\"" + std::string(key) + "\")', 'attStmt.get' is undefined)");
      return attStmt.get(key);
    }

    std::optional<Cbor> optionalCbor(const Cbor* value) { return value == nullptr ? std::nullopt : std::optional<Cbor>(*value); }

    /**
     * Verify an attestation response with fmt 'fido-u2f'
     */
    rt::Task<bool> verifyAttestationFIDOU2F(AttestationFormatVerifierOpts options) {
      const Bytes publicKey = convertCOSEtoPKCS(options.credentialPublicKey);
      const Bytes signatureBase = concatBytes({Bytes{0x00}, options.rpIdHash, options.clientDataHash, options.credentialID, publicKey});
      const Cbor* sig = statementMember(options.attStmt, "sig");
      const Cbor* x5c = statementMember(options.attStmt, "x5c");
      if (!truthy(x5c)) fail("No attestation certificate provided in attestation statement (FIDOU2F)");
      if (!truthy(sig)) fail("No attestation signature provided in attestation statement (FIDOU2F)");
      // FIDO spec says that aaguid _must_ equal 0x00 here to be legit
      const double aaguidToHex = rt::parseInt(toHex(options.aaguid), 16);
      if (aaguidToHex != 0) fail("AAGUID \"" + rt::numberToString(aaguidToHex) + "\" was not expected value");
      try {
        // Try validating the certificate path using the root certificates set via SettingsService
        std::vector<std::string> certs = mapCertsToPEM(x5c);
        rt::Task<bool> validating = validateCertificatePath(std::move(certs), options.rootCertificates);
        (void)co_await std::move(validating);
      } catch (const rt::JsError& err) {
        fail(err.message + " (FIDOU2F)");
      }
      VerifySignatureOptions verify;
      verify.signature = requireBytes(sig, "sig");
      verify.data = signatureBase;
      verify.x509Certificate = firstCertificate(x5c);
      verify.hashAlgorithm = numberCbor(-7);
      co_return verifySignature(verify);
    }

    /**
     * Verify an attestation response with fmt 'packed'
     */
    rt::Task<bool> verifyAttestationPacked(AttestationFormatVerifierOpts options) {
      const Cbor* sig = statementMember(options.attStmt, "sig");
      const Cbor* x5c = statementMember(options.attStmt, "x5c");
      const Cbor* alg = statementMember(options.attStmt, "alg");
      if (!truthy(sig)) fail("No attestation signature provided in attestation statement (Packed)");
      if (!truthy(alg)) fail("Attestation statement did not contain alg (Packed)");
      if (!isCOSEAlg(alg)) fail("Attestation statement contained invalid alg " + jsString(alg) + " (Packed)");
      const Bytes signatureBase = concatBytes({options.authData, options.clientDataHash});
      bool verified = false;
      if (truthy(x5c)) {
        const CertificateInfo info = getCertificateInfo(firstCertificate(x5c));
        const CertificateParty& subject = info.subject;
        if (subject.OU != std::optional<std::string>("Authenticator Attestation")) fail("Certificate OU was not \"Authenticator Attestation\" (Packed|Full)");
        if (!rt::truthy(subject.CN)) fail("Certificate CN was empty (Packed|Full)");
        if (!rt::truthy(subject.O)) fail("Certificate O was empty (Packed|Full)");
        if (!rt::truthy(subject.C) || rt::length(*subject.C) != 2) fail("Certificate C was not two-character ISO 3166 code (Packed|Full)");
        if (info.basicConstraintsCA) fail("Certificate basic constraints CA was not `false` (Packed|Full)");
        if (info.version != 2) fail("Certificate version was not `3` (ASN.1 value of 2) (Packed|Full)");
        if (info.notBefore > rt::dateNow()) fail("Certificate not good before \"" + rt::numberToString(info.notBefore) + "\" (Packed|Full)");
        if (info.notAfter < rt::dateNow()) fail("Certificate not good after \"" + rt::numberToString(info.notAfter) + "\" (Packed|Full)");
        // Validate attestation statement AAGUID against leaf cert AAGUID
        try {
          validateExtFIDOGenCEAAGUID(info.parsedCertificate, options.aaguid);
        } catch (const rt::JsError& err) {
          fail(err.message + " (Packed|Full)");
        }
        // MetadataService is DISABLED (never initialized): `getStatement` returns undefined.
        try {
          // Try validating the certificate path using the root certificates set via SettingsService
          std::vector<std::string> certs = mapCertsToPEM(x5c);
          rt::Task<bool> validating = validateCertificatePath(std::move(certs), options.rootCertificates);
          (void)co_await std::move(validating);
        } catch (const rt::JsError& err) {
          fail(err.message + " (Packed|Full)");
        }
        VerifySignatureOptions verify;
        verify.signature = requireBytes(sig, "sig");
        verify.data = signatureBase;
        verify.x509Certificate = firstCertificate(x5c);
        verified = verifySignature(verify);
      } else {
        VerifySignatureOptions verify;
        verify.signature = requireBytes(sig, "sig");
        verify.data = signatureBase;
        verify.credentialPublicKey = options.credentialPublicKey;
        verify.hashAlgorithm = optionalCbor(alg);
        verified = verifySignature(verify);
      }
      co_return verified;
    }

    // JS `a > b` / `a < b` where `a` is an unknown JSON value and `b` a number.
    double jsNumberOperand(const json::value* value) {
      if (value == nullptr) return std::numeric_limits<double>::quiet_NaN();
      return rt::toNumber(*value);
    }

    /**
     * Verify an attestation response with fmt 'android-safetynet'
     */
    rt::Task<bool> verifyAttestationAndroidSafetyNet(AttestationFormatVerifierOpts options) {
      const Cbor* alg = statementMember(options.attStmt, "alg");
      (void)alg;
      const Cbor* response = statementMember(options.attStmt, "response");
      const Cbor* ver = statementMember(options.attStmt, "ver");
      if (!truthy(ver)) fail("No ver value in attestation (SafetyNet)");
      if (!truthy(response)) fail("No response was included in attStmt by authenticator (SafetyNet)");
      // Prepare to verify a JWT
      const std::string jwt = rt::decodeUtf8(BytesView(requireBytes(response, "response")));
      const std::vector<std::string> jwtParts = rt::split(jwt, ".");
      if (jwtParts.size() < 2) throw rt::TypeError("undefined is not an object (evaluating 'data.length')");
      const json::value HEADER = rt::jsonParse(isoBase64URL::toUTF8String(jwtParts[0]));
      const json::value PAYLOAD = rt::jsonParse(isoBase64URL::toUTF8String(jwtParts[1]));
      if (PAYLOAD.is_null()) throw rt::TypeError("Cannot destructure property 'nonce' from null or undefined value");
      /**
       * START Verify PAYLOAD
       */
      const json::value* nonce = rt::get(PAYLOAD, "nonce");
      const json::value* ctsProfileMatch = rt::get(PAYLOAD, "ctsProfileMatch");
      const json::value* timestampMs = rt::get(PAYLOAD, "timestampMs");
      // verifyTimestampMS defaults to true
      {
        // Make sure timestamp is in the past
        const double now = rt::dateNow();
        if (jsNumberOperand(timestampMs) > rt::dateNow()) fail("Payload timestamp \"" + jsonString(timestampMs) + "\" was later than \"" + rt::numberToString(now) + "\" (SafetyNet)");
        // Consider a SafetyNet attestation valid within a minute of it being performed
        // `timestampMs + 60 * 1000`: string concatenation for anything that is not a number,
        // boolean, null or undefined.
        double timestampPlusDelay = std::numeric_limits<double>::quiet_NaN();
        std::string timestampPlusDelayText;
        if (timestampMs == nullptr) {
          timestampPlusDelay = std::numeric_limits<double>::quiet_NaN();
          timestampPlusDelayText = "NaN";
        } else if (rt::isNumber(*timestampMs) || timestampMs->is_bool() || timestampMs->is_null()) {
          timestampPlusDelay = rt::toNumber(*timestampMs) + 60 * 1000;
          timestampPlusDelayText = rt::numberToString(timestampPlusDelay);
        } else {
          timestampPlusDelayText = rt::toString(*timestampMs) + "60000";
          timestampPlusDelay = rt::stringToNumber(timestampPlusDelayText);
        }
        if (timestampPlusDelay < rt::dateNow()) fail("Payload timestamp \"" + timestampPlusDelayText + "\" has expired (SafetyNet)");
      }
      const Bytes nonceBase = concatBytes({options.authData, options.clientDataHash});
      const Bytes nonceBuffer = toHash(nonceBase);
      const std::string expectedNonce = isoBase64URL::fromBuffer(nonceBuffer, false);
      if (!jsonIs(nonce, expectedNonce)) fail("Could not verify payload nonce (SafetyNet)");
      if (options.attestationSafetyNetEnforceCTSCheck && !rt::truthy(ctsProfileMatch)) fail("Could not verify device integrity (SafetyNet)");
      /**
       * END Verify PAYLOAD
       */
      /**
       * START Verify Header
       */
      // `HEADER.x5c[0]` is definitely a base64 string
      const json::value* headerX5c = rt::get(HEADER, "x5c");
      const json::value* leafCert = headerX5c != nullptr && headerX5c->is_array() ? rt::get(headerX5c->as_array(), 0) : nullptr;
      if (leafCert == nullptr || !leafCert->is_string()) throw rt::TypeError("HEADER.x5c[0] is not a base64 string");
      const Bytes leafCertBuffer = isoBase64URL::toBuffer(leafCert->as_string(), false);
      const CertificateInfo leafCertInfo = getCertificateInfo(leafCertBuffer);
      // Ensure the certificate was issued to this hostname
      // See https://developer.android.com/training/safetynet/attestation#verify-attestation-response
      if (leafCertInfo.subject.CN != std::optional<std::string>("attest.android.com")) fail("Certificate common name was not \"attest.android.com\" (SafetyNet)");
      // MetadataService is DISABLED (never initialized): `getStatement` returns undefined.
      try {
        // Try validating the certificate path using the root certificates set via SettingsService
        std::vector<std::string> certs;
        for (const json::value& item : headerX5c->as_array()) {
          Cbor certBuffer;
          if (item.is_string()) {
            certBuffer = Cbor{std::string(item.as_string())};
          } else if (rt::isNumber(item)) {
            certBuffer = numberCbor(rt::toNumber(item));
          } else if (item.is_bool()) {
            certBuffer = Cbor{item.as_bool()};
          } else if (item.is_null()) {
            certBuffer = Cbor{nullptr};
          }
          certs.push_back(convertCertBufferToPEM(certBuffer));
        }
        rt::Task<bool> validating = validateCertificatePath(std::move(certs), options.rootCertificates);
        (void)co_await std::move(validating);
      } catch (const rt::JsError& err) {
        fail(err.message + " (SafetyNet)");
      }
      /**
       * END Verify Header
       */
      /**
       * START Verify Signature
       */
      if (jwtParts.size() < 3) throw rt::TypeError("undefined is not an object (evaluating 'data.length')");
      VerifySignatureOptions verify;
      verify.signature = isoBase64URL::toBuffer(jwtParts[2]);
      verify.data = rt::encodeUtf8(jwtParts[0] + "." + jwtParts[1]);
      verify.x509Certificate = leafCertBuffer;
      co_return verifySignature(verify);
    }

    // The AuthorizationList fields in @peculiar/asn1-android's declaration order (context tags).
    constexpr std::array<int, 45> AUTHORIZATION_LIST_TAGS = {1,   2,   3,   5,   6,   10,  200, 203, 303, 305, 400, 401, 402, 405, 503,
                                                              504, 505, 506, 507, 508, 509, 600, 601, 701, 702, 703, 704, 705, 706, 709,
                                                              710, 711, 712, 713, 714, 715, 716, 717, 718, 719, 720, 723, 724, -1, -1};

    // `authorizationList.allApplications !== undefined`: asn1-schema walks the optional fields in
    // declaration order and only reaches [600] when the elements before it matched in order.
    bool hasAllApplications(const asn1::Node& list) {
      std::size_t admission = 0;
      for (std::size_t i = 0; i < AUTHORIZATION_LIST_TAGS.size() && AUTHORIZATION_LIST_TAGS[i] >= 0; ++i) {
        const std::size_t dataIndex = i - admission;
        if (dataIndex >= list.value.size()) return false;
        const asn1::Node& element = list.value[dataIndex];
        bool matched = asn1::is(element, 3, AUTHORIZATION_LIST_TAGS[i], true);
        if (matched && AUTHORIZATION_LIST_TAGS[i] == 600) matched = !element.value.empty() && asn1::is(element.value[0], 1, 5, false);
        if (!matched) {
          admission++;
          continue;
        }
        if (AUTHORIZATION_LIST_TAGS[i] == 600) return true;
      }
      return false;
    }

    /**
     * Verify an attestation response with fmt 'android-key'
     */
    rt::Task<bool> verifyAttestationAndroidKey(AttestationFormatVerifierOpts options) {
      const Cbor* x5c = statementMember(options.attStmt, "x5c");
      const Cbor* sig = statementMember(options.attStmt, "sig");
      const Cbor* alg = statementMember(options.attStmt, "alg");
      if (!truthy(x5c)) fail("No attestation certificate provided in attestation statement (Android Key)");
      if (!truthy(sig)) fail("No attestation signature provided in attestation statement (Android Key)");
      if (!truthy(alg)) fail("Attestation statement did not contain alg (Android Key)");
      if (!isCOSEAlg(alg)) fail("Attestation statement contained invalid alg " + jsString(alg) + " (Android Key)");
      /**
       * Verify that the public key in the first certificate in x5c matches the credentialPublicKey in
       * the attestedCredentialData in authenticatorData.
       */
      // Find the public cert in the certificate as PKCS
      const Certificate parsedCert = parseCertificate(firstCertificate(x5c));
      // Convert the credentialPublicKey to PKCS
      const Bytes credPubKeyPKCS = convertCOSEtoPKCS(options.credentialPublicKey);
      if (!bytesEqual(credPubKeyPKCS, parsedCert.subjectPublicKey)) fail("Credential public key does not equal leaf cert public key (Android Key)");
      /**
       * Verify that the attestationChallenge field in the attestation certificate extension data is
       * identical to clientDataHash.
       */
      // Find Android KeyStore Extension in certificate extensions
      const Extension* extKeyStore = findExtension(parsedCert, "1.3.6.1.4.1.11129.2.1.17");
      if (extKeyStore == nullptr) fail("Certificate did not contain extKeyStore (Android Key)");
      const asn1::Node keyDescription = asn1::parse(extKeyStore->extnValue);
      if (!asn1::isUniversal(keyDescription, asn1::Type::Sequence) || keyDescription.value.size() < 8 ||
          !asn1::isUniversal(keyDescription.value[0], asn1::Type::Integer) || !asn1::isUniversal(keyDescription.value[1], asn1::Type::Enumerated) ||
          !asn1::isUniversal(keyDescription.value[2], asn1::Type::Integer) || !asn1::isUniversal(keyDescription.value[3], asn1::Type::Enumerated) ||
          !asn1::isUniversal(keyDescription.value[4], asn1::Type::OctetString) || !asn1::isUniversal(keyDescription.value[5], asn1::Type::OctetString) ||
          !asn1::isUniversal(keyDescription.value[6], asn1::Type::Sequence) || !asn1::isUniversal(keyDescription.value[7], asn1::Type::Sequence))
        schemaMismatch("KeyDescription");
      // Verify extKeyStore values
      const Bytes attestationChallenge = keyDescription.value[4].isConstructed ? Bytes() : keyDescription.value[4].valueHex;
      if (!bytesEqual(attestationChallenge, options.clientDataHash)) fail("Attestation challenge was not equal to client data hash (Android Key)");
      /**
       * The AuthorizationList.allApplications field is not present on either authorization list
       * (softwareEnforced nor teeEnforced), since PublicKeyCredential MUST be scoped to the RP ID.
       *
       * (i.e. These shouldn't contain the [600] tag)
       */
      if (hasAllApplications(keyDescription.value[7])) fail("teeEnforced contained \"allApplications [600]\" tag (Android Key)");
      if (hasAllApplications(keyDescription.value[6])) fail("teeEnforced contained \"allApplications [600]\" tag (Android Key)");
      // MetadataService is DISABLED (never initialized): `getStatement` returns undefined.
      /**
       * Verify that x5c contains a full certificate path.
       */
      std::vector<std::string> x5cPEM = mapCertsToPEM(x5c);
      std::vector<std::string> x5cNoRootPEM(x5cPEM.begin(), x5cPEM.empty() ? x5cPEM.begin() : x5cPEM.end() - 1);
      std::vector<std::string> x5cRootPEM;
      if (!x5cPEM.empty()) x5cRootPEM.push_back(x5cPEM.back());
      try {
        rt::Task<bool> validating = validateCertificatePath(x5cNoRootPEM, x5cRootPEM);
        (void)co_await std::move(validating);
      } catch (const rt::JsError& err) {
        throw rt::JsError(err.message + " (Android Key)", "Error", std::make_shared<rt::JsError>(err));
      }
      /**
       * Make sure the root certificate is one of the Google Hardware Attestation Root certificates
       *
       * https://developer.android.com/privacy-and-security/security-key-attestation#root_certificate
       */
      if (!options.rootCertificates.empty() &&
          (x5cRootPEM.empty() || std::find(options.rootCertificates.begin(), options.rootCertificates.end(), x5cRootPEM[0]) == options.rootCertificates.end()))
        fail("x5c root certificate was not a known root certificate (Android Key)");
      /**
       * Verify that sig is a valid signature over the concatenation of authenticatorData and
       * clientDataHash using the public key in the first certificate in x5c with the algorithm
       * specified in alg.
       */
      VerifySignatureOptions verify;
      verify.signature = requireBytes(sig, "sig");
      verify.data = concatBytes({options.authData, options.clientDataHash});
      verify.x509Certificate = firstCertificate(x5c);
      verify.hashAlgorithm = optionalCbor(alg);
      co_return verifySignature(verify);
    }

    // ---- TPM ----------------------------------------------------------------------------

    std::optional<std::string> TPM_ST(std::uint32_t value) {
      switch (value) {
        case 0x00c4: return "TPM_ST_RSP_COMMAND";
        case 0x8000: return "TPM_ST_NULL";
        case 0x8001: return "TPM_ST_NO_SESSIONS";
        case 0x8002: return "TPM_ST_SESSIONS";
        case 0x8014: return "TPM_ST_ATTEST_NV";
        case 0x8015: return "TPM_ST_ATTEST_COMMAND_AUDIT";
        case 0x8016: return "TPM_ST_ATTEST_SESSION_AUDIT";
        case 0x8017: return "TPM_ST_ATTEST_CERTIFY";
        case 0x8018: return "TPM_ST_ATTEST_QUOTE";
        case 0x8019: return "TPM_ST_ATTEST_TIME";
        case 0x801a: return "TPM_ST_ATTEST_CREATION";
        case 0x8021: return "TPM_ST_CREATION";
        case 0x8022: return "TPM_ST_VERIFIED";
        case 0x8023: return "TPM_ST_AUTH_SECRET";
        case 0x8024: return "TPM_ST_HASHCHECK";
        case 0x8025: return "TPM_ST_AUTH_SIGNED";
        case 0x8029: return "TPM_ST_FU_MANIFEST";
        default: return std::nullopt;
      }
    }

    // TPM_ALG (the duplicate 0x0004 key keeps its last value, 'TPM_ALG_SHA1').
    std::optional<std::string> TPM_ALG(std::uint32_t value) {
      switch (value) {
        case 0x0000: return "TPM_ALG_ERROR";
        case 0x0001: return "TPM_ALG_RSA";
        case 0x0004: return "TPM_ALG_SHA1";
        case 0x0005: return "TPM_ALG_HMAC";
        case 0x0006: return "TPM_ALG_AES";
        case 0x0007: return "TPM_ALG_MGF1";
        case 0x0008: return "TPM_ALG_KEYEDHASH";
        case 0x000a: return "TPM_ALG_XOR";
        case 0x000b: return "TPM_ALG_SHA256";
        case 0x000c: return "TPM_ALG_SHA384";
        case 0x000d: return "TPM_ALG_SHA512";
        case 0x0010: return "TPM_ALG_NULL";
        case 0x0012: return "TPM_ALG_SM3_256";
        case 0x0013: return "TPM_ALG_SM4";
        case 0x0014: return "TPM_ALG_RSASSA";
        case 0x0015: return "TPM_ALG_RSAES";
        case 0x0016: return "TPM_ALG_RSAPSS";
        case 0x0017: return "TPM_ALG_OAEP";
        case 0x0018: return "TPM_ALG_ECDSA";
        case 0x0019: return "TPM_ALG_ECDH";
        case 0x001a: return "TPM_ALG_ECDAA";
        case 0x001b: return "TPM_ALG_SM2";
        case 0x001c: return "TPM_ALG_ECSCHNORR";
        case 0x001d: return "TPM_ALG_ECMQV";
        case 0x0020: return "TPM_ALG_KDF1_SP800_56A";
        case 0x0021: return "TPM_ALG_KDF2";
        case 0x0022: return "TPM_ALG_KDF1_SP800_108";
        case 0x0023: return "TPM_ALG_ECC";
        case 0x0025: return "TPM_ALG_SYMCIPHER";
        case 0x0026: return "TPM_ALG_CAMELLIA";
        case 0x0040: return "TPM_ALG_CTR";
        case 0x0041: return "TPM_ALG_OFB";
        case 0x0042: return "TPM_ALG_CBC";
        case 0x0043: return "TPM_ALG_CFB";
        case 0x0044: return "TPM_ALG_ECB";
        default: return std::nullopt;
      }
    }

    std::optional<std::string> TPM_ECC_CURVE(std::uint32_t value) {
      switch (value) {
        case 0x0000: return "TPM_ECC_NONE";
        case 0x0001: return "TPM_ECC_NIST_P192";
        case 0x0002: return "TPM_ECC_NIST_P224";
        case 0x0003: return "TPM_ECC_NIST_P256";
        case 0x0004: return "TPM_ECC_NIST_P384";
        case 0x0005: return "TPM_ECC_NIST_P521";
        case 0x0010: return "TPM_ECC_BN_P256";
        case 0x0011: return "TPM_ECC_BN_P638";
        case 0x0020: return "TPM_ECC_SM2_P256";
        default: return std::nullopt;
      }
    }

    // `TPM_ECC_CURVE_COSE_CRV_MAP[curveID]`
    std::optional<double> TPM_ECC_CURVE_COSE_CRV_MAP(const std::optional<std::string>& curveID) {
      if (curveID == "TPM_ECC_NIST_P256") return 1;  // p256
      if (curveID == "TPM_ECC_NIST_P384") return 2;  // p384
      if (curveID == "TPM_ECC_NIST_P521") return 3;  // p521
      if (curveID == "TPM_ECC_BN_P256") return 1;    // p256
      if (curveID == "TPM_ECC_SM2_P256") return 1;   // p256
      return std::nullopt;
    }

    // `!TPM_MANUFACTURERS[value]` is false for the registry's ids and for every name an object
    // inherits from Object.prototype.
    bool isTpmManufacturer(std::string_view value) {
      static constexpr std::array<std::string_view, 31> IDS = {
        "id:414D4400", "id:414E5400", "id:41544D4C", "id:4252434D", "id:4353434F", "id:464C5953", "id:524F4343", "id:474F4F47",
        "id:48504900", "id:48504500", "id:48495349", "id:49424d00", "id:49424D00", "id:49465800", "id:494E5443", "id:4C454E00",
        "id:4D534654", "id:4E534D20", "id:4E545A00", "id:4E534700", "id:4E544300", "id:51434F4D", "id:534D534E", "id:53454345",
        "id:534E5300", "id:534D5343", "id:53544D20", "id:54584E00", "id:57454300", "id:5345414C", "id:FFFFF1D0"};
      static constexpr std::array<std::string_view, 12> INHERITED = {"constructor",      "__defineGetter__",     "__defineSetter__", "hasOwnProperty",
                                                                      "__lookupGetter__", "__lookupSetter__",     "isPrototypeOf",    "propertyIsEnumerable",
                                                                      "toString",         "valueOf",              "__proto__",        "toLocaleString"};
      return std::find(IDS.begin(), IDS.end(), value) != IDS.end() || std::find(INHERITED.begin(), INHERITED.end(), value) != INHERITED.end();
    }

    struct TpmPubArea {
      std::optional<std::string> type;
      bool hasRsa = false;
      double rsaExponent = 0;
      bool hasEcc = false;
      std::optional<std::string> eccCurveID;
      Bytes unique;
    };

    /**
     * Break apart a TPM attestation's pubArea buffer
     *
     * See 12.2.4 TPMT_PUBLIC here:
     * https://trustedcomputinggroup.org/wp-content/uploads/TPM-Rev-2.0-Part-2-Structures-00.96-130315.pdf
     */
    TpmPubArea parsePubArea(BytesView pubArea) {
      TpmPubArea parsed;
      std::size_t pointer = 0;
      parsed.type = TPM_ALG(getUint(pubArea, pointer, 2));
      pointer += 2;
      (void)getUint(pubArea, pointer, 2);  // nameAlg
      pointer += 2;
      (void)getUint(pubArea, pointer, 4);  // objectAttributes
      pointer += 4;
      // Slice out the authPolicy of dynamic length
      const std::size_t authPolicyLength = getUint(pubArea, pointer, 2);
      pointer += 2;
      pointer += authPolicyLength;
      // Extract additional curve params according to type
      if (parsed.type == "TPM_ALG_RSA") {
        (void)getUint(pubArea, pointer, 2);  // symmetric
        pointer += 2;
        (void)getUint(pubArea, pointer, 2);  // scheme
        pointer += 2;
        (void)getUint(pubArea, pointer, 2);  // keyBits
        pointer += 2;
        parsed.rsaExponent = getUint(pubArea, pointer, 4);
        pointer += 4;
        parsed.hasRsa = true;
        /**
         * See 11.2.4.5 TPM2B_PUBLIC_KEY_RSA here:
         * https://trustedcomputinggroup.org/wp-content/uploads/TPM-Rev-2.0-Part-2-Structures-00.96-130315.pdf
         */
        const std::size_t uniqueLength = getUint(pubArea, pointer, 2);
        pointer += 2;
        parsed.unique = sliceBytes(pubArea, pointer, pointer + uniqueLength);
      } else if (parsed.type == "TPM_ALG_ECC") {
        (void)getUint(pubArea, pointer, 2);  // symmetric
        pointer += 2;
        (void)getUint(pubArea, pointer, 2);  // scheme
        pointer += 2;
        parsed.eccCurveID = TPM_ECC_CURVE(getUint(pubArea, pointer, 2));
        pointer += 2;
        (void)getUint(pubArea, pointer, 2);  // kdf
        pointer += 2;
        parsed.hasEcc = true;
        /**
         * See 11.2.5.1 TPM2B_ECC_PARAMETER here:
         * https://trustedcomputinggroup.org/wp-content/uploads/TPM-Rev-2.0-Part-2-Structures-00.96-130315.pdf
         */
        // Retrieve X
        const std::size_t uniqueXLength = getUint(pubArea, pointer, 2);
        pointer += 2;
        const Bytes uniqueX = sliceBytes(pubArea, pointer, pointer + uniqueXLength);
        pointer += uniqueXLength;
        // Retrieve Y
        const std::size_t uniqueYLength = getUint(pubArea, pointer, 2);
        pointer += 2;
        const Bytes uniqueY = sliceBytes(pubArea, pointer, pointer + uniqueYLength);
        parsed.unique = concatBytes({uniqueX, uniqueY});
      } else {
        fail("Unexpected type \"" + parsed.type.value_or("undefined") + "\" (TPM)");
      }
      return parsed;
    }

    struct TpmCertInfo {
      double magic = 0;
      std::optional<std::string> type;
      Bytes extraData;
      std::optional<std::string> nameAlg;
      Bytes nameAlgBuffer;
      Bytes name;
    };

    /**
     * Cut up a TPM attestation's certInfo into intelligible chunks
     */
    TpmCertInfo parseCertInfo(BytesView certInfo) {
      TpmCertInfo parsed;
      std::size_t pointer = 0;
      // Get a magic constant
      parsed.magic = getUint(certInfo, pointer, 4);
      pointer += 4;
      // Determine the algorithm used for attestation
      parsed.type = TPM_ST(getUint(certInfo, pointer, 2));
      pointer += 2;
      // The name of a parent entity, can be ignored
      const std::size_t qualifiedSignerLength = getUint(certInfo, pointer, 2);
      pointer += 2;
      pointer += qualifiedSignerLength;
      // Get the expected hash of `attsToBeSigned`
      const std::size_t extraDataLength = getUint(certInfo, pointer, 2);
      pointer += 2;
      parsed.extraData = sliceBytes(certInfo, pointer, pointer + extraDataLength);
      pointer += extraDataLength;
      // Information about the TPM device's internal clock, can be ignored
      pointer += 8;
      (void)getUint(certInfo, pointer, 4);  // resetCount
      pointer += 4;
      (void)getUint(certInfo, pointer, 4);  // restartCount
      pointer += 4;
      pointer += 1;  // safe
      // TPM device firmware version
      pointer += 8;
      // Attested Name
      const std::size_t attestedNameLength = getUint(certInfo, pointer, 2);
      pointer += 2;
      parsed.name = sliceBytes(certInfo, pointer, pointer + attestedNameLength);
      pointer += attestedNameLength;
      // Attested qualified name, can be ignored
      (void)getUint(certInfo, pointer, 2);
      parsed.nameAlg = TPM_ALG(getUint(parsed.name, 0, 2));
      parsed.nameAlgBuffer = sliceBytes(parsed.name, 0, 2);
      return parsed;
    }

    // `attestedNameAlgToCOSEAlg(alg)`
    double attestedNameAlgToCOSEAlg(const std::optional<std::string>& alg) {
      if (alg == "TPM_ALG_SHA256") return -7;
      if (alg == "TPM_ALG_SHA384") return -35;
      if (alg == "TPM_ALG_SHA512") return -36;
      fail("Unexpected TPM attested name alg " + alg.value_or("undefined"));
    }

    // `eBuffer[0] + (eBuffer[1] << 8) + (eBuffer[2] << 16)`; std::nullopt when the sum is not a
    // number (a string concatenation or NaN), which never equals the pubArea exponent.
    std::optional<double> exponentSum(const Cbor& eBuffer) {
      const auto element = [&](std::size_t index) -> std::optional<const Cbor*> {
        if (eBuffer.kind() == Cbor::Kind::array) return index < eBuffer.items().size() ? std::optional<const Cbor*>(&eBuffer.items()[index]) : std::nullopt;
        return std::nullopt;
      };
      const auto shifted = [&](std::size_t index, int shift) -> double {
        double value = 0;
        if (eBuffer.kind() == Cbor::Kind::bytes) value = index < eBuffer.bytes().size() ? eBuffer.bytes()[index] : 0;
        else if (const auto item = element(index)) value = cborToNumber(**item);
        return static_cast<double>(rt::toInt32(rt::toInt32(value) * std::pow(2.0, shift)));
      };
      double first = std::numeric_limits<double>::quiet_NaN();
      if (eBuffer.kind() == Cbor::Kind::bytes) {
        if (!eBuffer.bytes().empty()) first = eBuffer.bytes()[0];
      } else if (const auto item = element(0)) {
        const Cbor& value = **item;
        if (value.kind() == Cbor::Kind::number) first = value.number();
        else if (value.kind() == Cbor::Kind::boolean) first = value.boolean() ? 1 : 0;
        else if (value.kind() == Cbor::Kind::null_) first = 0;
        else if (value.kind() != Cbor::Kind::undefined_) return std::nullopt;  // string concatenation
      } else if (eBuffer.kind() == Cbor::Kind::string) {
        if (!eBuffer.text().empty()) return std::nullopt;  // `"x"[0] + ...` concatenates
      }
      const double sum = first + shifted(1, 8) + shifted(2, 16);
      if (std::isnan(sum)) return std::nullopt;
      return sum;
    }

    // The TPM values buried in a subjectAltName directoryName.
    struct TcgAtTpmValues {
      std::optional<std::string> tcgAtTpmManufacturer;
      std::optional<std::string> tcgAtTpmModel;
      std::optional<std::string> tcgAtTpmVersion;
    };

    TcgAtTpmValues getTcgAtTpmValues(const std::vector<RelativeDistinguishedName>& root) {
      TcgAtTpmValues values;
      for (const RelativeDistinguishedName& relName : root) {
        for (const NameAttribute& attr : relName) {
          if (attr.type == "2.23.133.2.1") values.tcgAtTpmManufacturer = attr.value;
          else if (attr.type == "2.23.133.2.2") values.tcgAtTpmModel = attr.value;
          else if (attr.type == "2.23.133.2.3") values.tcgAtTpmVersion = attr.value;
        }
      }
      return values;
    }

    rt::Task<bool> verifyAttestationTPM(AttestationFormatVerifierOpts options) {
      const Cbor* ver = statementMember(options.attStmt, "ver");
      const Cbor* sig = statementMember(options.attStmt, "sig");
      const Cbor* alg = statementMember(options.attStmt, "alg");
      const Cbor* x5c = statementMember(options.attStmt, "x5c");
      const Cbor* pubArea = statementMember(options.attStmt, "pubArea");
      const Cbor* certInfo = statementMember(options.attStmt, "certInfo");
      /**
       * Verify structures
       */
      if (ver == nullptr || ver->kind() != Cbor::Kind::string || ver->text() != "2.0") fail("Unexpected ver \"" + jsString(ver) + "\", expected \"2.0\" (TPM)");
      if (!truthy(sig)) fail("No attestation signature provided in attestation statement (TPM)");
      if (!truthy(alg)) fail("Attestation statement did not contain alg (TPM)");
      if (!isCOSEAlg(alg)) fail("Attestation statement contained invalid alg " + jsString(alg) + " (TPM)");
      if (!truthy(x5c)) fail("No attestation certificate provided in attestation statement (TPM)");
      if (!truthy(pubArea)) fail("Attestation statement did not contain pubArea (TPM)");
      if (!truthy(certInfo)) fail("Attestation statement did not contain certInfo (TPM)");
      const Bytes& pubAreaBytes = requireBytes(pubArea, "pubArea");
      const TpmPubArea parsedPubArea = parsePubArea(pubAreaBytes);
      // Verify that the public key specified by the parameters and unique fields of pubArea is
      // identical to the credentialPublicKey in the attestedCredentialData in authenticatorData.
      const Cbor cosePublicKey = isoCBOR::decodeFirst(options.credentialPublicKey);
      const Cbor& coseKey = requireMap(cosePublicKey, "cosePublicKey", "COSEKEYS.kty");
      if (parsedPubArea.type == "TPM_ALG_RSA") {
        if (!isCOSEPublicKeyRSA(coseKey)) fail("Credential public key with kty " + jsString(coseKey.get(COSEKEYS::kty)) + " did not match TPM_ALG_RSA");
        const Cbor* n = coseKey.get(COSEKEYS::n);
        const Cbor* e = coseKey.get(COSEKEYS::e);
        if (!truthy(n)) fail("COSE public key missing n (TPM|RSA)");
        if (!truthy(e)) fail("COSE public key missing e (TPM|RSA)");
        if (!areEqual(parsedPubArea.unique, *n)) fail("PubArea unique is not same as credentialPublicKey (TPM|RSA)");
        if (!parsedPubArea.hasRsa) fail("Parsed pubArea type is RSA, but missing parameters.rsa (TPM|RSA)");
        // If `exponent` is equal to 0x00, then exponent is the default RSA exponent of 2^16+1 (65537)
        const double pubAreaExponent = parsedPubArea.rsaExponent != 0 ? parsedPubArea.rsaExponent : 65537;
        // Do some bit shifting to get to an integer
        const std::optional<double> eSum = exponentSum(*e);
        if (!eSum || pubAreaExponent != *eSum) fail("Unexpected public key exp, expected " + rt::numberToString(pubAreaExponent) + " (TPM|RSA)");
      } else if (parsedPubArea.type == "TPM_ALG_ECC") {
        if (!isCOSEPublicKeyEC2(coseKey)) fail("Credential public key with kty " + jsString(coseKey.get(COSEKEYS::kty)) + " did not match TPM_ALG_ECC");
        const Cbor* crv = coseKey.get(COSEKEYS::crv);
        const Cbor* x = coseKey.get(COSEKEYS::x);
        const Cbor* y = coseKey.get(COSEKEYS::y);
        if (!truthy(crv)) fail("COSE public key missing crv (TPM|ECC)");
        if (!truthy(x)) fail("COSE public key missing x (TPM|ECC)");
        if (!truthy(y)) fail("COSE public key missing y (TPM|ECC)");
        const Cbor uniqueXY = bytesCbor(jsConcat({x, y}));
        if (!areEqual(parsedPubArea.unique, uniqueXY)) fail("PubArea unique is not same as public key x and y (TPM|ECC)");
        if (!parsedPubArea.hasEcc) fail("Parsed pubArea type is ECC, but missing parameters.ecc (TPM|ECC)");
        const std::optional<double> pubAreaCurveIDMapToCOSECRV = TPM_ECC_CURVE_COSE_CRV_MAP(parsedPubArea.eccCurveID);
        if (!pubAreaCurveIDMapToCOSECRV || !numberIs(crv, *pubAreaCurveIDMapToCOSECRV)) fail("Public area key curve ID did not match public key crv (TPM|ECC)");
      } else {
        fail("Unsupported pubArea.type \"" + parsedPubArea.type.value_or("undefined") + "\"");
      }
      const Bytes& certInfoBytes = requireBytes(certInfo, "certInfo");
      const TpmCertInfo parsedCertInfo = parseCertInfo(certInfoBytes);
      if (parsedCertInfo.magic != 0xff544347) fail("Unexpected magic value \"" + rt::numberToString(parsedCertInfo.magic) + "\", expected \"0xff544347\" (TPM)");
      if (parsedCertInfo.type != "TPM_ST_ATTEST_CERTIFY")
        fail("Unexpected type \"" + parsedCertInfo.type.value_or("undefined") + "\", expected \"TPM_ST_ATTEST_CERTIFY\" (TPM)");
      // Hash pubArea to create pubAreaHash using the nameAlg in attested
      const Cbor nameAlg = numberCbor(attestedNameAlgToCOSEAlg(parsedCertInfo.nameAlg));
      const Bytes pubAreaHash = toHash(pubAreaBytes, &nameAlg);
      // Concatenate attested.nameAlg and pubAreaHash to create attestedName.
      const Bytes attestedName = concatBytes({parsedCertInfo.nameAlgBuffer, pubAreaHash});
      // Check that certInfo.attested.name is equals to attestedName.
      if (!bytesEqual(parsedCertInfo.name, attestedName)) fail("Attested name comparison failed (TPM)");
      // Concatenate authData with clientDataHash to create attToBeSigned
      const Bytes attToBeSigned = concatBytes({options.authData, options.clientDataHash});
      // Hash attToBeSigned using the algorithm specified in attStmt.alg to create attToBeSignedHash
      const Bytes attToBeSignedHash = toHash(attToBeSigned, alg);
      // Check that certInfo.extraData is equals to attToBeSignedHash.
      if (!bytesEqual(parsedCertInfo.extraData, attToBeSignedHash)) fail("CertInfo extra data did not equal hashed attestation (TPM)");
      /**
       * Verify signature
       */
      const std::optional<double> x5cLength = cborLength(*x5c);
      if (x5cLength && *x5cLength < 1) fail("No certificates present in x5c array (TPM)");
      // Pick a leaf AIK certificate of the x5c array and parse it.
      const CertificateInfo leafCertInfo = getCertificateInfo(firstCertificate(x5c));
      if (leafCertInfo.basicConstraintsCA) fail("Certificate basic constraints CA was not `false` (TPM)");
      // Check that certificate is of version 3 (value must be set to 2).
      if (leafCertInfo.version != 2) fail("Certificate version was not `3` (ASN.1 value of 2) (TPM)");
      // Check that Subject sequence is empty.
      if (rt::length(leafCertInfo.subject.combined) > 0) fail("Certificate subject was not empty (TPM)");
      // Check that certificate is currently valid
      if (leafCertInfo.notBefore > rt::dateNow()) fail("Certificate not good before \"" + rt::numberToString(leafCertInfo.notBefore) + "\" (TPM)");
      // Check that certificate has not expired
      if (leafCertInfo.notAfter < rt::dateNow()) fail("Certificate not good after \"" + rt::numberToString(leafCertInfo.notAfter) + "\" (TPM)");
      /**
       * Plumb the depths of the certificate's ASN.1-formatted data for some values we need to verify
       */
      const Certificate& parsedCert = leafCertInfo.parsedCertificate;
      if (!parsedCert.extensions) fail("Certificate was missing extensions (TPM)");
      std::optional<std::vector<asn1::Node>> subjectAltNamePresent;
      std::optional<std::vector<std::string>> extKeyUsage;
      for (const Extension& ext : *parsedCert.extensions) {
        if (ext.extnID == "2.5.29.17") {
          const asn1::Node names = asn1::parse(ext.extnValue);
          if (!asn1::isUniversal(names, asn1::Type::Sequence)) schemaMismatch("SubjectAlternativeName");
          subjectAltNamePresent = names.value;
        } else if (ext.extnID == "2.5.29.37") {
          const asn1::Node usages = asn1::parse(ext.extnValue);
          if (!asn1::isUniversal(usages, asn1::Type::Sequence)) schemaMismatch("ExtendedKeyUsage");
          std::vector<std::string> oids;
          for (const asn1::Node& usage : usages.value) {
            if (!asn1::isUniversal(usage, asn1::Type::ObjectIdentifier)) schemaMismatch("ExtendedKeyUsage");
            oids.push_back(asn1::oidString(usage));
          }
          extKeyUsage = std::move(oids);
        }
      }
      // Check that certificate contains subjectAltName (2.5.29.17) extension,
      if (!subjectAltNamePresent) fail("Certificate did not contain subjectAltName extension (TPM)");
      // TPM-specific values are buried within `directoryName`, so first make sure there are values
      // there.
      if (subjectAltNamePresent->empty()) throw rt::TypeError("undefined is not an object (evaluating 'subjectAltNamePresent[0].directoryName')");
      const asn1::Node& firstName = subjectAltNamePresent->front();
      std::vector<RelativeDistinguishedName> directoryName;
      bool hasDirectoryName = false;
      if (asn1::is(firstName, 3, 4, true)) {
        if (firstName.value.empty()) schemaMismatch("GeneralName");
        directoryName = parseName(firstName.value[0], "Name");
        hasDirectoryName = true;
      }
      if (hasDirectoryName && directoryName.empty()) throw rt::TypeError("undefined is not an object (evaluating 'subjectAltNamePresent[0].directoryName?.[0].length')");
      if (!hasDirectoryName || directoryName.front().empty()) fail("Certificate subjectAltName extension directoryName was empty (TPM)");
      const TcgAtTpmValues tpmValues = getTcgAtTpmValues(directoryName);
      if (!rt::truthy(tpmValues.tcgAtTpmManufacturer) || !rt::truthy(tpmValues.tcgAtTpmModel) || !rt::truthy(tpmValues.tcgAtTpmVersion))
        fail("Certificate contained incomplete subjectAltName data (TPM)");
      if (!extKeyUsage) fail("Certificate did not contain ExtendedKeyUsage extension (TPM)");
      // Check that tcpaTpmManufacturer (2.23.133.2.1) field is set to a valid manufacturer ID.
      if (!isTpmManufacturer(*tpmValues.tcgAtTpmManufacturer)) fail("Could not match TPM manufacturer \"" + *tpmValues.tcgAtTpmManufacturer + "\" (TPM)");
      // Check that certificate contains extKeyUsage (2.5.29.37) extension and it must contain
      // tcg-kp-AIKCertificate (2.23.133.8.3) OID.
      if (extKeyUsage->empty() || extKeyUsage->front() != "2.23.133.8.3")
        fail("Unexpected extKeyUsage \"" + (extKeyUsage->empty() ? std::string("undefined") : extKeyUsage->front()) + "\", expected \"2.23.133.8.3\" (TPM)");
      // Validate attestation statement AAGUID against leaf cert AAGUID
      try {
        validateExtFIDOGenCEAAGUID(parsedCert, options.aaguid);
      } catch (const rt::JsError& err) {
        fail(err.message + " (TPM)");
      }
      // MetadataService is DISABLED (never initialized): `getStatement` returns undefined.
      try {
        // Try validating the certificate path using the root certificates set via SettingsService
        std::vector<std::string> certs = mapCertsToPEM(x5c);
        rt::Task<bool> validating = validateCertificatePath(std::move(certs), options.rootCertificates);
        (void)co_await std::move(validating);
      } catch (const rt::JsError& err) {
        fail(err.message + " (TPM)");
      }
      // Verify signature over certInfo with the public key extracted from AIK certificate.
      // In the wise words of Yuriy Ackermann: "Get Martini friend, you are done!"
      VerifySignatureOptions verify;
      verify.signature = requireBytes(sig, "sig");
      verify.data = certInfoBytes;
      verify.x509Certificate = firstCertificate(x5c);
      verify.hashAlgorithm = optionalCbor(alg);
      co_return verifySignature(verify);
    }

    rt::Task<bool> verifyAttestationApple(AttestationFormatVerifierOpts options) {
      const Cbor* x5c = statementMember(options.attStmt, "x5c");
      if (!truthy(x5c)) fail("No attestation certificate provided in attestation statement (Apple)");
      /**
       * Verify certificate path
       */
      try {
        std::vector<std::string> certs = mapCertsToPEM(x5c);
        rt::Task<bool> validating = validateCertificatePath(std::move(certs), options.rootCertificates);
        (void)co_await std::move(validating);
      } catch (const rt::JsError& err) {
        fail(err.message + " (Apple)");
      }
      /**
       * Compare nonce in certificate extension to computed nonce
       */
      const Certificate parsedCredCert = parseCertificate(firstCertificate(x5c));
      if (!parsedCredCert.extensions) fail("credCert missing extensions (Apple)");
      const Extension* extCertNonce = findExtension(parsedCredCert, "1.2.840.113635.100.8.2");
      if (extCertNonce == nullptr) fail("credCert missing \"1.2.840.113635.100.8.2\" extension (Apple)");
      const Bytes nonceToHash = concatBytes({options.authData, options.clientDataHash});
      const Bytes nonce = toHash(nonceToHash);
      /**
       * Ignore the first six ASN.1 structure bytes that define the nonce as an OCTET STRING. Should
       * trim off <Buffer 30 24 a1 22 04 20>
       */
      const Bytes extNonce = sliceBytes(extCertNonce->extnValue, 6, extCertNonce->extnValue.size());
      if (!bytesEqual(nonce, extNonce)) fail("credCert nonce was not expected value (Apple)");
      /**
       * Verify credential public key matches the Subject Public Key of credCert
       */
      const Bytes credPubKeyPKCS = convertCOSEtoPKCS(options.credentialPublicKey);
      if (!bytesEqual(credPubKeyPKCS, parsedCredCert.subjectPublicKey)) fail("Credential public key does not equal credCert public key (Apple)");
      co_return true;
    }

    // ---- the library's entry points ---------------------------------------------------

    // `supportedCOSEAlgorithmIdentifiers` (generateRegistrationOptions.ts)
    constexpr std::array<double, 10> supportedCOSEAlgorithmIdentifiers = {-8, -7, -36, -37, -38, -39, -257, -258, -259, -65535};

    struct RegistrationInfo {
      std::string credentialId;  // credential.id (base64url)
      Bytes publicKey;           // credential.publicKey
      double counter = 0;        // credential.counter
      // credential.transports: `response.response.transports` (std::nullopt is undefined).
      std::optional<json::value> transports;
    };

    struct VerifiedRegistrationResponse {
      bool verified = false;
      std::optional<RegistrationInfo> registrationInfo;
    };

    /**
     * Verify that the user has legitimately completed the registration process
     */
    rt::Task<VerifiedRegistrationResponse> verifyRegistrationResponse(json::value response, std::string expectedChallenge, std::string expectedOrigin,
                                                                      std::string expectedRPID, bool requireUserVerification) {
      const bool requireUserPresence = true;
      const json::value* id = rt::get(response, "id");
      const json::value* rawId = rt::get(response, "rawId");
      const json::value* credentialType = rt::get(response, "type");
      const json::value* attestationResponse = rt::get(response, "response");
      // Ensure credential specified an ID
      if (!rt::truthy(id)) fail("Missing credential ID");
      // Ensure ID is base64url-encoded
      if (rawId == nullptr || !rt::strictEquals(*id, *rawId)) fail("Credential ID was not base64url-encoded");
      // Make sure credential type is public-key
      if (!jsonIs(credentialType, "public-key")) fail("Unexpected credential type " + jsonString(credentialType) + ", expected \"public-key\"");
      const json::value* clientDataJSONText = rt::get(attestationResponse, "clientDataJSON");
      if (clientDataJSONText == nullptr || !clientDataJSONText->is_string()) throw rt::TypeError("clientDataJSON is not a string");
      const ClientData clientDataJSON = destructureClientData(decodeClientDataJSON(clientDataJSONText->as_string()));
      const json::value* type = clientDataJSON.member("type");
      const json::value* origin = clientDataJSON.member("origin");
      const json::value* challenge = clientDataJSON.member("challenge");
      const json::value* tokenBinding = clientDataJSON.member("tokenBinding");
      // Make sure we're handling an registration
      if (!jsonIs(type, "webauthn.create")) fail("Unexpected registration response type: " + jsonString(type));
      // Ensure the device provided the challenge we gave it
      if (!jsonIs(challenge, expectedChallenge))
        fail("Unexpected registration response challenge \"" + jsonString(challenge) + "\", expected \"" + expectedChallenge + "\"");
      // Check that the origin is our site
      if (!jsonIs(origin, expectedOrigin)) fail("Unexpected registration response origin \"" + jsonString(origin) + "\", expected \"" + expectedOrigin + "\"");
      if (rt::truthy(tokenBinding)) {
        if (!(tokenBinding->is_object() || tokenBinding->is_array())) fail("Unexpected value for TokenBinding \"" + jsonString(tokenBinding) + "\"");
        if (!statusIn(tokenBinding, {"present", "supported", "not-supported"}))
          fail("Unexpected tokenBinding.status value of \"" + jsonString(rt::get(tokenBinding, "status")) + "\"");
      }
      const json::value* attestationObjectText = rt::get(attestationResponse, "attestationObject");
      if (attestationObjectText == nullptr || !attestationObjectText->is_string()) throw rt::TypeError("attestationObject is not a string");
      const Bytes attestationObject = isoBase64URL::toBuffer(attestationObjectText->as_string());
      const Cbor decodedAttestationObject = isoCBOR::decodeFirst(attestationObject);
      const Cbor& attestationMap = requireMap(decodedAttestationObject, "decodedAttestationObject", "\"fmt\"");
      const Cbor* fmt = attestationMap.get("fmt");
      const Cbor* authDataValue = attestationMap.get("authData");
      const Cbor attStmt = attestationMap.get("attStmt") != nullptr ? *attestationMap.get("attStmt") : Cbor();
      const Bytes& authData = requireBytes(authDataValue, "authData");
      const ParsedAuthenticatorData parsedAuthData = parseAuthenticatorData(authData);
      // Make sure the response's RP ID is ours
      if (!expectedRPID.empty()) (void)matchExpectedRPID(parsedAuthData.rpIdHash, {expectedRPID});
      // Make sure someone was physically present
      if (requireUserPresence && !parsedAuthData.flags.up) fail("User presence was required, but user was not present");
      // Enforce user verification if specified
      if (requireUserVerification && !parsedAuthData.flags.uv) fail("User verification was required, but user could not be verified");
      if (!parsedAuthData.credentialID) fail("No credential ID was provided by authenticator");
      if (!parsedAuthData.credentialPublicKey) fail("No public key was provided by authenticator");
      if (!parsedAuthData.aaguid) fail("No AAGUID was present during registration");
      const Cbor decodedPublicKey = isoCBOR::decodeFirst(*parsedAuthData.credentialPublicKey);
      const Cbor* alg = requireMap(decodedPublicKey, "decodedPublicKey", "COSEKEYS.alg").get(COSEKEYS::alg);
      if (alg == nullptr || alg->kind() != Cbor::Kind::number) fail("Credential public key was missing numeric alg");
      // Make sure the key algorithm is one we specified within the registration options
      if (std::find(supportedCOSEAlgorithmIdentifiers.begin(), supportedCOSEAlgorithmIdentifiers.end(), alg->number()) == supportedCOSEAlgorithmIdentifiers.end())
        fail("Unexpected public key alg \"" + rt::numberToString(alg->number()) + "\", expected one of \"-8, -7, -36, -37, -38, -39, -257, -258, -259, -65535\"");
      const Bytes clientDataHash = toHash(isoBase64URL::toBuffer(clientDataJSONText->as_string()));
      const std::string fmtText = fmt != nullptr && fmt->kind() == Cbor::Kind::string ? fmt->text() : std::string();
      // Prepare arguments to pass to the relevant verification method
      AttestationFormatVerifierOpts verifierOpts{
        .aaguid = *parsedAuthData.aaguid,
        .attStmt = attStmt,
        .authData = authData,
        .clientDataHash = clientDataHash,
        .credentialID = *parsedAuthData.credentialID,
        .credentialPublicKey = *parsedAuthData.credentialPublicKey,
        .rootCertificates = getRootCertificates(fmtText),
        .rpIdHash = parsedAuthData.rpIdHash,
        .attestationSafetyNetEnforceCTSCheck = true,
      };
      /**
       * Verification can only be performed when attestation = 'direct'
       */
      bool verified = false;
      const bool knownFormat = fmt != nullptr && fmt->kind() == Cbor::Kind::string;
      if (knownFormat && fmtText == "fido-u2f") {
        rt::Task<bool> verifying = verifyAttestationFIDOU2F(std::move(verifierOpts));
        verified = co_await std::move(verifying);
      } else if (knownFormat && fmtText == "packed") {
        rt::Task<bool> verifying = verifyAttestationPacked(std::move(verifierOpts));
        verified = co_await std::move(verifying);
      } else if (knownFormat && fmtText == "android-safetynet") {
        rt::Task<bool> verifying = verifyAttestationAndroidSafetyNet(std::move(verifierOpts));
        verified = co_await std::move(verifying);
      } else if (knownFormat && fmtText == "android-key") {
        rt::Task<bool> verifying = verifyAttestationAndroidKey(std::move(verifierOpts));
        verified = co_await std::move(verifying);
      } else if (knownFormat && fmtText == "tpm") {
        rt::Task<bool> verifying = verifyAttestationTPM(std::move(verifierOpts));
        verified = co_await std::move(verifying);
      } else if (knownFormat && fmtText == "apple") {
        rt::Task<bool> verifying = verifyAttestationApple(std::move(verifierOpts));
        verified = co_await std::move(verifying);
      } else if (knownFormat && fmtText == "none") {
        // `attStmt.size > 0`: only a non-empty Map has a size above zero; null and undefined throw.
        if (attStmt.kind() == Cbor::Kind::null_ || attStmt.kind() == Cbor::Kind::undefined_)
          throw rt::TypeError(std::string(attStmt.kind() == Cbor::Kind::null_ ? "null" : "undefined") + " is not an object (evaluating 'attStmt.size')");
        if (attStmt.kind() == Cbor::Kind::map && !attStmt.entries().empty()) fail("None attestation had unexpected attestation statement");
        // This is the weaker of the attestations, so there's nothing else to really check
        verified = true;
      } else {
        fail("Unsupported Attestation Format: " + jsString(fmt));
      }
      if (!verified) co_return VerifiedRegistrationResponse{};
      parseBackupFlags(parsedAuthData.flags);
      const json::value* transports = rt::get(attestationResponse, "transports");
      RegistrationInfo info;
      info.credentialId = isoBase64URL::fromBuffer(*parsedAuthData.credentialID);
      info.publicKey = *parsedAuthData.credentialPublicKey;
      info.counter = parsedAuthData.counter;
      if (transports != nullptr) info.transports = *transports;
      VerifiedRegistrationResponse result;
      result.verified = true;
      result.registrationInfo = std::move(info);
      co_return result;
    }

    struct VerifiedAuthenticationResponse {
      bool verified = false;
      double newCounter = 0;
    };

    /**
     * Verify that the user has legitimately completed the authentication process
     */
    VerifiedAuthenticationResponse verifyAuthenticationResponse(const json::value& response, std::string_view expectedChallenge, std::string_view expectedOrigin,
                                                                const std::string& expectedRPID, bool requireUserVerification, const Bytes& credentialPublicKey,
                                                                double credentialCounter) {
      const json::value* id = rt::get(response, "id");
      const json::value* rawId = rt::get(response, "rawId");
      const json::value* credentialType = rt::get(response, "type");
      const json::value* assertionResponse = rt::get(response, "response");
      // Ensure credential specified an ID
      if (!rt::truthy(id)) fail("Missing credential ID");
      // Ensure ID is base64url-encoded
      if (rawId == nullptr || !rt::strictEquals(*id, *rawId)) fail("Credential ID was not base64url-encoded");
      // Make sure credential type is public-key
      if (!jsonIs(credentialType, "public-key")) fail("Unexpected credential type " + jsonString(credentialType) + ", expected \"public-key\"");
      const json::value* clientDataJSONText = rt::get(assertionResponse, "clientDataJSON");
      if (clientDataJSONText == nullptr || !clientDataJSONText->is_string()) fail("Credential response clientDataJSON was not a string");
      const ClientData clientDataJSON = destructureClientData(decodeClientDataJSON(clientDataJSONText->as_string()));
      const json::value* type = clientDataJSON.member("type");
      const json::value* origin = clientDataJSON.member("origin");
      const json::value* challenge = clientDataJSON.member("challenge");
      const json::value* tokenBinding = clientDataJSON.member("tokenBinding");
      // Make sure we're handling an authentication
      if (!jsonIs(type, "webauthn.get")) fail("Unexpected authentication response type: " + jsonString(type));
      // Ensure the device provided the challenge we gave it
      if (!jsonIs(challenge, expectedChallenge))
        fail("Unexpected authentication response challenge \"" + jsonString(challenge) + "\", expected \"" + std::string(expectedChallenge) + "\"");
      // Check that the origin is our site
      if (!jsonIs(origin, expectedOrigin))
        fail("Unexpected authentication response origin \"" + jsonString(origin) + "\", expected \"" + std::string(expectedOrigin) + "\"");
      const json::value* authenticatorData = rt::get(assertionResponse, "authenticatorData");
      if (!isBase64URLValue(authenticatorData)) fail("Credential response authenticatorData was not a base64url string");
      const json::value* signatureText = rt::get(assertionResponse, "signature");
      if (!isBase64URLValue(signatureText)) fail("Credential response signature was not a base64url string");
      const json::value* userHandle = rt::get(assertionResponse, "userHandle");
      if (rt::truthy(userHandle) && !userHandle->is_string()) fail("Credential response userHandle was not a string");
      if (rt::truthy(tokenBinding)) {
        if (!(tokenBinding->is_object() || tokenBinding->is_array())) fail("ClientDataJSON tokenBinding was not an object");
        if (!statusIn(tokenBinding, {"present", "supported", "notSupported"}))
          fail("Unexpected tokenBinding status " + jsonString(rt::get(tokenBinding, "status")));
      }
      const Bytes authDataBuffer = isoBase64URL::toBuffer(authenticatorData->as_string());
      const ParsedAuthenticatorData parsedAuthData = parseAuthenticatorData(authDataBuffer);
      // Make sure the response's RP ID is ours
      (void)matchExpectedRPID(parsedAuthData.rpIdHash, {expectedRPID});
      /**
       * Use WebAuthn spec-defined rules for verifying UP and UV flags
       */
      // WebAuthn only requires the user presence flag be true
      if (!parsedAuthData.flags.up) fail("User not present during authentication");
      // Enforce user verification if required
      if (requireUserVerification && !parsedAuthData.flags.uv) fail("User verification required, but user could not be verified");
      const Bytes clientDataHash = toHash(isoBase64URL::toBuffer(clientDataJSONText->as_string()));
      const Bytes signatureBase = concatBytes({authDataBuffer, clientDataHash});
      const Bytes signature = isoBase64URL::toBuffer(signatureText->as_string());
      const double counter = parsedAuthData.counter;
      if ((counter > 0 || credentialCounter > 0) && counter <= credentialCounter) {
        // Error out when the counter in the DB is greater than or equal to the counter in the
        // dataStruct. It's related to how the authenticator maintains the number of times its been
        // used for this client. If this happens, then someone's somehow increased the counter
        // on the device without going through this site
        fail("Response counter value " + rt::numberToString(counter) + " was lower than expected " + rt::numberToString(credentialCounter));
      }
      parseBackupFlags(parsedAuthData.flags);
      VerifySignatureOptions verify;
      verify.signature = signature;
      verify.data = signatureBase;
      verify.credentialPublicKey = credentialPublicKey;
      return VerifiedAuthenticationResponse{.verified = verifySignature(verify), .newCounter = counter};
    }

    // `{ id, type: "public-key" }` entries of allowCredentials.
    struct AllowCredential {
      std::string id;
    };

    /**
     * Prepare a value to pass into navigator.credentials.get(...) for authenticator authentication
     */
    json::object generateAuthenticationOptions(const std::string& rpID, double timeout, const std::optional<std::vector<AllowCredential>>& allowCredentials,
                                               const std::string& userVerification) {
      /**
       * WebAuthn spec says that 16 bytes is a good minimum:
       *
       * "In order to prevent replay attacks, the challenges MUST contain enough entropy to make
       * guessing them infeasible. Challenges SHOULD therefore be at least 16 bytes long."
       *
       * Just in case, let's double it
       */
      Bytes challenge(32);
      rt::crypto::getRandomValues(std::span<std::uint8_t>(challenge));
      json::object options;
      options["rpId"] = rpID;
      options["challenge"] = isoBase64URL::fromBuffer(challenge);
      if (allowCredentials) {
        json::array mapped;
        for (const AllowCredential& cred : *allowCredentials) {
          if (!isoBase64URL::isBase64URL(cred.id)) fail("allowCredential id \"" + cred.id + "\" is not a valid base64url string");
          json::object entry;
          entry["id"] = isoBase64URL::trimPadding(cred.id);
          entry["type"] = "public-key";
          mapped.push_back(std::move(entry));
        }
        options["allowCredentials"] = std::move(mapped);
      }
      rt::objectSet(options, "timeout", rt::makeNumber(timeout));
      options["userVerification"] = userVerification;
      return options;
    }
  }

  // =====================================================================================
  // src/auth/passkeys.ts
  // =====================================================================================

  namespace {
    struct PasskeyChallengeRecord {
      std::string challenge;
      std::string type;  // "registration" | "authentication"
      std::string origin;
      std::string rp_id;
      std::optional<std::string> audience_origin{};
      std::optional<std::string> user_id{};
      std::optional<std::string> handle{};
      std::optional<bool> is_admin{};
      double created_at_ms;
      double expires_at_ms;
    };

    void tag_invoke(const json::value_from_tag&, json::value& out, const PasskeyChallengeRecord& value) {
      out = Writer()
              .required("challenge", value.challenge)
              .required("type", value.type)
              .required("origin", value.origin)
              .required("rp_id", value.rp_id)
              .orUndefined("audience_origin", value.audience_origin)
              .orUndefined("user_id", value.user_id)
              .orUndefined("handle", value.handle)
              .orUndefined("is_admin", value.is_admin)
              .required("created_at_ms", value.created_at_ms)
              .required("expires_at_ms", value.expires_at_ms)
              .done();
    }

    PasskeyChallengeRecord tag_invoke(const json::value_to_tag<PasskeyChallengeRecord>&, const json::value& value) {
      const Reader in(value, "PasskeyChallengeRecord");
      return {
        .challenge = in.required<std::string>("challenge"),
        .type = in.required<std::string>("type"),
        .origin = in.required<std::string>("origin"),
        .rp_id = in.required<std::string>("rp_id"),
        .audience_origin = in.orUndefined<std::string>("audience_origin"),
        .user_id = in.orUndefined<std::string>("user_id"),
        .handle = in.orUndefined<std::string>("handle"),
        .is_admin = in.orUndefined<bool>("is_admin"),
        .created_at_ms = in.required<double>("created_at_ms"),
        .expires_at_ms = in.required<double>("expires_at_ms"),
      };
    }

    struct PasskeyLoginStartThrottleRecord {
      double window_start_ms;
      double count;
    };

    void tag_invoke(const json::value_from_tag&, json::value& out, const PasskeyLoginStartThrottleRecord& value) {
      out = Writer().required("window_start_ms", value.window_start_ms).required("count", value.count).done();
    }

    PasskeyLoginStartThrottleRecord tag_invoke(const json::value_to_tag<PasskeyLoginStartThrottleRecord>&, const json::value& value) {
      const Reader in(value, "PasskeyLoginStartThrottleRecord");
      return {.window_start_ms = in.required<double>("window_start_ms"), .count = in.required<double>("count")};
    }

    // A typed record read back from storage (the strict codec; see the header).
    template <class T> T storedRecord(const rt::KvValue& value) { return json::value_to<T>(value.toJson()); }

    template <class T> rt::KvValue toStored(const T& record) { return rt::KvValue(json::value_from(record)); }

    rt::KvKey AUTH_PREFIX() { return rt::KvKey{"uos_ai", "auth"}; }
    constexpr std::string_view PASSKEY_CANONICAL_ORIGIN = "https://ai.ubq.fi";
    constexpr std::string_view RP_NAME = "UbiquityOS AI Gateway";

    // `String(part)` of a stored value used as a key part, for the SDK's key encoder error.
    std::string storedValueText(const rt::KvValue& value) {
      if (value.isArray()) {
        std::vector<std::string> parts;
        for (const rt::KvValue& item : value.asArray()) parts.push_back(item.isNullish() ? std::string() : storedValueText(item));
        return rt::join(parts, ",");
      }
      if (value.isBytes()) {
        std::vector<std::string> parts;
        for (std::uint8_t byte : value.asBytes()) parts.push_back(std::to_string(byte));
        return rt::join(parts, ",");
      }
      try {
        return rt::toString(value.toJson());
      } catch (const rt::JsError&) {
        return "[object Object]";
      }
    }

    // `[...AUTH_PREFIX, name, part]` where `part` is an unchecked value: the SDK packs strings,
    // numbers, bigints, booleans and Uint8Arrays and throws for anything else.
    rt::KvKey authKey(std::string_view name, const rt::KvValue& part) {
      if (part.isString()) return rt::KvKey{AUTH_PREFIX(), std::string(name), part.asString()};
      if (part.isNumber()) return rt::KvKey{AUTH_PREFIX(), std::string(name), part.asNumber()};
      if (part.isBool()) return rt::KvKey{AUTH_PREFIX(), std::string(name), part.asBool()};
      if (part.isBigInt()) return rt::KvKey{AUTH_PREFIX(), std::string(name), part.asBigInt()};
      if (part.isBytes()) return rt::KvKey{AUTH_PREFIX(), std::string(name), part.asBytes()};
      throw rt::JsError("Unsupported keyPart: " + std::string(part.isUndefined() ? "undefined" : "object") + " " + storedValueText(part));
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PasskeyUserRecord& value) {
    out = Writer()
            .required("id", value.id)
            .required("handle", value.handle)
            .required("is_admin", value.is_admin)
            .required("credential_ids", value.credential_ids)
            .required("created_at_ms", value.created_at_ms)
            .required("updated_at_ms", value.updated_at_ms)
            .done();
  }

  PasskeyUserRecord tag_invoke(const json::value_to_tag<PasskeyUserRecord>&, const json::value& value) {
    const Reader in(value, "PasskeyUserRecord");
    return {
      .id = in.required<std::string>("id"),
      .handle = in.required<std::string>("handle"),
      .is_admin = in.required<bool>("is_admin"),
      .credential_ids = in.required<std::vector<std::string>>("credential_ids"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PasskeyCredentialRecord& value) {
    out = Writer()
            .required("credential_id", value.credential_id)
            .required("user_id", value.user_id)
            .required("public_key", value.public_key)
            .required("sign_count", value.sign_count)
            .required("transports", value.transports)
            .required("created_at_ms", value.created_at_ms)
            .done();
  }

  PasskeyCredentialRecord tag_invoke(const json::value_to_tag<PasskeyCredentialRecord>&, const json::value& value) {
    const Reader in(value, "PasskeyCredentialRecord");
    return {
      .credential_id = in.required<std::string>("credential_id"),
      .user_id = in.required<std::string>("user_id"),
      .public_key = in.required<std::string>("public_key"),
      .sign_count = in.required<double>("sign_count"),
      .transports = in.required<json::value>("transports"),
      .created_at_ms = in.required<double>("created_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PasskeySessionRecord& value) {
    out = Writer()
            .required("token", value.token)
            .required("user_id", value.user_id)
            .required("created_at_ms", value.created_at_ms)
            .required("expires_at_ms", value.expires_at_ms)
            .orUndefined("audience_origin", value.audience_origin)
            .done();
  }

  PasskeySessionRecord tag_invoke(const json::value_to_tag<PasskeySessionRecord>&, const json::value& value) {
    const Reader in(value, "PasskeySessionRecord");
    return {
      .token = in.required<std::string>("token"),
      .user_id = in.required<std::string>("user_id"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
      .audience_origin = in.orUndefined<std::string>("audience_origin"),
    };
  }

  rt::KvKey passkeyUserKey(const std::string& userId) { return rt::KvKey{AUTH_PREFIX(), "users", userId}; }
  rt::KvKey passkeyHandleKey(const std::string& handle) { return rt::KvKey{AUTH_PREFIX(), "handles", handle}; }
  rt::KvKey passkeyCredentialKey(const std::string& credentialId) { return rt::KvKey{AUTH_PREFIX(), "credentials", credentialId}; }
  rt::KvKey passkeyChallengeKey(const std::string& challenge) { return rt::KvKey{AUTH_PREFIX(), "challenges", challenge}; }
  rt::KvKey passkeySessionKey(const std::string& token) { return rt::KvKey{AUTH_PREFIX(), "sessions", token}; }
  rt::KvKey passkeyLoginStartThrottleKey(double windowStartMs) { return rt::KvKey{AUTH_PREFIX(), "login-start-throttle", windowStartMs}; }

  namespace {
    double nowMs() { return rt::dateNow(); }

    // Strip leading and trailing separators without a backtracking-prone regex.
    // Equivalent to `.replace(/^-+|-+$/g, "")` for every input.
    std::string trimPasskeyHandleSeparators(const std::string& value) {
      double start = 0;
      double end = static_cast<double>(rt::length(value));
      while (start < end && rt::charAt(value, start) == "-") start += 1;
      while (end > start && rt::charAt(value, end - 1) == "-") end -= 1;
      return rt::slice(value, start, end);
    }
  }

  std::string normalizePasskeyHandle(const json::value* value) {
    static const rt::Regex INVALID_RUN("[^a-z0-9._-]+", "g");
    static const rt::Regex REPEATED_SEPARATOR("-{2,}", "g");
    const std::optional<std::string> raw = utils::getString(value);
    const std::string text = raw ? rt::toLowerCase(rt::trim(*raw)) : std::string();
    return rt::slice(rt::replace(trimPasskeyHandleSeparators(rt::replace(text, INVALID_RUN, "-")), REPEATED_SEPARATOR, "-"), 0, 96);
  }

  std::string normalizePasskeyHandle(const json::value& value) { return normalizePasskeyHandle(&value); }

  rt::Task<std::string> buildPasskeyHandle(std::string seed) {
    const std::string trimmed = rt::trim(seed);
    if (trimmed.empty()) co_return std::string();
    rt::Task<std::string> hashing = utils::sha256Hex(trimmed);
    const std::string digest = co_await std::move(hashing);
    const std::string fingerprint = rt::slice(digest, 0, 16);
    co_return "uos-passkey-" + fingerprint;
  }

  bool isPasskeyUserAdmin(const PasskeyUserRecord* user) { return user != nullptr && user->is_admin; }
  bool isPasskeyUserAdmin(const PasskeyUserRecord& user) { return isPasskeyUserAdmin(&user); }

  namespace {
    json::value number(double value) { return rt::makeNumber(value); }

    json::object serializePasskeyUser(const PasskeyUserRecord& user) {
      json::object out;
      out["id"] = user.id;
      out["handle"] = user.handle;
      out["is_admin"] = isPasskeyUserAdmin(user);
      out["credential_count"] = number(static_cast<double>(user.credential_ids.size()));
      out["created_at_ms"] = number(user.created_at_ms);
      out["updated_at_ms"] = number(user.updated_at_ms);
      return out;
    }

    std::string parseClientChallenge(const std::string& encoded) {
      const std::vector<std::uint8_t> decoded = utils::base64UrlDecode(encoded);
      const std::string jsonText = rt::decodeUtf8(BytesView(decoded));
      const json::value parsed = rt::jsonParse(jsonText);
      if (parsed.is_null()) throw rt::TypeError("null is not an object (evaluating 'parsed.challenge')");
      const std::optional<std::string> challenge = utils::getString(rt::get(parsed, "challenge"));
      return challenge ? rt::trim(*challenge) : std::string();
    }

    // `null` distinguishes a malformed clientDataJSON payload from a well-formed one
    // that carries no usable challenge, which callers report separately.
    // (`encoded` is unchecked request JSON: base64UrlDecode throws for anything but a string.)
    std::optional<std::string> parseClientChallengeOrNull(const json::value& encoded) {
      if (!encoded.is_string()) return std::nullopt;
      try {
        return parseClientChallenge(std::string(encoded.as_string()));
      } catch (const rt::JsError&) {
        return std::nullopt;
      }
    }

    // The request body is untrusted JSON: only a non-null object can carry the
    // WebAuthn response shape that the server verifier validates field by field.
    bool isWebauthnResponseObject(const json::value* value) { return value != nullptr && (value->is_object() || value->is_array()); }

    // Deno types a listed value as present, but a stored record can still be absent.
    // Widening the read keeps the storage-boundary guard meaningful, so a missing
    // record is dropped instead of serialized.
    std::optional<PasskeyUserRecord> readStoredPasskeyUser(const rt::KvEntry<>& entry) {
      if (!entry.value.truthy()) return std::nullopt;
      return storedRecord<PasskeyUserRecord>(entry.value);
    }

    std::optional<std::string> firstHeaderValue(const std::optional<std::string>& value) {
      std::string first;
      if (value) first = rt::trim(rt::split(*value, ",").front());
      if (first.empty()) return std::nullopt;
      return first;
    }

    std::optional<std::string> parseOrigin(const std::optional<std::string>& value) {
      const std::string trimmed = value ? rt::trim(*value) : std::string();
      if (trimmed.empty() || trimmed == "null") return std::nullopt;
      try {
        const std::string origin = rt::URL(trimmed).origin();
        if (origin == "null") return std::nullopt;
        return origin;
      } catch (const rt::JsError&) {
        return std::nullopt;
      }
    }

    std::optional<std::string> parseOriginFromHost(const std::optional<std::string>& host, const std::string& protocol) {
      if (!rt::truthy(host)) return std::nullopt;
      try {
        return rt::URL(protocol + "://" + *host).origin();
      } catch (const rt::JsError&) {
        return std::nullopt;
      }
    }

    bool isLoopbackHost(const std::string& hostname) { return hostname == "localhost" || hostname == "127.0.0.1" || hostname == "[::1]" || hostname == "::1"; }

    bool isTrustedPasskeyOrigin(const std::string& origin) {
      try {
        const rt::URL url(origin);
        if (url.origin() == PASSKEY_CANONICAL_ORIGIN) return true;
        return (url.protocol() == "http:" || url.protocol() == "https:") && isLoopbackHost(url.hostname());
      } catch (const rt::JsError&) {
        return false;
      }
    }

    std::string firstTrustedPasskeyOrigin(const std::vector<std::optional<std::string>>& origins) {
      for (const std::optional<std::string>& origin : origins) {
        if (rt::truthy(origin) && isTrustedPasskeyOrigin(*origin)) return *origin;
      }
      return std::string(PASSKEY_CANONICAL_ORIGIN);
    }
  }

  PasskeyRequestMeta getPasskeyRequestMeta(const rt::Request& req) { return getPasskeyRequestMeta(req, nullptr); }

  PasskeyRequestMeta getPasskeyRequestMeta(const rt::Request& req, const json::value* clientOrigin) {
    static const rt::Regex TRAILING_COLON(":$", "");
    const rt::URL url(req.url());
    const std::optional<std::string> forwardedProtocol = firstHeaderValue(req.headers().get("x-forwarded-proto"));
    const std::string protocol = forwardedProtocol ? *forwardedProtocol : rt::replace(url.protocol(), TRAILING_COLON, "");
    const std::optional<std::string> forwardedHost = firstHeaderValue(req.headers().get("x-forwarded-host"));
    const std::optional<std::string> host = firstHeaderValue(req.headers().get("host"));
    const std::string origin = firstTrustedPasskeyOrigin({
      parseOrigin(utils::getString(clientOrigin)),
      parseOrigin(req.headers().get("origin")),
      parseOrigin(req.headers().get("referer")),
      parseOriginFromHost(forwardedHost, protocol),
      parseOriginFromHost(host, protocol),
      url.origin(),
    });
    const rt::URL originUrl(origin);
    return {.origin = originUrl.origin(), .rpId = originUrl.hostname()};
  }

  namespace {
    // `getKvOrError()`: the store, or the 503 response when none is available.
    struct KvOrError {
      std::shared_ptr<rt::Kv> kv;
      std::optional<rt::Response> response;
    };

    rt::Task<KvOrError> getKvOrError() {
      auto opening = ::uos::kv::getKv();
      std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) co_return KvOrError{.kv = nullptr, .response = http::openaiError(503, "Passkey auth requires Deno KV", std::string("server_error"))};
      co_return KvOrError{.kv = std::move(kv), .response = std::nullopt};
    }

    // `Record<string, unknown> | Response`: the parsed body (an object, or an array, which
    // utils.isRecord accepts), or the error response.
    struct PasskeyJson {
      json::value raw;
      std::optional<rt::Response> response;
    };

    rt::Task<PasskeyJson> readPasskeyJson(rt::Request req, bool allowEmpty) {
      rt::Task<request::JsonBodyReadResult> reading = request::readJsonBodyWithLimit(req, PASSKEY_MAX_REQUEST_BODY_BYTES);
      request::JsonBodyReadResult result = co_await std::move(reading);
      if (const auto* failed = std::get_if<request::JsonBodyReadResultNotOk>(&result)) {
        if (allowEmpty && failed->kind == "empty") co_return PasskeyJson{.raw = json::object(), .response = std::nullopt};
        co_return PasskeyJson{.raw = nullptr,
                              .response = failed->kind == "too_large" ? http::openaiError(413, "Passkey request body is too large", std::string("invalid_request_error"))
                                                                      : http::openaiError(400, "Invalid JSON body", std::string("invalid_request_error"))};
      }
      json::value value = std::get<request::JsonBodyReadResultOk>(result).value;
      if (!utils::isRecord(value)) co_return PasskeyJson{.raw = nullptr, .response = http::openaiError(400, "Invalid JSON body", std::string("invalid_request_error"))};
      co_return PasskeyJson{.raw = std::move(value), .response = std::nullopt};
    }

    rt::Task<std::optional<PasskeyUserRecord>> getUserByHandle(std::shared_ptr<rt::Kv> kv, std::string handle) {
      const rt::KvEntryMaybe<> userIdEntry = co_await kv->get(passkeyHandleKey(handle));
      if (!userIdEntry.value.truthy()) co_return std::nullopt;
      const rt::KvKey userKey = authKey("users", userIdEntry.value);
      const rt::KvEntryMaybe<> userEntry = co_await kv->get(userKey);
      if (userEntry.value.isNullish()) co_return std::nullopt;
      co_return storedRecord<PasskeyUserRecord>(userEntry.value);
    }
  }

  rt::Task<bool> hasPasskeyUsers() {
    auto opening = ::uos::kv::getKv();
    const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
    if (!kv) co_return false;
    // One entry is enough to answer the question, and `limit: 1` keeps the scan
    // bounded; materializing it avoids an unused loop binding.
    rt::KvListSelector selector;
    selector.prefix = rt::KvKey{AUTH_PREFIX(), "users"};
    rt::KvListOptions options;
    options.limit = 1;
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector, options);
    const std::vector<rt::KvEntry<>> users = co_await rt::kvListToArray(*iterator);
    co_return !users.empty();
  }

  namespace {
    // The atomic write of updatePasskeyCredentialSignCount on the stored record object, so
    // members the record type does not declare survive (`{ ...entry.value, sign_count }`).
    rt::Task<bool> commitSignCount(std::shared_ptr<rt::Kv> kv, rt::KvKey key, rt::KvValue stored, std::string versionstamp, double signCount) {
      rt::KvObject next = stored.isObject() ? stored.asObject() : rt::KvObject();
      next.set("sign_count", rt::KvValue(signCount));
      const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(rt::AtomicCheck(key, versionstamp)).set(key, rt::KvValue(std::move(next)));
      const rt::KvCommitResult commit = co_await operation->commit();
      co_return commit.ok;
    }
  }

  rt::Task<bool> updatePasskeyCredentialSignCount(std::shared_ptr<rt::Kv> kv, PasskeyCredentialEntry entry, double signCount) {
    rt::Task<bool> committing = commitSignCount(std::move(kv), entry.key, toStored(entry.value), entry.versionstamp, signCount);
    const bool ok = co_await std::move(committing);
    co_return ok;
  }

  namespace {
    rt::Task<PasskeyChallengeRecord> saveChallenge(std::shared_ptr<rt::Kv> kv, PasskeyChallengeRecord input) {
      const double createdAtMs = nowMs();
      PasskeyChallengeRecord record = std::move(input);
      record.created_at_ms = createdAtMs;
      record.expires_at_ms = createdAtMs + PASSKEY_CHALLENGE_TTL_MS;
      const rt::KvKey key = passkeyChallengeKey(record.challenge);
      const rt::KvValue stored = toStored(record);
      rt::KvSetOptions options;
      options.expireIn = PASSKEY_CHALLENGE_TTL_MS;
      (void)co_await kv->set(key, stored, options);
      co_return record;
    }

    rt::Task<std::optional<PasskeyChallengeRecord>> consumeChallenge(std::shared_ptr<rt::Kv> kv, std::string challenge) {
      const rt::KvKey key = passkeyChallengeKey(challenge);
      const rt::KvEntryMaybe<> entry = co_await kv->get(key);
      if (!entry.value.truthy()) co_return std::nullopt;
      const PasskeyChallengeRecord record = storedRecord<PasskeyChallengeRecord>(entry.value);
      if (record.expires_at_ms <= nowMs()) {
        const std::unique_ptr<rt::AtomicOperation> expired = kv->atomic();
        expired->check(entry).delete_(key);
        (void)co_await expired->commit();
        co_return std::nullopt;
      }
      const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(entry).delete_(key);
      const rt::KvCommitResult commit = co_await operation->commit();
      if (!commit.ok) co_return std::nullopt;
      co_return record;
    }

    rt::Task<PasskeySessionRecord> createSession(std::shared_ptr<rt::Kv> kv, std::string userId, std::optional<std::string> audienceOrigin) {
      const double createdAtMs = nowMs();
      const std::string token = "uos_ai_session_" + rt::crypto::randomUUID();
      PasskeySessionRecord record{
        .token = token,
        .user_id = userId,
        .created_at_ms = createdAtMs,
        .expires_at_ms = createdAtMs + PASSKEY_SESSION_TTL_MS,
        .audience_origin = rt::truthy(audienceOrigin) ? audienceOrigin : std::nullopt,
      };
      const rt::KvKey key = passkeySessionKey(token);
      const rt::KvValue stored = toStored(record);
      rt::KvSetOptions options;
      options.expireIn = PASSKEY_SESSION_TTL_MS;
      (void)co_await kv->set(key, stored, options);
      co_return record;
    }

    rt::Response conflict(std::string message) { return http::openaiError(409, std::move(message), std::string("invalid_request_error")); }
  }

  rt::Task<SavedPasskeyRegistration> saveVerifiedPasskeyRegistration(std::shared_ptr<rt::Kv> kv, SaveVerifiedPasskeyRegistrationInput input) {
    const rt::KvEntryMaybe<> handleEntry = co_await kv->get(passkeyHandleKey(input.handle));
    if (handleEntry.value.truthy() && !(handleEntry.value.isString() && handleEntry.value.asString() == input.userId)) {
      co_return SavedPasskeyRegistrationRejected{.response = conflict("A passkey account already exists for this username")};
    }

    const double createdAtMs = nowMs();
    const PasskeyCredentialRecord credentialRecord{
      .credential_id = input.credentialId,
      .user_id = input.userId,
      .public_key = input.publicKey,
      .sign_count = input.signCount,
      .transports = input.transports,
      .created_at_ms = createdAtMs,
    };

    const rt::KvEntryMaybe<> existingUserEntry = co_await kv->get(passkeyUserKey(input.userId));
    PasskeyUserRecord userRecord;
    rt::KvValue storedUser;
    std::optional<PasskeyUserRecord> existing;
    if (existingUserEntry.value.truthy()) {
      existing = storedRecord<PasskeyUserRecord>(existingUserEntry.value);
      userRecord = *existing;
      userRecord.handle = input.handle;
      userRecord.is_admin = isPasskeyUserAdmin(*existing);
      // `Array.from(new Set([...existing.credential_ids, input.credentialId]))`
      std::vector<std::string> credentialIds;
      for (const std::string& id : existing->credential_ids)
        if (!rt::includes(credentialIds, id)) credentialIds.push_back(id);
      if (!rt::includes(credentialIds, input.credentialId)) credentialIds.push_back(input.credentialId);
      userRecord.credential_ids = credentialIds;
      userRecord.updated_at_ms = createdAtMs;
      rt::KvObject merged = existingUserEntry.value.asObject();
      merged.set("handle", rt::KvValue(userRecord.handle));
      merged.set("is_admin", rt::KvValue(userRecord.is_admin));
      rt::KvArray ids;
      for (const std::string& id : credentialIds) ids.emplace_back(id);
      merged.set("credential_ids", rt::KvValue(std::move(ids)));
      merged.set("updated_at_ms", rt::KvValue(createdAtMs));
      storedUser = rt::KvValue(std::move(merged));
    } else {
      userRecord = PasskeyUserRecord{
        .id = input.userId,
        .handle = input.handle,
        .is_admin = input.isAdmin,
        .credential_ids = {input.credentialId},
        .created_at_ms = createdAtMs,
        .updated_at_ms = createdAtMs,
      };
      storedUser = toStored(userRecord);
    }

    const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
    atomic->check(existingUserEntry)
      .check(handleEntry)
      .set(passkeyUserKey(userRecord.id), storedUser)
      .set(passkeyHandleKey(input.handle), rt::KvValue(userRecord.id))
      .set(passkeyCredentialKey(input.credentialId), toStored(credentialRecord));
    if (existing && existing->handle != input.handle) atomic->delete_(passkeyHandleKey(existing->handle));
    const rt::KvCommitResult commit = co_await atomic->commit();
    if (!commit.ok) co_return SavedPasskeyRegistrationRejected{.response = conflict("Passkey account was modified concurrently; retry")};

    co_return SavedPasskeyRegistrationOk{.user = std::move(userRecord)};
  }

  rt::Task<std::optional<PasskeySession>> getPasskeySession(std::string token) {
    auto opening = ::uos::kv::getKv();
    const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
    if (!kv) co_return std::nullopt;
    const rt::KvKey sessionKey = passkeySessionKey(token);
    const rt::KvEntryMaybe<> sessionEntry = co_await kv->get(sessionKey);
    if (!sessionEntry.value.truthy()) co_return std::nullopt;
    const PasskeySessionRecord session = storedRecord<PasskeySessionRecord>(sessionEntry.value);
    if (session.expires_at_ms <= nowMs()) {
      const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(sessionEntry).delete_(sessionKey);
      (void)co_await operation->commit();
      co_return std::nullopt;
    }
    const rt::KvEntryMaybe<> userEntry = co_await kv->get(passkeyUserKey(session.user_id));
    if (!userEntry.value.truthy()) co_return std::nullopt;
    co_return PasskeySession{.token = token, .user = storedRecord<PasskeyUserRecord>(userEntry.value), .session = session};
  }

  namespace {
    std::optional<std::string> getCookieValue(const rt::Request& req, std::string_view name) {
      const std::string cookieHeader = req.headers().get("cookie").value_or("");
      for (const std::string& part : rt::split(cookieHeader, ";")) {
        const std::int64_t separator = rt::indexOf(part, "=");
        if (separator < 0 || rt::trim(rt::slice(part, 0, static_cast<double>(separator))) != name) continue;
        const std::string rawValue = rt::trim(rt::slice(part, static_cast<double>(separator) + 1));
        if (rawValue.empty()) return std::nullopt;
        try {
          return rt::decodeURIComponent(rawValue);
        } catch (const rt::JsError&) {
          return std::nullopt;
        }
      }
      return std::nullopt;
    }

    std::string buildRelayCookie(const std::string& token, double expiresAtMs) {
      const double maxAge = rt::Math::max(0.0, rt::Math::ceil((expiresAtMs - nowMs()) / 1000));
      return std::string(PASSKEY_RELAY_COOKIE_NAME) + "=" + rt::encodeURIComponent(token) + "; Path=/; Max-Age=" + rt::numberToString(maxAge) +
             "; HttpOnly; Secure; SameSite=None";
    }

    std::string clearRelayCookie() { return std::string(PASSKEY_RELAY_COOKIE_NAME) + "=; Path=/; Max-Age=0; HttpOnly; Secure; SameSite=None"; }
  }

  rt::Task<std::optional<PasskeySession>> getPasskeySessionFromRequest(rt::Request req) {
    std::optional<std::string> token = http::getBearerToken(req);
    if (!token) token = getCookieValue(req, PASSKEY_RELAY_COOKIE_NAME);
    if (!rt::truthy(token)) co_return std::nullopt;
    rt::Task<std::optional<PasskeySession>> reading = getPasskeySession(*token);
    std::optional<PasskeySession> session = co_await std::move(reading);
    co_return session;
  }

  namespace {
    std::optional<std::string> getRequestAudienceOrigin(const rt::Request& req) {
      const std::optional<std::string> headerOrigin = req.headers().get("origin");
      if (headerOrigin) return relay::parseTrustedAuthRelayOrigin(headerOrigin);
      return relay::parseTrustedAuthRelayOrigin(rt::URL(req.url()).searchParams().get("cors_origin"));
    }
  }

  rt::Task<std::optional<PasskeySession>> getPasskeySessionForRequest(rt::Request req) { return getPasskeySessionForRequest(std::move(req), std::nullopt); }

  rt::Task<std::optional<PasskeySession>> getPasskeySessionForRequest(rt::Request req, std::optional<std::string> authenticatedPasskeyToken) {
    std::optional<PasskeySession> session;
    if (rt::truthy(authenticatedPasskeyToken)) {
      rt::Task<std::optional<PasskeySession>> reading = getPasskeySession(*authenticatedPasskeyToken);
      session = co_await std::move(reading);
    } else {
      rt::Task<std::optional<PasskeySession>> reading = getPasskeySessionFromRequest(req);
      session = co_await std::move(reading);
    }
    if (!session || !rt::truthy(session->session.audience_origin)) co_return session;
    if (getRequestAudienceOrigin(req) == session->session.audience_origin) co_return session;
    co_return std::nullopt;
  }

  namespace {
    rt::Headers noStore() { return rt::Headers{{"Cache-Control", "no-store"}}; }

    rt::Response invalidRequest(double status, std::string message) { return http::openaiError(status, std::move(message), std::string("invalid_request_error")); }

    json::object publicKeyCredential(const std::string& id) {
      json::object entry;
      entry["id"] = id;
      entry["type"] = "public-key";
      return entry;
    }
  }

  rt::Task<rt::Response> handlePasskeyRegisterStart(rt::Request req) { return handlePasskeyRegisterStart(std::move(req), PasskeyRegisterStartOptions{}); }

  rt::Task<rt::Response> handlePasskeyRegisterStart(rt::Request req, PasskeyRegisterStartOptions options) {
    rt::Task<PasskeyJson> reading = readPasskeyJson(req, false);
    const PasskeyJson body = co_await std::move(reading);
    if (body.response) co_return *body.response;
    const json::value& raw = body.raw;
    rt::Task<KvOrError> opening = getKvOrError();
    const KvOrError kvOrError = co_await std::move(opening);
    if (kvOrError.response) co_return *kvOrError.response;
    const std::shared_ptr<rt::Kv> kv = kvOrError.kv;

    std::string token;
    if (options.authenticatedPasskeyToken) token = *options.authenticatedPasskeyToken;
    else if (std::optional<std::string> bearer = http::getBearerToken(req)) token = *bearer;
    std::optional<PasskeySession> existingSession;
    if (!token.empty()) {
      rt::Task<std::optional<PasskeySession>> reading2 = getPasskeySessionForRequest(req, options.authenticatedPasskeyToken);
      existingSession = co_await std::move(reading2);
    }
    if (rt::truthy(options.authenticatedPasskeyToken) && !existingSession) co_return http::openaiError(401, "Unauthorized", std::string("invalid_api_key"));
    const std::string requestedHandle = normalizePasskeyHandle(rt::get(raw, "handle"));
    std::string tokenHandle;
    if (!token.empty()) {
      rt::Task<std::string> building = buildPasskeyHandle(token);
      tokenHandle = co_await std::move(building);
    }
    std::optional<PasskeyUserRecord> requestedUser;
    if (!requestedHandle.empty()) {
      rt::Task<std::optional<PasskeyUserRecord>> finding = getUserByHandle(kv, requestedHandle);
      requestedUser = co_await std::move(finding);
    }
    std::optional<PasskeyUserRecord> tokenUser;
    if (!tokenHandle.empty()) {
      rt::Task<std::optional<PasskeyUserRecord>> finding = getUserByHandle(kv, tokenHandle);
      tokenUser = co_await std::move(finding);
    }
    if (requestedUser && (!existingSession || requestedUser->id != existingSession->user.id) && (!tokenUser || requestedUser->id != tokenUser->id)) {
      co_return conflict("A passkey account already exists for this username");
    }
    std::optional<PasskeyUserRecord> existingUser = existingSession ? std::optional<PasskeyUserRecord>(existingSession->user) : tokenUser ? tokenUser : requestedUser;
    const std::string userId = existingUser ? existingUser->id : rt::crypto::randomUUID();
    // First non-empty candidate wins, so an empty stored or requested handle still
    // falls through to the next source.
    std::string handle;
    for (const std::optional<std::string>& candidate :
         {existingSession ? std::optional<std::string>(existingSession->user.handle) : std::nullopt, std::optional<std::string>(requestedHandle),
          std::optional<std::string>(tokenHandle)}) {
      if (rt::truthy(candidate)) {
        handle = *candidate;
        break;
      }
    }
    if (handle.empty()) {
      rt::Task<std::string> building = buildPasskeyHandle(userId);
      handle = co_await std::move(building);
    }
    if (handle.empty()) co_return invalidRequest(400, "username is required");
    const bool isAdmin = existingUser ? isPasskeyUserAdmin(*existingUser) : options.defaultIsAdmin == true;
    const PasskeyRequestMeta meta = getPasskeyRequestMeta(req, rt::get(raw, "client_origin"));

    Bytes challengeBytes(32);
    rt::crypto::getRandomValues(std::span<std::uint8_t>(challengeBytes));
    const std::string challenge = utils::base64UrlEncode(challengeBytes);
    PasskeyChallengeRecord input{
      .challenge = challenge,
      .type = "registration",
      .origin = meta.origin,
      .rp_id = meta.rpId,
      .audience_origin = std::nullopt,
      .user_id = userId,
      .handle = handle,
      .is_admin = isAdmin,
      .created_at_ms = 0,
      .expires_at_ms = 0,
    };
    rt::Task<PasskeyChallengeRecord> saving = saveChallenge(kv, std::move(input));
    (void)co_await std::move(saving);

    const std::vector<std::uint8_t> userIdBytes = rt::encodeUtf8(userId);
    json::object rp;
    rp["id"] = meta.rpId;
    rp["name"] = RP_NAME;
    json::object user;
    user["id"] = utils::base64UrlEncode(userIdBytes);
    user["name"] = handle;
    user["displayName"] = handle;
    json::array pubKeyCredParams;
    for (const double alg : {-7.0, -257.0}) {
      json::object param;
      param["type"] = "public-key";
      param["alg"] = number(alg);
      pubKeyCredParams.push_back(std::move(param));
    }
    json::object authenticatorSelection;
    authenticatorSelection["residentKey"] = "required";
    authenticatorSelection["userVerification"] = isAdmin ? "required" : "preferred";
    json::array excludeCredentials;
    if (existingUser)
      for (const std::string& id : existingUser->credential_ids) excludeCredentials.push_back(publicKeyCredential(id));
    json::object publicKey;
    publicKey["rp"] = std::move(rp);
    publicKey["user"] = std::move(user);
    publicKey["challenge"] = challenge;
    publicKey["pubKeyCredParams"] = std::move(pubKeyCredParams);
    publicKey["timeout"] = number(PASSKEY_CHALLENGE_TTL_MS);
    publicKey["attestation"] = "none";
    publicKey["authenticatorSelection"] = std::move(authenticatorSelection);
    publicKey["excludeCredentials"] = std::move(excludeCredentials);
    json::object payload;
    payload["publicKey"] = std::move(publicKey);
    payload["handle"] = handle;
    co_return http::json(200, payload, noStore());
  }

  rt::Task<rt::Response> handlePasskeyRegisterFinish(rt::Request req) {
    rt::Task<PasskeyJson> reading = readPasskeyJson(req, false);
    const PasskeyJson body = co_await std::move(reading);
    if (body.response) co_return *body.response;
    const json::value& raw = body.raw;
    rt::Task<KvOrError> opening = getKvOrError();
    const KvOrError kvOrError = co_await std::move(opening);
    if (kvOrError.response) co_return *kvOrError.response;
    const std::shared_ptr<rt::Kv> kv = kvOrError.kv;

    const json::value* storedResponse = rt::get(raw, "response");
    const json::value* response = isWebauthnResponseObject(storedResponse) ? storedResponse : nullptr;
    const json::value* clientDataJson = rt::get(rt::get(response, "response"), "clientDataJSON");
    if (!rt::truthy(clientDataJson)) co_return invalidRequest(400, "Invalid passkey response");

    const std::optional<std::string> challenge = parseClientChallengeOrNull(*clientDataJson);
    if (!rt::truthy(challenge)) co_return invalidRequest(400, "Invalid passkey response");

    rt::Task<std::optional<PasskeyChallengeRecord>> consuming = consumeChallenge(kv, *challenge);
    const std::optional<PasskeyChallengeRecord> challengeRecord = co_await std::move(consuming);
    if (!challengeRecord || challengeRecord->type != "registration" || !rt::truthy(challengeRecord->user_id)) {
      co_return invalidRequest(400, "Invalid passkey challenge");
    }

    std::optional<rt::Response> outcome;
    bool failed = false;
    try {
      rt::Task<VerifiedRegistrationResponse> verifying =
        verifyRegistrationResponse(*response, challengeRecord->challenge, challengeRecord->origin, challengeRecord->rp_id, challengeRecord->is_admin == true);
      const VerifiedRegistrationResponse verification = co_await std::move(verifying);
      if (!verification.verified || !verification.registrationInfo) {
        outcome = invalidRequest(400, "Invalid passkey attestation");
      } else {
        const RegistrationInfo& registrationInfo = *verification.registrationInfo;
        const std::string credentialId = registrationInfo.credentialId;
        const std::string publicKey = utils::base64UrlEncode(registrationInfo.publicKey);
        const json::value handleValue = challengeRecord->handle ? json::value(*challengeRecord->handle) : json::value();
        const std::string handle = normalizePasskeyHandle(challengeRecord->handle ? &handleValue : nullptr);
        if (handle.empty()) {
          outcome = invalidRequest(400, "username is required");
        } else {
          json::value transports = registrationInfo.transports && !registrationInfo.transports->is_null() ? *registrationInfo.transports : json::value(json::array());
          SaveVerifiedPasskeyRegistrationInput input{
            .userId = *challengeRecord->user_id,
            .handle = handle,
            .isAdmin = challengeRecord->is_admin == true,
            .credentialId = credentialId,
            .publicKey = publicKey,
            .signCount = registrationInfo.counter,
            .transports = std::move(transports),
          };
          rt::Task<SavedPasskeyRegistration> saving = saveVerifiedPasskeyRegistration(kv, std::move(input));
          const SavedPasskeyRegistration saved = co_await std::move(saving);
          if (const auto* rejected = std::get_if<SavedPasskeyRegistrationRejected>(&saved)) {
            outcome = rejected->response;
          } else {
            const PasskeyUserRecord& user = std::get<SavedPasskeyRegistrationOk>(saved).user;
            rt::Task<PasskeySessionRecord> creating = createSession(kv, user.id, std::nullopt);
            const PasskeySessionRecord session = co_await std::move(creating);
            json::object payload;
            payload["token"] = session.token;
            payload["user_id"] = user.id;
            payload["handle"] = user.handle;
            payload["credential_count"] = number(static_cast<double>(user.credential_ids.size()));
            payload["expires_at_ms"] = number(session.expires_at_ms);
            outcome = http::json(200, payload, noStore());
          }
        }
      }
    } catch (...) {
      failed = true;
    }
    if (failed) co_return invalidRequest(400, "Invalid passkey attestation");
    co_return *outcome;
  }

  namespace {
    // `{ allowed: true } | { allowed: false; retry_after_seconds: number }`
    struct PasskeyLoginStartBudget {
      bool allowed;
      double retry_after_seconds = 0;
    };

    double passkeyLoginStartWindowStartMs(double atMs) { return std::floor(atMs / PASSKEY_LOGIN_START_WINDOW_MS) * PASSKEY_LOGIN_START_WINDOW_MS; }

    /**
     * Consumes one slot of the current anonymous login-start window. The window is
     * aligned to the epoch so the reset instant depends only on the clock, and a
     * rejected request writes nothing: callers must not issue a challenge.
     */
    rt::Task<PasskeyLoginStartBudget> consumePasskeyLoginStartBudget(std::shared_ptr<rt::Kv> kv, double atMs) {
      const double windowStartMs = passkeyLoginStartWindowStartMs(atMs);
      const double resetAtMs = windowStartMs + PASSKEY_LOGIN_START_WINDOW_MS;
      const double retryAfterSeconds = rt::Math::max(1.0, rt::Math::ceil((resetAtMs - atMs) / 1000));
      const rt::KvKey key = passkeyLoginStartThrottleKey(windowStartMs);
      // A losing atomic commit means another request consumed a slot, so the count
      // can rise at most to the limit before this request must fail closed. At most
      // one attempt per slot is enough to let a full concurrent burst resolve
      // exactly like a sequential one without ever exceeding the bound.
      for (double attempt = 0; attempt < PASSKEY_LOGIN_START_LIMIT; attempt += 1) {
        rt::KvGetOptions strong;
        strong.consistency = rt::KvConsistencyLevel::strong;
        const rt::KvEntryMaybe<> entry = co_await kv->get(key, strong);
        const double count = entry.value.isNullish() ? 0 : storedRecord<PasskeyLoginStartThrottleRecord>(entry.value).count;
        if (count >= PASSKEY_LOGIN_START_LIMIT) co_return PasskeyLoginStartBudget{.allowed = false, .retry_after_seconds = retryAfterSeconds};
        const PasskeyLoginStartThrottleRecord next{.window_start_ms = windowStartMs, .count = count + 1};
        rt::KvSetOptions expiry;
        expiry.expireIn = PASSKEY_LOGIN_START_WINDOW_MS * 2;
        const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry).set(key, toStored(next), expiry);
        const rt::KvCommitResult commit = co_await operation->commit();
        if (commit.ok) co_return PasskeyLoginStartBudget{.allowed = true, .retry_after_seconds = 0};
      }
      co_return PasskeyLoginStartBudget{.allowed = false, .retry_after_seconds = retryAfterSeconds};
    }
  }

  rt::Task<rt::Response> handlePasskeyLoginStart(rt::Request req) {
    rt::Task<PasskeyJson> reading = readPasskeyJson(req, true);
    const PasskeyJson body = co_await std::move(reading);
    if (body.response) co_return *body.response;
    const json::value& raw = body.raw;
    rt::Task<KvOrError> opening = getKvOrError();
    const KvOrError kvOrError = co_await std::move(opening);
    if (kvOrError.response) co_return *kvOrError.response;
    const std::shared_ptr<rt::Kv> kv = kvOrError.kv;

    const std::string handle = normalizePasskeyHandle(rt::get(raw, "handle"));
    const std::optional<std::string> relayOrigin = utils::getString(rt::get(raw, "relay_origin"));
    const std::string rawAudienceOrigin = relayOrigin ? rt::trim(*relayOrigin) : std::string();
    const std::optional<std::string> audienceOrigin = !rawAudienceOrigin.empty() ? relay::parseTrustedAuthRelayOrigin(rawAudienceOrigin) : std::nullopt;
    if (!rawAudienceOrigin.empty() && !rt::truthy(audienceOrigin)) co_return invalidRequest(400, "Invalid passkey relay origin");

    rt::Task<PasskeyLoginStartBudget> budgeting = consumePasskeyLoginStartBudget(kv, nowMs());
    const PasskeyLoginStartBudget budget = co_await std::move(budgeting);
    if (!budget.allowed) {
      http::OpenaiErrorOptions errorOptions;
      errorOptions.type = "rate_limit_error";
      errorOptions.headers = rt::Headers{{"Cache-Control", "no-store"}, {"Retry-After", rt::numberToString(budget.retry_after_seconds)}};
      co_return http::openaiError(429, "Too many passkey login attempts; retry after the current window", std::string("rate_limit_exceeded"), errorOptions);
    }

    std::optional<std::vector<AllowCredential>> allowCredentials;
    // Discoverable credentials may belong to an admin; request the verification
    // that the finish handler will require once the account is identified.
    std::string userVerification = "required";
    if (!handle.empty()) {
      rt::Task<std::optional<PasskeyUserRecord>> finding = getUserByHandle(kv, handle);
      const std::optional<PasskeyUserRecord> user = co_await std::move(finding);
      // Unknown handles and handles without any registered passkey take the same
      // discoverable path as an empty handle, so the response cannot reveal
      // whether the account exists or has passkeys registered.
      if (user && !user->credential_ids.empty()) {
        std::vector<AllowCredential> credentials;
        for (const std::string& id : user->credential_ids) credentials.push_back(AllowCredential{.id = id});
        allowCredentials = std::move(credentials);
        userVerification = isPasskeyUserAdmin(*user) ? "required" : "preferred";
      }
    }

    const PasskeyRequestMeta meta = getPasskeyRequestMeta(req, rt::get(raw, "client_origin"));
    const json::object publicKey = generateAuthenticationOptions(meta.rpId, PASSKEY_CHALLENGE_TTL_MS, allowCredentials, userVerification);
    co_await rt::microtask();

    PasskeyChallengeRecord input{
      .challenge = std::string(publicKey.at("challenge").as_string()),
      .type = "authentication",
      .origin = meta.origin,
      .rp_id = meta.rpId,
      .audience_origin = rt::truthy(audienceOrigin) ? audienceOrigin : std::nullopt,
      .user_id = std::nullopt,
      .handle = std::nullopt,
      .is_admin = std::nullopt,
      .created_at_ms = 0,
      .expires_at_ms = 0,
    };
    rt::Task<PasskeyChallengeRecord> saving = saveChallenge(kv, std::move(input));
    (void)co_await std::move(saving);
    json::object payload;
    payload["publicKey"] = publicKey;
    co_return http::json(200, payload, noStore());
  }

  namespace {
    // A usable assertion credential needs both its stored record and the versionstamp
    // that makes the later sign-count update conditional on an unchanged record.
    struct PasskeyAssertionCredential {
      rt::KvKey key;
      PasskeyCredentialRecord credential;
      rt::KvValue stored;  // the record object as stored, for the sign-count write
      std::string versionstamp;
    };

    // `passkeyCredentialKey(response.id)`: the id is unchecked request JSON.
    rt::KvKey credentialKeyFor(const json::value& id) {
      if (id.is_string()) return passkeyCredentialKey(std::string(id.as_string()));
      if (rt::isNumber(id)) return rt::KvKey{AUTH_PREFIX(), "credentials", rt::toNumber(id)};
      if (id.is_bool()) return rt::KvKey{AUTH_PREFIX(), "credentials", id.as_bool()};
      throw rt::JsError("Unsupported keyPart: object " + rt::toString(id));
    }

    rt::Task<std::optional<PasskeyAssertionCredential>> readPasskeyAssertionCredential(std::shared_ptr<rt::Kv> kv, rt::KvKey credentialKey) {
      const rt::KvEntryMaybe<> entry = co_await kv->get(credentialKey);
      if (!entry.value.truthy() || !rt::truthy(entry.versionstamp)) co_return std::nullopt;
      co_return PasskeyAssertionCredential{
        .key = entry.key, .credential = storedRecord<PasskeyCredentialRecord>(entry.value), .stored = entry.value, .versionstamp = *entry.versionstamp};
    }
  }

  rt::Task<rt::Response> handlePasskeyLoginFinish(rt::Request req) {
    rt::Task<PasskeyJson> reading = readPasskeyJson(req, false);
    const PasskeyJson body = co_await std::move(reading);
    if (body.response) co_return *body.response;
    const json::value& raw = body.raw;
    rt::Task<KvOrError> opening = getKvOrError();
    const KvOrError kvOrError = co_await std::move(opening);
    if (kvOrError.response) co_return *kvOrError.response;
    const std::shared_ptr<rt::Kv> kv = kvOrError.kv;

    const json::value* storedResponse = rt::get(raw, "response");
    const json::value* response = isWebauthnResponseObject(storedResponse) ? storedResponse : nullptr;
    const json::value* clientDataJson = rt::get(rt::get(response, "response"), "clientDataJSON");
    const json::value* responseId = rt::get(response, "id");
    if (!rt::truthy(responseId) || !rt::truthy(clientDataJson)) co_return invalidRequest(400, "Invalid passkey response");

    const std::optional<std::string> challenge = parseClientChallengeOrNull(*clientDataJson);
    if (!challenge) co_return invalidRequest(400, "Invalid passkey response");

    rt::Task<std::optional<PasskeyChallengeRecord>> consuming = consumeChallenge(kv, *challenge);
    const std::optional<PasskeyChallengeRecord> challengeRecord = co_await std::move(consuming);
    if (!challengeRecord || challengeRecord->type != "authentication") co_return invalidRequest(400, "Invalid passkey challenge");

    const rt::KvKey lookupKey = credentialKeyFor(*responseId);
    rt::Task<std::optional<PasskeyAssertionCredential>> looking = readPasskeyAssertionCredential(kv, lookupKey);
    const std::optional<PasskeyAssertionCredential> storedCredential = co_await std::move(looking);
    if (!storedCredential) co_return invalidRequest(400, "Unknown passkey");
    const rt::KvKey& credentialKey = storedCredential->key;
    const PasskeyCredentialRecord& credential = storedCredential->credential;
    const std::string& versionstamp = storedCredential->versionstamp;
    const rt::KvEntryMaybe<> userEntry = co_await kv->get(passkeyUserKey(credential.user_id));
    if (!userEntry.value.truthy()) co_return http::openaiError(401, "Unauthorized", std::string("invalid_api_key"));
    const PasskeyUserRecord user = storedRecord<PasskeyUserRecord>(userEntry.value);

    std::optional<rt::Response> outcome;
    rt::Reason failure;
    try {
      const std::vector<std::uint8_t> publicKeyBytes = utils::base64UrlDecode(credential.public_key);
      const VerifiedAuthenticationResponse verification =
        verifyAuthenticationResponse(*response, challengeRecord->challenge, challengeRecord->origin, challengeRecord->rp_id, isPasskeyUserAdmin(user),
                                     publicKeyBytes, credential.sign_count);
      co_await rt::microtask();
      if (!verification.verified) {
        outcome = invalidRequest(400, "Invalid passkey assertion");
      } else {
        rt::Task<bool> updating = commitSignCount(kv, credentialKey, storedCredential->stored, versionstamp, verification.newCounter);
        const bool updated = co_await std::move(updating);
        if (!updated) {
          outcome = conflict("Passkey credential was modified concurrently; retry");
        } else {
          rt::Task<PasskeySessionRecord> creating = createSession(kv, credential.user_id, challengeRecord->audience_origin);
          const PasskeySessionRecord session = co_await std::move(creating);
          const bool relaySession = rt::truthy(challengeRecord->audience_origin);
          json::object payload;
          if (relaySession) payload["relay_session"] = true;
          else payload["token"] = session.token;
          payload["user_id"] = user.id;
          payload["handle"] = user.handle;
          payload["credential_count"] = number(static_cast<double>(user.credential_ids.size()));
          payload["expires_at_ms"] = number(session.expires_at_ms);
          rt::Headers headers = noStore();
          if (relaySession) headers.set("Set-Cookie", buildRelayCookie(session.token, session.expires_at_ms));
          outcome = http::json(200, payload, headers);
        }
      }
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      if (!responseId->is_string()) throw rt::TypeError("response.id.slice is not a function. (In 'response.id.slice(0, 12)', 'response.id.slice' is undefined)");
      rt::console::warn("[ai.ubq.fi] passkey assertion verification failed",
                        rt::console::Value::object({
                          {"message", rt::isError(failure) ? rt::errorMessage(failure) : rt::reasonToString(failure)},
                          {"expected_origin", challengeRecord->origin},
                          {"expected_rp_id", challengeRecord->rp_id},
                          {"credential_id_prefix", rt::slice(std::string(responseId->as_string()), 0, 12)},
                          {"has_user_handle", !user.handle.empty()},
                          {"require_user_verification", isPasskeyUserAdmin(user)},
                        }));
      co_return invalidRequest(400, "Invalid passkey assertion");
    }
    co_return *outcome;
  }

  rt::Task<rt::Response> handlePasskeySession(rt::Request req) { return handlePasskeySession(std::move(req), PasskeySessionOptions{}); }

  rt::Task<rt::Response> handlePasskeySession(rt::Request req, PasskeySessionOptions options) {
    rt::Task<std::optional<PasskeySession>> reading = getPasskeySessionForRequest(req, options.authenticatedPasskeyToken);
    const std::optional<PasskeySession> session = co_await std::move(reading);
    if (!session) co_return http::openaiError(401, "Unauthorized", std::string("invalid_api_key"));
    json::object user;
    user["id"] = session->user.id;
    user["handle"] = session->user.handle;
    user["is_admin"] = isPasskeyUserAdmin(session->user);
    user["credential_count"] = number(static_cast<double>(session->user.credential_ids.size()));
    json::object sessionView;
    sessionView["created_at_ms"] = number(session->session.created_at_ms);
    sessionView["expires_at_ms"] = number(session->session.expires_at_ms);
    json::object payload;
    payload["user"] = std::move(user);
    payload["session"] = std::move(sessionView);
    co_return http::json(200, payload, noStore());
  }

  rt::Task<rt::Response> handlePasskeyLogout(rt::Request req) { return handlePasskeyLogout(std::move(req), PasskeySessionOptions{}); }

  rt::Task<rt::Response> handlePasskeyLogout(rt::Request req, PasskeySessionOptions options) {
    rt::Task<std::optional<PasskeySession>> reading = getPasskeySessionForRequest(req, options.authenticatedPasskeyToken);
    const std::optional<PasskeySession> session = co_await std::move(reading);
    if (session) {
      auto opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (kv) co_await kv->delete_(passkeySessionKey(session->token));
    }
    rt::ResponseInit init;
    init.status = 204;
    init.headers = rt::Headers{{"Cache-Control", "no-store"}, {"Set-Cookie", clearRelayCookie()}};
    co_return rt::Response(rt::BodyInit(nullptr), init);
  }

  rt::Task<rt::Response> handlePasskeyUsersList() {
    rt::Task<KvOrError> opening = getKvOrError();
    const KvOrError kvOrError = co_await std::move(opening);
    if (kvOrError.response) co_return *kvOrError.response;
    std::vector<PasskeyUserRecord> users;
    rt::KvListSelector selector;
    selector.prefix = rt::KvKey{AUTH_PREFIX(), "users"};
    const std::unique_ptr<rt::KvListIterator> iterator = kvOrError.kv->list(selector);
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      std::optional<PasskeyUserRecord> user = readStoredPasskeyUser(*entry);
      if (user) users.push_back(std::move(*user));
    }
    rt::sort(users, [](const PasskeyUserRecord& a, const PasskeyUserRecord& b) { return b.updated_at_ms - a.updated_at_ms; });
    json::array data;
    for (const PasskeyUserRecord& user : users) data.push_back(serializePasskeyUser(user));
    json::object payload;
    payload["object"] = "list";
    payload["data"] = std::move(data);
    co_return http::json(200, payload, noStore());
  }

  rt::Task<rt::Response> handlePasskeyUsersUpdate(rt::Request req) {
    rt::Task<KvOrError> opening = getKvOrError();
    const KvOrError kvOrError = co_await std::move(opening);
    if (kvOrError.response) co_return *kvOrError.response;
    const std::shared_ptr<rt::Kv> kv = kvOrError.kv;

    json::value raw;
    bool parsed = true;
    try {
      rt::Task<json::value> reading = req.json();
      raw = co_await std::move(reading);
    } catch (...) {
      parsed = false;
    }
    if (!parsed) co_return invalidRequest(400, "Invalid JSON body");
    if (!utils::isRecord(raw)) co_return invalidRequest(400, "Invalid JSON body");

    const std::optional<std::string> rawId = utils::getString(rt::get(raw, "id"));
    const std::string id = rawId ? rt::trim(*rawId) : std::string();
    if (id.empty()) co_return invalidRequest(400, "id is required");
    const json::value* isAdmin = rt::get(raw, "is_admin");
    if (isAdmin == nullptr || !isAdmin->is_bool()) co_return invalidRequest(400, "is_admin must be a boolean");

    const rt::KvKey key = passkeyUserKey(id);
    const rt::KvEntryMaybe<> entry = co_await kv->get(key);
    if (!entry.value.truthy()) co_return http::openaiError(404, "Passkey user not found", std::string("not_found"));

    PasskeyUserRecord next = storedRecord<PasskeyUserRecord>(entry.value);
    next.is_admin = isAdmin->as_bool();
    next.updated_at_ms = nowMs();
    rt::KvObject merged = entry.value.asObject();
    merged.set("is_admin", rt::KvValue(next.is_admin));
    merged.set("updated_at_ms", rt::KvValue(next.updated_at_ms));
    const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
    operation->check(entry).set(key, rt::KvValue(std::move(merged)));
    const rt::KvCommitResult commit = co_await operation->commit();
    if (!commit.ok) co_return conflict("Passkey user was modified concurrently; retry");

    json::object payload;
    payload["user"] = serializePasskeyUser(next);
    co_return http::json(200, payload, noStore());
  }
}
