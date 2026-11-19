// Interoperability of the durable store (rt::SqliteKv behind src/storage/kv.ts's openKv) with
// the TypeScript store (the repository's #kv opener over the pinned native-kv addon, denokv
// 9fe65cc + the rollback patch, under bun 1.4.0). The TypeScript side ran on throwaway
// databases; what it wrote and read is recorded in kv_durable_interop_vectors.inc, so these
// cases are hermetic:
//   - a database the TS store wrote (every reader vector's value, keys of every part type,
//     KvU64 and byte rows, arithmetic, expiry, deletes, queue rows) reads back in C++ as the TS
//     store reads it, and the C++ open sweeps its row once the recorded expiry has passed;
//   - a database the C++ store writes is, byte for byte where it matters, the one the TS store
//     read back (sha256 of the TS store's dump);
//   - two C++ processes alternate and contend on one file through the shared cursor and the
//     queue (the dev-time run did the same with a TS and a C++ process, docs/rt/kv-durable.md).
// The "interop child" cases are the C++ halves of those dev-time runs (no-ops unless
// UOS_KV_INTEROP_CHILD names them); the TS halves are the scripts kv-durable.md describes.
// Links: tests/kv_durable_interop_test.cpp src/storage/kv.cpp src/storage/native_kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/storage/key_compatibility.cpp
//
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/kv_sqlite.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#if defined(__APPLE__)
#include "uos/rt/url.hpp"
#endif
#include "uos/storage/kv.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace uos;
using namespace uos::rt;

namespace {
#if defined(__APPLE__)
  std::string darwinSnapshotFile(const std::string& file) {
    // Stopped snapshots need Apple's immutable readonly form; live WAL reads
    // must keep their ordinary connection and observe the sidecars.
    if (!fs::existsSync(file + "-wal") && !fs::existsSync(file + "-shm") && !fs::existsSync(file + "-journal"))
      return pathToFileURL(file).href() + "?immutable=1";
    return file;
  }
#endif
  constexpr double INF = std::numeric_limits<double>::infinity();
  const double NAN_ = std::numeric_limits<double>::quiet_NaN();

  struct SerializedVector {
    std::string name;
    std::string hex;
    KvValue expected;
  };

#include "rt/kv_jsc_deserialize_vectors.inc"

  struct RecordedKvRow {
    const char* k;
    int vector;
    const char* v;
    int encoding;
    std::int64_t version;
    std::int64_t seq;
    std::int64_t expirationMs;
  };

  struct RecordedQueueRow {
    std::int64_t ts;
    const char* id;
    const char* data;
    const char* backoffSchedule;
    const char* keysIfUndelivered;
  };

#include "kv_durable_interop_vectors.inc"

