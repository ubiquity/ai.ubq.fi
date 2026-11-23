// Differential cases for src/storage/native-kv.ts (no direct TS tests): the guard runs over a
// scripted fake of the SDK store, written identically in TypeScript and here; every expected
// result and call log was recorded from Bun 1.4.0 running the TypeScript module.
#include "uos/storage/native_kv.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include <deque>
#include <variant>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::storage::native_kv;

  const char* const RECORDED = R"JSON([{"name": "get retries locked", "result": {"ok": "v:a/1"}, "calls": ["get a/1", "get a/1", "get a/1"]}, {"name": "get rethrows other errors", "result": {"error": "Error: boom"}, "calls": ["get a"]}, {"name": "get rethrows non-Error values", "result": {"error": "thrown database is locked"}, "calls": ["get a"]}, {"name": "get retries embedded lock text", "result": {"ok": "v:a"}, "calls": ["get a", "get a"]}, {"name": "getMany retries", "result": {"ok": "v:a,v:b"}, "calls": ["getMany a,b", "getMany a,b"]}, {"name": "set commits through a checked atomic", "result": {"ok": "{\"ok\":true,\"versionstamp\":\"00000000000000020000\"}"}, "calls": ["set k=v expireIn=1000", "commit", "commit"]}, {"name": "set failure", "result": {"error": "Error: Unconditional KV set failed"}, "calls": ["set k=v expireIn=undefined", "commit"]}, {"name": "delete success", "result": {"ok": "undefined"}, "calls": ["delete k", "commit"]}, {"name": "delete failure", "result": {"error": "Error: Unconditional KV delete failed"}, "calls": ["delete k", "commit"]}, {"name": "sum on non-U64 becomes TypeError", "result": {"error": "TypeError: Atomic arithmetic requires an absent key or KvU64 value (cause Error: Failed to perform 'sum' mutation on a non-U64 value in the database)"}, "calls": ["sum n 1", "commit", "commit"]}, {"name": "min on non-U64 becomes TypeError", "result": {"error": "TypeError: Atomic arithmetic requires an absent key or KvU64 value (cause Error: Failed to perform 'min' mutation on a non-U64 value in the database)"}, "calls": ["min n 1", "commit", "commit"]}, {"name": "max on non-U64 becomes TypeError", "result": {"error": "TypeError: Atomic arithmetic requires an absent key or KvU64 value (cause Error: Failed to perform 'max' mutation on a non-U64 value in the database)"}, "calls": ["max n 1", "commit", "commit"]}, {"name": "near-miss arithmetic message is rethrown", "result": {"error": "Error: Failed to perform 'sum' mutation on a non-U64 value in the database!"}, "calls": ["sum n 1", "commit"]}, {"name": "TypeError arithmetic message converts too", "result": {"error": "TypeError: Atomic arithmetic requires an absent key or KvU64 value (cause RangeError: Failed to perform 'max' mutation on a non-U64 value in the database)"}, "calls": ["max n 1", "commit"]}, {"name": "non-Error commit failure", "result": {"error": "thrown Failed to perform 'sum' mutation on a non-U64 value in the database"}, "calls": ["sum n 1", "commit"]}, {"name": "atomic chain forwards builders", "result": {"ok": "{\"ok\":true,\"versionstamp\":\"00000000000000020000\"}"}, "calls": ["check c@null", "set k=v expireIn=undefined", "delete d", "commit"]}, {"name": "list prefix selector", "result": {"ok": "false c1"}, "calls": ["list prefix=a limit=undefined cursor=undefined", "list prefix=a start=a/bytes[] limit=undefined cursor=undefined", "next"]}, {"name": "list prefix end selector", "result": {"ok": "false c1"}, "calls": ["list prefix=a end=b limit=undefined cursor=undefined", "list start=a/bytes[] end=b limit=undefined cursor=undefined", "next"]}, {"name": "list prefix start selector", "result": {"ok": "false c1"}, "calls": ["list prefix=a start=a/x limit=undefined cursor=undefined", "next"]}, {"name": "list start end selector", "result": {"ok": "false c1"}, "calls": ["list start=a end=b limit=undefined cursor=undefined", "next"]}, {"name": "list cursor without limit", "result": {"ok": "cursor Error: Cannot get cursor before first iteration | e1 cursor=c1 | e2 cursor=c2 | done cursor="}, "calls": ["list prefix=p limit=undefined cursor=undefined", "list prefix=p start=p/bytes[] limit=undefined cursor=undefined", "next", "next", "next"]}, {"name": "list cursor at limit", "result": {"ok": "cursor Error: Cannot get cursor before first iteration | e1 cursor=c1 | e2 cursor=c2 | done cursor=c2"}, "calls": ["list prefix=p limit=2 cursor=undefined", "list prefix=p start=p/bytes[] limit=2 cursor=undefined", "next", "next", "next"]}, {"name": "list cursor below limit", "result": {"ok": "cursor Error: Cannot get cursor before first iteration | e1 cursor=c1 | e2 cursor=c2 | done cursor="}, "calls": ["list prefix=p limit=3 cursor=undefined", "list prefix=p start=p/bytes[] limit=3 cursor=undefined", "next", "next", "next"]}, {"name": "list cursor given", "result": {"ok": "cursor start | done cursor="}, "calls": ["list prefix=p limit=undefined cursor=start", "list prefix=p start=p/bytes[] limit=undefined cursor=start", "next"]}, {"name": "list empty", "result": {"ok": "cursor Error: Cannot get cursor before first iteration | done cursor="}, "calls": ["list prefix=p limit=undefined cursor=undefined", "list prefix=p start=p/bytes[] limit=undefined cursor=undefined", "next"]}, {"name": "list next retries locked", "result": {"ok": "cursor Error: Cannot get cursor before first iteration | e1 cursor=c1 | done cursor="}, "calls": ["list prefix=p limit=undefined cursor=undefined", "list prefix=p start=p/bytes[] limit=undefined cursor=undefined", "next", "next", "next"]}, {"name": "list return delegates", "result": {"ok": "{\"done\":true}"}, "calls": ["list prefix=p limit=undefined cursor=undefined", "list prefix=p start=p/bytes[] limit=undefined cursor=undefined", "next", "return"]}, {"name": "close delegates", "result": {"ok": "undefined"}, "calls": ["close"]}, {"name": "retry deadline", "result": "Error: database is locked", "attempts": 501, "elapsed": 5000}, {"name": "retry sync run", "result": "sync", "attempts": 3}])JSON";

  std::string keyText(const rt::KvKey& key) {
    std::string out;
    for (std::size_t index = 0; index < key.size(); ++index) {
      if (index) out += "/";
      const rt::KvKeyPart& part = key[index];
      if (part.isBytes()) {
        out += "bytes[";
        for (std::size_t byte = 0; byte < part.bytes().size(); ++byte) out += (byte ? "," : "") + std::to_string(part.bytes()[byte]);
        out += "]";
      } else if (part.isBigInt()) {
        out += part.bigint().toString() + "n";
      } else {
        out += part.toString();
      }
    }
    return out;
  }

  std::string describe(const rt::Reason& reason) {
    try {
      std::rethrow_exception(reason);
    } catch (const rt::JsError& error) {
      std::string text = error.name + ": " + error.message;
      if (error.cause) text += " (cause " + error.cause->name + ": " + error.cause->message + ")";
      return text;
    } catch (...) {
      return "thrown " + rt::reasonToString(reason);
    }
  }

  // One scripted step: `undefined` (proceed), a commit result, or a thrown reason.
  using Step = std::variant<std::monostate, rt::KvCommitResult, rt::Reason>;
  Step locked() { return rt::makeReason(rt::JsError("database is locked")); }

  struct State {
    std::vector<std::string> calls;
    std::deque<Step> getPlan;
    std::deque<Step> commitPlan;
    std::deque<Step> nextPlan;
    std::vector<std::string> entries;
  };

  std::optional<rt::KvCommitResult> take(std::deque<Step>& plan) {
    if (plan.empty()) return std::nullopt;
    Step step = std::move(plan.front());
    plan.pop_front();
    if (const auto* reason = std::get_if<rt::Reason>(&step)) std::rethrow_exception(*reason);
    if (const auto* result = std::get_if<rt::KvCommitResult>(&step)) return *result;
    return std::nullopt;
  }

  class FakeIterator final : public rt::KvListIterator {
  public:
    explicit FakeIterator(std::shared_ptr<State> state) : state_(std::move(state)), entries_(state_->entries) {}
    std::string cursor() const override {
      if (index_ == 0) throw rt::JsError("native cursor before first entry");
      return "c" + std::to_string(index_);
    }
    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      state_->calls.push_back("next");
      take(state_->nextPlan);
      if (index_ >= entries_.size()) co_return std::nullopt;
      index_ += 1;
      rt::KvEntry<> entry{rt::KvKey{entries_[index_ - 1]}, rt::KvValue(static_cast<double>(index_)), "v"};
      co_return entry;
    }
    rt::Task<void> return_() override {
      state_->calls.push_back("return");
      co_return;
    }

  private:
    std::shared_ptr<State> state_;
    std::vector<std::string> entries_;
    std::size_t index_ = 0;
  };

  class FakeAtomic final : public rt::AtomicOperation {
  public:
    explicit FakeAtomic(std::shared_ptr<State> state) : state_(std::move(state)) {}
    using rt::AtomicOperation::check;
    using rt::AtomicOperation::max;
    using rt::AtomicOperation::min;
    using rt::AtomicOperation::sum;
    rt::AtomicOperation& check(std::vector<rt::AtomicCheck> checks) override {
      std::string text = "check ";
      for (std::size_t index = 0; index < checks.size(); ++index)
        text += (index ? "," : "") + keyText(checks[index].key) + "@" + checks[index].versionstamp.value_or("null");
      state_->calls.push_back(text);
      return *this;
    }
    rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { return *this; }
    rt::AtomicOperation& sum(rt::KvKey key, const rt::BigInt& n) override { return log("sum", key, n); }
    rt::AtomicOperation& min(rt::KvKey key, const rt::BigInt& n) override { return log("min", key, n); }
    rt::AtomicOperation& max(rt::KvKey key, const rt::BigInt& n) override { return log("max", key, n); }
    rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options) override {
      state_->calls.push_back("set " + keyText(key) + "=" + value.asString() + " expireIn=" + (options.expireIn ? rt::numberToString(*options.expireIn) : "undefined"));
      return *this;
    }
    rt::AtomicOperation& delete_(rt::KvKey key) override {
      state_->calls.push_back("delete " + keyText(key));
      return *this;
    }
    rt::Task<rt::KvCommitResult> commit() override {
      state_->calls.push_back("commit");
      const std::optional<rt::KvCommitResult> result = take(state_->commitPlan);
      const rt::KvCommitResult committed = result ? *result : rt::KvCommitResult{true, "00000000000000020000"};
      co_return committed;
    }

  private:
    rt::AtomicOperation& log(const char* name, const rt::KvKey& key, const rt::BigInt& n) {
      state_->calls.push_back(std::string(name) + " " + keyText(key) + " " + n.toString());
      return *this;
    }
    std::shared_ptr<State> state_;
  };

  class FakeKv final : public rt::Kv {
  public:
    explicit FakeKv(std::shared_ptr<State> state) : state_(std::move(state)) {}
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions) override {
      state_->calls.push_back("get " + keyText(key));
      take(state_->getPlan);
      rt::KvEntryMaybe<> entry{key, rt::KvValue("v:" + keyText(key)), "00000000000000010000"};
      co_return entry;
    }
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions) override {
      std::string text = "getMany ";
      for (std::size_t index = 0; index < keys.size(); ++index) text += (index ? "," : "") + keyText(keys[index]);
      state_->calls.push_back(text);
      take(state_->getPlan);
      std::vector<rt::KvEntryMaybe<>> entries;
      for (const auto& key : keys) entries.push_back(rt::KvEntryMaybe<>{key, rt::KvValue("v:" + keyText(key)), "00000000000000010000"});
      co_return entries;
    }
    rt::Task<rt::KvCommitResult> set(rt::KvKey, rt::KvValue, rt::KvSetOptions) override { throw rt::JsError("native set must not be called"); }
    rt::Task<void> delete_(rt::KvKey) override { throw rt::JsError("native delete must not be called"); }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options) override {
      std::vector<std::string> parts;
      if (selector.prefix) parts.push_back("prefix=" + keyText(*selector.prefix));
      if (selector.start) parts.push_back("start=" + keyText(*selector.start));
      if (selector.end) parts.push_back("end=" + keyText(*selector.end));
      std::string text = "list";
      for (const auto& part : parts) text += " " + part;
      text += " limit=" + (options.limit ? rt::numberToString(*options.limit) : std::string("undefined"));
      text += " cursor=" + options.cursor.value_or("undefined");
      state_->calls.push_back(text);
      return std::make_unique<FakeIterator>(state_);
    }
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<FakeAtomic>(state_); }
    void close() override { state_->calls.push_back("close"); }

  private:
    std::shared_ptr<State> state_;
  };

  std::string commitText(const rt::KvCommitResult& result) {
    return result.ok ? R"({"ok":true,"versionstamp":")" + result.versionstamp + "\"}" : std::string(R"({"ok":false})");
  }

  using Body = std::function<rt::Task<std::string>(rt::Kv&)>;
  using Setup = std::function<void(State&)>;

  // run(name, setup, body) of the recording script: { name, result, calls }.
  rt::Task<json::value> run(std::string name, Setup setup, Body body) {
    auto state = std::make_shared<State>();
    setup(*state);
    std::unique_ptr<rt::Kv> kv = guardNativeKv(std::make_shared<FakeKv>(state));
    json::object result;
    rt::Reason failure;
    try {
      std::string ok = co_await body(*kv);
      result["ok"] = json::string(ok);
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) result["error"] = json::string(describe(failure));
    json::array calls;
    for (const auto& call : state->calls) calls.push_back(json::string(call));
    json::object row;
    row["name"] = json::string(name);
    row["result"] = std::move(result);
    row["calls"] = std::move(calls);
    co_return row;
  }

  rt::Task<std::string> iterate(rt::Kv& kv, rt::KvListOptions options) {
    rt::KvListSelector selector;
    selector.prefix = rt::KvKey{"p"};
    std::unique_ptr<rt::KvListIterator> iterator = kv.list(selector, options);
    std::vector<std::string> steps;
    try {
      steps.push_back("cursor " + iterator->cursor());
    } catch (...) {
      steps.push_back("cursor " + describe(std::current_exception()));
    }
    for (;;) {
      const std::optional<rt::KvEntry<>> entry = co_await iterator->next();
      const std::string label = entry ? entry->key[0].string() : "done";
      try {
        steps.push_back(label + " cursor=" + iterator->cursor());
      } catch (...) {
        steps.push_back((entry ? std::string("entry") : std::string("done")) + " cursor " + describe(std::current_exception()));
      }
      if (!entry) break;
    }
    std::string out;
    for (std::size_t index = 0; index < steps.size(); ++index) out += (index ? " | " : "") + steps[index];
    co_return out;
  }

  rt::Task<std::string> listFirst(rt::Kv& kv, rt::KvListSelector selector) {
    std::unique_ptr<rt::KvListIterator> iterator = kv.list(selector, rt::KvListOptions{});
    const std::optional<rt::KvEntry<>> first = co_await iterator->next();
    co_return std::string(first ? "false " : "true ") + iterator->cursor();
  }

  rt::Task<std::string> arithmetic(rt::Kv& kv, std::string op) {
    std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
    const rt::KvKey key{"n"};
    if (op == "sum") operation->sum(key, 1);
    if (op == "min") operation->min(key, 1);
    if (op == "max") operation->max(key, 1);
    const rt::KvCommitResult result = co_await operation->commit();
    co_return commitText(result);
  }
}

