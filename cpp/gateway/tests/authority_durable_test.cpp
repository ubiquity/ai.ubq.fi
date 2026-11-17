// Port of tests/authority-durable.test.ts.
//
// The TypeScript test runs DurableAuthorityStore on bun:sqlite databases (files
// under a temporary directory, or ":memory:"). The C++ runtime links no SQLite,
// so `Database` below is an in-memory stand-in for one SQLite file: it executes
// exactly the statements the store and this test issue, with SQLite's results,
// and rolls a transaction back when its callback throws. Reopening a "file"
// is a second connection to the same FakeFile.
#include "uos/authority/admission_state.hpp"
#include "uos/authority/authority_rpc.hpp"
#include "uos/authority/contracts.hpp"
#include "uos/authority/durable_store.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {
  using namespace uos;
  using namespace uos::authority::contracts;
  using uos::authority::admission_state::createAuthorityState;
  using uos::authority::admission_state::PortableAuthority;
  using uos::authority::admission_state::Selection;
  using uos::authority::admission_state::Usage;
  using uos::authority::authority_rpc::AuthorityReply;
  using uos::authority::authority_rpc::AuthorityRpcClient;
  using uos::authority::authority_rpc::AuthorityRpcServer;
  using uos::authority::durable_store::AuthoritySqlStorage;
  using uos::authority::durable_store::DurableAuthorityStore;

  // `await assert.rejects(promise, /pattern/)`: String(error) must match.
  template <class T> rt::Task<void> rejectsMatching(rt::Task<T> task, const char* pattern, const char* file, int line) {
    bool rejected = false;
    std::string text;
    try {
      co_await std::move(task);
    } catch (...) {
      rejected = true;
      text = rt::reasonToString(std::current_exception());
    }
    if (!rejected) rt::test::report(file, line, std::string("expected a rejection matching /") + pattern + "/", false);
    else if (!rt::Regex(pattern).test(text)) rt::test::report(file, line, std::string("expected a rejection matching /") + pattern + "/, got " + text, false);
  }
#define ASSERT_REJECTS(task, pattern) co_await rejectsMatching((task), pattern, __FILE__, __LINE__)

  // `assert.throws(() => ..., /pattern/)`
  void throwsMatching(const std::function<void()>& body, const char* pattern, const char* file, int line) {
    try {
      body();
    } catch (...) {
      const std::string text = rt::reasonToString(std::current_exception());
      if (!rt::Regex(pattern).test(text)) rt::test::report(file, line, std::string("expected a throw matching /") + pattern + "/, got " + text, false);
      return;
    }
    rt::test::report(file, line, std::string("expected a throw matching /") + pattern + "/", false);
  }
