# uos::rt values: JSON, numbers, Map/Set, nullable

Oracle: Bun 1.4.0 (JavaScriptCore). Headers: `uos/rt/json.hpp` (+ `src/rt/json.cpp`),
`uos/rt/number.hpp` (+ `src/rt/number.cpp`), `uos/rt/ordered.hpp`, `uos/rt/nullable.hpp`.
Strings are canonical WTF-8 (`docs/rt/text.md`). Every behaviour below is pinned by bun-generated
vectors in `tests/rt/{json,number,ordered,nullable}_test.cpp`.

## Conventions (binding for every porter)

1. **Canonical number.** A JS number inside a `json::value` is `kind::int64` when it is an
   integer with |x| <= 2^53-1 and not -0; every other number (fractions, -0, NaN, +-Infinity,
   |x| >= 2^53) is `kind::double_`. `jsonParse`, `makeNumber`, `objectSet`/`objectDefine` and the
   Nullable writers store numbers this way, so boost's `==` on two canonical values is JS
   equality of their numbers. Readers (`toNumber`, `getNumber`, `jsonStringify`, `deepEqual`,
   `isNumber`, ...) accept int64, uint64 and double; an int64/uint64 beyond 2^53 reads as the
   nearest double, as the JS literal would. `uint64` is never produced. Never branch on boost
   number kinds and never call `as_int64()`/`as_double()` on JSON data; use the getters. A value
   built from C++ literals (`json::value(1.0)`) is not canonical: compare with `rt::deepEqual`, or
   run it through `rt::canonicalNumbers`.
2. **undefined.** A `json::value` never holds `undefined`. An absent member is `nullptr`
   (`rt::get`) / `std::nullopt` (typed getters) / `Nullable<T>::undefined`. JSON.stringify output
   is the JS output: writing an undefined member removes it (`objectSet(obj, k, Nullable/optional)`),
   an undefined array element is written as `nullptr` (`n.toJson().value_or(nullptr)`).
   `T | undefined` -> `std::optional<T>`; `T | null` -> `std::optional<T>`; both and the TS tells
   them apart -> `rt::Nullable<T>`.
3. **Property order.** JS enumerates array-index keys (`"0"`..`"4294967294"`, canonical decimal)
   ascending, then the other keys in insertion order. `json::object` keeps insertion order. All rt
   enumerations (`jsonStringify`, `objectKeys/Values/Entries`, spread, assign) use JS order whatever
   the storage order; `jsonParse` and the rt writers also keep storage in JS order. Plain
   `obj[key] = v` / `emplace` / `insert_or_assign` equal JS assignment when no index key is
   involved. **Never call `json::object::erase`** (it moves the last member into the hole): use
   `rt::objectDelete`.
4. **Equality.** `===` -> `rt::strictEquals` (objects/arrays by identity = same `json::value`
   address); structural -> `rt::deepEqual` (numbers by `Object.is`). `x == null` ->
   `rt::isNullish(p)`. Never compare non-canonical JSON with boost `==`.
5. **Depth.** `jsonParse` and the recursive helpers refuse nesting deeper than
   `rt::kJsonMaxDepth` (128) with `RangeError("Maximum call stack size exceeded.")`.
6. **Errors** carry JavaScriptCore's text (`rt::SyntaxError`, `rt::RangeError`, `rt::TypeError`).

## JSON

| TypeScript                                       | C++                                                                                                                                          |
| ------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------- |
| `JSON.parse(text)`                               | `rt::jsonParse(text)` (SyntaxError `"JSON Parse error: ..."`, exact JSC text)                                                                |
| `await response.json()` / `request.json()`       | `rt::jsonParseBody(decodedText, bodyHadZeroBytes)` (`"Unexpected end of JSON input"` / `"Failed to parse JSON"`)                             |
| `JSON.stringify(v)`                              | `rt::jsonStringify(v)`                                                                                                                       |
| `JSON.stringify(v, null, 2)` / `(v, null, "\t")` | `rt::jsonStringify(v, 2)` / `rt::jsonStringify(v, "\t")`                                                                                     |
| `JSON.stringify(v, (k, x) => ...)`               | `rt::jsonStringify(v, replacer[, gap])` -> `std::optional<std::string>`; replacer returns `std::optional<json::value>` (nullopt = undefined) |
| `JSON.stringify(str)`                            | `rt::jsonQuote(str)`                                                                                                                         |
| `JSON.parse(JSON.stringify(v))`                  | write it literally; `structuredClone(v)` -> `rt::structuredClone(v)` (a copy)                                                                |