namespace {
  struct Scenario {
    std::string name;
    Setup setup;
    Body body;
  };

  // The scenarios of the recording script, in its order.
  std::vector<Scenario> scenarios() {
    std::vector<Scenario> out;
    const Setup none = [](State&) {};
    const Body getValue = [](rt::Kv& kv) -> rt::Task<std::string> {
      const rt::KvEntryMaybe<> entry = co_await kv.get(rt::KvKey{"a", 1});
      co_return entry.value.asString();
    };
    const Body getA = [](rt::Kv& kv) -> rt::Task<std::string> {
      const rt::KvEntryMaybe<> entry = co_await kv.get(rt::KvKey{"a"});
      co_return entry.value.asString();
    };
    out.push_back({"get retries locked", [](State& s) { s.getPlan = {locked(), locked(), std::monostate{}}; }, getValue});
    out.push_back({"get rethrows other errors", [](State& s) { s.getPlan = {rt::makeReason(rt::JsError("boom"))}; }, getA});
    out.push_back({"get rethrows non-Error values", [](State& s) { s.getPlan = {rt::thrown(json::value("database is locked"))}; }, getA});
    out.push_back({"get retries embedded lock text", [](State& s) { s.getPlan = {rt::makeReason(rt::TypeError("SQLITE_BUSY: database is locked (5)")), std::monostate{}}; }, getA});
    out.push_back({"getMany retries", [](State& s) { s.getPlan = {locked(), std::monostate{}}; }, [](rt::Kv& kv) -> rt::Task<std::string> {
                     std::vector<rt::KvKey> keys{rt::KvKey{"a"}, rt::KvKey{"b"}};
                     const std::vector<rt::KvEntryMaybe<>> entries = co_await kv.getMany(keys);
                     co_return entries[0].value.asString() + "," + entries[1].value.asString();
                   }});
    out.push_back({"set commits through a checked atomic", [](State& s) { s.commitPlan = {locked(), std::monostate{}}; }, [](rt::Kv& kv) -> rt::Task<std::string> {
                     const rt::KvSetOptions options{.expireIn = 1000};
                     const rt::KvCommitResult result = co_await kv.set(rt::KvKey{"k"}, rt::KvValue("v"), options);
                     co_return commitText(result);
                   }});
    out.push_back({"set failure", [](State& s) { s.commitPlan = {rt::KvCommitResult{false, ""}}; }, [](rt::Kv& kv) -> rt::Task<std::string> {
                     const rt::KvCommitResult result = co_await kv.set(rt::KvKey{"k"}, rt::KvValue("v"));
                     co_return commitText(result);
                   }});
    const Body deleteKey = [](rt::Kv& kv) -> rt::Task<std::string> {
      co_await kv.delete_(rt::KvKey{"k"});
      co_return std::string("undefined");
    };
    out.push_back({"delete success", none, deleteKey});
    out.push_back({"delete failure", [](State& s) { s.commitPlan = {rt::KvCommitResult{false, ""}}; }, deleteKey});
    for (const std::string op : {"sum", "min", "max"}) {
      out.push_back({op + " on non-U64 becomes TypeError",
                     [op](State& s) {
                       s.commitPlan = {locked(), rt::makeReason(rt::JsError("Failed to perform '" + op + "' mutation on a non-U64 value in the database"))};
                     },
                     [op](rt::Kv& kv) { return arithmetic(kv, op); }});
    }
    const Body sum = [](rt::Kv& kv) { return arithmetic(kv, "sum"); };
    const Body max = [](rt::Kv& kv) { return arithmetic(kv, "max"); };
    out.push_back({"near-miss arithmetic message is rethrown",
                   [](State& s) { s.commitPlan = {rt::makeReason(rt::JsError("Failed to perform 'sum' mutation on a non-U64 value in the database!"))}; }, sum});
    out.push_back({"TypeError arithmetic message converts too",
                   [](State& s) { s.commitPlan = {rt::makeReason(rt::RangeError("Failed to perform 'max' mutation on a non-U64 value in the database"))}; }, max});
    out.push_back({"non-Error commit failure",
                   [](State& s) { s.commitPlan = {rt::thrown(json::value("Failed to perform 'sum' mutation on a non-U64 value in the database"))}; }, sum});
    out.push_back({"atomic chain forwards builders", none, [](rt::Kv& kv) -> rt::Task<std::string> {
                     std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
                     operation->check(rt::AtomicCheck{rt::KvKey{"c"}, std::nullopt}).set(rt::KvKey{"k"}, rt::KvValue("v")).delete_(rt::KvKey{"d"});
                     const rt::KvCommitResult result = co_await operation->commit();
                     co_return commitText(result);
                   }});
    const auto withEntries = [](std::vector<std::string> entries) { return Setup([entries](State& s) { s.entries = entries; }); };
    rt::KvListSelector prefix;
    prefix.prefix = rt::KvKey{"a"};
    rt::KvListSelector prefixEnd = prefix;
    prefixEnd.end = rt::KvKey{"b"};
    rt::KvListSelector prefixStart = prefix;
    prefixStart.start = rt::KvKey{"a", "x"};
    rt::KvListSelector startEnd;
    startEnd.start = rt::KvKey{"a"};
    startEnd.end = rt::KvKey{"b"};
    const std::vector<std::pair<std::string, rt::KvListSelector>> selectors{{"prefix", prefix}, {"prefix end", prefixEnd}, {"prefix start", prefixStart}, {"start end", startEnd}};
    for (const auto& [label, selector] : selectors)
      out.push_back({"list " + label + " selector", withEntries({"x"}), [selector](rt::Kv& kv) { return listFirst(kv, selector); }});
    const auto iterateWith = [](rt::KvListOptions options) { return Body([options](rt::Kv& kv) { return iterate(kv, options); }); };
    out.push_back({"list cursor without limit", withEntries({"e1", "e2"}), iterateWith({})});
    out.push_back({"list cursor at limit", withEntries({"e1", "e2"}), iterateWith({.limit = 2})});
    out.push_back({"list cursor below limit", withEntries({"e1", "e2"}), iterateWith({.limit = 3})});
    out.push_back({"list cursor given", withEntries({}), iterateWith({.cursor = "start"})});
    out.push_back({"list empty", withEntries({}), iterateWith({})});
    out.push_back({"list next retries locked",
                   [](State& s) {
                     s.entries = {"e1"};
                     s.nextPlan = {locked(), std::monostate{}};
                   },
                   iterateWith({})});
    out.push_back({"list return delegates", withEntries({"e1"}), [](rt::Kv& kv) -> rt::Task<std::string> {
                     rt::KvListSelector selector;
                     selector.prefix = rt::KvKey{"p"};
                     std::unique_ptr<rt::KvListIterator> iterator = kv.list(selector, rt::KvListOptions{});
                     co_await iterator->next();
                     co_await iterator->return_();
                     co_return std::string(R"({"done":true})");
                   }});
    out.push_back({"close delegates", none, [](rt::Kv& kv) -> rt::Task<std::string> {
                     kv.close();
                     co_return std::string("undefined");
                   }});
    return out;
  }
}

