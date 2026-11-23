#include "uos/kv_conversion/convert.hpp"

#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/url.hpp"
#include "uos/storage/kv.hpp"

#include <array>
#include <exception>
#include <string_view>
#include <utility>

namespace uos::kv_conversion::convert {
  rt::Task<std::string> fileDigest(std::string path) {
    rt::crypto::Hash hash = rt::crypto::createHash("sha256");
    const rt::ReadableStream<std::vector<std::uint8_t>> stream = rt::bun::file(path).stream();
    const rt::ReadableStreamDefaultReader<std::vector<std::uint8_t>> reader = stream.getReader();
    for (;;) {
      const rt::ReadableStreamReadResult<std::vector<std::uint8_t>> chunk = co_await reader.read();
      if (chunk.done) break;
      hash.update(*chunk.value);
    }
    co_return hash.digest("hex");
  }

  namespace {
    using native_schema::NativeReader;
    using native_schema::NativeRevision;
    using native_schema::NativeSnapshotReader;
    using rt::record::Reader;
    using rt::record::Writer;
    namespace bun_sqlite = native_schema::bun_sqlite;

    void removeTree(const std::string& path) {
      rt::fs::RmOptions options;
      options.recursive = true;
      options.force = true;
      rt::fs::rmSync(path, options);
    }

    rt::Task<SnapshotIdentity> snapshotIdentity(std::string path) {
      const rt::fs::Stats stat = rt::fs::lstatSync(path);
      if (!stat.isFile()) throw rt::JsError("Source must be a regular SQLite file");
      const std::string wal = path + "-wal";
      if (rt::fs::existsSync(wal) && rt::fs::lstatSync(wal).size != 0) throw rt::JsError("Source must be stopped, drained and checkpointed; nonempty WAL is refused");
      if (rt::fs::existsSync(path + "-journal")) throw rt::JsError("Source has an unsettled rollback journal");
      rt::Task<std::string> digesting = fileDigest(path);
      std::string sha256 = co_await std::move(digesting);
      SnapshotIdentity identity{
        .sha256 = std::move(sha256),
        .size = std::to_string(stat.size),
        .device = std::to_string(stat.dev),
        .inode = std::to_string(stat.ino),
        .mtime_ns = std::to_string(stat.mtimeNs),
        .wal = rt::fs::existsSync(wal) ? "empty" : "absent",
        .drain_boundary = "checkpointed-no-wal",
      };
      co_return identity;
    }

    rt::Task<SnapshotIdentity> privateSnapshotIdentity(std::string path, std::optional<SnapshotIdentity> expected) {
      static constexpr std::array<std::string_view, 3> sidecars{"-wal", "-shm", "-journal"};
      for (const std::string_view suffix : sidecars) {
        if (rt::fs::existsSync(path + std::string(suffix))) throw rt::JsError("Exclusive conversion snapshot must not have SQLite sidecars");
      }
      rt::Task<SnapshotIdentity> identifying = snapshotIdentity(path);
      SnapshotIdentity identity = co_await std::move(identifying);
      if (expected && !(identity == *expected)) throw rt::JsError("Private conversion snapshot changed");
      co_return identity;
    }

    struct OpenedSnapshot {
      NativeSnapshotReader reader;
      NativeRevision revision;
    };

    rt::Task<OpenedSnapshot> openPrivateSnapshot(std::string path, SnapshotIdentity expected) {
      // URI is explicit on every platform; no normal WAL open may create snapshot sidecars.
      rt::Task<SnapshotIdentity> checking = privateSnapshotIdentity(path, std::move(expected));
      co_await std::move(checking);
      rt::URL uri = rt::pathToFileURL(path);
      uri.searchParams().set("immutable", "1");
      const rt::sqlite::Database database(uri.href(), rt::sqlite::constants::SQLITE_OPEN_READONLY | rt::sqlite::constants::SQLITE_OPEN_URI);
      NativeSnapshotReader reader = native_schema::createNativeSnapshotReader(bun_sqlite::adapt(database));
      std::optional<NativeRevision> revision;
      try {
        revision = native_schema::verifyNativeSchema(reader, storage::kv::BUN_KV_CODEC);
      } catch (...) {
        reader.close();
        throw;
      }
      OpenedSnapshot opened{.reader = std::move(reader), .revision = *revision};
      co_return opened;
    }

  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SnapshotIdentity& value) {
    out = Writer()
            .required("sha256", value.sha256)
            .required("size", value.size)
            .required("device", value.device)
            .required("inode", value.inode)
            .required("mtime_ns", value.mtime_ns)
            .required("wal", value.wal)
            .required("drain_boundary", value.drain_boundary)
            .done();
  }

  SnapshotIdentity tag_invoke(const json::value_to_tag<SnapshotIdentity>&, const json::value& value) {
    const Reader in(value, "SnapshotIdentity");
    return {
      .sha256 = in.required<std::string>("sha256"),
      .size = in.required<std::string>("size"),
      .device = in.required<std::string>("device"),
      .inode = in.required<std::string>("inode"),
      .mtime_ns = in.required<std::string>("mtime_ns"),
      .wal = in.required<std::string>("wal"),
      .drain_boundary = in.required<std::string>("drain_boundary"),
    };
  }

  namespace detail {
    rt::Task<SnapshotIdentity> withPrivateConsistentBunSnapshot(std::string sourcePath, std::string stagingParent, SnapshotRead read) {
      const std::string source = rt::path::resolve(sourcePath);
      rt::Task<SnapshotIdentity> identifying = snapshotIdentity(source);
      const SnapshotIdentity sourceIdentity = co_await std::move(identifying);
      const std::string staging = rt::fs::mkdtempSync(rt::path::join(rt::path::resolve(stagingParent), ".uos-kv-capture-"));
      std::optional<NativeSnapshotReader> reader;
      rt::Reason failure;
      try {
        rt::fs::chmodSync(staging, 0700);
        const std::string snapshot = rt::path::join(staging, "source.sqlite3");
        rt::fs::copyFileSync(source, snapshot, rt::fs::constants::COPYFILE_EXCL);
        rt::fs::chmodSync(snapshot, 0600);
        rt::Task<SnapshotIdentity> copying = privateSnapshotIdentity(snapshot, std::nullopt);
        const SnapshotIdentity identity = co_await std::move(copying);
        bool changed = identity.sha256 != sourceIdentity.sha256;
        if (!changed) {
          rt::Task<SnapshotIdentity> rereading = snapshotIdentity(source);
          const SnapshotIdentity current = co_await std::move(rereading);
          changed = !(current == sourceIdentity);
        }
        if (changed) throw rt::JsError("Source changed during snapshot");
        rt::Task<OpenedSnapshot> opening = openPrivateSnapshot(snapshot, identity);
        const OpenedSnapshot opened = co_await std::move(opening);
        reader = opened.reader;
        const NativeReader view = *reader;
        rt::Task<void> reading = read(view, opened.revision);
        co_await std::move(reading);
        reader->close();
        reader.reset();
        rt::Task<SnapshotIdentity> verifying = privateSnapshotIdentity(snapshot, identity);
        co_await std::move(verifying);
        rt::Task<SnapshotIdentity> finalIdentity = snapshotIdentity(source);
        const SnapshotIdentity after = co_await std::move(finalIdentity);
        if (!(after == sourceIdentity)) throw rt::JsError("Source changed during snapshot read");
      } catch (...) {
        failure = std::current_exception();
      }
      if (reader) reader->close();
      removeTree(staging);
      if (failure) std::rethrow_exception(failure);
      co_return sourceIdentity;
    }
  }

}
