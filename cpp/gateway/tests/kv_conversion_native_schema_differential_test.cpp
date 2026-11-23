// Differential cases for src/kv-conversion/native-schema.ts. Its bun tests need a real SQLite
// binding (rt has none yet), so verifyNativeSchema is replayed against transcripts recorded from
// the TS under Bun 1.4.0 (tests/kv_conversion_native_schema_differential_vectors.inc): the reader
// answers exactly the queries the TS made, in order, with the rows SQLite returned, and the C++
// verdict must equal the TS verdict. validateNativeRow and createNativeSnapshotReader cases were
// recorded from the TS on directly built rows and a scripted database.
#include "uos/kv_conversion/native_schema.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"
#include <map>
#include <stdexcept>

#include "kv_conversion_native_schema_differential_vectors.inc"

namespace {
  using namespace uos::kv_conversion::native_schema;
  namespace rt = uos::rt;
  namespace json = uos::json;

  bun_sqlite::Value valueOf(const json::value& tagged) {
    if (tagged.is_null()) return nullptr;
    if (tagged.is_string()) return std::string(tagged.as_string());
    const json::object& object = tagged.as_object();
    if (const json::value* bigint = rt::get(object, "b")) return rt::BigInt::fromString(std::string(bigint->as_string())).toInt64();
    if (const json::value* number = rt::get(object, "n")) return rt::stringToNumber(std::string(number->as_string()));
    return rt::fromHex(std::string(rt::get(object, "x")->as_string()));
  }

  std::optional<bun_sqlite::Row> rowOf(const json::value& tagged) {
    if (tagged.is_null()) return std::nullopt;
    bun_sqlite::Row row;
    for (const json::value& cell : tagged.as_array()) row.set(std::string(cell.as_array()[0].as_string()), valueOf(cell.as_array()[1]));
    return row;
  }

  struct Step {
    std::string sql;
    std::string method;
    json::value result;
  };

  // A NativeReader that answers exactly the recorded queries, in order.
  struct Replay {
    std::vector<Step> steps;
    std::size_t at = 0;
    std::size_t answered = 0;
  };

  NativeReader replayReader(const std::shared_ptr<Replay>& replay) {
    NativeReader reader;
    reader.query = [replay](const std::string& sql) {
      if (replay->at >= replay->steps.size()) throw std::logic_error("unrecorded query " + sql);
      const Step step = replay->steps[replay->at++];
      if (step.sql != sql) throw std::logic_error("query " + sql + " where the TS ran " + step.sql);
      NativeReadStatement statement;
      statement.get = [replay, step] {
        if (step.method != "get") throw std::logic_error("get() where the TS called " + step.method + "() for " + step.sql);
        ++replay->answered;
        return rowOf(step.result);
      };
      statement.all = [replay, step] {
        if (step.method != "all") throw std::logic_error("all() where the TS called " + step.method + "() for " + step.sql);
        ++replay->answered;
        std::vector<bun_sqlite::Row> rows;
        for (const json::value& row : step.result.as_array()) rows.push_back(*rowOf(row));
        return rows;
      };
      statement.iterate = []() -> bun_sqlite::RowIterator { throw std::logic_error("iterate() is not used by verifyNativeSchema"); };
      return statement;
    };
    return reader;
  }

  json::value verdictOf(const std::shared_ptr<Replay>& replay, std::string_view codec) {
    json::object verdict;
    try {
      const NativeRevision revision = verifyNativeSchema(replayReader(replay), codec);
      verdict["ok"] = true;
      json::object fields;
      fields["k"] = std::to_string(revision.k);
      fields["version"] = std::to_string(revision.version);
      fields["seq"] = std::to_string(revision.seq);
      verdict["revision"] = fields;
    } catch (const rt::JsError& error) {
      verdict["ok"] = false;
      verdict["name"] = error.name;
      verdict["message"] = error.message;
    }
    return verdict;
  }