ASYNC_TEST("differential: the native KV guard matches Bun over a scripted store") {
  std::vector<json::value> actual;
  for (const Scenario& scenario : scenarios()) {
    rt::Task<json::value> task = run(scenario.name, scenario.setup, scenario.body);
    json::value row = co_await std::move(task);
    actual.push_back(std::move(row));
  }
  const json::value recorded = rt::jsonParse(RECORDED);
  std::size_t compared = 0;
  for (const json::value& row : recorded.as_array()) {
    if (!rt::hasOwn(row.as_object(), "calls")) continue;
    ASSERT(compared < actual.size());
    if (!rt::deepEqual(actual[compared], row)) EXPECT_EQ(actual[compared], row);
    compared += 1;
  }
  EXPECT_EQ(compared, actual.size());
}

ASYNC_TEST("differential: retrySqliteBusy gives up at its 5 s deadline like Bun") {
  const double start = rt::dateNow();
  int attempts = 0;
  std::function<rt::Task<void>()> alwaysLocked = [&attempts]() -> rt::Task<void> {
    attempts += 1;
    throw rt::JsError("database is locked");
    co_return;
  };
  auto retried = retrySqliteBusy<void>(std::move(alwaysLocked));
  EXPECT_REJECTS_MESSAGE(std::move(retried), rt::JsError, "database is locked");
  // Recorded from Bun with Date.now and Bun.sleep replaced by the same fake clock.
  EXPECT_EQ(attempts, 501);
  EXPECT_EQ(rt::dateNow() - start, 5000.0);
  int syncAttempts = 0;
  std::function<rt::Task<std::string>()> eventually = [&syncAttempts]() -> rt::Task<std::string> {
    syncAttempts += 1;
    if (syncAttempts < 3) throw rt::JsError("database is locked");
    co_return std::string("sync");
  };
  auto succeeding = retrySqliteBusy<std::string>(std::move(eventually));
  const std::string value = co_await std::move(succeeding);
  EXPECT_EQ(value, "sync");
  EXPECT_EQ(syncAttempts, 3);
}

