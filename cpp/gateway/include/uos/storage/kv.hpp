#pragma once
// Port of src/storage/kv.ts (the `#kv` import): the facade every module opens the store through.
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/storage/key_compatibility.hpp"
#include "uos/storage/native_addon.hpp"
#include "uos/storage/value_compatibility.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace uos::storage::kv {
  // `export type { ... } from "@deno/kv"`: the store interface is rt's.
  using rt::AtomicCheck;
  using rt::AtomicOperation;
  using rt::Kv;
  // `KvCommitResult | KvCommitError`: rt::KvCommitResult carries both (`{ ok: false }` has an empty versionstamp).
  using KvCommitError = rt::KvCommitResult;
  using rt::KvCommitResult;
  using rt::KvConsistencyLevel;
  using rt::KvEntry;
  using rt::KvEntryMaybe;
  using rt::KvKey;
  using rt::KvKeyPart;
  using rt::KvListIterator;
  using rt::KvListOptions;
  using rt::KvListSelector;
  using rt::KvMutation;
  // `export { KvU64 } from "@deno/kv"`
  using rt::KvU64;
  using key_compatibility::assertPortableKvKeyBytes;
  using value_compatibility::assertPortableKvValue;
  using native_addon::prepareNativeKvBackend;
  using native_addon::verifyNativeKvArtifact;
  using native_addon::NativeKvArtifactBinding;
  using native_addon::VerifiedNativeKvArtifact;

  inline constexpr std::string_view BUN_KV_DATABASE_PATH = ".data/kv-bun.sqlite3";
  inline constexpr std::string_view BUN_KV_CODEC = "bun-jsc-v1";
  inline constexpr std::string_view BUN_KV_CODEC_TABLE = "uos_kv_metadata";

  // ---- `import { openKv as openNativeKv } from "@deno/kv"`, `import { deserialize } from "node:v8"` ----
  // Process-wide replaceable functions (a test installs its own). The default opener is the
  // SDK's local SQLite implementation, rt::SqliteKv (uos/rt/kv_sqlite.hpp, docs/rt/kv-durable.md),
  // opened directly rather than through the NapiInterface; the default reader is
  // rt::kvJscDeserialize, Bun's node:v8 `deserialize`.

  // The `{ implementation, napi, encodeV8, decodeV8 }` options of the SDK's openKv.
  struct NativeKvOpenOptions {
    std::string implementation;
    native_addon::NapiInterface napi;
    std::function<rt::KvBytes(const rt::KvValue& value)> encodeV8;
    std::function<rt::KvValue(std::span<const std::uint8_t> bytes)> decodeV8;
  };
  using NativeKvOpener = std::function<rt::Task<std::shared_ptr<rt::Kv>>(std::string path, NativeKvOpenOptions options)>;
  NativeKvOpener& openNativeKv();

  // node:v8 `deserialize(bytes)`: the stored value a serialized buffer holds.
  using V8Deserialize = std::function<rt::KvValue(std::span<const std::uint8_t> bytes)>;
  V8Deserialize& deserialize();

  /** Open only native SQLite databases whose values belong to the explicit Bun codec. */
  // `openKv(path = BUN_KV_DATABASE_PATH)`: the no-argument overload is the default path.
  rt::Task<std::shared_ptr<rt::Kv>> openKv();
  rt::Task<std::shared_ptr<rt::Kv>> openKv(std::string path);
}
