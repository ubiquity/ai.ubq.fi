#pragma once
// Port of src/kv-conversion/profile-json.ts.
// JSON wire vocabulary retained from @deno/kv-utils 0.1.4 (MIT, Deno authors).
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/stream.hpp"

#include <string>
#include <vector>

namespace uos::kv_conversion::profile_json {
  // `{ type: string; value?: unknown; byteLength?: number }`: a wire record, built and read as JSON.
  using KvValueJson = json::object;

  // `{ key: KvValueJson[]; value: KvValueJson; versionstamp: string }`
  struct KvEntryJson {
    std::vector<KvValueJson> key;
    KvValueJson value;
    std::string versionstamp;
    friend bool operator==(const KvEntryJson&, const KvEntryJson&) = default;
  };

  KvValueJson keyPartToJson(const rt::KvKeyPart& value);

  std::vector<KvValueJson> keyToJson(const rt::KvKey& key);

  // `valueToJson(value, ancestors = new Set())`. A KvValue is a tree, so the TypeError for a
  // cyclic value ("Profile JSON cannot represent cyclic values; use full runtime conversion")
  // cannot occur.
  KvValueJson valueToJson(const rt::KvValue& value);

  // `toValue(input: unknown): unknown`; nullptr is undefined. JavaScriptCore's TypeErrors for the
  // shapes the TS indexes without checking (`value.map` on a non-array, `detail.message` on
  // null, destructuring a non-iterable Map entry) carry Bun's text.
  rt::KvValue toValue(const json::value& input);
  rt::KvValue toValue(const json::value* input);

  // `toKey(json: readonly KvValueJson[]): KvKey`; the array overload takes unchecked parts as
  // `JSON.parse(line) as KvEntryJson` hands them over. A part whose value toValue cannot make a
  // key part (undefined, null, an array or an object behind a "string" or "boolean" tag) throws
  // Error "Unsupported keyPart: <typeof> <String(part)>", the SDK's error when the TS key is used.
  rt::KvKey toKey(const std::vector<KvValueJson>& json);
  rt::KvKey toKey(const json::array& json);

  KvEntryJson entryToJson(const rt::KvEntry<>& entry);

  /** This is the existing profile NDJSON contract, not an expiry-preserving database export. */
  // `exportEntries(kv: Kv, selector)`: the store must outlive the stream.
  rt::ReadableStream<rt::ByteChunk> exportEntries(rt::Kv& kv, rt::KvListSelector selector);

  // Boost.JSON conversions: value_from writes the members in declaration order; value_to is
  // strict (rt::TypeError). Profile lines are read with toKey / toValue, as the TS reads them.
  void tag_invoke(const json::value_from_tag&, json::value& out, const KvEntryJson& value);
  KvEntryJson tag_invoke(const json::value_to_tag<KvEntryJson>&, const json::value& value);
}