ASYNC_TEST("the guard over MemoryKv keeps the store's observable behaviour") {
  auto memory = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  std::unique_ptr<rt::Kv> kv = guardNativeKv(memory);
  const rt::KvCommitResult written = co_await kv->set(rt::KvKey{"a", "b"}, rt::KvValue("value"));
  EXPECT(written.ok);
  co_await kv->set(rt::KvKey{"a"}, rt::KvValue("prefix row"));
  const rt::KvEntryMaybe<> entry = co_await kv->get(rt::KvKey{"a", "b"});
  EXPECT_EQ(entry.value.asString(), "value");
  rt::KvListSelector selector;
  selector.prefix = rt::KvKey{"a"};
  std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector, rt::KvListOptions{});
  const std::vector<rt::KvEntry<>> rows = co_await rt::kvListToArray(*iterator);
  ASSERT_EQ(rows.size(), std::size_t(1));
  EXPECT_EQ(rows[0].value.asString(), "value");
  EXPECT_EQ(iterator->cursor(), "");
  std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
  operation->sum(rt::KvKey{"a", "b"}, 1);
  EXPECT_REJECTS_MESSAGE(operation->commit(), rt::TypeError, "Atomic arithmetic requires an absent key or KvU64 value");
  co_await kv->delete_(rt::KvKey{"a", "b"});
  const rt::KvEntryMaybe<> removed = co_await kv->get(rt::KvKey{"a", "b"});
  EXPECT(!removed.versionstamp.has_value());
  kv->close();
}
