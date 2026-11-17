#pragma once
// Port of src/kv-conversion/convert.ts.
#include "uos/kv_conversion/cursors.hpp"
#include "uos/kv_conversion/legacy_codec.hpp"
#include "uos/kv_conversion/native_schema.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace uos::kv_conversion::convert {
  struct ConvertOptions {
    cursors::ConversionDirection direction;
    std::string source;
    std::string destination;
    std::string receipt;
    std::string deno;
    std::string denoSha256;
    std::optional<rt::AbortSignal> signal{};
  };

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

  // The receipt convertKvDatabase writes beside the destination and returns.
  struct ConversionReceipt {
    struct Source {
      std::string path;
      std::string codec;
      SnapshotIdentity file;
      SnapshotIdentity snapshot;
      double row_count;
      std::string content_sha256;
    };
    struct Destination {
      struct File {
        std::string sha256;
        std::string size;
      };
      std::string path;
      std::string codec;
      File file;
      double row_count;
      std::string content_sha256;
    };
    struct SchemaMetadata {
      struct MigrationState {
        std::string k;
        std::string version;
      };
      struct DataVersion {
        std::string k;
        std::string version;
        std::string seq;
      };
      // The `{ type, name, sql }` rows of `SELECT type,name,sql FROM sqlite_master ORDER BY name`.
      json::array source;
      MigrationState migration_state;
      DataVersion data_version;
      std::string user_version;
      std::string application_id;
    };
    struct LastNativeRevision {
      std::string version;
      std::string seq;
    };
    // "uos-kv-runtime-conversion-v1"
    std::string schema;
    cursors::ConversionDirection direction;
    Source source;
    Destination destination;
    double native_schema_version;
    SchemaMetadata schema_metadata;
    LastNativeRevision last_native_revision;
    std::string absolute_expiry_sha256;
    double transcoded_cursors;
    legacy_codec::LegacyCodec::Identity deno;
    std::string bun_version;
    std::string completed_at;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt::Source& value);
  ConversionReceipt::Source tag_invoke(const json::value_to_tag<ConversionReceipt::Source>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt::Destination::File& value);
  ConversionReceipt::Destination::File tag_invoke(const json::value_to_tag<ConversionReceipt::Destination::File>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt::Destination& value);
  ConversionReceipt::Destination tag_invoke(const json::value_to_tag<ConversionReceipt::Destination>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt::SchemaMetadata::MigrationState& value);
  ConversionReceipt::SchemaMetadata::MigrationState tag_invoke(const json::value_to_tag<ConversionReceipt::SchemaMetadata::MigrationState>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt::SchemaMetadata::DataVersion& value);
  ConversionReceipt::SchemaMetadata::DataVersion tag_invoke(const json::value_to_tag<ConversionReceipt::SchemaMetadata::DataVersion>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt::SchemaMetadata& value);
  ConversionReceipt::SchemaMetadata tag_invoke(const json::value_to_tag<ConversionReceipt::SchemaMetadata>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt::LastNativeRevision& value);
  ConversionReceipt::LastNativeRevision tag_invoke(const json::value_to_tag<ConversionReceipt::LastNativeRevision>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ConversionReceipt& value);
  ConversionReceipt tag_invoke(const json::value_to_tag<ConversionReceipt>&, const json::value& value);

  /** Convert a drained full database, never a filtered profile export or a writable live handle. */
  // Bind the Task before awaiting it (GCC 12 rule for aggregate arguments, docs/rt/async.md).
  [[nodiscard]] rt::Task<ConversionReceipt> convertKvDatabase(ConvertOptions options);
}