  bun_sqlite::Row validRow() {
    return bun_sqlite::Row{
      {"k", std::vector<std::uint8_t>{2, 97, 0}},
      {"v", std::vector<std::uint8_t>{1, 2}},
      {"v_encoding", std::int64_t{1}},
      {"version", std::int64_t{10}},
      {"seq", std::int64_t{0}},
      {"expiration_ms", std::int64_t{-1}},
    };
  }

  std::string validated(const bun_sqlite::Row& row, NativeRevision revision = {.k = 0, .version = 10, .seq = 3}) {
    try {
      validateNativeRow(row, revision);
      return "ok";
    } catch (const rt::JsError& error) {
      return error.name + " " + error.message;
    }
  }

  bun_sqlite::Row with(bun_sqlite::Row row, std::string column, bun_sqlite::Value value) {
    row.set(std::move(column), std::move(value));
    return row;
  }

  bun_sqlite::Row without(bun_sqlite::Row row, std::string_view column) {
    row.delete_(column);
    return row;
  }

  // A scripted bun:sqlite database, as in the oracle run: statements answer one row {a: 1n}.
  class ScriptedStatement final : public bun_sqlite::Statement {
  public:
    ScriptedStatement(std::string sql_, std::int64_t params_, bool safe_, std::vector<std::string>& log_) : sql(std::move(sql_)), params(params_), safe(safe_), log(log_) {}
    std::int64_t paramsCount() const override { return params; }
    bool hasSafeIntegers() const override { return safe; }
    void safeIntegers(bool enabled) override { log.push_back(std::string("safeIntegers(") + (enabled ? "true" : "false") + ") " + sql); }
    std::optional<bun_sqlite::Row> get() override {
      log.push_back("get " + sql);
      return bun_sqlite::Row{{"a", std::int64_t{1}}};
    }
    std::vector<bun_sqlite::Row> all() override {
      log.push_back("all " + sql);
      return {bun_sqlite::Row{{"a", std::int64_t{1}}}};
    }
    bun_sqlite::RowIterator iterate() override {
      log.push_back("iterate " + sql);
      auto done = std::make_shared<bool>(false);
      return [done]() -> std::optional<bun_sqlite::Row> {
        if (*done) return std::nullopt;
        *done = true;
        return bun_sqlite::Row{{"a", std::int64_t{1}}};
      };
    }

  private:
    std::string sql;
    std::int64_t params;
    bool safe;
    std::vector<std::string>& log;
  };

  class ScriptedDatabase final : public bun_sqlite::Database {
  public:
    std::vector<std::string> log;
    std::shared_ptr<bun_sqlite::Statement> query(const std::string& sql) override {
      log.push_back("query " + sql);
      return std::make_shared<ScriptedStatement>(sql, sql == "params" ? 1 : 0, sql != "no-safe" && sql != "safe-not-function", log);
    }
    void close(bool throwOnError) override { log.push_back(std::string("close(") + (throwOnError ? "true" : "false") + ")"); }
  };
}

