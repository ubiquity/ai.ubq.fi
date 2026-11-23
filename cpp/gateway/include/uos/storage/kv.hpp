#pragma once
// Port of src/storage/kv.ts (the `#kv` import): the facade every module opens the store through.
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/storage/key_compatibility.hpp"
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

  inline constexpr std::string_view BUN_KV_DATABASE_PATH = ".data/kv-bun.sqlite3";
  inline constexpr std::string_view BUN_KV_CODEC = "bun-jsc-v1";
  inline constexpr std::string_view BUN_KV_CODEC_TABLE = "uos_kv_metadata";

  // The tests can replace the linked SQLite opener or value decoder.
  struct NativeKvOpenOptions {
    std::string implementation;
    std::function<rt::KvBytes(const rt::KvValue& value)> encodeV8;
    std::function<rt::KvValue(std::span<const std::uint8_t> bytes)> decodeV8;
  };
  using NativeKvOpener = std::function<rt::Task<std::shared_ptr<rt::Kv>>(std::string path, NativeKvOpenOptions options)>;
  NativeKvOpener& openNativeKv();

  // node:v8 `deserialize(bytes)`: the stored value a serialized buffer holds.
  using V8Deserialize = std::function<rt::KvValue(std::span<const std::uint8_t> bytes)>;
  V8Deserialize& deserialize();

  /** Open only native SQLite databases whose values belong to the explicit Bun codec. */
  rt::Task<std::shared_ptr<rt::Kv>> openKv(std::string path);
}