#define ASSERT_THROWS(body, pattern) throwsMatching([&] { body; }, pattern, __FILE__, __LINE__)

  // ---- the SQLite stand-in

  struct FakeFile {
    bool table = false;
    std::optional<json::object> row;
  };

  json::value bindingJson(const AuthoritySqlStorage::Binding& binding) {
    if (std::holds_alternative<std::nullptr_t>(binding)) return nullptr;
    if (const auto* text = std::get_if<std::string>(&binding)) return json::value(*text);
    return rt::makeNumber(std::get<double>(binding));
  }

  const std::string CREATE =
    "CREATE TABLE IF NOT EXISTS uos_authority_atom (id INTEGER PRIMARY KEY CHECK(id = 1), schema_version INTEGER NOT NULL, revision INTEGER NOT NULL, state_json TEXT NOT NULL)";

  class Database {
  public:
    explicit Database(std::shared_ptr<FakeFile> file_) : file(std::move(file_)) {}
    // `database.query(query).all(...bindings)`
    std::vector<json::object> all(std::string_view query, const std::vector<AuthoritySqlStorage::Binding>& bindings = {}) {
      if (closed) throw std::logic_error("Cannot use a closed database");
      FakeFile& data = *file;
      const auto row = [&data](std::initializer_list<const char*> columns) {
        std::vector<json::object> rows;
        if (!data.row) return rows;
        json::object out;
        for (const char* column : columns) out[column] = data.row->at(column);
        rows.push_back(std::move(out));
        return rows;
      };
      if (query == CREATE) {
        data.table = true;
        return {};
      }
      if (!data.table) throw std::logic_error("no such table: uos_authority_atom");
      if (query == "SELECT schema_version, revision, state_json FROM uos_authority_atom WHERE id = 1") return row({"schema_version", "revision", "state_json"});
      if (query == "SELECT schema_version FROM uos_authority_atom") return row({"schema_version"});
      if (query == "SELECT state_json FROM uos_authority_atom") return row({"state_json"});
      if (query == "INSERT INTO uos_authority_atom (id, schema_version, revision, state_json) VALUES (1, ?, 0, ?)") {
        if (data.row) throw std::logic_error("UNIQUE constraint failed: uos_authority_atom.id");
        json::object inserted;
        inserted["schema_version"] = bindingJson(bindings.at(0));
        inserted["revision"] = 0;
        inserted["state_json"] = bindingJson(bindings.at(1));
        data.row = std::move(inserted);
        return {};
      }
      if (query == "UPDATE uos_authority_atom SET revision = ?, state_json = ? WHERE id = 1 AND revision = ? RETURNING revision") {
        if (!data.row || !rt::deepEqual(data.row->at("revision"), bindingJson(bindings.at(2)))) return {};
        (*data.row)["revision"] = bindingJson(bindings.at(0));
        (*data.row)["state_json"] = bindingJson(bindings.at(1));
        return row({"revision"});
      }
      if (query == "UPDATE uos_authority_atom SET schema_version = 2") {
        if (data.row) (*data.row)["schema_version"] = 2;
        return {};
      }
      if (query == "UPDATE uos_authority_atom SET schema_version = 1, state_json = ?") {
        if (data.row) {
          (*data.row)["schema_version"] = 1;
          (*data.row)["state_json"] = bindingJson(bindings.at(0));
        }
        return {};
      }
      if (query == "UPDATE uos_authority_atom SET state_json = ?") {
        if (data.row) (*data.row)["state_json"] = bindingJson(bindings.at(0));
        return {};
      }
      throw std::logic_error("unsupported statement: " + std::string(query));
    }
    // `database.query(query).run(...bindings)`
    void run(std::string_view query, const std::vector<AuthoritySqlStorage::Binding>& bindings = {}) { all(query, bindings); }
    // `database.query(query).get()`
    std::optional<json::object> get(std::string_view query) {
      std::vector<json::object> rows = all(query);
      if (rows.empty()) return std::nullopt;
      return rows[0];
    }
    // `database.transaction(callback)()`
    void transaction(const std::function<void()>& callback) {
      const FakeFile saved = *file;
      try {
        callback();
      } catch (...) {
        *file = saved;
        throw;
      }
    }
    void close() { closed = true; }

  private:
    std::shared_ptr<FakeFile> file;
    bool closed = false;
  };

  class Storage : public AuthoritySqlStorage, public AuthoritySqlStorage::Sql {
  public:
    Storage(std::shared_ptr<Database> database_, std::function<void()> after_) : database(std::move(database_)), after(std::move(after_)) {}
    AuthoritySqlStorage::Sql& sql() override { return *this; }
    Cursor exec(std::string_view query, std::vector<Binding> bindings) override { return Cursor(database->all(query, bindings)); }
    void transactionSync(const std::function<void()>& callback) override {
      database->transaction([&] {
        callback();
        after();
      });
    }

  private:
    std::shared_ptr<Database> database;
    std::function<void()> after;
  };

  std::shared_ptr<AuthoritySqlStorage> storage(const std::shared_ptr<Database>& database, std::function<void()> after = [] {}) {
    return std::make_shared<Storage>(database, std::move(after));
  }

  // ---- fixtures

  const AuthorityScope scope{.namespace_ = "synthetic-durable", .epoch = 1};
  const double maximumBytes = 262'144;

  AuthorityState initial(bool paid = false) {
    const AccountBinding account{
      .accountId = "a",
      .fingerprint = "fp",
      .credentialGeneration = 1,
      .routingGeneration = 1,
      .resetGeneration = 1,
      .overageVersion = 1,
      .overageEnabled = false,
      .enabled = true,
      .models = {"fixture"},
      .proof = paid ? std::optional<AccountProof>(AccountProof{.kind = "quota_exhausted", .model = "fixture", .fingerprint = "fp", .observedAtMs = 1, .validUntilMs = 10'000})
                    : std::nullopt,
    };
    std::vector<PrincipalPolicy> policies;
    for (const char* principalId : {"p", "q"}) {
      policies.push_back(PrincipalPolicy{
        .principalId = principalId,
        .version = 1,
        .deleted = false,
        .windowResetAtMs = 10'000,
        .requestLimit = 2,
        .kernelLimit = 2,
        .concurrencyLimit = 2,
        .paidLimitMicrocredits = 100,
      });
    }
    return createAuthorityState(scope, {account}, policies);
  }

  AdmitInput input(const std::string& requestId, const std::string& principalId = "p") {
    return AdmitInput{
      .scope = scope,
      .requestId = requestId,
      .principalId = principalId,
      .policyVersion = 1,
      .digest = requestId,
      .route = "/v1/responses",
      .model = "fixture",
      .nowMs = 100,
    };
  }

  std::shared_ptr<AuthorityRpcClient> client(const std::shared_ptr<DurableAuthorityStore>& store) {
    return std::make_shared<AuthorityRpcClient>(std::make_shared<AuthorityRpcServer>(std::make_shared<PortableAuthority>(store, scope), maximumBytes));
  }

  std::shared_ptr<DurableAuthorityStore> durable(const std::shared_ptr<Database>& database, const AuthorityState& state, std::function<void()> after = [] {}) {
    return std::make_shared<DurableAuthorityStore>(storage(database, std::move(after)), state, maximumBytes);
  }

  // `async () => await store.read()` / `async () => await store.compareAndSwap(...)`
  rt::Task<AuthoritySnapshot> readOf(std::shared_ptr<DurableAuthorityStore> store) {
    rt::Promise<AuthoritySnapshot> reading = store->read();
    AuthoritySnapshot snapshot = co_await reading.wait();
    co_return snapshot;
  }
  rt::Task<bool> swapOf(std::shared_ptr<DurableAuthorityStore> store, double revision, AuthorityState state) {
    rt::Promise<bool> swapping = store->compareAndSwap(revision, std::move(state));
    const bool swapped = co_await swapping.wait();
    co_return swapped;
  }

  const std::string WINDOW = R"(["p",10000])";

  json::object commandOf(std::string operation, json::array arguments) {
    json::object out;
    out["operation"] = std::move(operation);
    out["arguments"] = std::move(arguments);
    return out;
  }

  const json::value invalidOperation = rt::jsonParse(R"({"error":{"code":"invalid_authority_operation","status":400}})");

  std::optional<std::string> accountIdOf(const std::optional<AccountBinding>& value) { return value ? std::optional<std::string>(value->accountId) : std::nullopt; }

  template <class T> constexpr bool hasRead = requires(T& value) { value.read(); };
  template <class T> constexpr bool hasCompareAndSwap = requires(T& value) { value.compareAndSwap(0, AuthorityState{}); };
  template <class T> constexpr bool hasPublish = requires(T& value) { value.publish(); };
}

ASYNC_TEST("two authority clients contend on one actual SQLite atom without exceeding principal quota") {
  const auto database = std::make_shared<Database>(std::make_shared<FakeFile>());
  const auto store = durable(database, initial());
  const auto left = client(store);
  const auto right = client(durable(database, initial()));
  std::vector<rt::Promise<AdmissionHandle>> admissions;
  admissions.push_back(rt::Promise(left->admit(input("one"))));
  admissions.push_back(rt::Promise(right->admit(input("two"))));
  admissions.push_back(rt::Promise(left->admit(input("three"))));
  const auto outcomes = co_await rt::allSettled(std::move(admissions)).wait();
  EXPECT_EQ(std::count_if(outcomes.begin(), outcomes.end(), [](const auto& result) { return result.status == "fulfilled"; }), 2);
  rt::Task<AdmissionHandle> admitting = right->admit(input("other", "q"));
  const AdmissionHandle other = co_await std::move(admitting);
  rt::Task<Selection> selecting = right->selection(other);
  const Selection selected = co_await std::move(selecting);
  EXPECT_EQ(accountIdOf(selected.account), std::optional<std::string>("a"));
  rt::Task<AuthoritySnapshot> reading = readOf(store);
  const AuthoritySnapshot saved = co_await std::move(reading);
  EXPECT(saved.state.active == (ActiveAccount{.accountId = "a", .fingerprint = "fp", .generation = 1}));
  const PrincipalWindow* window = saved.state.windows.get(WINDOW);
  ASSERT(window != nullptr);
  EXPECT_EQ(window->active, 2);
  EXPECT_EQ(window->reserved, 2);
  EXPECT_EQ(window->dispatched, 0);
  EXPECT_EQ(saved.state.admissions.keys().size(), 3u);
  database->close();
}

ASYNC_TEST("restart preserves dispatch intent and refuses duplicate dispatch or an uncertain refund") {
  const auto file = std::make_shared<FakeFile>();
  auto database = std::make_shared<Database>(file);
  auto store = durable(database, initial(true));
  auto authority = client(store);
  rt::Task<AdmissionHandle> admitting = authority->admit(input("pending"));
  const AdmissionHandle handle = co_await std::move(admitting);
  const AttemptInput attempt{.attemptId = "paid", .provider = "surplus", .maximumExposureMicrocredits = 40, .account = std::nullopt};
  rt::Task<void> reserving = authority->reserveAttempt(handle, attempt, 100);
  co_await std::move(reserving);
  rt::Task<bool> beginning = authority->beginDispatch(handle, "paid", 100);
  const bool begun = co_await std::move(beginning);
  EXPECT_EQ(begun, true);
  rt::Task<AuthoritySnapshot> readingBefore = readOf(store);
  const AuthoritySnapshot before = co_await std::move(readingBefore);
  database->close();
  database = std::make_shared<Database>(file);
  store = durable(database, initial(true));
  authority = client(store);
  rt::Task<AuthoritySnapshot> readingAfter = readOf(store);
  const AuthoritySnapshot after = co_await std::move(readingAfter);
  EXPECT(after == before);
  rt::Task<AdmissionHandle> again = authority->admit(input("pending"));
  const AdmissionHandle repeated = co_await std::move(again);
  EXPECT(repeated == handle);
  rt::Task<void> reservingAgain = authority->reserveAttempt(handle, attempt, 100);
  co_await std::move(reservingAgain);
  rt::Task<bool> beginningAgain = authority->beginDispatch(handle, "paid", 100);
  const bool begunAgain = co_await std::move(beginningAgain);
  EXPECT_EQ(begunAgain, false);
  rt::Task<void> releasing = authority->releaseBeforeDispatch(handle, "paid");
  ASSERT_REJECTS(std::move(releasing), "dispatch_indeterminate");
  rt::Task<void> cancelling = authority->cancelUndispatched(handle);
  ASSERT_REJECTS(std::move(cancelling), "dispatch_indeterminate");
  rt::Task<std::string> settling = authority->settle(handle);
  const std::string settled = co_await std::move(settling);
  EXPECT_EQ(settled, "pending");
  rt::Task<AuthoritySnapshot> readingLast = readOf(store);
  const AuthoritySnapshot last = co_await std::move(readingLast);
  const PrincipalWindow* window = last.state.windows.get(WINDOW);
  ASSERT(window != nullptr);
  EXPECT_EQ(window->active, 1);
  EXPECT_EQ(window->reserved, 1);
  EXPECT_EQ(window->paidReservedMicrocredits, 40);
  database->close();
}

ASYNC_TEST("durable terminal and late usage replay settle once without erasing unknown provider exposure") {
  const auto file = std::make_shared<FakeFile>();
  auto database = std::make_shared<Database>(file);
  auto store = durable(database, initial(true));
  auto authority = client(store);
  rt::Task<AdmissionHandle> admitting = authority->admit(input("billing"));
  const AdmissionHandle handle = co_await std::move(admitting);
  const AttemptInput attempt{.attemptId = "paid", .provider = "surplus", .maximumExposureMicrocredits = 40, .account = std::nullopt};
  rt::Task<void> reserving = authority->reserveAttempt(handle, attempt, 100);
  co_await std::move(reserving);
  rt::Task<bool> beginning = authority->beginDispatch(handle, "paid", 100);
  co_await std::move(beginning);
  rt::Task<void> confirming = authority->confirmDispatched(handle, "paid");
  co_await std::move(confirming);
  const TerminalEvidence terminal{.outcome = "completed", .providerRequestId = "synthetic-charge", .spendMicrocredits = std::nullopt, .fallback = std::nullopt};
  rt::Task<void> terminating = authority->observeTerminal(handle, "paid", terminal);
  co_await std::move(terminating);
  database->close();
  database = std::make_shared<Database>(file);
  store = durable(database, initial(true));
  authority = client(store);
  rt::Task<void> replaying = authority->observeTerminal(handle, "paid", terminal);
  co_await std::move(replaying);
  rt::Task<void> delivering = authority->observeDelivery(handle, "unobserved");
  co_await std::move(delivering);
  rt::Task<std::string> settling = authority->settle(handle);
  const std::string settled = co_await std::move(settling);
  EXPECT_EQ(settled, "settled");
  {
    rt::Task<AuthoritySnapshot> reading = readOf(store);
    const AuthoritySnapshot saved = co_await std::move(reading);
    const PrincipalWindow* window = saved.state.windows.get(WINDOW);
    ASSERT(window != nullptr);
    EXPECT_EQ(window->active, 0);
    EXPECT_EQ(window->dispatched, 1);
    EXPECT_EQ(window->kernelCompleted, 1);
    EXPECT_EQ(window->paidReservedMicrocredits, 40);
  }
  const Usage usage{.providerRequestId = "synthetic-charge", .spendMicrocredits = 7};
  rt::Task<void> using1 = authority->observeUsage(handle, "paid", usage);
  co_await std::move(using1);
  rt::Task<void> using2 = authority->observeUsage(handle, "paid", usage);
  co_await std::move(using2);
  rt::Task<std::string> settlingAgain = authority->settle(handle);
  const std::string settledAgain = co_await std::move(settlingAgain);
  EXPECT_EQ(settledAgain, "settled");
  {
    rt::Task<AuthoritySnapshot> reading = readOf(store);
    const AuthoritySnapshot saved = co_await std::move(reading);
    const PrincipalWindow* window = saved.state.windows.get(WINDOW);
    ASSERT(window != nullptr);
    EXPECT_EQ(window->active, 0);
    EXPECT_EQ(window->dispatched, 1);
    EXPECT_EQ(window->kernelCompleted, 1);
    EXPECT_EQ(window->paidReservedMicrocredits, 0);
    EXPECT_EQ(window->paidSettledMicrocredits, 7);
  }
  const Usage conflicting{.providerRequestId = "synthetic-charge", .spendMicrocredits = 8};
  rt::Task<void> using3 = authority->observeUsage(handle, "paid", conflicting);
  ASSERT_REJECTS(std::move(using3), "usage_evidence_conflict");
  database->close();
}

ASYNC_TEST("failed SQLite transaction rolls back the whole atom and stale CAS never overwrites a successor") {
  const auto database = std::make_shared<Database>(std::make_shared<FakeFile>());
  auto fail = std::make_shared<bool>(false);
  const auto store = durable(database, initial(), [fail] {
    if (*fail) throw rt::JsError("synthetic commit failure");
  });
  rt::Task<AuthoritySnapshot> readingBefore = readOf(store);
  const AuthoritySnapshot before = co_await std::move(readingBefore);
  *fail = true;
  AuthorityState nine = before.state;
  nine.sequence = 9;
  rt::Task<bool> failing = swapOf(store, before.revision, nine);
  ASSERT_REJECTS(std::move(failing), "synthetic commit failure");
  rt::Task<AuthoritySnapshot> readingAfter = readOf(store);
  const AuthoritySnapshot after = co_await std::move(readingAfter);
  EXPECT(after == before);
  *fail = false;
  AuthorityState two = before.state;
  two.sequence = 2;
  rt::Task<bool> swapping = swapOf(store, before.revision, two);
  const bool swapped = co_await std::move(swapping);
  EXPECT_EQ(swapped, true);
  AuthorityState stale = before.state;
  stale.sequence = 99;
  rt::Task<bool> staleSwap = swapOf(store, before.revision, stale);
  const bool staleSwapped = co_await std::move(staleSwap);
  EXPECT_EQ(staleSwapped, false);
  rt::Task<AuthoritySnapshot> readingLast = readOf(store);
  const AuthoritySnapshot last = co_await std::move(readingLast);
  EXPECT_EQ(last.state.sequence, 2);
  database->close();
}

ASYNC_TEST("corrupt schema, malformed state, wrong scope and oversized snapshots fail closed without reset") {
  const auto database = std::make_shared<Database>(std::make_shared<FakeFile>());
  const auto store = durable(database, initial());
  rt::Task<AuthoritySnapshot> readingBefore = readOf(store);
  const AuthoritySnapshot before = co_await std::move(readingBefore);
  AuthorityState otherScope = before.state;
  otherScope.scope = AuthorityScope{.namespace_ = scope.namespace_, .epoch = 2};
  rt::Task<bool> scoped = swapOf(store, before.revision, otherScope);
  ASSERT_REJECTS(std::move(scoped), "namespace_or_epoch_mismatch");
  AuthorityState oversized = before.state;
  oversized.active = ActiveAccount{.accountId = rt::repeat("x", maximumBytes), .fingerprint = "fp", .generation = 1};
  rt::Task<bool> large = swapOf(store, before.revision, oversized);
  ASSERT_REJECTS(std::move(large), "authority_state_too_large");
  rt::Task<AuthoritySnapshot> readingAfter = readOf(store);
  const AuthoritySnapshot after = co_await std::move(readingAfter);
  EXPECT(after == before);
  database->run("UPDATE uos_authority_atom SET schema_version = 2");
  ASSERT_THROWS(durable(database, initial()), "authority_schema_mismatch");
  const std::optional<json::object> schema = database->get("SELECT schema_version FROM uos_authority_atom");
  EXPECT(schema && rt::deepEqual(schema->at("schema_version"), json::value(2)));
  json::object scopeOnly;
  scopeOnly["scope"] = toJson(scope);
  database->run("UPDATE uos_authority_atom SET schema_version = 1, state_json = ?", {rt::jsonStringify(scopeOnly)});
  ASSERT_THROWS(durable(database, initial()), "invalid_authority_state");
  database->run("UPDATE uos_authority_atom SET state_json = ?", {std::string("{")});
  rt::Task<AuthoritySnapshot> broken = readOf(store);
  ASSERT_REJECTS(std::move(broken), "invalid_authority_state");
  database->close();
}

ASYNC_TEST("data-plane RPC cannot read or replace state and preserves structured quota and namespace refusal") {
  const auto database = std::make_shared<Database>(std::make_shared<FakeFile>());
  const auto store = durable(database, initial());
  const auto server = std::make_shared<AuthorityRpcServer>(std::make_shared<PortableAuthority>(store, scope), 2'000);
  const auto authority = std::make_shared<AuthorityRpcClient>(server);
  const std::vector<std::string> operations{"read", "compareAndSwap", "publish", "setPolicy", "__proto__"};
  for (const std::string& operation : operations) {
    const json::value command = rt::jsonParse(rt::jsonStringify(commandOf(operation, json::array{})));
    rt::Task<AuthorityReply> invoking = server->invoke(command);
    const AuthorityReply reply = co_await std::move(invoking);
    EXPECT(rt::deepEqual(reply, invalidOperation));
  }
  AdmitInput wrong = input("wrong");
  wrong.scope.epoch = 2;
  rt::Task<AdmissionHandle> wrongScope = authority->admit(wrong);
  ASSERT_REJECTS(std::move(wrongScope), "namespace_or_epoch_mismatch");
  AdmitInput huge = input("huge");
  huge.digest = rt::repeat("x", 3'000);
  rt::Task<AdmissionHandle> hugeCommand = authority->admit(huge);
  ASSERT_REJECTS(std::move(hugeCommand), "authority_command_too_large");
  rt::Task<AuthoritySnapshot> reading = readOf(store);
  const AuthoritySnapshot saved = co_await std::move(reading);
  EXPECT_EQ(saved.state.admissions.size(), 0u);
  rt::Task<AdmissionHandle> one = authority->admit(input("one"));
  co_await std::move(one);
  rt::Task<AdmissionHandle> two = authority->admit(input("two"));
  co_await std::move(two);
  rt::Task<AdmissionHandle> three = authority->admit(input("three"));
  ASSERT_REJECTS(std::move(three), "principal_quota_exceeded");
  // `"read" in authority || "compareAndSwap" in authority || "publish" in authority` is false.
  EXPECT(!hasRead<AuthorityRpcClient>);
  EXPECT(!hasCompareAndSwap<AuthorityRpcClient>);
  EXPECT(!hasPublish<AuthorityRpcClient>);
  database->close();
}

ASYNC_TEST("RPC refuses uncorrelated or invalid usage identifiers without clearing unknown paid exposure") {
  const std::vector<json::value> identifiers{nullptr, "", " ", 0, json::object{}};
  for (const json::value& providerRequestId : identifiers) {
    const auto database = std::make_shared<Database>(std::make_shared<FakeFile>());
    const auto store = durable(database, initial(true));
    const auto server = std::make_shared<AuthorityRpcServer>(std::make_shared<PortableAuthority>(store, scope), maximumBytes);
    const auto authority = std::make_shared<AuthorityRpcClient>(server);
    rt::Task<AdmissionHandle> admitting = authority->admit(input("uncorrelated"));
    const AdmissionHandle handle = co_await std::move(admitting);
    const AttemptInput attempt{.attemptId = "paid", .provider = "surplus", .maximumExposureMicrocredits = 40, .account = std::nullopt};
    rt::Task<void> reserving = authority->reserveAttempt(handle, attempt, 100);
    co_await std::move(reserving);
    rt::Task<bool> beginning = authority->beginDispatch(handle, "paid", 100);
    co_await std::move(beginning);
    rt::Task<void> confirming = authority->confirmDispatched(handle, "paid");
    co_await std::move(confirming);
    const TerminalEvidence terminal{.outcome = "completed", .providerRequestId = std::nullopt, .spendMicrocredits = std::nullopt, .fallback = std::nullopt};
    rt::Task<void> terminating = authority->observeTerminal(handle, "paid", terminal);
    co_await std::move(terminating);
    json::object usage;
    usage["providerRequestId"] = providerRequestId;
    usage["spendMicrocredits"] = 0;
    const json::value command = rt::jsonParse(rt::jsonStringify(commandOf("observeUsage", json::array{toJson(handle), "paid", usage})));
    rt::Task<AuthorityReply> invoking = server->invoke(command);
    const AuthorityReply reply = co_await std::move(invoking);
    rt::Task<AuthoritySnapshot> reading = readOf(store);
    const AuthoritySnapshot saved = co_await std::move(reading);
    EXPECT(rt::deepEqual(reply, invalidOperation));
    EXPECT_EQ(saved.state.windows.get(WINDOW)->paidReservedMicrocredits, 40);
    EXPECT_EQ(saved.state.admissions.get("uncorrelated")->attempts.get("paid")->billingSettled, false);
    database->close();
  }
}

ASYNC_TEST("RPC validates every operation's complete payload before any authority mutation") {
  const auto database = std::make_shared<Database>(std::make_shared<FakeFile>());
  const auto store = durable(database, initial());
  const auto server = std::make_shared<AuthorityRpcServer>(std::make_shared<PortableAuthority>(store, scope), maximumBytes);
  const auto shapeClient = client(store);
  rt::Task<AdmissionHandle> admitting = shapeClient->admit(input("shape"));
  const AdmissionHandle handle = co_await std::move(admitting);
  const json::value handleJson = toJson(handle);
  json::object badHandle = handleJson.as_object();
  badHandle["fence"] = -1;
  const auto bad = [](const std::function<void(json::object&)>& change) {
    json::object value = toJson(input("bad")).as_object();
    change(value);
    return value;
  };
  const auto terminal = [](json::value spend) {
    json::object value;
    value["outcome"] = "completed";
    value["providerRequestId"] = nullptr;
    value["spendMicrocredits"] = std::move(spend);
    value["fallback"] = nullptr;
    return value;
  };
  const auto attemptJson = [](const char* provider, double exposure) {
    json::object value;
    value["attemptId"] = "a";
    value["provider"] = provider;
    value["maximumExposureMicrocredits"] = rt::makeNumber(exposure);
    value["account"] = nullptr;
    return value;
  };
  json::object unknownProof;
  unknownProof["kind"] = "unknown";
  unknownProof["model"] = "fixture";
  unknownProof["fingerprint"] = "fp";
  unknownProof["observedAtMs"] = 1;
  unknownProof["validUntilMs"] = 2;
  json::object negativeUsage;
  negativeUsage["providerRequestId"] = "id";
  negativeUsage["spendMicrocredits"] = -1;
  const std::vector<json::object> commands{
    commandOf("admit", json::array{bad([](json::object& value) { value["requestId"] = nullptr; })}),
    commandOf("admit", json::array{bad([](json::object& value) { value["nowMs"] = 0.5; })}),
    commandOf("admit", json::array{bad([](json::object& value) { value["route"] = "/arbitrary"; })}),
    commandOf("reserveAttempt", json::array{handleJson, attemptJson("arbitrary", 0), 100}),
    commandOf("reserveAttempt", json::array{handleJson, attemptJson("codex", -1), 100}),
    commandOf("beginDispatch", json::array{handleJson, "a", "100"}),
    commandOf("selection", json::array{badHandle}),
    commandOf("reselectSubscription", json::array{handleJson, rt::makeNumber(rt::Number::MAX_SAFE_INTEGER + 1)}),
    commandOf("confirmDispatched", json::array{badHandle, "a"}),
    commandOf("releaseBeforeDispatch", json::array{handleJson, nullptr}),
    commandOf("observeProof", json::array{handleJson, "a", unknownProof}),
    commandOf("observeTerminal", json::array{handleJson, "a", terminal(json::value(0.5))}),
    commandOf("observeTerminal", json::array{handleJson, "a", terminal(json::value(0)), "true"}),
    commandOf("observeDelivery", json::array{handleJson, "assumed_delivered"}),
    commandOf("observeUsage", json::array{handleJson, "a", negativeUsage}),
    commandOf("resetEligible", json::array{handleJson, -1}),
    commandOf("settle", json::array{badHandle}),
    commandOf("cancelUndispatched", json::array{badHandle}),
    commandOf("admit", json::array{bad([](json::object& value) { value["forgedQuota"] = 999; })}),
  };
  rt::Task<AuthoritySnapshot> readingBefore = readOf(store);
  const AuthoritySnapshot before = co_await std::move(readingBefore);
  for (const json::object& value : commands) {
    const json::value command = rt::jsonParse(rt::jsonStringify(value));
    rt::Task<AuthorityReply> invoking = server->invoke(command);
    const AuthorityReply reply = co_await std::move(invoking);
    EXPECT(rt::deepEqual(reply, invalidOperation));
    rt::Task<AuthoritySnapshot> reading = readOf(store);
    const AuthoritySnapshot after = co_await std::move(reading);
    EXPECT(after == before);
  }
  database->close();
}

ASYNC_TEST("restore and CAS reject negative, fractional and unsafe persisted accounting fields without reseeding") {
  const auto database = std::make_shared<Database>(std::make_shared<FakeFile>());
  const auto store = durable(database, initial(true));
  const auto authority = client(store);
  rt::Task<AdmissionHandle> admitting = authority->admit(input("corrupt"));
  const AdmissionHandle handle = co_await std::move(admitting);
  rt::Task<AuthoritySnapshot> readingBefore = readOf(store);
  const AuthoritySnapshot before = co_await std::move(readingBefore);
  std::vector<std::function<void(AuthorityState&, double)>> candidates{
    [](AuthorityState& state, double value) { state.sequence = value; },
    [](AuthorityState& state, double value) {
      AccountBinding changed = state.accounts[0];
      changed.credentialGeneration = value;
      state.accounts[0] = changed;
    },
    [](AuthorityState& state, double value) {
      PrincipalPolicy changed = *state.policies.get("p");
      changed.paidLimitMicrocredits = value;
      state.policies.set("p", changed);
    },
    [handle](AuthorityState& state, double value) {
      AdmissionHandle changed = handle;
      changed.fence = value;
      state.admissions.get("corrupt")->handle = changed;
    },
  };
  for (double PrincipalWindow::*field : {&PrincipalWindow::reserved, &PrincipalWindow::dispatched, &PrincipalWindow::kernelReserved, &PrincipalWindow::kernelCompleted,
                                          &PrincipalWindow::active, &PrincipalWindow::paidReservedMicrocredits, &PrincipalWindow::paidSettledMicrocredits}) {
    candidates.push_back([field](AuthorityState& state, double value) { state.windows.get(WINDOW)->*field = value; });
  }
  for (const auto& mutate : candidates) {
    const std::vector<double> values{-100.0, 0.5, rt::Number::MAX_SAFE_INTEGER + 1};
    for (const double value : values) {
      AuthorityState corrupted = before.state;
      mutate(corrupted, value);
      rt::Task<bool> swapping = swapOf(store, before.revision, corrupted);
      ASSERT_REJECTS(std::move(swapping), "invalid_authority_state");
      rt::Task<AuthoritySnapshot> reading = readOf(store);
      const AuthoritySnapshot after = co_await std::move(reading);
      EXPECT(after == before);
      const std::string encoded = rt::jsonStringify(toJson(corrupted));
      database->run("UPDATE uos_authority_atom SET state_json = ?", {encoded});
      ASSERT_THROWS(durable(database, initial(true)), "invalid_authority_state");
      rt::Task<AuthoritySnapshot> broken = readOf(store);
      ASSERT_REJECTS(std::move(broken), "invalid_authority_state");
      const std::optional<json::object> row = database->get("SELECT state_json FROM uos_authority_atom");
      EXPECT(row && rt::deepEqual(row->at("state_json"), json::value(encoded)));
      database->run("UPDATE uos_authority_atom SET state_json = ?", {rt::jsonStringify(toJson(before.state))});
    }
  }
  database->close();
}

ASYNC_TEST("validated RPC preserves legitimate proof, reselection, release and optional terminal arguments") {
  const auto database = std::make_shared<Database>(std::make_shared<FakeFile>());
  AuthorityState seed = initial();
  AccountBinding second = seed.accounts[0];
  second.accountId = "b";
  second.fingerprint = "b-fp";
  seed.accounts.push_back(second);
  const auto store = durable(database, seed);
  const auto server = std::make_shared<AuthorityRpcServer>(std::make_shared<PortableAuthority>(store, scope), maximumBytes);
  const auto authority = std::make_shared<AuthorityRpcClient>(server);
  rt::Task<AdmissionHandle> admitting = authority->admit(input("valid-operations"));
  const AdmissionHandle handle = co_await std::move(admitting);
  rt::Task<Selection> selecting = authority->selection(handle);
  const Selection selection = co_await std::move(selecting);
  const AttemptInput first{.attemptId = "a", .provider = "codex", .maximumExposureMicrocredits = 0, .account = selection.account};
  rt::Task<void> reserving = authority->reserveAttempt(handle, first, 100);
  co_await std::move(reserving);
  rt::Task<bool> beginning = authority->beginDispatch(handle, "a", 100);
  const bool begun = co_await std::move(beginning);
  EXPECT_EQ(begun, true);
  rt::Task<void> confirming = authority->confirmDispatched(handle, "a");
  co_await std::move(confirming);
  const AccountProof proof{.kind = "quota_exhausted", .model = "fixture", .fingerprint = "fp", .observedAtMs = 100, .validUntilMs = 1'000};
  rt::Task<void> proving = authority->observeProof(handle, "a", proof);
  co_await std::move(proving);
  const TerminalEvidence failed{.outcome = "failed", .providerRequestId = std::nullopt, .spendMicrocredits = 0, .fallback = "quota_exhausted"};
  rt::Task<void> terminating = authority->observeTerminal(handle, "a", failed, false);
  co_await std::move(terminating);
  rt::Task<bool> checking = authority->resetEligible(handle, 100);
  const bool eligible = co_await std::move(checking);
  EXPECT_EQ(eligible, false);
  rt::Task<std::optional<AccountBinding>> reselecting = authority->reselectSubscription(handle, 100);
  const std::optional<AccountBinding> selected = co_await std::move(reselecting);
  EXPECT_EQ(accountIdOf(selected), std::optional<std::string>("b"));
  const AttemptInput released{.attemptId = "released", .provider = "codex", .maximumExposureMicrocredits = 0, .account = selected};
  rt::Task<void> reservingReleased = authority->reserveAttempt(handle, released, 100);
  co_await std::move(reservingReleased);
  rt::Task<void> releasing = authority->releaseBeforeDispatch(handle, "released");
  co_await std::move(releasing);
  const AttemptInput hop{.attemptId = "b", .provider = "codex", .maximumExposureMicrocredits = 0, .account = selected};
  rt::Task<void> reservingHop = authority->reserveAttempt(handle, hop, 100);
  co_await std::move(reservingHop);
  rt::Task<bool> beginningHop = authority->beginDispatch(handle, "b", 100);
  co_await std::move(beginningHop);
  rt::Task<void> confirmingHop = authority->confirmDispatched(handle, "b");
  co_await std::move(confirmingHop);
  const TerminalEvidence evidence{.outcome = "completed", .providerRequestId = "b", .spendMicrocredits = 0, .fallback = std::nullopt};
  const json::value command = commandOf("observeTerminal", json::array{toJson(handle), "b", toJson(evidence)});
  rt::Task<AuthorityReply> invoking = server->invoke(command);
  const AuthorityReply reply = co_await std::move(invoking);
  EXPECT(rt::deepEqual(reply, json::object{}));  // { value: undefined }
  rt::Task<void> delivering = authority->observeDelivery(handle, "unobserved");
  co_await std::move(delivering);
  rt::Task<std::string> settling = authority->settle(handle);
  const std::string settled = co_await std::move(settling);
  EXPECT_EQ(settled, "settled");
  rt::Task<AuthoritySnapshot> reading = readOf(store);
  const AuthoritySnapshot saved = co_await std::move(reading);
  const PrincipalWindow* window = saved.state.windows.get(WINDOW);
  ASSERT(window != nullptr);
  EXPECT_EQ(window->dispatched, 1);
  EXPECT_EQ(window->kernelCompleted, 1);
  EXPECT_EQ(window->active, 0);
  database->close();
}