Stringify facts: numbers as `String(n)`, NaN/Infinity as `null`, `-0` as `0`; escapes `\" \\ \b \f \n \r \t`,
other controls `\u00xx` (lowercase), lone surrogates `\udxxx`, U+2028/2029 and DEL unescaped.
Parse facts: duplicate key keeps the first position and the last value; `"__proto__"` is an
ordinary key; `-0` stays `-0`; `1e` / `1e+` stop the number before `e`.

## Unknown values (`unknown`, `any`, parsed JSON)

| TypeScript                                                 | C++                                                                                                                                                                                                            |
| ---------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `typeof v`                                                 | `rt::typeOf(v)` / `rt::typeOf(ptr)` (`"undefined"` for nullptr)                                                                                                                                                |
| `typeof v === "number"` / `"string"` / `"boolean"`         | `rt::isNumber(v)` / `v.is_string()` / `v.is_bool()`                                                                                                                                                            |
| `Array.isArray(v)`                                         | `v.is_array()`                                                                                                                                                                                                 |
| `isRecord(v)` (non-null, non-array object)                 | `rt::isRecord(v)` / `rt::isRecord(ptr)`                                                                                                                                                                        |
| `v == null` / `v != null`                                  | `rt::isNullish(ptr)` / `!rt::isNullish(ptr)`                                                                                                                                                                   |
| `if (v)`, `Boolean(v)`, `!v`                               | `rt::truthy(v / ptr / double / string_view / optional / integer)`                                                                                                                                              |
| `a ?? b`                                                   | optional: `a.value_or(b)`; pointer: `rt::isNullish(a) ? b : *a`; Nullable: `a.valueOr(b)`                                                                                                                      |
| `a \|\| b`                                                 | `rt::truthy(a) ? a : b`                                                                                                                                                                                        |
| `Number(v)`, `+v`                                          | `rt::toNumber(v)` (`nullptr` -> NaN)                                                                                                                                                                           |
| `String(v)`, `` `${v}` ``                                  | `rt::toString(v)` (`nullptr` -> `"undefined"`)                                                                                                                                                                 |
| `Number.isFinite/isNaN/isInteger/isSafeInteger(v)`         | `rt::Number::isFinite(v)` ... (false for non-numbers)                                                                                                                                                          |
| `isNaN(v)` / `isFinite(v)` (global, coercing)              | `rt::isNaN(v)` / `rt::isFinite(v)`                                                                                                                                                                             |
| `a === b`                                                  | `rt::strictEquals(a, b)`; numbers: `a == b`                                                                                                                                                                    |
| `Object.is(a, b)`                                          | `rt::sameValue(a, b)` (doubles)                                                                                                                                                                                |
| `assert.deepEqual` / `expect().toEqual` / `Bun.deepEquals` | `rt::deepEqual(a, b)` (they agree on JSON data; on JS objects `toEqual`/`Bun.deepEquals` ignore undefined members, `assert.deepEqual` from `node:assert/strict` does not: compare such structs field by field) |
| `expect(r).toMatchObject(e)`                               | `rt::matchesObject(r, e)`                                                                                                                                                                                      |

## Objects

