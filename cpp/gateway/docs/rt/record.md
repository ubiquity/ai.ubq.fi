# uos::rt record conversion

`uos/rt/record.hpp` (`uos::rt::record`) writes the bodies of a record struct's Boost.JSON `tag_invoke` pair. Declare both
overloads in the module header, in the struct's namespace (ADL finds them there; a `std::variant` alias through its
alternatives). The `.cpp` includes the header and adds `namespace { using rt::record::Reader; using rt::record::Writer; }`.

| TS member       | C++ member              | `Writer()`, in declaration order        | `const Reader in(value, "Type");` |
| --------------- | ----------------------- | --------------------------------------- | --------------------------------- |
| `k: T`          | `T k;`                  | `.required("k", v.k)`                   | `in.required<T>("k")`             |
| `k: T \| null`  | `std::optional<T> k;`   | `.orNull("k", v.k)`                     | `in.orNull<T>("k")`               |
| `k?: T`         | `std::optional<T> k{};` | `.orUndefined("k", v.k)`: nullopt omits | `in.orUndefined<T>("k")`          |
| `k?: T \| null` | `rt::Nullable<T> k{};`  | `.nullable("k", v.k)`                   | `in.nullable<T>("k")`             |

| `T`                                                                                       | reads                                                                  | writes                                           | `<wanted>`                                                    |
| ----------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- | ------------------------------------------------ | ------------------------------------------------------------- |
| `double`                                                                                  | any number                                                             | canonical number                                 | `number`                                                      |
| integer type (`std::int64_t`, `int`, `std::uint8_t`, ...; not `bool` or a character type) | a number that is an integer in `T`'s range (`-0` reads as 0)           | canonical number; beyond 2^53 the nearest double | `number`, or `integer from <min> to <max>` for another number |
| `bool`, `std::string`                                                                     | boolean, string                                                        | as is                                            | `boolean`, `string`                                           |
| `json::value`                                                                             | anything, as is                                                        | through `rt::canonicalNumbers`                   | `JSON value`                                                  |
| `json::object`, `json::array`                                                             | object, array, as is                                                   | through `rt::canonicalNumbers`                   | `object`, `array`                                             |
| `std::vector<T>`                                                                          | array; an element reports as `<k>[]`                                   | array                                            | `<wanted of T>[]`                                             |
| enum, record or variant with its own pair                                                 | its `value_to` (for a record or variant, once the member is an object) | its `value_from`                                 | `string` (enum), `object`                                     |
| anything else (`float`, `char`, `std::string_view`, `std::optional`, maps, no pair)       | `static_assert`                                                        | `static_assert`                                  | `rt::record::Readable<T>`, `Writable<T>`                      |

Reader throws `rt::TypeError("<Type>.<k> is <JS type>, not <wanted>")` with a JS type of `undefined`, `null`, `array`, `object`,
`string`, `number` or `boolean` and `<wanted>` plus the row's ` or null`, ` or undefined` or `, null or undefined` (a non-object
record: `<Type> is <JS type>, not object`). A non-rt exception from a member's own `value_to` (Boost's `system_error`, say)
becomes `rt::TypeError("<Type>.<k>: <reason>")`; rt errors pass through.

Caveats: a present but invalid enum throws the enum's own message, without the `<Type>.<k>` prefix. A nested record reports
under its own Reader label (label a struct only one member holds by its path, `"Job.lease"`); elements carry no index. JSON
members are canonicalised on write but read as is, as Boost's `value_to` and `rt::fromJsonValue` read them. A class member must
be a JSON object (hand-write a class whose JSON is a string, number or array), and implicit conversions count: an alternative is
written through its variant's pair. Reader borrows `value` and the label (pass the parameter and a literal); call `done()` once.

```cpp
// TS: Job = { id: string; owner: string | null; lease?: Lease; cost?: number | null; steps: Step[] }
struct Hold { static constexpr std::string_view type = "hold"; double until_ms; };
struct Done { static constexpr std::string_view type = "done"; };
using Step = std::variant<Hold, Done>;
struct Job { std::string id; std::optional<std::string> owner; std::optional<Lease> lease{};
             rt::Nullable<double> cost{}; std::vector<Step> steps; };
void tag_invoke(const json::value_from_tag&, json::value& out, const Step& value) {
  if (const Hold* hold = std::get_if<Hold>(&value))
    out = Writer().required("type", std::string(Hold::type)).required("until_ms", hold->until_ms).done();
  else out = Writer().required("type", std::string(Done::type)).done();
}
Step tag_invoke(const json::value_to_tag<Step>&, const json::value& value) {
  const Reader in(value, "Step");
  const std::string type = in.required<std::string>("type");
  if (type == Hold::type) return Hold{.until_ms = in.required<double>("until_ms")};
  if (type == Done::type) return Done{};
  in.fail("type", rt::get(value, "type"), "\"hold\" | \"done\"");
}
void tag_invoke(const json::value_from_tag&, json::value& out, const Job& value) {
  out = Writer().required("id", value.id).orNull("owner", value.owner).orUndefined("lease", value.lease)
          .nullable("cost", value.cost).required("steps", value.steps).done();
}
Job tag_invoke(const json::value_to_tag<Job>&, const json::value& value) {
  const Reader in(value, "Job");
  return {.id = in.required<std::string>("id"), .owner = in.orNull<std::string>("owner"),
          .lease = in.orUndefined<Lease>("lease"), .cost = in.nullable<double>("cost"),
          .steps = in.required<std::vector<Step>>("steps")};
}
```