  std::string hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : bytes) {
      out += digits[byte >> 4];
      out += digits[byte & 15];
    }
    return out;
  }

  std::string num(double value) { return value == 0 && std::signbit(value) ? "-0" : numberToString(value); }

  // The interop scripts' `describe(value)`.
  std::string describe(const KvValue& value) {
    return std::visit(
      [&](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, KvUndefined>) return "u";
        else if constexpr (std::is_same_v<T, std::nullptr_t>) return "n";
        else if constexpr (std::is_same_v<T, bool>) return item ? "t" : "f";
        else if constexpr (std::is_same_v<T, double>) return "d:" + num(item);
        else if constexpr (std::is_same_v<T, std::string>) return "s:" + jsonQuote(item);
        else if constexpr (std::is_same_v<T, BigInt>) return "b:" + item.toString();
        else if constexpr (std::is_same_v<T, KvU64>) return "u64:" + item.toBigInt().toString();
        else if constexpr (std::is_same_v<T, KvBytes>) return "u8:" + hex(item);
        else if constexpr (std::is_same_v<T, KvArrayBuffer>) return "ab:" + hex(item.bytes);
        else if constexpr (std::is_same_v<T, KvArrayBufferView>) {
          if (item.kind == KvViewKind::Uint8Array && item.byteOffset == 0 && item.byteLength == item.buffer.size()) return "u8:" + hex(item.buffer);
          return "view:" + std::string(kvViewKindName(item.kind)) + ":" + std::to_string(item.byteOffset) + ":" + std::to_string(item.byteLength) + ":" + hex(item.buffer);
        } else if constexpr (std::is_same_v<T, KvDate>) return "date:" + num(item.time);
        else if constexpr (std::is_same_v<T, KvRegExp>) {
          std::string source = item.source;
          if (item.flags.find('v') == std::string::npos) source = Regex(item.source, item.flags).source();
          return "re:" + jsonQuote(source) + "/" + item.flags;
        } else if constexpr (std::is_same_v<T, KvError>) {
          const auto optional = [](const std::optional<std::string>& text) { return text ? jsonQuote(*text) : std::string("-"); };
          return "err:" + item.name + ":" + optional(item.message) + ":" + std::to_string(item.line) + ":" + std::to_string(item.column) + ":" + optional(item.sourceURL) + ":" +
                 optional(item.stack);
        } else if constexpr (std::is_same_v<T, KvArray>) {
          std::string out = "[";
          for (std::size_t index = 0; index < item.size(); ++index) out += (index ? "," : "") + (item[index].template is<KvHole>() ? std::string("_") : describe(item[index]));
          return out + "]";
        } else if constexpr (std::is_same_v<T, KvMap>) {
          std::string out = "Map{";
          bool first = true;
          for (const auto& [key, entry] : item) {
            out += (first ? "" : ",") + describe(key) + "=>" + describe(entry);
            first = false;
          }
          return out + "}";
        } else if constexpr (std::is_same_v<T, KvSet>) {
          std::string out = "Set{";
          bool first = true;
          for (const KvValue& member : item) {
            out += (first ? "" : ",") + describe(member);
            first = false;
          }
          return out + "}";
        } else if constexpr (std::is_same_v<T, KvObject>) {
          std::string out = "{";
          bool first = true;
          for (const auto& [key, member] : item) {
            out += (first ? "" : ",") + jsonQuote(key) + ":" + describe(member);
            first = false;
          }
          return out + "}";
        } else {
          return "_";
        }
      },
      value.variant());
  }

  std::string showColumn(const sqlite::Value& value) {
    if (std::holds_alternative<std::nullptr_t>(value)) return "null";
    if (const auto* integer = std::get_if<std::int64_t>(&value)) return std::to_string(*integer);
    if (const auto* real = std::get_if<double>(&value)) return numberToString(*real);
    if (const auto* text = std::get_if<std::string>(&value)) return jsonQuote(*text);
    return "x" + hex(std::get<sqlite::Bytes>(value));
  }

  // The dump script's raw rows.
  std::vector<std::string> rawRows(const std::string& file) {
    sqlite::DatabaseOptions options;
    options.readonly = true;
    options.safeIntegers = true;
#if defined(__APPLE__)
    const sqlite::Database database(darwinSnapshotFile(file), options);
#else
    const sqlite::Database database(file, options);
#endif
    std::vector<std::string> lines;
    for (const char* table : {"data_version", "migration_state", "uos_kv_metadata", "kv", "queue", "queue_running"}) {
      for (const sqlite::Row& row : database.query(std::string("SELECT * FROM ") + table).all()) {
        std::string line = std::string("row ") + table;
        for (const auto& [name, value] : row) line += " " + name + "=" + showColumn(value);
        lines.push_back(line);
      }
    }
    database.close();
    return lines;
  }

  // The parts writeExtras keys rows with.
  std::vector<KvKeyPart> keyParts() {
    std::vector<KvKeyPart> parts;
    parts.emplace_back(KvBytes{0});
    parts.emplace_back(KvBytes{});
    parts.emplace_back("");
    parts.emplace_back("a");
    parts.emplace_back("\xc3\xa9");
    parts.emplace_back("\xf0\x9f\x98\x80");
    parts.emplace_back(-0.0);
    parts.emplace_back(0.0);
    parts.emplace_back(1.5);
    parts.emplace_back(-INF);
    parts.emplace_back(INF);
    parts.emplace_back(NAN_);
    parts.emplace_back(BigInt(0));
    parts.emplace_back(BigInt(-1));
    parts.emplace_back(BigInt(2).pow(BigInt(70)));
    parts.emplace_back(false);
    parts.emplace_back(true);
    return parts;
  }

  Task<void> commitArithmetic(std::shared_ptr<Kv> kv, std::string name, KvMutationType type, std::int64_t operand) {
    std::unique_ptr<AtomicOperation> operation = kv->atomic();
    KvKey key{name};
    if (type == KvMutationType::sum) operation->sum(key, operand);
    else if (type == KvMutationType::min) operation->min(key, operand);
    else operation->max(key, operand);
    const KvCommitResult result = co_await operation->commit();
    if (!result.ok) throw JsError("arithmetic commit failed");
  }

  // extras.ts writeExtras.
  Task<void> writeExtras(std::shared_ptr<Kv> kv) {
    const std::vector<KvKeyPart> parts = keyParts();
    for (std::size_t index = 0; index < parts.size(); ++index) co_await kv->set(KvKey{"k", parts[index]}, KvValue(static_cast<double>(index)));
    co_await kv->set(KvKey{"u64", 1}, KvValue(KvU64(std::numeric_limits<std::uint64_t>::max())));
    struct Arithmetic {
      const char* key;
      KvMutationType type;
      std::int64_t operand;
    };
    const std::vector<Arithmetic> arithmetic = {{"sum", KvMutationType::sum, 5}, {"sum", KvMutationType::sum, 5}, {"min", KvMutationType::min, 7},
                                                {"min", KvMutationType::min, 3}, {"max", KvMutationType::max, 7}, {"max", KvMutationType::max, 9}};
    for (const Arithmetic& step : arithmetic) {
      std::string key = step.key;
      auto committing = commitArithmetic(kv, std::move(key), step.type, step.operand);
      co_await std::move(committing);
    }
    const KvValue plainBytes(KvBytes{1, 2, 3});
    co_await kv->set(KvKey{"bytes", 1}, plainBytes);
    const KvValue viewBytes(KvArrayBufferView{KvViewKind::Uint8Array, KvBytes{0, 1, 2, 3, 4}, 1, 3});
    co_await kv->set(KvKey{"bytes", 2}, viewBytes);
    co_await kv->set(KvKey{"exp"}, KvValue(std::string("soon")), {.expireIn = 3'600'000});
    co_await kv->set(KvKey{"gone"}, KvValue(std::string("x")));
    co_await kv->delete_(KvKey{"gone"});
    auto* queue = dynamic_cast<KvEnqueue*>(kv.get());
    if (!queue) throw JsError("the store has no queue");
    KvEnqueueOptions delayed;
    delayed.delay = 1000;
    std::vector<KvKey> dead;
    dead.push_back(KvKey{"dead", 1});
    delayed.keysIfUndelivered = dead;
    const KvValue job = KvValue::object({{"job", 1}});
    auto first = queue->enqueue(job, delayed);
    co_await std::move(first);
    const KvEnqueueOptions plain;
    auto second = queue->enqueue(KvValue(std::string("plain")), plain);
    co_await std::move(second);
    std::unique_ptr<AtomicOperation> pair = kv->atomic();
    pair->check(AtomicCheck{KvKey{"pair", "a"}, std::nullopt}).set(KvKey{"pair", "a"}, KvValue(1)).set(KvKey{"pair", "b"}, KvValue(2));
    const KvCommitResult paired = co_await pair->commit();
    if (!paired.ok) throw JsError("pair commit failed");
  }

  }