TEST("verifyNativeSchema returns the TS verdict for every recorded SQLite transcript") {
  const json::value vectors = rt::jsonParse(kNativeSchemaVectors);
  const json::array& sql = vectors.as_object().at("sql").as_array();
  const json::array& results = vectors.as_object().at("results").as_array();
  std::size_t checked = 0;
  for (const json::value& variant : vectors.as_object().at("variants").as_array()) {
    const json::object& entry = variant.as_object();
    const std::string name(entry.at("name").as_string());
    auto replay = std::make_shared<Replay>();
    for (const json::value& step : entry.at("steps").as_array()) {
      const json::array& indexes = step.as_array();
      const json::array& result = results[static_cast<std::size_t>(rt::toNumber(indexes[1]))].as_array();
      replay->steps.push_back(Step{std::string(sql[static_cast<std::size_t>(rt::toNumber(indexes[0]))].as_string()), std::string(result[0].as_string()), result[1]});
    }
    json::value verdict;
    try {
      verdict = verdictOf(replay, std::string(entry.at("codec").as_string()));
    } catch (const std::logic_error& mismatch) {
      verdict = json::value(std::string("replay mismatch: ") + mismatch.what());
    }
    if (!rt::deepEqual(verdict, entry.at("verdict"))) {
      ::uos::rt::test::report(__FILE__, __LINE__, name + ": verdict " + rt::jsonStringify(verdict) + ", TS " + rt::jsonStringify(entry.at("verdict")), false);
    } else if (replay->answered != replay->steps.size() || replay->at != replay->steps.size()) {
      ::uos::rt::test::report(__FILE__, __LINE__, name + ": made " + std::to_string(replay->answered) + " of " + std::to_string(replay->steps.size()) + " recorded queries", false);
    }
    ++checked;
  }
  EXPECT_EQ(checked, 79u);
}

TEST("validateNativeRow checks types, binary columns, encoding, counter size and revision bounds in TS order") {
  EXPECT_EQ(validated(validRow()), "ok");
  EXPECT_EQ(validated(with(with(validRow(), "v_encoding", std::int64_t{2}), "v", std::vector<std::uint8_t>(8))), "ok");
  EXPECT_EQ(validated(with(with(validRow(), "v_encoding", std::int64_t{2}), "v", std::vector<std::uint8_t>(7))), "Error Malformed native unsigned counter");
  EXPECT_EQ(validated(with(validRow(), "v_encoding", std::int64_t{3})), "ok");
  EXPECT_EQ(validated(with(validRow(), "v_encoding", std::int64_t{0})), "Error Unsupported native value encoding");
  EXPECT_EQ(validated(with(validRow(), "v_encoding", std::int64_t{4})), "Error Unsupported native value encoding");
  EXPECT_EQ(validated(with(validRow(), "v_encoding", std::int64_t{9223372036854775807})), "Error Unsupported native value encoding");
  EXPECT_EQ(validated(with(validRow(), "v_encoding", 1.0)), "Error Unsupported native row metadata types");
  EXPECT_EQ(validated(with(validRow(), "version", 1.0)), "Error Unsupported native row metadata types");
  EXPECT_EQ(validated(with(validRow(), "seq", std::string("0"))), "Error Unsupported native row metadata types");
  EXPECT_EQ(validated(with(validRow(), "expiration_ms", nullptr)), "Error Unsupported native row metadata types");
  EXPECT_EQ(validated(without(validRow(), "expiration_ms")), "Error Unsupported native row metadata types");
  EXPECT_EQ(validated(with(validRow(), "k", std::string("abc"))), "Error Invalid native binary row");
  EXPECT_EQ(validated(with(validRow(), "k", std::vector<std::uint8_t>{})), "Error Invalid native binary row");
  EXPECT_EQ(validated(with(validRow(), "k", nullptr)), "Error Invalid native binary row");
  EXPECT_EQ(validated(with(validRow(), "v", std::string("x"))), "Error Invalid native binary row");
  EXPECT_EQ(validated(without(validRow(), "v")), "Error Invalid native binary row");
  EXPECT_EQ(validated(with(validRow(), "v", std::vector<std::uint8_t>{})), "ok");
  EXPECT_EQ(validated(with(validRow(), "version", std::int64_t{-1})), "Error Invalid native revision or absolute expiration");
  EXPECT_EQ(validated(with(validRow(), "version", std::int64_t{10})), "ok");
  EXPECT_EQ(validated(with(validRow(), "version", std::int64_t{11})), "Error Invalid native revision or absolute expiration");
  EXPECT_EQ(validated(with(validRow(), "seq", std::int64_t{-1})), "Error Invalid native revision or absolute expiration");
  EXPECT_EQ(validated(with(validRow(), "seq", std::int64_t{9223372036854775807})), "ok");
  EXPECT_EQ(validated(with(validRow(), "expiration_ms", std::int64_t{-1})), "ok");
  EXPECT_EQ(validated(with(validRow(), "expiration_ms", std::int64_t{-2})), "Error Invalid native revision or absolute expiration");
  EXPECT_EQ(validated(with(validRow(), "expiration_ms", std::int64_t{0})), "ok");
  EXPECT_EQ(validated(with(validRow(), "expiration_ms", std::numeric_limits<std::int64_t>::min())), "Error Invalid native revision or absolute expiration");
  EXPECT_EQ(validated(with(with(validRow(), "k", std::string("abc")), "version", 1.0)), "Error Unsupported native row metadata types");
  EXPECT_EQ(validated(with(with(validRow(), "k", std::vector<std::uint8_t>{}), "v_encoding", std::int64_t{9})), "Error Invalid native binary row");
  EXPECT_EQ(validated(with(with(validRow(), "v_encoding", std::int64_t{7}), "v", std::vector<std::uint8_t>(1))), "Error Unsupported native value encoding");
  EXPECT_EQ(validated(with(with(with(validRow(), "v_encoding", std::int64_t{2}), "v", std::vector<std::uint8_t>(1)), "version", std::int64_t{-5})), "Error Malformed native unsigned counter");
  EXPECT_EQ(validated(with(validRow(), "version", std::int64_t{9223372036854775807}), {.k = 0, .version = 9223372036854775807, .seq = 0}), "ok");
}

