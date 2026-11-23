#pragma once
// Port of src/kv-conversion/convert.ts.
#include "uos/kv_conversion/native_schema.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace uos::kv_conversion::convert {
  rt::Task<std::string> fileDigest(std::string path);

  // `Awaited<ReturnType<typeof snapshotIdentity>>`: decimal strings of the bigint lstat fields.
  struct SnapshotIdentity {
    std::string sha256;
    std::string size;
    std::string device;
    std::string inode;
    std::string mtime_ns;
    // "empty" | "absent"
    std::string wal;
    // "checkpointed-no-wal"
    std::string drain_boundary;
    friend bool operator==(const SnapshotIdentity&, const SnapshotIdentity&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const SnapshotIdentity& value);
  SnapshotIdentity tag_invoke(const json::value_to_tag<SnapshotIdentity>&, const json::value& value);

  // `{ value: T; sourceIdentity }`
  template <class T> struct PrivateConsistentBunSnapshot {
    T value;
    SnapshotIdentity sourceIdentity;
  };

  namespace detail {
    // The callback with its result type erased: it stores the value where the caller reads it.
    using SnapshotRead = std::function<rt::Task<void>(native_schema::NativeReader reader, native_schema::NativeRevision revision)>;
    // withPrivateConsistentBunSnapshot without the value: the source identity.
    [[nodiscard]] rt::Task<SnapshotIdentity> withPrivateConsistentBunSnapshot(std::string sourcePath, std::string stagingParent, SnapshotRead read);
  }

  /** Read a private consistent codec-marked copy, not proof of a stopped owner.
   * The caller must independently establish quiescence before state adoption.
   * Hash/checkpoint guards detect mutation; they do not establish process stoppage.
   * The callback cannot retain a live reader after this finite operation returns.
   */
  // `read: (reader, revision) => T | Promise<T>` returns an rt::Task<T> (a synchronous TS
  // callback is a coroutine that does not suspend; one returning undefined returns
  // std::monostate). The reader it receives is the snapshot reader without `close`.
  template <class T>
  [[nodiscard]] rt::Task<PrivateConsistentBunSnapshot<T>> withPrivateConsistentBunSnapshot(
    std::string sourcePath, std::string stagingParent, std::function<rt::Task<T>(native_schema::NativeReader reader, native_schema::NativeRevision revision)> read) {
    auto value = std::make_shared<std::optional<T>>();
    detail::SnapshotRead erased = [read = std::move(read), value](native_schema::NativeReader reader, native_schema::NativeRevision revision) -> rt::Task<void> {
      rt::Task<T> reading = read(std::move(reader), revision);
      T result = co_await std::move(reading);
      value->emplace(std::move(result));
    };
    rt::Task<SnapshotIdentity> running = detail::withPrivateConsistentBunSnapshot(std::move(sourcePath), std::move(stagingParent), std::move(erased));
    SnapshotIdentity sourceIdentity = co_await std::move(running);
    PrivateConsistentBunSnapshot<T> result{std::move(**value), std::move(sourceIdentity)};
    co_return result;
  }

}