// Dev-time child: what the C++ store (openKv) reads from a database, then its raw rows.
ASYNC_TEST("interop child: dump") {
  if (env::get("UOS_KV_INTEROP_CHILD") != std::optional<std::string>("dump")) co_return;
  const std::string file = *env::get("UOS_KV_INTEROP_PATH");
  std::shared_ptr<Kv> kv = co_await storage::kv::openKv(file);
  KvListSelector all;
  all.prefix = KvKey{};
  auto iterator = kv->list(all);
  while (std::optional<KvEntry<>> entry = co_await iterator->next()) std::printf("entry %s %s %s\n", hex(packKey(entry->key)).c_str(), entry->versionstamp.c_str(), describe(entry->value).c_str());
  kv->close();
  for (const std::string& line : rawRows(file)) std::printf("%s\n", line.c_str());
}

// Dev-time child: the interop corpus written through the C++ store (openKv).
ASYNC_TEST("interop child: write") {
  if (env::get("UOS_KV_INTEROP_CHILD") != std::optional<std::string>("write")) co_return;
  const std::string file = *env::get("UOS_KV_INTEROP_PATH");
  std::shared_ptr<Kv> kv = co_await storage::kv::openKv(file);
  const std::vector<SerializedVector> vectors = serializedVectors();
  for (std::size_t index = 0; index < vectors.size(); ++index) co_await kv->set(KvKey{"v", static_cast<double>(index)}, vectors[index].expected);
  co_await writeExtras(kv);
  kv->close();
  std::printf("wrote %zu corpus values\n", vectors.size());
}