TEST("createNativeSnapshotReader enables safe integers, forwards get/all/iterate and closes with throwOnError") {
  const auto database = std::make_shared<ScriptedDatabase>();
  const NativeSnapshotReader reader = createNativeSnapshotReader(database);
  for (const std::string sql : {"SELECT 1", "params", "no-safe", "safe-not-function"}) {
    try {
      const NativeReadStatement statement = reader.query(sql);
      const std::optional<bun_sqlite::Row> row = statement.get();
      database->log.push_back("get a=" + std::to_string(std::get<std::int64_t>(*row->get(std::string_view("a")))));
      database->log.push_back("all=" + std::to_string(statement.all().size()));
      const bun_sqlite::RowIterator next = statement.iterate();
      std::size_t count = 0;
      while (next()) ++count;
      database->log.push_back("iterate=" + std::to_string(count));
    } catch (const rt::JsError& error) {
      database->log.push_back("THROW " + error.name + " " + error.message);
    }
  }
  reader.close();
  EXPECT_EQ(database->log, (std::vector<std::string>{
                             "query SELECT 1",
                             "safeIntegers(true) SELECT 1",
                             "get SELECT 1",
                             "get a=1",
                             "all SELECT 1",
                             "all=1",
                             "iterate SELECT 1",
                             "iterate=1",
                             "query params",
                             "THROW Error Conversion snapshot queries must not have bound parameters",
                             "query no-safe",
                             "THROW Error Bun statement safe integers are unavailable",
                             "query safe-not-function",
                             "THROW Error Bun statement safe integers are unavailable",
                             "close(true)",
                           }));
}

TEST("NATIVE_DDL is the TS list") {
  EXPECT_EQ(NATIVE_DDL.size(), 6u);
  EXPECT_EQ(std::string(NATIVE_DDL[0]), "CREATE TABLE data_version (k INTEGER PRIMARY KEY, version INTEGER NOT NULL, seq INTEGER NOT NULL DEFAULT 0)");
  EXPECT_EQ(std::string(NATIVE_DDL[5]),
            "CREATE TABLE queue_running (deadline INTEGER NOT NULL, id TEXT NOT NULL, data BLOB NOT NULL, backoff_schedule TEXT NOT NULL, keys_if_undelivered BLOB NOT NULL, "
            "PRIMARY KEY (deadline, id))");
}