| TypeScript                                                             | C++                                                                                                                                                                                                                             |
| ---------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `obj.k`, `obj[k]` (own member)                                         | `rt::get(obj, k)` -> pointer, nullptr = undefined; on unknown: `rt::get(value, k)`                                                                                                                                              |
| typed read (`typeof obj.k === "string" ? obj.k : undefined`)           | `rt::getString/getNumber/getBool(obj or value, k)` -> optional; `rt::getObject/getArray` -> pointer                                                                                                                             |
| `arr[i]`                                                               | `rt::get(arr, i)` -> pointer                                                                                                                                                                                                    |
| `value[0]` on unknown JSON (an array, or `obj["0"]` on an object)      | `rt::get(value, 0)` with any integer type (not bool or a character type): the element, the member `String(index)`, or nullptr (negative, past the end, other types). A literal `0` no longer converts to a null string_view key |
| `k in obj`, `Object.hasOwn(obj, k)`, `obj.hasOwnProperty(k)`           | `rt::hasOwn(obj, k)`                                                                                                                                                                                                            |
| `obj[k] = v`                                                           | `rt::objectSet(obj, k, v)`                                                                                                                                                                                                      |
| `{ ...a, k: v }` member, `Object.defineProperty`, `Object.fromEntries` | `rt::objectDefine(obj, k, v)`, `rt::objectFromEntries(pairs)`                                                                                                                                                                   |
| `delete obj[k]`                                                        | `rt::objectDelete(obj, k)`                                                                                                                                                                                                      |
| `{ ...a, ...b }`                                                       | `rt::objectSpread(a, b, ...)`                                                                                                                                                                                                   |
| `Object.assign(target, src)`                                           | `rt::objectAssign(target, src)`                                                                                                                                                                                                 |
| `Object.keys/values/entries(obj)`                                      | `rt::objectKeys(obj)` (strings) / `rt::objectValues(obj)` / `rt::objectEntries(obj)` (pointers, JS order)                                                                                                                       |
| `Object.keys(obj).length`                                              | `obj.size()`                                                                                                                                                                                                                    |
| `Object.freeze(x)`                                                     | `const`                                                                                                                                                                                                                         |

`objectSet` models `[[Set]]`: `obj.__proto__ = v` without an own `"__proto__"` member creates
nothing (JS calls the prototype setter). `objectDefine`/spread/`jsonParse` treat it as a key.

## Arrays

| TypeScript                                                | C++                                                                                                                                                                                                             |
| --------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `a.sort((x, y) => ...)`, `a.toSorted(cmp)`                | `rt::sort(container, cmp)`, `rt::toSorted(container, cmp)`; `cmp` returns a number (a `bool` comparator does not compile); NaN or 0 keep order; stable                                                          |
| `a.sort()` (no comparator)                                | `rt::sort(vector<string>)` UTF-16 order; `rt::sort(vector<double>)` by `String(n)` (`[10,9,1]` -> `[1,10,9]`); `rt::sort(json::array)` by `String(v)`                                                           |
| `a.includes(x)`                                           | `rt::includes(vec, x)` (SameValueZero: finds NaN), `rt::includes(jsonArray, v)`                                                                                                                                 |
| `a.indexOf(x)`                                            | `rt::indexOf(vec, x)` (strict: never finds NaN), `-1` when absent                                                                                                                                               |
| `a.at(i)`                                                 | `rt::at(vec, i)` / `rt::at(jsonArray, i)` -> pointer (negative from the end)                                                                                                                                    |
| `a[i]` with a computed number `i` (`slots[account.slot]`) | `rt::element(vec, i)` / `rt::element(jsonArray, i)` -> pointer: only a canonical array index below the length reads (-0 reads 0); negative, fractional, NaN or infinite reads nullptr (undefined). Not `rt::at` |
| `a.flat()` / `a.flat(d)`                                  | `rt::flat(jsonArray, d)`, `rt::flat(vector<vector<T>>)`                                                                                                                                                         |
| `Array.from(iterable)`, `[...iterable]`                   | `rt::arrayFrom(range)`                                                                                                                                                                                          |

## Numbers (`uos/rt/number.hpp`)