namespace {
  // alternate.ts: the C++ half of the mixed-process alternation.
  Task<std::string> alternate(std::string file, std::string me, std::string other, int rounds, int contended) {
    std::shared_ptr<Kv> kv = co_await storage::kv::openKv(file);
    std::vector<std::string> seen;
    for (int round = 0; round < rounds; round += 1) {
      for (;;) {
        const KvEntryMaybe<> turn = co_await kv->get(KvKey{"turn"});
        if (turn.value.isString() && turn.value.asString() == me) break;
        co_await rt::sleep(2.0);
      }
      for (;;) {
        const KvEntryMaybe<> counter = co_await kv->get(KvKey{"counter"});
        const double next = (counter.value.isNumber() ? counter.value.asNumber() : 0) + 1;
        const KvValue record = KvValue::object({{"by", me}, {"round", round}});
        std::unique_ptr<AtomicOperation> operation = kv->atomic();
        operation->check(counter).set(KvKey{"counter"}, KvValue(next)).set(KvKey{"log", next}, record);
        const KvCommitResult result = co_await operation->commit();
        if (result.ok) break;
      }
      const KvEntryMaybe<> saved = co_await kv->get(KvKey{"cursor"});
      KvListSelector selector;
      selector.prefix = KvKey{"log"};
      KvListOptions options;
      if (saved.value.isString()) options.cursor = saved.value.asString();
      std::unique_ptr<KvListIterator> iterator = kv->list(selector, options);
      std::optional<std::string> last = options.cursor;
      while (std::optional<KvEntry<>> entry = co_await iterator->next()) {
        seen.push_back(numberToString(entry->key[1].number()) + ":" + entry->value["by"].asString() + ":" + numberToString(entry->value["round"].asNumber()));
        last = iterator->cursor();
      }
      auto* queue = dynamic_cast<KvEnqueue*>(kv.get());
      if (!queue) throw JsError("the store has no queue");
      KvEnqueueOptions delayed;
      delayed.delay = 60'000;
      const KvValue message = KvValue::object({{"by", me}, {"round", round}});
      auto enqueuing = queue->enqueue(message, delayed);
      co_await std::move(enqueuing);
      const KvEntryMaybe<> turn = co_await kv->get(KvKey{"turn"});
      const KvValue cursor = last ? KvValue(*last) : KvValue();
      std::unique_ptr<AtomicOperation> pass = kv->atomic();
      pass->check(turn).set(KvKey{"cursor"}, cursor).set(KvKey{"turn"}, KvValue(other));
      const KvCommitResult passed = co_await pass->commit();
      if (!passed.ok) throw JsError("turn changed under its owner");
    }
    for (int n = 0; n < contended; n += 1) {
      for (int attempt = 0;; attempt += 1) {
        const KvEntryMaybe<> entry = co_await kv->get(KvKey{"contended"});
        const double next = (entry.value.isNumber() ? entry.value.asNumber() : 0) + 1;
        std::unique_ptr<AtomicOperation> operation = kv->atomic();
        operation->check(entry).set(KvKey{"contended"}, KvValue(next));
        const KvCommitResult result = co_await operation->commit();
        if (result.ok) break;
        if (attempt > 1000) throw JsError("contention did not settle");
      }
    }
    kv->close();
    co_return me + " seen " + join(seen, " ");
  }
}

