#pragma once
// JavaScript's view of stored values (rt::KvValue, what a KV read returns): `typeof` and the record
// guards, the json::value view handed to guards written against `unknown`, JSON.stringify,
// String(value), `instanceof Uint8Array`, `===` on key parts, and the conversions between keys and
// stored arrays with the @deno/kv SDK's key-part error. Oracle: Bun 1.4.0 on values read back from
// the store (node:v8 structured clone; a top-level KvU64 or Uint8Array is stored raw). Reference:
// docs/rt/kv.md, "Stored values seen from JavaScript".
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace uos::rt {
  // ---- typeof and record guards ---------------------------------------------------------------

  // `typeof value`: "undefined" (also an array hole read as an element), "object" (null and every
  // object kind: arrays, Map, Set, Date, RegExp, Error, KvU64, buffers and views), "boolean",
  // "number", "string" or "bigint".
  std::string_view kvTypeof(const KvValue& value) noexcept;
  // `typeof value === "object" && value !== null`, arrays included: src/utils.ts `isRecord`.
  bool kvIsObject(const KvValue& value) noexcept;
  // `typeof value === "object" && value !== null && !Array.isArray(value)`: rt::isRecord's shape
  // (Map, Set, Date, KvU64, buffers ... are records).
  bool kvIsRecord(const KvValue& value) noexcept;

  // ---- the JSON view of a stored value ---------------------------------------------------------

  // How storedJsonView maps what JSON cannot hold. Whatever the options: null, booleans, strings
  // and arrays are kept; numbers are kept canonically (rt::makeNumber: NaN, +-Infinity and -0 stay
  // doubles, so typeof, Number.isFinite and === read them exactly); objects keep their members in
  // JS order; Date, RegExp, Error, Map, Set, ArrayBuffer and DataView are `{}` (their own
  // enumerable properties: typeof, isRecord, Object.keys and own-member reads see what JS sees;
  // inherited or non-enumerable reads such as `error.message`, `map.size`, `date.getTime` are not
  // represented). See docs/rt/kv.md for which options reproduce which earlier local helper.
  struct StoredJsonViewOptions {
    enum class UndefinedView : std::uint8_t {
      omit,         // object members: the member is absent (elsewhere treated as null)
      null,         // null
      emptyObject,  // {}
    };
    enum class BigIntView : std::uint8_t {
      emptyObject,    // {} (default): no number, string, boolean or null test accepts it
      null,           // null
      decimalString,  // String(n)
      number,         // Number(n), the nearest double
    };
    // An object member holding undefined. omit (default) is exact for reads (`=== undefined`,
    // typeof, `??`, truthiness, typed tests); emptyObject is exact for Object.keys, hasOwn and
    // `in` (and for guards that reject any present member of the wrong type), while reads see
    // an object.
    UndefinedView undefinedMember = UndefinedView::omit;
    // An array element holding undefined, and an array hole (default null, as JSON.stringify).
    UndefinedView undefinedElement = UndefinedView::null;
    UndefinedView hole = UndefinedView::null;
    // A stored `undefined` itself (default null: false for isRecord, typeof checks and
    // truthiness, like undefined; pass nullptr to a guard's pointer overload for `=== undefined`).
    UndefinedView topLevelUndefined = UndefinedView::null;
    // A bigint, also a KvU64's `value` and a BigInt64Array/BigUint64Array element.
    BigIntView bigint = BigIntView::emptyObject;
    // true (default): a KvU64 is `{ value: <bigint> }` (its own enumerable member); false: `{}`.
    bool kvU64Members = true;
    // true (default): a typed array, Uint8Array included, is `{ "0": n, "1": n, ... }` (its own
    // enumerable index members, element values as numbers); false: `{}`.
    bool typedArrayMembers = true;
  };

  // A stored value as the json::value a JS guard over `unknown` would observe, for the
  // json::value-based guards and normalizers (`normalizeX(entry.value)`). RangeError "Maximum
  // call stack size exceeded." past rt::kJsonMaxDepth.
  json::value storedJsonView(const KvValue& value);
  json::value storedJsonView(const KvValue& value, const StoredJsonViewOptions& options);

  // ---- JSON.stringify ---------------------------------------------------------------------------

  // `JSON.stringify(value)` / `(value, null, space)` / `(value, null, gap)`, byte-identical to
  // Bun: std::nullopt for undefined; undefined members are skipped and undefined elements and
  // holes print null; a typed array (Uint8Array included) prints its index members; a Date its
  // toJSON (ISO text, null when invalid); Map, Set, RegExp, Error, ArrayBuffer and DataView `{}`;
  // a bigint anywhere (also a KvU64's value or a BigInt64Array element) throws TypeError
  // "JSON.stringify cannot serialize BigInt.". Numbers as rt::jsonStringify.
  std::optional<std::string> kvJsonStringify(const KvValue& value);
  std::optional<std::string> kvJsonStringify(const KvValue& value, int space);
  std::optional<std::string> kvJsonStringify(const KvValue& value, std::string_view gap);

  // ---- String(value) ----------------------------------------------------------------------------

  // `String(value)` / `${value}`: arrays and typed arrays join their elements with ","
  // (undefined, null and holes print ""), a Date prints Date.prototype.toString (UTC), a RegExp
  // "/source/flags", an Error "Name: message" (Error.prototype.toString), a KvU64 and a plain
  // object "[object Object]", Map/Set/ArrayBuffer/DataView "[object Map]" ...; a plain object
  // with an own `toString` member (never callable when stored) throws TypeError "No default
  // value", as Bun does.
  std::string kvToString(const KvValue& value);

  // ---- instanceof Uint8Array --------------------------------------------------------------------

  // `value instanceof Uint8Array`: KvBytes, and a Uint8Array view into a larger buffer
  // (KvArrayBufferView of kind Uint8Array); not Uint8ClampedArray or other views.
  bool kvIsUint8Array(const KvValue& value) noexcept;
  // The bytes such a Uint8Array covers (its view only), std::nullopt for any other value. The span
  // lives as long as `value`.
  std::optional<std::span<const std::uint8_t>> kvUint8ArrayBytes(const KvValue& value) noexcept;

  // ---- keys and key parts -----------------------------------------------------------------------

  // A key part as the stored value JS holds for it (`{ request_key: key }` stores the array).
  KvValue kvValueFromKeyPart(const KvKeyPart& part);
  // `[...key]` as a stored array.
  KvValue kvValueFromKey(const KvKey& key);
  // The key part a stored value packs as (`value as KvKeyPart`): a string, number, bigint,
  // boolean or Uint8Array (a view packs the bytes it covers); std::nullopt for anything else.
  std::optional<KvKeyPart> kvKeyPartFromValue(const KvValue& value);
  // As kvKeyPartFromValue, but where the SDK's encoder rejects the value it throws its Error
  // "Unsupported keyPart: <typeof value> <String(value)>" (e.g. "Unsupported keyPart: object
  // [object Object]", "Unsupported keyPart: undefined undefined"). The TS throws when the key is
  // used (encoded), so convert where the TS reads, writes or lists with the key.
  KvKeyPart toKvKeyPart(const KvValue& value);
  // `value as KvKey` for a stored array (TypeError "KV value is <kind>, not Array" for another
  // kind: the TS checks Array.isArray first): every element through toKvKeyPart, holes skipped
  // (the SDK encodes keys with flatMap, which skips them).
  KvKey kvKeyFromValue(const KvValue& value);
  // `left === right` on key parts: strings, booleans and bigints by value, numbers by `===` (NaN
  // never, 0 === -0); a Uint8Array is an object, equal only to itself (the same KvKeyPart object).
  bool kvKeyPartStrictEquals(const KvKeyPart& left, const KvKeyPart& right) noexcept;
  // `left.length === right.length && left.every((part, index) => part === right[index])`.
  bool kvKeyStrictEquals(const KvKey& left, const KvKey& right) noexcept;
}