| TypeScript                                                                                 | C++                                                                                                                                                                                                                                      |
| ------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `String(n)`, `` `${n}` ``, `n.toString()`                                                  | `rt::numberToString(n)` (never `std::to_string`/iostreams)                                                                                                                                                                               |
| `Number(s)`, `+s`                                                                          | `rt::stringToNumber(s)` ("" -> 0, hex/octal/binary, Infinity, StrWhiteSpace trimmed)                                                                                                                                                     |
| `parseInt(s, r)`, `Number.parseInt`                                                        | `rt::parseInt(s, r)` (r already ToInt32; 0 = absent)                                                                                                                                                                                     |
| `Number.isInteger/isSafeInteger/isFinite/isNaN(n)`                                         | `rt::Number::isInteger(n)` ...                                                                                                                                                                                                           |
| `Number.MAX_SAFE_INTEGER`, `EPSILON`, `MAX_VALUE`, `MIN_VALUE`, `POSITIVE_INFINITY`, `NaN` | `rt::Number::MAX_SAFE_INTEGER` ...                                                                                                                                                                                                       |
| `Math.round/trunc/floor/ceil/sign/abs(x)`                                                  | `rt::Math::round(x)` ... (round: halves toward +Infinity, keeps -0)                                                                                                                                                                      |
| `Math.max(...)`, `Math.min(...)`                                                           | `rt::Math::max(a, b, ...)` / `rt::Math::max(span)` (NaN wins, empty -> -Infinity)                                                                                                                                                        |
| a constant built with `Math.ceil/floor/trunc/abs/max/min`                                  | `inline constexpr double X = rt::Math::ceil(A / B) * 4;` (these six are constexpr: the run time calls the same library cores, a constant expression a bit-exact twin, `rt::Math::detail::*Constant`; `UOS_RT_MATH_CONSTEXPR` is defined) |
| `a ** b`, `Math.pow`                                                                       | `rt::Math::pow(a, b)`                                                                                                                                                                                                                    |
| `Math.imul(a, b)`                                                                          | `rt::Math::imul(a, b)`                                                                                                                                                                                                                   |
| `x \| 0`, `x >>> 0`                                                                        | `rt::toInt32(x)`, `rt::toUint32(x)`                                                                                                                                                                                                      |
| `a & b`, `\|`, `^`, `~a`, `<<`, `>>`, `>>>`                                                | `rt::bitAnd/bitOr/bitXor/bitNot/shiftLeft/shiftRight/shiftRightUnsigned`                                                                                                                                                                 |
| `a % b`                                                                                    | `rt::mod(a, b)` (sign of the dividend, -0 kept)                                                                                                                                                                                          |
| `+ - * /`, `<`, `===` on numbers                                                           | plain `double` operators                                                                                                                                                                                                                 |

## BigInt

int64/uint64 do not suffice: `kv-conversion` (typed values, `-(2n ** 120n)`) and
`native-authority-sql` (`bigintPart`, up to 255 bytes) need arbitrary size. `rt::BigInt` is
arbitrary precision with JSC's limits (2^20 bits) and error texts.

| TypeScript                                 | C++                                                                                                                     |
| ------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------- |
| `123n`, `BigInt(n)`, `BigInt(s)`           | `rt::BigInt(123)`, `rt::BigInt::fromNumber(n)` (RangeError "Not an integer"), `rt::BigInt::fromString(s)` (SyntaxError) |
| `String(b)`, `b.toString(16)`, `Number(b)` | `b.toString()`, `b.toString(16)`, `b.toNumber()`                                                                        |
| `BigInt.asUintN(64, b)`, `asIntN`          | `rt::BigInt::asUintN(64, b)`, `rt::BigInt::asIntN(64, b)`; as native: `b.toUint64()` / `b.toInt64()`                    |
| `+ - * / % ** & \| ^ ~ << >>`, `<`, `===`  | the C++ operators, `b.pow(e)`, `<=>`, `==`                                                                              |

## Map and Set (`uos/rt/ordered.hpp`)