// Dev-time child: the C++ half of alternate.ts, on the real clock.
TEST("interop child: alternate") {
  if (env::get("UOS_KV_INTEROP_CHILD") != std::optional<std::string>("alternate")) return;
  const std::string summary = blockOn(alternate(*env::get("UOS_KV_INTEROP_PATH"), env::get("UOS_KV_INTEROP_ME").value_or("cpp"),
                                                env::get("UOS_KV_INTEROP_OTHER").value_or("ts"), std::stoi(*env::get("UOS_KV_INTEROP_ROUNDS")),
                                                std::stoi(*env::get("UOS_KV_INTEROP_CONTENDED"))));
  std::printf("%s\n", summary.c_str());
}

// Dev-time child: a row expiring after UOS_KV_INTEROP_TTL ms beside a durable one (expiry.ts write).
TEST("interop child: ttl") {
  if (env::get("UOS_KV_INTEROP_CHILD") != std::optional<std::string>("ttl")) return;
  const double ttl = std::stod(*env::get("UOS_KV_INTEROP_TTL"));
  const std::string file = *env::get("UOS_KV_INTEROP_PATH");
  blockOn(async([file, ttl]() -> Task<void> {
    std::shared_ptr<Kv> kv = co_await storage::kv::openKv(file);
    const KvSetOptions options{.expireIn = ttl};
    co_await kv->set(KvKey{"ttl"}, KvValue(std::string("short")), options);
    co_await kv->set(KvKey{"keep"}, KvValue(std::string("kept")));
    kv->close();
  }));
  std::printf("written at %s\n", numberToString(dateNow()).c_str());
}

// ---- Hermetic cases ---------------------------------------------------------------------------

namespace {
  KvBytes unhex(std::string_view text) {
    KvBytes out;
    for (std::size_t index = 0; index + 1 < text.size(); index += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(std::string(text.substr(index, 2)), nullptr, 16)));
    return out;
  }

  std::string sha256(std::string_view text) { return crypto::createHash("sha256").update(text).digest("hex"); }

  struct Directory {
    std::string path = fs::realpathSync(fs::mkdtempSync(path::join(os::tmpdir(), "uos-kv-interop-")));
    ~Directory() {
      fs::RmOptions options;
      options.recursive = true;
      options.force = true;
      fs::rmSync(path, options);
    }
    Directory() = default;
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
  };


  // What the dump script prints: every entry openKv lists, then the raw rows.
  Task<std::vector<std::string>> dumpLines(std::string file) {
    std::vector<std::string> lines;
    std::shared_ptr<Kv> kv = co_await storage::kv::openKv(file);
    KvListSelector all;
    all.prefix = KvKey{};
    std::unique_ptr<KvListIterator> iterator = kv->list(all);
    while (std::optional<KvEntry<>> entry = co_await iterator->next()) lines.push_back("entry " + hex(packKey(entry->key)) + " " + entry->versionstamp + " " + describe(entry->value));
    kv->close();
    for (const std::string& line : rawRows(file)) lines.push_back(line);
    co_return lines;
  }

  // The recording's normalization: versions, the data version, queue ids and times, expiry times.
  std::string normalized(const std::vector<std::string>& lines) {
    static const Regex VERSION(" version=\\d+");
    static const Regex EXPIRY(" expiration_ms=[1-9]\\d*");
    static const Regex QUEUE("^row queue ts=\\d+ id=\"[^\"]*\"");
    std::string out;
    for (const std::string& line : lines) {
      if (startsWith(line, "row data_version ")) continue;
      if (startsWith(line, "entry ")) {
        const std::vector<std::string> parts = split(line, " ");
        std::string rest;
        for (std::size_t index = 3; index < parts.size(); ++index) rest += (index > 3 ? " " : "") + parts[index];
        out += parts[0] + " " + parts[1] + " " + rest + "\n";
        continue;
      }
      std::string normal = line;
      if (startsWith(normal, "row kv ")) normal = replace(replace(normal, VERSION, ""), EXPIRY, " expiration_ms=T");
      if (startsWith(normal, "row queue ")) normal = replace(normal, QUEUE, "row queue ts=T id=ID");
      out += normal + "\n";
    }
    return out;
  }

  // The value each recorded row holds, independently of its bytes.
  KvValue expectedValue(const RecordedKvRow& row, const std::vector<SerializedVector>& vectors) {
    const KvKey key = unpackKey(unhex(row.k));
    if (key[0].string() == "v") {
      const auto index = static_cast<std::size_t>(key[1].number());
      if (row.encoding == 3) return KvValue(unhex(row.v));
      return vectors.at(index).expected;
    }
    const std::string& kind = key[0].string();
    if (kind == "k") {
      const std::vector<KvKeyPart> parts = keyParts();
      for (std::size_t index = 0; index < parts.size(); ++index)
        if (packKeyPart(parts[index]) == packKeyPart(key[1])) return KvValue(static_cast<double>(index));
    }
    if (kind == "u64") return KvValue(KvU64(std::numeric_limits<std::uint64_t>::max()));
    if (kind == "sum") return KvValue(KvU64(10));
    if (kind == "min") return KvValue(KvU64(3));
    if (kind == "max") return KvValue(KvU64(9));
    if (kind == "bytes") return KvValue(KvBytes{1, 2, 3});
    if (kind == "exp") return KvValue(std::string("soon"));
    if (kind == "pair") return KvValue(key[1].string() == "a" ? 1.0 : 2.0);
    throw std::logic_error("no expectation for the recorded key " + show(key));
  }

  // How this binary was started (the SDK loader, its arguments and the binary).
  std::vector<std::string> selfCommand() {
#if defined(__APPLE__)
    return {uos::rt::process::execPath()};
#else
    const std::vector<std::uint8_t> bytes = fs::readFileSync("/proc/self/cmdline");
    std::vector<std::string> argv;
    std::string current;
    for (const std::uint8_t byte : bytes) {
      if (byte == 0) {
        argv.push_back(current);
        current.clear();
      } else {
        current.push_back(static_cast<char>(byte));
      }
    }
    const bool viaLoader = !argv.empty() && includes(argv[0], "ld-linux");
    argv.resize(std::min(argv.size(), viaLoader ? std::size_t{4} : std::size_t{1}));
    return argv;
#endif
  }
}

