// Differential cases for src/kv-conversion/convert.ts: complete conversions and their refusals,
// driven end to end against a fake `deno` shell script that answers the pinned identity check
// and drains the codec's stdin (the rows are counters and raw bytes, which the converter copies
// without the codec or node:v8). The same fixtures were converted by the TypeScript module under
// Bun 1.4.0, which recorded every expected value (tests/kv_conversion_convert_differential_vectors.inc).
// The SQLite files differ by library version (Bun links SQLite 3.53.2, rt::sqlite 3.40.1), so
// file digests, sizes and inode identities are compared against the C++ run's own files.
// Fixtures live under rt::os::tmpdir() (TMPDIR).
// Links: tests/kv_conversion_convert_differential_test.cpp src/kv_conversion/convert.cpp src/kv_conversion/cursors.cpp
//        src/kv_conversion/legacy_codec.cpp src/kv_conversion/native_schema.cpp src/kv_conversion/typed_value.cpp src/storage/kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp src/storage/value_compatibility.cpp src/storage/native_addon.cpp
//        src/storage/key_compatibility.cpp
#include "uos/kv_conversion/convert.hpp"

#include "uos/kv_conversion/legacy_codec.hpp"
#include "uos/kv_conversion/native_schema.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <cstdint>
#include <exception>
#include <functional>
#include <stdexcept>
#include <optional>
#include <string>
#include <vector>