| TypeScript                                                                       | C++                                                                                           |
| -------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------- |
| `new Map<K, V>()`, `new Map([[k, v]])`                                           | `rt::OrderedMap<K, V> m;`, `rt::OrderedMap<K, V> m{{k, v}};`                                  |
| `m.get(k)` / `m.has(k)` / `m.set(k, v)` / `m.delete(k)` / `m.clear()` / `m.size` | `m.get(k)` (pointer) / `m.has(k)` / `m.set(k, v)` / `m.delete_(k)` / `m.clear()` / `m.size()` |
| `for (const [k, v] of m)`, `m.forEach((v, k) => ...)`                            | `for (auto& [k, v] : m)`, `m.forEach([](V& v, const K& k) {...})`                             |
| `m.keys()`, `m.values()`, `m.entries()`                                          | live ranges `m.keys()`, `m.values()`, `m.entries()`                                           |
| `[...m.keys()]`, `Array.from(m)`                                                 | `rt::arrayFrom(m.keys())`, `rt::arrayFrom(m)` (vector of pairs)                               |
| `m.keys().next().value` (oldest)                                                 | `*m.keys().begin()` (check `m.size()` first)                                                  |
| `new Set(xs)`, `s.add(x)`                                                        | `rt::OrderedSet<K> s(xs.begin(), xs.end());`, `s.add(x)`; otherwise as Map                    |

Semantics: insertion order; re-set keeps the position; delete + set moves to the end; entries
added during iteration are visited, deleted ones not yet reached are skipped, `clear()` ends the
iteration unless entries are added afterwards; references to an entry stay valid until every
iterator is gone even if the entry is deleted. Keys use SameValueZero (`NaN` == `NaN`, -0 stored
as +0); `std::string` keys accept `std::string_view` lookups; object identity keys -> pointer keys.
Use `std::unordered_map`/`std::unordered_set` when iteration order is never observed.

## Nullable (`uos/rt/nullable.hpp`)

| TypeScript                                                                        | C++                                                                                                                                                                                    |
| --------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `let x: T \| null \| undefined`                                                   | `rt::Nullable<T> x;` (undefined), `= nullptr`, `= value`, `= rt::undefined`                                                                                                            |
| `x === undefined` / `x === null` / `x == null`                                    | `x == rt::undefined` / `x == nullptr` / `x.isNullish()`                                                                                                                                |
| `x ?? d`                                                                          | `x.valueOr(d)`                                                                                                                                                                         |
| `x` as value                                                                      | `*x`, `x->m`, `x.value()` (throws `std::bad_optional_access` when nullish)                                                                                                             |
| `{ k: x }` into JSON                                                              | `rt::objectSet(obj, "k", x)` / `rt::objectDefine(...)` (undefined removes `k`); also for `std::optional<T>` (nullopt = undefined)                                                      |
| array element `x` into JSON                                                       | `arr.push_back(x.toJson().value_or(nullptr))`                                                                                                                                          |
| `const x = obj.k` (T \| null \| undefined)                                        | `rt::getNullable<T>(obj, "k")` -> `std::optional<Nullable<T>>`, nullopt when `k` holds another type                                                                                    |
| `std::optional` as `T \| undefined` / `T \| null`                                 | `rt::orUndefined(opt)` / `rt::orNull(opt)`                                                                                                                                             |
| `for (const x of values)`, `values.includes(x)` over `(T \| null \| undefined)[]` | `std::vector<rt::Nullable<T>>` with range-for, `std::find`, `==` (iterator comparisons over Nullable containers compile since the `x === value` operator deduces its Nullable operand) |

## Known divergences

- `sort` with an inconsistent comparator (one that is not a consistent order, e.g. returns NaN
  for some pairs or random results): JavaScriptCore's TimSort and `rt::sort` can order elements
  differently; both stay permutations. Consistent comparators give identical results.
- Inherited `Object.prototype` members (`obj.toString`, `"constructor" in obj`) read as absent.
  `obj.__proto__ = {...}` changes no own members in either language, but later inherited reads
  are not modelled.
- `obj.k = undefined; obj.k = 1` moves `k` to the end in C++ (undefined deletes the member);
  JS keeps `k` in place. `Object.keys` after `obj.k = undefined` lists `k` in JS only.
- Documents nested deeper than 128 levels: `RangeError` from `jsonParse` (JSC parses them,
  and fails only in JSON.stringify/structuredClone beyond about 40000 levels).
- `JSON.stringify` of BigInt (TypeError), cycles, `toJSON` and functions has no json::value
  input; `Date` objects serialise through `toISOString` (text component).