ASYNC_TEST("a database the TypeScript store wrote reads back in C++ as TypeScript reads it") {
  const Directory directory;
  const std::string file = directory.path + "/store.sqlite3";
  // An empty marked store (what openKv creates), then the recorded rows in place of its own.
  std::shared_ptr<Kv> created = co_await storage::kv::openKv(file);
  created->close();
  const std::vector<SerializedVector> vectors = serializedVectors();
  {
    sqlite::DatabaseOptions options;
    options.safeIntegers = true;
    const sqlite::Database database(file, options);
    database.run("DELETE FROM data_version");
    database.run("INSERT INTO data_version (k, version, seq) VALUES (0, ?, 0)", RECORDED_DATA_VERSION);
    const sqlite::Statement insertKv = database.query("INSERT INTO kv (k, v, v_encoding, version, seq, expiration_ms) VALUES (?, ?, ?, ?, ?, ?)");
    for (const RecordedKvRow& row : recordedKvRows()) {
      const KvBytes value = row.vector >= 0 ? unhex(vectors.at(static_cast<std::size_t>(row.vector)).hex) : unhex(row.v);
      insertKv.run(unhex(row.k), value, std::int64_t{row.encoding}, row.version, row.seq, row.expirationMs);
    }
    const sqlite::Statement insertQueue = database.query("INSERT INTO queue (ts, id, data, backoff_schedule, keys_if_undelivered) VALUES (?, ?, ?, ?, ?)");
    for (const RecordedQueueRow& row : recordedQueueRows()) insertQueue.run(row.ts, row.id, unhex(row.data), row.backoffSchedule, row.keysIfUndelivered);
    database.close();
  }
  std::int64_t expiry = 0;
  for (const RecordedKvRow& row : recordedKvRows())
    if (row.expirationMs > 0) expiry = row.expirationMs;
  ASSERT(expiry > 0);

  // Before the recorded expiry: every row, its value and its versionstamp.
  {
    SqliteKvOptions options;
    options.now = [expiry] { return static_cast<double>(expiry - 1000); };
    SqliteKv kv(file, options);
    KvListSelector all;
    all.prefix = KvKey{};
    std::unique_ptr<KvListIterator> iterator = kv.list(all);
    const std::vector<KvEntry<>> entries = co_await kvListToArray(*iterator);
    EXPECT_EQ(entries.size(), recordedKvRows().size());
    std::size_t matched = 0;
    for (const RecordedKvRow& row : recordedKvRows()) {
      const KvBytes key = unhex(row.k);
      for (const KvEntry<>& entry : entries) {
        if (packKey(entry.key) != key) continue;
        const KvValue expected = expectedValue(row, vectors);
        if (!(entry.value == expected)) EXPECT_EQ(show(entry.value), show(expected));
        EXPECT_EQ(entry.versionstamp, formatVersionstamp(static_cast<std::uint64_t>(row.version)));
        ++matched;
      }
    }
    EXPECT_EQ(matched, recordedKvRows().size());
    kv.close();
  }
  // The C++ store leaves the queue as the TS store wrote it.
  {
    sqlite::DatabaseOptions options;
    options.readonly = true;
    options.safeIntegers = true;
#if defined(__APPLE__)
    const sqlite::Database database(darwinSnapshotFile(file), options);
#else
    const sqlite::Database database(file, options);
#endif
    const std::vector<sqlite::Row> queue = database.query("SELECT ts, id, data, backoff_schedule, keys_if_undelivered FROM queue ORDER BY rowid").all();
    ASSERT_EQ(queue.size(), recordedQueueRows().size());
    for (std::size_t index = 0; index < queue.size(); ++index) {
      const RecordedQueueRow& recorded = recordedQueueRows()[index];
      EXPECT_EQ(*sqlite::getBigInt(queue[index].get(std::string_view("ts"))), recorded.ts);
      EXPECT_EQ(*sqlite::getString(queue[index].get(std::string_view("id"))), std::string(recorded.id));
      EXPECT(*sqlite::getBytes(queue[index].get(std::string_view("data"))) == unhex(recorded.data));
      EXPECT_EQ(*sqlite::getString(queue[index].get(std::string_view("backoff_schedule"))), std::string(recorded.backoffSchedule));
      EXPECT_EQ(*sqlite::getString(queue[index].get(std::string_view("keys_if_undelivered"))), std::string(recorded.keysIfUndelivered));
    }
    database.close();
  }
  // At the recorded expiry the open sweeps the expired row, advancing the data version by one.
  {
    SqliteKvOptions options;
    options.now = [expiry] { return static_cast<double>(expiry); };
    SqliteKv kv(file, options);
    const KvEntryMaybe<> expired = co_await kv.get(KvKey{"exp"});
    EXPECT(!expired.versionstamp);
    kv.close();
    sqlite::DatabaseOptions inspect;
    inspect.readonly = true;
    inspect.safeIntegers = true;
#if defined(__APPLE__)
    const sqlite::Database database(darwinSnapshotFile(file), inspect);
#else
    const sqlite::Database database(file, inspect);
#endif
    EXPECT_EQ(*sqlite::getBigInt(database.query("SELECT version FROM data_version WHERE k = 0").get()->get(std::string_view("version"))), RECORDED_DATA_VERSION + 1);
    database.close();
  }
  // Through the production opener (marker check and guard): the same entries.
  std::shared_ptr<Kv> opened = co_await storage::kv::openKv(file);
  KvListSelector all;
  all.prefix = KvKey{};
  std::unique_ptr<KvListIterator> listed = opened->list(all);
  const std::vector<KvEntry<>> production = co_await kvListToArray(*listed);
  EXPECT_EQ(production.size(), recordedKvRows().size() - 1);
  for (const KvEntry<>& entry : production) {
    for (const RecordedKvRow& row : recordedKvRows())
      if (unhex(row.k) == packKey(entry.key)) EXPECT(entry.value == expectedValue(row, vectors));
  }
  opened->close();
}