#include "kv_conversion_convert_differential_vectors.inc"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace convert = uos::kv_conversion::convert;
  namespace native_schema = uos::kv_conversion::native_schema;
  namespace legacy_codec = uos::kv_conversion::legacy_codec;

  struct FixtureRow {
    std::string k;
    std::string v;
    std::int64_t e;
    std::int64_t version;
    std::int64_t seq;
    std::int64_t exp;
  };

  struct Scenario {
    std::string direction;
    std::string codec;
    std::vector<FixtureRow> rows;
    std::int64_t revision;
    std::optional<std::string> pin{};
    bool abort = false;
  };

  const FixtureRow COUNTER{.k = "02636f756e74657200", .v = "2a00000000000000", .e = 2, .version = 3, .seq = 0, .exp = -1};
  const FixtureRow RAW{.k = "0272617700", .v = "0800ff", .e = 3, .version = 5, .seq = 1, .exp = 1700000000000};

  FixtureRow with(FixtureRow row, const std::function<void(FixtureRow&)>& change) {
    change(row);
    return row;
  }

  // The scenarios of the recording script, by name.
  Scenario scenario(const std::string& name) {
    if (name == "deno-to-bun copies counters and bytes") return {.direction = "deno-to-bun", .codec = "deno-v8", .rows = {COUNTER, RAW}, .revision = 9};
    if (name == "bun-to-deno copies counters and bytes") return {.direction = "bun-to-deno", .codec = "bun-jsc-v1", .rows = {COUNTER, RAW}, .revision = 9};
    if (name == "empty database") return {.direction = "deno-to-bun", .codec = "deno-v8", .rows = {}, .revision = 0};
    if (name == "malformed counter")
      return {.direction = "deno-to-bun", .codec = "deno-v8", .rows = {with(COUNTER, [](FixtureRow& row) { row.v = "2a000000000000"; })}, .revision = 9};
    if (name == "row newer than revision")
      return {.direction = "bun-to-deno", .codec = "bun-jsc-v1", .rows = {with(RAW, [](FixtureRow& row) { row.version = 10; })}, .revision = 9};
    if (name == "negative expiration")
      return {.direction = "deno-to-bun", .codec = "deno-v8", .rows = {with(RAW, [](FixtureRow& row) { row.exp = -2; })}, .revision = 9};
    if (name == "unsupported encoding")
      return {.direction = "deno-to-bun", .codec = "deno-v8", .rows = {with(RAW, [](FixtureRow& row) { row.e = 4; })}, .revision = 9};
    if (name == "truncated key")
      return {.direction = "bun-to-deno", .codec = "bun-jsc-v1", .rows = {with(RAW, [](FixtureRow& row) { row.k = "02726177"; })}, .revision = 9};
    if (name == "negative NaN key")
      return {.direction = "deno-to-bun", .codec = "deno-v8", .rows = {with(RAW, [](FixtureRow& row) { row.k = "026e616e00210007ffffffffffff"; })}, .revision = 9};
    if (name == "wrong source codec") return {.direction = "bun-to-deno", .codec = "deno-v8", .rows = {RAW}, .revision = 9};
    if (name == "codec pin mismatch") return {.direction = "deno-to-bun", .codec = "deno-v8", .rows = {RAW}, .revision = 9, .pin = rt::repeat("0", 64)};
    if (name == "aborted before start") return {.direction = "deno-to-bun", .codec = "deno-v8", .rows = {RAW}, .revision = 9, .abort = true};
    if (name == "unknown direction") return {.direction = "sideways", .codec = "deno-v8", .rows = {RAW}, .revision = 9};
    throw std::logic_error("no scenario " + name);
  }

  const json::object& recorded() {
    static const json::object value = rt::jsonParse(CONVERT_RECORDED).as_object();
    return value;
  }

  void build(const std::string& path, const Scenario& s) {
    rt::sqlite::DatabaseOptions options;
    options.create = true;
    options.strict = true;
    options.safeIntegers = true;
    const rt::sqlite::Database db(path, options);
    db.run("PRAGMA journal_mode=DELETE");
    for (const std::string_view ddl : native_schema::NATIVE_DDL) db.run(ddl);
    db.run("INSERT INTO migration_state(k,version) VALUES(0,3)");
    std::vector<rt::sqlite::Binding> revision;
    revision.emplace_back(s.revision);
    db.run("INSERT INTO data_version(k,version,seq) VALUES(0,?,0)", revision);
    if (s.codec == "bun-jsc-v1") {
      db.run("CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID");
      db.run("INSERT INTO uos_kv_metadata(key,value) VALUES('codec','bun-jsc-v1')");
    }
    for (const FixtureRow& row : s.rows) {
      std::vector<rt::sqlite::Binding> values;
      values.emplace_back(rt::fromHex(row.k));
      values.emplace_back(rt::fromHex(row.v));
      values.emplace_back(row.e);
      values.emplace_back(row.version);
      values.emplace_back(row.seq);
      values.emplace_back(row.exp);
      db.run("INSERT INTO kv(k,v,v_encoding,version,seq,expiration_ms) VALUES(?,?,?,?,?,?)", values);
    }
    db.close(true);
  }

  rt::sqlite::Database readOnly(const std::string& path) {
    rt::sqlite::DatabaseOptions options;
    options.readonly = true;
    options.safeIntegers = true;
    return rt::sqlite::Database(path, options);
  }

  std::string text(const rt::sqlite::Value* value) {
    if (const std::int64_t* integer = rt::sqlite::getBigInt(value)) return std::to_string(*integer);
    if (const rt::sqlite::Bytes* bytes = rt::sqlite::getBytes(value)) return rt::toHex(*bytes);
    return "?";
  }

  // `[hex(k), hex(v), String(v_encoding), ...]` of every destination row.
  json::value rawRows(const std::string& path) {
    const rt::sqlite::Database db = readOnly(path);
    json::array rows;
    for (const rt::sqlite::Row& row : db.query("SELECT k,v,v_encoding,version,seq,expiration_ms FROM kv ORDER BY k").all()) {
      json::array fields;
      for (const char* name : {"k", "v", "v_encoding", "version", "seq", "expiration_ms"}) fields.emplace_back(text(row.get(std::string_view(name))));
      rows.emplace_back(std::move(fields));
    }
    db.close(true);
    return json::value(std::move(rows));
  }

  json::value metadata(const std::string& path) {
    const rt::sqlite::Database db = readOnly(path);
    json::value out = nullptr;
    if (db.query("SELECT name FROM sqlite_master WHERE name='uos_kv_metadata'").get()) {
      json::array rows;
      for (const rt::sqlite::Row& row : db.query("SELECT key,value FROM uos_kv_metadata ORDER BY key").all()) rows.emplace_back(rt::sqlite::toJson(row));
      out = std::move(rows);
    }
    db.close(true);
    return out;
  }

  rt::Task<void> runScenario(std::string name) {
    const json::object& expected = recorded().at(name).as_object();
    const Scenario s = scenario(name);
    const std::string base = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "uos-convert-differential-"));
    const std::string root = rt::fs::mkdtempSync(rt::path::join(base, "case-"));
    const std::string fake = rt::path::join(root, "fake-deno");
    rt::fs::WriteFileOptions executable;
    executable.mode = 0700;
    rt::fs::writeFileSync(fake, std::string(recorded().at("script").as_string()), executable);
    auto digestingFake = legacy_codec::fileDigest(fake);
    const std::string sha = co_await std::move(digestingFake);
    const std::string source = rt::path::join(root, "source.sqlite3");
    build(source, s);
    auto digestingSource = legacy_codec::fileDigest(source);
    const std::string before = co_await std::move(digestingSource);
    const std::string destination = rt::path::join(root, "destination.sqlite3");
    const std::string receiptPath = rt::path::join(root, "receipt.json");
    convert::ConvertOptions options{
      .direction = s.direction,
      .source = source,
      .destination = destination,
      .receipt = receiptPath,
      .deno = fake,
      .denoSha256 = s.pin.value_or(sha),
    };
    if (s.abort) options.signal = rt::AbortSignal::abort(rt::makeReason(rt::JsError("synthetic interruption")));
    auto converting = convert::convertKvDatabase(options);
    std::optional<convert::ConversionReceipt> result;
    rt::Reason failure;
    try {
      result = co_await std::move(converting);
    } catch (...) {
      failure = std::current_exception();
    }
    if (result) {
      ASSERT(expected.contains("receipt"));
      json::value normalized = rt::jsonParse(rt::replaceAll(rt::jsonStringify(json::value_from(*result)), root, "<root>"));
      json::object& receipt = normalized.as_object();
      receipt["source"].as_object()["file"] = "<file>";
      receipt["source"].as_object()["snapshot"] = "<file>";
      receipt["destination"].as_object()["file"] = "<file>";
      receipt["completed_at"] = "<completed_at>";
      EXPECT_EQ(rt::jsonStringify(normalized), rt::jsonStringify(expected.at("receipt")));
      EXPECT_EQ(rt::fs::readFileSync(receiptPath, "utf8") == rt::jsonStringify(json::value_from(*result), 2) + "\n", expected.at("receiptFile").as_bool());
      EXPECT_EQ(rawRows(destination), expected.at("rows"));
      EXPECT_EQ(metadata(destination), expected.at("metadata"));
      EXPECT_EQ(result->source.file == result->source.snapshot, expected.at("identityMatches").as_bool());
      // The C++ run's own files.
      EXPECT_EQ(result->source.file.sha256, before);
      EXPECT_EQ(result->source.file.size, std::to_string(rt::fs::lstatSync(source).size));
      auto digestingDestination = legacy_codec::fileDigest(destination);
      const std::string published = co_await std::move(digestingDestination);
      EXPECT_EQ(result->destination.file.sha256, published);
      EXPECT_EQ(result->destination.file.size, std::to_string(rt::fs::lstatSync(destination).size));
      EXPECT(rt::Date(rt::Date::parse(result->completed_at)).toISOString() == result->completed_at);
    } else {
      ASSERT(expected.contains("error"));
      const json::object& error = expected.at("error").as_object();
      EXPECT_EQ(rt::errorName(failure).value_or("?"), std::string(error.at("name").as_string()));
      const std::string message = rt::replaceAll(rt::replaceAll(rt::errorMessage(failure).value_or("?"), root, "<root>"), sha, "<sha>");
      EXPECT_EQ(message, std::string(error.at("message").as_string()));
    }
    auto digestingAfter = legacy_codec::fileDigest(source);
    const std::string after = co_await std::move(digestingAfter);
    EXPECT_EQ(after == before, expected.at("sourceUnchanged").as_bool());
    std::vector<std::string> entries = rt::fs::readdirSync(root);
    rt::sort(entries);
    json::array listed;
    for (const std::string& entry : entries) listed.emplace_back(entry);
    EXPECT_EQ(json::value(listed), expected.at("entries"));
    rt::fs::RmOptions removal;
    removal.recursive = true;
    removal.force = true;
    rt::fs::rmSync(base, removal);
  }
}

