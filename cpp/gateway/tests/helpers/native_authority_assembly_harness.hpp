#pragma once
// Original Bun test traces; only their synthetic private verifier/source callbacks are scripted.
// NativeAuthority and every SQL/accounting module execute normally against isolated real SQLite.
#include "tests/helpers/native_authority_harness.hpp"
#include "uos/authority/native_authority.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json_document.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/record.hpp"
#include <deque>
#include <map>
#include <set>

namespace uos::tests::helpers::native_authority_assembly_harness {
  namespace base = native_authority_harness;
  namespace rt = uos::rt;
  namespace contracts = authority::native_authority_contracts;
  namespace runtime = authority::native_runtime_contracts;
  namespace principal = authority::native_principal_contracts;
  namespace rpc = authority::native_authority_rpc;
  namespace engine = authority::native_authority;
  using base::text;
  using base::maybeText;
  template<class T> T decode(const json::value& value) { return json::value_to<T>(value); }
  inline const json::value& vectors() {
    static const auto value = [] {
      const std::string encoded(
#include "tests/native_authority_assembly_vectors.inc"
      );
      const auto bytes = rt::gzip::gunzipSync(rt::fromBase64(encoded));
      // The checked-in assembly traces expand to about 39 MiB as one document.
      return rt::jsonParseWithLimit({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, 40 * 1024 * 1024);
    }();
    return value;
  }
  inline json::value field(const json::value& value, const std::string& key) { const auto* member = rt::get(value, key); return member ? *member : json::value(nullptr); }
  inline std::int64_t idOf(const json::value& value, const std::string& key = "id") { return static_cast<std::int64_t>(rt::getNumber(value, key).value_or(-1)); }
  inline void equal(const json::value& actual, const json::value& expected, const std::string& label) {
    if (!rt::deepEqual(actual, expected)) { const auto diff = base::difference(actual, expected); std::fprintf(stderr, "      %s: %.5000s\n", label.c_str(), diff.c_str()); rt::test::report(__FILE__, __LINE__, "original Bun assembly trace differs", true); }
  }
  struct SourceError : rt::JsError {
    std::optional<double> status;
    explicit SourceError(const json::value& value) : rt::JsError(text(value, "message"), text(value, "name", "Error")), status(rt::getNumber(value, "status")) { code = maybeText(value, "code"); }
  };
  inline json::object errorJson(rt::Reason reason) {
    try { std::rethrow_exception(reason); }
    catch (const SourceError& error) { return {{"name", error.name}, {"message", error.message}, {"code", base::nullable(error.code)}, {"status", base::nullable(error.status)}}; }
    catch (...) { return base::errorJson(std::current_exception()); }
  }
  inline rt::Reason reasonOf(const json::value& value) { return std::make_exception_ptr(SourceError(value)); }
  inline json::value normalSql(json::value value) {
    if (value.is_array()) { for (auto& item : value.as_array()) item = normalSql(item); }
    else if (value.is_object()) for (auto& item : value.as_object()) {
      if (item.key() == "row_json" && item.value().is_string()) item.value() = rt::jsonParse(std::string(item.value().as_string()));
      else item.value() = normalSql(item.value());
    }
    return value;
  }
  // This fixture races real native WebCrypto work: its generated global revision labels
  // differ even between unchanged Bun executions. Compare their complete equality graph.
  class ConcurrentRevisionComparison {
    std::map<std::string, std::string> expectedToActual;
    std::map<std::string, std::string> actualToExpected;
    std::map<std::string, std::pair<std::string, std::string>> rowHistory;
    static std::string revision(const json::value& row) {
      const auto value = text(row, "revision");
      if (value.size() != 21 || value.front() != 'r' || !std::all_of(value.begin() + 1, value.end(), [](char c) { return c >= '0' && c <= '9'; })) throw rt::JsError("invalid generated revision");
      return value;
    }
    static bool referenceField(std::string_view name) {
      return name == "revision" || name == "versionstamp" || name == "hashRevision" || name == "poolRevision" || name == "capacityRevision" || name == "activePoolVersionstamp" || name == "expectedRevision";
    }
    static json::value normalizeWith(json::value value, const std::map<std::string, std::string>& mapping) {
      if (value.is_array()) for (auto& item : value.as_array()) item = normalizeWith(item, mapping);
      else if (value.is_object()) for (auto& item : value.as_object()) {
        if (referenceField(item.key()) && item.value().is_string()) {
          const auto found = mapping.find(std::string(item.value().as_string()));
          if (found != mapping.end()) item.value() = found->second;
        } else item.value() = normalizeWith(item.value(), mapping);
      }
      return value;
    }
  public:
    json::value normalize(json::value value) const { return normalizeWith(std::move(value), actualToExpected); }
    void compare(const json::value& actual, const json::value& expected) {
      auto forward = expectedToActual, reverse = actualToExpected; auto history = rowHistory;
      for (const auto& database : expected.as_object()) {
        const auto* actualDatabase = rt::get(actual, database.key());
        if (!actualDatabase) throw rt::JsError("missing concurrent database");
        const auto expectedRows = field(database.value(), "rows"), actualRows = field(*actualDatabase, "rows");
        const auto actualMeta = field(*actualDatabase, "meta"), expectedMeta = field(database.value(), "meta");
        if (!rt::deepEqual(actualMeta, expectedMeta)) throw rt::JsError("concurrent write count/metadata differs");
        const auto sequence = rt::getNumber(actualMeta.as_array().front(), "sequence").value();
        for (const auto& table : expectedRows.as_object()) {
          const auto* actualTable = rt::get(actualRows, table.key());
          if (!actualTable || !actualTable->is_array() || actualTable->as_array().size() != table.value().as_array().size()) throw rt::JsError("missing/extra concurrent row write");
          for (const auto& expectedRow : table.value().as_array()) {
            const auto key = text(expectedRow, "row_key"); const json::value* actualRow = nullptr;
            for (const auto& candidate : actualTable->as_array()) if (text(candidate, "row_key") == key) actualRow = &candidate;
            if (!actualRow) throw rt::JsError("missing concurrent row identity");
            const auto expectedRevision = revision(expectedRow), actualRevision = revision(*actualRow);
            if (rt::stringToNumber(actualRevision.substr(1)) > sequence || rt::stringToNumber(expectedRevision.substr(1)) > sequence) throw rt::JsError("revision exceeds write sequence");
            const auto [oldForward, insertedForward] = forward.emplace(expectedRevision, actualRevision);
            const auto [oldReverse, insertedReverse] = reverse.emplace(actualRevision, expectedRevision);
            if ((!insertedForward && oldForward->second != actualRevision) || (!insertedReverse && oldReverse->second != expectedRevision)) throw rt::JsError("inconsistent/noninjective generated revision mapping");
            const auto identity = std::string(database.key()) + "/" + std::string(table.key()) + "/" + key;
            const auto old = history.find(identity);
            if (old != history.end() && ((expectedRevision < old->second.first || actualRevision < old->second.second) || ((expectedRevision == old->second.first) != (actualRevision == old->second.second)))) throw rt::JsError("concurrent row revision ordering differs");
            history[identity] = {expectedRevision, actualRevision};
          }
        }
      }
      const auto normalized = normalizeWith(actual, reverse);
      if (!rt::deepEqual(normalized, expected)) throw rt::JsError("concurrent semantic/CAS state differs: " + base::difference(normalized, expected));
      expectedToActual = std::move(forward); actualToExpected = std::move(reverse); rowHistory = std::move(history);
    }
  };
  class Replay {
    std::string scratch = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "w07c5-native-authority-"));
    base::Isolated isolated;
    struct Callback {
      json::value event;
      bool consumed = false;
      rt::Resolvers<void> started = rt::withResolvers<void>();
      rt::Resolvers<json::value> result = rt::withResolvers<json::value>();
    };
    json::value spec;
    std::map<std::int64_t, std::shared_ptr<Callback>> callbacks;
    std::map<std::string, std::deque<std::shared_ptr<Callback>>> callbackQueues;
    std::deque<std::string> randomValues;
    rt::crypto::ScopedRandomSource random;
    std::map<std::int64_t, rt::sqlite::Database> databases;
    std::set<std::int64_t> closed;
    std::map<std::int64_t, std::shared_ptr<engine::NativeAuthority>> authorities;
    std::map<std::int64_t, rpc::NativeBoundRuntimePort> runtimePorts;
    std::map<std::int64_t, rpc::NativeRetainedHistoryPort> retainedPorts;
    std::map<std::int64_t, rt::Promise<json::value>> calls;
    std::map<std::int64_t, std::shared_ptr<principal::NativePrincipalControlInitial>> controlSources;
    std::map<std::int64_t, std::vector<std::string>> transactions;
    std::vector<std::string> paths;
    ConcurrentRevisionComparison revisions;
    std::size_t step = 0;
    bool concurrentDigestFixture() const { return text(spec, "name") == "review P1 original paid window remains charged when paid admission occurs after rollover"; }
    void compareStates(const json::value& actual, const json::value& expected, const std::string& label) {
      if (!concurrentDigestFixture()) { equal(actual, expected, label); return; }
      try { revisions.compare(actual, expected); }
      catch (const rt::JsError& error) { std::fprintf(stderr, "      %s: %.5000s\n", label.c_str(), error.message.c_str()); rt::test::report(__FILE__, __LINE__, "concurrent complete state/revision graph differs", true); }
    }
    std::shared_ptr<Callback> take(const std::string& path, const json::array& args) {
      auto& queue = callbackQueues[path];
      if (queue.empty()) throw rt::JsError("unexpected synthetic source callback " + path);
      auto callback = queue.front(); queue.pop_front();
      equal(args, field(callback->event, "args"), "callback " + path + " arguments");
      callback->consumed = true; callback->started.resolve(); return callback;
    }
    json::value sync(const std::string& path, json::array args = {}) {
      const auto callback = take(path, args);
      if (rt::getBool(callback->event, "async") == true) throw rt::JsError("async callback called synchronously: " + path);
      if (const auto* error = rt::get(callback->event, "error")) std::rethrow_exception(reasonOf(*error));
      return field(callback->event, "result");
    }
    rt::Promise<json::value> begin(const std::string& path, json::array args) {
      const auto callback = take(path, args);
      if (rt::getBool(callback->event, "async") == true) return callback->result.promise;
      if (const auto* error = rt::get(callback->event, "error")) return rt::Promise<json::value>::reject(reasonOf(*error));
      return rt::Promise<json::value>::resolve(field(callback->event, "result"));
    }
    template<class T> static rt::Task<T> awaited(rt::Promise<json::value> pending) { auto value = co_await pending.wait(); co_return decode<T>(value); }
    static rt::Task<void> awaitedVoid(rt::Promise<json::value> pending) { auto value = co_await pending.wait(); (void)value; }
    static principal::VerifiedControlIdentity controlIdentity(const json::value& value) {
      if (text(value, "kind") == "principal_resolver") return principal::VerifiedKeyAuth{text(value, "subjectId"), text(value, "contextBinding"), text(value, "intentDigest"), text(value, "principalId"), text(value, "tokenHash")};
      authority::native_api_key_audit::ApiKeyAuditContext audit;
      const auto auditValue = field(value, "auditContext"); audit.request_id = maybeText(auditValue, "request_id");
      if (const auto* auth = rt::get(auditValue, "auth"); auth && !auth->is_null()) {
        const auto method = field(*auth, "method");
        if (text(method, "kind") != "disabled" || rt::getBool(*auth, "ok") != true) throw rt::JsError("unexpected original synthetic audit identity");
        audit.auth = auth::index::AdminAuthResultOk{text(*auth, "token"), auth::index::AdminAuthMethodDisabled(), rt::getBool(*auth, "is_super_admin").value_or(false)};
      }
      principal::VerifiedKeyAdmin admin{text(value, "subjectId"), text(value, "contextBinding"), text(value, "intentDigest"), maybeText(value, "targetKeyId"), audit};
      if (const auto* token = rt::get(value, "createToken")) { if (token->is_null()) admin.createToken = nullptr; else admin.createToken = std::string(token->as_string()); }
      return admin;
    }
    static rt::Task<principal::VerifiedControlIdentity> controlVerified(rt::Promise<json::value> pending) { auto value = co_await pending.wait(); co_return controlIdentity(value); }
    static paid_fallback::index::PaidFallbackPolicyFields paidPolicy(const json::value& value) {
      const rt::record::Reader in(value, "synthetic paid policy");
      return {in.required<bool>("paid_fallback_enabled"), in.required<double>("paid_fallback_limit_microcredits"), in.required<double>("paid_fallback_spent_microcredits"), in.required<double>("paid_fallback_reserved_microcredits"), in.orNull<std::string>("paid_fallback_reservation_request_id"), in.required<std::vector<std::string>>("paid_fallback_model_ids"), in.required<double>("paid_fallback_quota_per_credit"), in.orUndefined<json::object>("paid_fallback_max_exposure_microcredits"), in.orNull<double>("paid_fallback_pricing_checked_at_ms")};
    }
    static rt::Task<paid_fallback::index::InitializePaidFallbackPolicyResult> priced(rt::Promise<json::value> pending) {
      const auto value = co_await pending.wait(); const rt::record::Reader in(value, "synthetic paid pricing");
      co_return paid_fallback::index::InitializePaidFallbackPolicyResult{in.required<std::vector<std::string>>("paid_fallback_model_ids"), in.required<double>("paid_fallback_quota_per_credit"), in.required<json::object>("paid_fallback_max_exposure_microcredits"), in.required<double>("paid_fallback_pricing_checked_at_ms")};
    }
    std::shared_ptr<principal::NativePrincipalControlInitial> controlInitial(const json::value& event) {
      const auto value = field(event, "control"); if (value.is_null()) return nullptr;
      const auto sourceId = idOf(event, "sourceId"); auto& shared = controlSources[sourceId]; if (shared) return shared;
      shared = std::make_shared<principal::NativePrincipalControlInitial>(); auto& initial = *shared;
      initial.grants = decode<std::vector<principal::NativeKeyControlGrant>>(field(value, "grants"));
      const auto records = field(value, "records");
      for (const auto& row : records.as_array()) initial.records.push_back({decode<::uos::types::ApiKeyRecord>(field(row, "record")), decode<::uos::types::ApiKeyHashRecord>(field(row, "hashRecord"))});
      initial.verifier.verify = [this](json::value privateContext, std::string intent, double now) { return controlVerified(begin("control.verifier.verify", json::array{privateContext, intent, rt::makeNumber(now)})); };
      auto& source = initial.sources; source.localDevelopmentKeyId = text(field(value, "sources"), "localDevelopmentKeyId");
      source.defaults = [this](double now) { const auto result = sync("control.sources.defaults", json::array{rt::makeNumber(now)}); return principal::NativePrincipalControlInitial::Sources::Defaults{rt::getNumber(result, "expiryMs").value(), rt::getNumber(result, "usageLimitRequests").value()}; };
      source.defaultPaidPolicy = [this] { return paidPolicy(sync("control.sources.defaultPaidPolicy")); };
      source.generateToken = [this] { return std::string(sync("control.sources.generateToken").as_string()); };
      source.hashToken = [this](std::string token) { return awaited<std::string>(begin("control.sources.hashToken", json::array{token})); };
      source.randomUUID = [this] { return std::string(sync("control.sources.randomUUID").as_string()); };
      source.audit.randomUUID = [this] { return std::string(sync("control.sources.audit.randomUUID").as_string()); };
      source.audit.now = [this] { return rt::toNumber(sync("control.sources.audit.now")); };
      source.audit.release = [this] { return std::string(sync("control.sources.audit.release").as_string()); };
      source.initializePricing = [this] { return priced(begin("control.sources.initializePricing", {})); };
      source.pricingFailure = [this](rt::Reason error) { const auto result = sync("control.sources.pricingFailure", json::array{json::object{{"$error", errorJson(error)}}}); return principal::NativeControlOutcomeRefused{rt::getNumber(result, "status").value(), text(result, "code"), text(result, "message")}; };
      if (const auto* sourceValue = rt::get(value, "sources"); sourceValue && !rt::getBool(event, "noUnrostered").value_or(false)) {
        source.allowUnrosteredUnlimited = [this](principal::VerifiedKeyAdmin admin, ::uos::types::ApiKeyRecord record) -> std::optional<bool> {
          json::object encoded{{"kind", "key_admin"}, {"subjectId", admin.subjectId}, {"contextBinding", admin.contextBinding}, {"intentDigest", admin.intentDigest}, {"targetKeyId", base::nullable(admin.targetKeyId)}};
          const auto& expected = callbackQueues["control.sources.allowUnrosteredUnlimited"];
          if (expected.empty()) throw rt::JsError("unexpected unrostered source callback");
          const auto expectedArgs = field(expected.front()->event, "args");
          equal(json::value_from(record), expectedArgs.as_array()[1], "unrostered record input");
          const auto expectedAdmin = expectedArgs.as_array()[0];
          for (const auto& item : encoded) equal(item.value(), field(expectedAdmin, std::string(item.key())), "unrostered verified identity");
          const auto result = sync("control.sources.allowUnrosteredUnlimited", expectedArgs.as_array()); return result.is_bool() ? std::optional<bool>(result.as_bool()) : std::nullopt;
        };
      }
      source.cleanupApiLogs = [this](std::string keyId) { return awaitedVoid(begin("control.sources.cleanupApiLogs", json::array{keyId})); };
      return shared;
    }
    std::shared_ptr<runtime::NativeRuntimeInitial> runtimeInitial(const json::value& event) {
      const auto value = field(event, "runtime"); if (value.is_null()) return nullptr;
      auto initial = std::make_shared<runtime::NativeRuntimeInitial>();
      initial->grants = decode<std::vector<runtime::NativeRuntimeGrant>>(field(value, "grants"));
      const auto records = field(value, "records");
      for (const auto& row : records.as_array()) initial->records.push_back({decode<::uos::types::ApiKeyRecord>(field(row, "record")), decode<::uos::types::ApiKeyHashRecord>(field(row, "hashRecord"))});
      initial->verifier.verify = [this](json::value privateContext, std::string intent, double now) { return awaited<runtime::VerifiedNativeRuntime>(begin("runtime.verifier.verify", json::array{privateContext, intent, rt::makeNumber(now)})); };
      initial->verifier.verifyRouting = [this](json::value privateContext, runtime::NativeValidatedRequest request, std::string intent, double now) { return awaited<runtime::NativeTrustedRoutingDecision>(begin("runtime.verifier.verifyRouting", json::array{privateContext, request.rawJson, intent, rt::makeNumber(now)})); };
      if (const auto* retained = rt::get(value, "retainedHistory")) {
        runtime::NativeRetainedHistoryInitial history;
        history.grants = decode<std::vector<runtime::NativeRetainedHistoryGrant>>(field(*retained, "grants"));
        history.verifier.verify = [this](json::value privateContext, std::string intent, double now) { return awaited<runtime::VerifiedNativeRetainedHistory>(begin("runtime.retainedHistory.verifier.verify", json::array{privateContext, intent, rt::makeNumber(now)})); };
        initial->retainedHistory = std::move(history);
      }
      return initial;
    }
    json::object snapshot(const rt::sqlite::Database& database) const {
      json::object rows;
      for (const auto& table : database.query("SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'uos_native_%' AND name<>'uos_native_authority_meta' ORDER BY name").all()) {
        const auto name = *rt::sqlite::getString(table.get(std::string_view("name"))); json::array entries;
        for (const auto& row : database.query("SELECT * FROM " + name + " ORDER BY row_key").all()) { auto encoded = rt::sqlite::toJson(row); encoded["row_json"] = rt::jsonParse(*rt::getString(encoded, "row_json")); entries.push_back(encoded); }
        if (!entries.empty()) rows[name] = entries;
      }
      json::array meta;
      if (database.query("SELECT name FROM sqlite_master WHERE name='uos_native_authority_meta'").get()) for (const auto& row : database.query("SELECT * FROM uos_native_authority_meta").all()) meta.push_back(rt::sqlite::toJson(row));
      return {{"meta", meta}, {"rows", rows}};
    }
    json::object states() const { json::object result; for (const auto& [id, database] : databases) if (!closed.contains(id)) result[rt::numberToString(static_cast<double>(id))] = snapshot(database); return result; }
    static rt::sqlite::Binding binding(const json::value& value) {
      if (const auto big = rt::getString(value, "$bigint")) return rt::sqlite::Binding(rt::BigInt::fromString(*big));
      if (const auto* bytes = rt::getArray(value, "$bytes")) { rt::Bytes result; for (const auto& item : *bytes) result.push_back(static_cast<std::uint8_t>(rt::toNumber(item))); return rt::sqlite::Binding(result); }
      return rt::sqlite::Binding(value);
    }
    json::value sql(const json::value& event) {
      auto& database = databases.at(idOf(event, "db")); const auto query = text(event, "query"); const auto kind = text(event, "kind");
      std::vector<rt::sqlite::Binding> args; auto raw = field(event, "args");
      if (kind == "exec" && raw.is_array() && raw.as_array().size() == 1 && raw.as_array()[0].is_array()) raw = raw.as_array()[0];
      for (const auto& value : raw.as_array()) args.push_back(binding(value));
      if (kind == "prepare") { database.query(query); return nullptr; }
      if (kind == "exec") { const auto value = database.run(query, args); return json::object{{"changes", rt::makeNumber(value.changes)}, {"lastInsertRowid", std::visit([](auto v) { return rt::makeNumber(static_cast<double>(v)); }, value.lastInsertRowid)}}; }
      const auto statement = database.query(query);
      if (kind == "run") { const auto value = statement.run(args); return json::object{{"changes", rt::makeNumber(value.changes)}, {"lastInsertRowid", std::visit([](auto v) { return rt::makeNumber(static_cast<double>(v)); }, value.lastInsertRowid)}}; }
      if (kind == "get") { const auto row = statement.get(args); return row ? json::value(rt::sqlite::toJson(*row)) : json::value(nullptr); }
      json::array result; for (const auto& row : statement.all(args)) result.push_back(rt::sqlite::toJson(row)); return result;
    }
    static rt::Task<json::value> callControl(std::shared_ptr<engine::NativeAuthority> authority, principal::NativeControlBinding bindingValue, principal::NativeControlCommand command) { auto task = authority->control(bindingValue, command); const auto reply = co_await std::move(task); co_return json::value_from(reply); }
    static rt::Task<json::value> callRuntime(rpc::NativeBoundRuntimePort port, contracts::NativeCommand command) { auto task = port.execute(command); const auto reply = co_await std::move(task); co_return reply; }
    static rt::Task<json::value> callRetained(rpc::NativeRetainedHistoryPort port, contracts::NativeCommand command) { auto task = port.execute(command); const auto reply = co_await std::move(task); co_return reply; }
    static rt::Task<json::value> callRouting(rpc::NativeBoundRuntimePort port, std::string handle, std::string operationId) { auto task = port.bindTrustedRoutingDecision(handle, operationId); const auto reply = co_await std::move(task); co_return reply; }
  public:
    explicit Replay(json::value scenario) : spec(std::move(scenario)), random([this](std::span<std::uint8_t> bytes) {
      if (randomValues.empty()) throw rt::JsError("unexpected engine randomness");
      auto value = randomValues.front(); randomValues.pop_front(); value.erase(std::remove(value.begin(), value.end(), '-'), value.end()); const auto decoded = rt::fromHex(value);
      if (bytes.size() != decoded.size()) throw rt::JsError("unexpected random byte count");
      std::copy(decoded.begin(), decoded.end(), bytes.begin());
    }) {
      for (const auto& event : *rt::getArray(spec, "events")) {
        if (text(event, "op") == "callback") { auto callback = std::make_shared<Callback>(); callback->event = event; callbacks.emplace(idOf(event), callback); callbackQueues[text(event, "path")].push_back(callback); }
        if (text(event, "op") == "random") randomValues.push_back(text(event, "value"));
      }
    }
    ~Replay() { for (auto& [id, database] : databases) if (!closed.contains(id)) database.close(); rt::fs::rmSync(scratch, {.recursive = true, .force = true}); }
    rt::Task<void> run() {
      const auto deadline = rt::setTimeout([this] { throw rt::JsError("assembly trace stalled at event " + std::to_string(step)); }, 30000);
      const auto cleanup = rt::scopeExit([deadline] { rt::clearTimeout(deadline); });
      for (const auto& event : *rt::getArray(spec, "events")) {
        const auto op = text(event, "op"); const auto id = idOf(event);
        if (op == "database") {
          const auto file = text(event, "file"); std::string path = ":memory:";
          if (file != ":memory:") { path = rt::path::join(scratch, "assembly-" + file + ".sqlite3"); if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path); }
          databases.emplace(id, rt::sqlite::Database(path));
        } else if (op == "construct") {
          json::object observed;
          try {
            const auto storage = base::storage(databases.at(idOf(event, "db"))); const auto initial = decode<contracts::NativeAuthorityInitial>(field(event, "initial"));
            const auto control = controlInitial(event); const auto runtimeValue = runtimeInitial(event);
            authorities[id] = std::make_shared<engine::NativeAuthority>(storage, initial, [this] { return rt::toNumber(sync("clock")); }, control, runtimeValue);
          } catch (...) { observed["error"] = errorJson(std::current_exception()); }
          if (const auto* error = rt::get(event, "error")) equal(field(observed, "error"), *error, "constructor error"); else equal(observed, json::object(), "constructor success");
          equal(states(), field(event, "state"), "constructor SQLite state");
        } else if (op == "bind") {
          json::object observed;
          try {
            const auto bindingValue = field(event, "binding");
            if (text(event, "kind") == "bindRetainedHistory") retainedPorts[id] = authorities.at(idOf(event, "authority"))->bindRetainedHistory(bindingValue);
            else runtimePorts[id] = authorities.at(idOf(event, "authority"))->bindAuthenticatedRuntime(bindingValue);
          } catch (...) { observed["error"] = errorJson(std::current_exception()); }
          if (const auto* error = rt::get(event, "error")) equal(field(observed, "error"), *error, "private binding error"); else equal(observed, json::object(), "private binding success");
        } else if (op == "call") {
          const auto args = field(event, "args").as_array(); const auto method = text(event, "method");
          if (rt::get(event, "error")) throw rt::JsError("unexpected synchronous original async-call throw");
          if (method == "control") calls.emplace(id, rt::Promise<json::value>(callControl(authorities.at(idOf(event, "authority")), decode<principal::NativeControlBinding>(args[0]), decode<principal::NativeControlCommand>(args[1]))));
          else if (method == "bindTrustedRoutingDecision") calls.emplace(id, rt::Promise<json::value>(callRouting(runtimePorts.at(idOf(event, "port")), std::string(args[0].as_string()), std::string(args[1].as_string()))));
          else if (retainedPorts.contains(idOf(event, "port"))) calls.emplace(id, rt::Promise<json::value>(callRetained(retainedPorts.at(idOf(event, "port")), decode<contracts::NativeCommand>(args[0]))));
          else calls.emplace(id, rt::Promise<json::value>(callRuntime(runtimePorts.at(idOf(event, "port")), decode<contracts::NativeCommand>(args[0]))));
          (void)calls.at(id).catch_([](rt::Reason) { return json::value(nullptr); });
        } else if (op == "callback") {
          auto& callback = callbacks.at(id); co_await callback->started.promise.wait();
        } else if (op == "callbackEnd") {
          auto& callback = callbacks.at(id); co_await callback->started.promise.wait();
          if (const auto* error = rt::get(event, "error")) callback->result.reject(reasonOf(*error)); else callback->result.resolve(field(event, "result"));
        } else if (op == "callEnd") {
          json::object observed;
          try { const auto result = co_await calls.at(id).wait(); observed["result"] = result; } catch (...) { observed["error"] = errorJson(std::current_exception()); }
          if (const auto* error = rt::get(event, "error")) equal(field(observed, "error"), *error, "assembly error"); else equal(field(observed, "result"), field(event, "result"), "assembly reply");
          calls.erase(id);
          // Native WebCrypto completion latency does not order independent active controls.
          // Keep every reply/error and require the complete settled state at quiescence.
          if (!concurrentDigestFixture() || calls.empty()) compareStates(states(), field(event, "state"), "assembly full SQLite state at event " + std::to_string(step));
        } else if (op == "sql") {
          json::object observed; try { observed["result"] = sql(event); } catch (...) { observed["error"] = errorJson(std::current_exception()); }
          if (const auto* error = rt::get(event, "error")) equal(field(observed, "error"), *error, "external fixture SQL error");
          else equal(concurrentDigestFixture() ? revisions.normalize(normalSql(field(observed, "result"))) : normalSql(field(observed, "result")), normalSql(field(event, "result")), "original external SQL observation");
        } else if (op == "transactionBegin") {
          const auto databaseId = idOf(event, "db"); auto& stack = transactions[databaseId];
          if (stack.empty()) databases.at(databaseId).run("BEGIN " + text(event, "kind"));
          else databases.at(databaseId).run("SAVEPOINT assembly_" + std::to_string(stack.size()));
          stack.push_back("assembly_" + std::to_string(stack.size()));
        } else if (op == "transactionEnd") {
          const auto databaseId = idOf(event, "db"); auto& stack = transactions[databaseId]; const auto name = stack.back(); stack.pop_back(); const bool commit = rt::getBool(event, "commit").value_or(false);
          if (stack.empty()) databases.at(databaseId).run(commit ? "COMMIT" : "ROLLBACK");
          else { if (!commit) databases.at(databaseId).run("ROLLBACK TO " + name); databases.at(databaseId).run("RELEASE " + name); }
        } else if (op == "databaseClose") {
          const auto key = rt::numberToString(static_cast<double>(id));
          compareStates(json::object{{key, snapshot(databases.at(id))}}, json::object{{key, field(event, "state")}}, "close/reopen durable SQLite state"); databases.at(id).close(); closed.insert(id);
        } else if (op != "random") throw rt::JsError("unknown assembly event " + op);
        ++step;
      }
      if (!calls.empty() || !randomValues.empty()) throw rt::JsError("unsettled assembly calls/random inputs");
      for (const auto& [id, callback] : callbacks) { (void)id; if (!callback->consumed) throw rt::JsError("unconsumed source callback"); }
    }
  };
  inline rt::Task<void> replay(json::value scenario) { Replay test(std::move(scenario)); co_await test.run(); }
  inline bool registerScenarios(const std::string& group) {
    for (const auto& scenario : vectors().as_array()) if (text(scenario, "group") == group) {
      bool containsAuthority = false; for (const auto& event : *rt::getArray(scenario, "events")) if (text(event, "op") == "construct") containsAuthority = true;
      if (containsAuthority) rt::test::registry().push_back({text(scenario, "name"), [scenario] { rt::FakeClock clock(base::NOW); rt::Loop loop(clock); rt::blockOn(loop, replay(scenario)); }});
    }
    return true;
  }
}