ASYNC_TEST("a database the C++ store writes is the one the TypeScript store read back") {
  const Directory directory;
  const std::string file = directory.path + "/written.sqlite3";
  std::shared_ptr<Kv> kv = co_await storage::kv::openKv(file);
  const std::vector<SerializedVector> vectors = serializedVectors();
  for (std::size_t index = 0; index < vectors.size(); ++index) co_await kv->set(KvKey{"v", static_cast<double>(index)}, vectors[index].expected);
  co_await writeExtras(kv);
  kv->close();
  const std::vector<std::string> lines = co_await dumpLines(file);
  EXPECT_EQ(lines.size(), std::size_t{230});
  EXPECT_EQ(sha256(normalized(lines)), std::string(TS_DUMP_OF_CPP_DATABASE_SHA256));
}

ASYNC_TEST("two C++ processes alternate and contend on one file through cursors, CAS and the queue") {
  const Directory directory;
  const std::string file = directory.path + "/shared.sqlite3";
  std::shared_ptr<Kv> kv = co_await storage::kv::openKv(file);
  co_await kv->set(KvKey{"turn"}, KvValue(std::string("a")));
  constexpr int rounds = 10;
  constexpr int contended = 50;
  std::vector<process::Subprocess> children;
  for (const std::string& me : {std::string("a"), std::string("b")}) {
    std::vector<std::pair<std::string, std::string>> environment;
    environment.emplace_back("UOS_KV_INTEROP_CHILD", "alternate");
    environment.emplace_back("UOS_KV_INTEROP_PATH", file);
    environment.emplace_back("UOS_KV_INTEROP_ME", me);
    environment.emplace_back("UOS_KV_INTEROP_OTHER", me == "a" ? "b" : "a");
    environment.emplace_back("UOS_KV_INTEROP_ROUNDS", std::to_string(rounds));
    environment.emplace_back("UOS_KV_INTEROP_CONTENDED", std::to_string(contended));
    environment.emplace_back("TMPDIR", os::tmpdir());
    process::SpawnOptions options;
    options.env = environment;
    options.stdout = process::Stdio::pipe;
    options.stderr = process::Stdio::inherit;
    std::vector<std::string> command = selfCommand();
    command.emplace_back("interop child: alternate");
    children.push_back(process::spawn(command, options));
  }
  std::vector<std::string> outputs;
  for (const process::Subprocess& child : children) {
    const Response response{BodyInit(child.stdout())};
    auto reading = response.text();
    const std::string text = co_await std::move(reading);
    auto exited = child.exited();
    const int code = co_await exited.wait();
    EXPECT_EQ(code, 0);
    outputs.push_back(text);
  }
  // Each process resumed from the cursor the other saved, so each saw exactly its own entries.
  std::string seenA = "a seen", seenB = "b seen";
  for (int round = 0; round < rounds; round += 1) {
    seenA += " " + std::to_string(2 * round + 1) + ":a:" + std::to_string(round);
    seenB += " " + std::to_string(2 * round + 2) + ":b:" + std::to_string(round);
  }
  EXPECT(includes(outputs.at(0), seenA));
  EXPECT(includes(outputs.at(1), seenB));
  EXPECT_EQ((co_await kv->get(KvKey{"counter"})).value, KvValue(2 * rounds));
  EXPECT_EQ((co_await kv->get(KvKey{"contended"})).value, KvValue(2 * contended));
  KvListSelector log;
  log.prefix = KvKey{"log"};
  std::unique_ptr<KvListIterator> iterator = kv->list(log);
  const std::vector<KvEntry<>> entries = co_await kvListToArray(*iterator);
  EXPECT_EQ(entries.size(), static_cast<std::size_t>(2 * rounds));
  kv->close();
  sqlite::DatabaseOptions inspect;
  inspect.readonly = true;
#if defined(__APPLE__)
  const sqlite::Database database(darwinSnapshotFile(file), inspect);
#else
  const sqlite::Database database(file, inspect);
#endif
  EXPECT_EQ(*sqlite::getNumber(database.query("SELECT count(*) AS n FROM queue").get()->get(std::string_view("n"))), 2.0 * rounds);
  EXPECT_EQ(*sqlite::getString(database.query("PRAGMA integrity_check").get()->get(std::string_view("integrity_check"))), std::string("ok"));
  database.close();
}