ASYNC_TEST("convert: deno-to-bun copies counters and bytes") {
  auto running = runScenario("deno-to-bun copies counters and bytes");
  co_await std::move(running);
}
ASYNC_TEST("convert: bun-to-deno copies counters and bytes") {
  auto running = runScenario("bun-to-deno copies counters and bytes");
  co_await std::move(running);
}
ASYNC_TEST("convert: empty database") {
  auto running = runScenario("empty database");
  co_await std::move(running);
}
ASYNC_TEST("convert: malformed counter") {
  auto running = runScenario("malformed counter");
  co_await std::move(running);
}
ASYNC_TEST("convert: row newer than revision") {
  auto running = runScenario("row newer than revision");
  co_await std::move(running);
}
ASYNC_TEST("convert: negative expiration") {
  auto running = runScenario("negative expiration");
  co_await std::move(running);
}
ASYNC_TEST("convert: unsupported encoding") {
  auto running = runScenario("unsupported encoding");
  co_await std::move(running);
}
ASYNC_TEST("convert: truncated key") {
  auto running = runScenario("truncated key");
  co_await std::move(running);
}
ASYNC_TEST("convert: negative NaN key") {
  auto running = runScenario("negative NaN key");
  co_await std::move(running);
}
ASYNC_TEST("convert: wrong source codec") {
  auto running = runScenario("wrong source codec");
  co_await std::move(running);
}
ASYNC_TEST("convert: codec pin mismatch") {
  auto running = runScenario("codec pin mismatch");
  co_await std::move(running);
}
ASYNC_TEST("convert: aborted before start") {
  auto running = runScenario("aborted before start");
  co_await std::move(running);
}
ASYNC_TEST("convert: unknown direction") {
  auto running = runScenario("unknown direction");
  co_await std::move(running);
}
