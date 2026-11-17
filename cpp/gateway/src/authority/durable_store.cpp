#include "uos/authority/durable_store.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>
#include <utility>

namespace uos::authority::durable_store {
  using contracts::AuthorityError;
  using contracts::fromJson;
  using contracts::sameScope;
  using contracts::toJson;

  namespace {
    using Guard = std::function<bool(const json::value* value)>;
    using Shape = std::vector<std::pair<std::string, Guard>>;

    bool record(const json::value* value) { return value != nullptr && value->is_object(); }
    const Guard text = [](const json::value* value) { return value != nullptr && value->is_string(); };
    const Guard number = [](const json::value* value) {
      return value != nullptr && rt::isNumber(*value) && rt::Number::isSafeInteger(*value) && rt::toNumber(*value) >= 0;
    };
    const Guard boolean = [](const json::value* value) { return value != nullptr && value->is_bool(); };
    Guard oneOf(std::vector<json::value> values) {
      return [values = std::move(values)](const json::value* value) {
        return value != nullptr && std::any_of(values.begin(), values.end(), [value](const json::value& candidate) { return rt::strictEquals(candidate, *value); });
      };
    }
    Guard nullable(Guard guard) {
      return [guard = std::move(guard)](const json::value* value) { return (value != nullptr && value->is_null()) || guard(value); };
    }
    Guard array(Guard guard) {
      return [guard = std::move(guard)](const json::value* value) {
        if (value == nullptr || !value->is_array()) return false;
        const json::array& items = value->get_array();
        return std::all_of(items.begin(), items.end(), [&guard](const json::value& item) { return guard(&item); });
      };
    }
    Guard dictionary(Guard guard) {
      return [guard = std::move(guard)](const json::value* value) {
        if (!record(value)) return false;
        const std::vector<const json::value*> values = rt::objectValues(value->get_object());
        return std::all_of(values.begin(), values.end(), [&guard](const json::value* item) { return guard(item); });
      };
    }
    Guard fields(Shape shape) {
      return [shape = std::move(shape)](const json::value* value) {
        if (!record(value)) return false;
        const json::object& members = value->get_object();
        return members.size() == shape.size() &&
               std::all_of(shape.begin(), shape.end(), [&members](const auto& entry) { return rt::hasOwn(members, entry.first) && entry.second(rt::get(members, entry.first)); });
      };
    }

    const Guard scopeGuard = fields({{"namespace", text}, {"epoch", number}});
    const Guard proof = fields({
      {"kind", oneOf({"quota_exhausted", "credential_invalid", "transient"})},
      {"model", text},
      {"fingerprint", text},
      {"observedAtMs", number},
      {"validUntilMs", number},
    });
    const Guard account = fields({
      {"accountId", text},
      {"fingerprint", text},
      {"credentialGeneration", number},
      {"routingGeneration", number},
      {"resetGeneration", number},
      {"overageVersion", number},
      {"overageEnabled", boolean},
      {"enabled", boolean},
      {"models", array(text)},
      {"proof", nullable(proof)},
    });
    const Guard handle = fields({{"scope", scopeGuard}, {"requestId", text}, {"fence", number}});
    const Guard terminal = fields({
      {"outcome", oneOf({"completed", "incomplete", "failed", "cancelled"})},
      {"providerRequestId", nullable(text)},
      {"spendMicrocredits", nullable(number)},
      {"fallback", oneOf({nullptr, "quota_exhausted", "capacity_exhausted"})},
    });
    const Guard attempt = fields({
      {"input", fields({{"attemptId", text}, {"provider", oneOf({"codex", "surplus", "openlux"})}, {"maximumExposureMicrocredits", number}, {"account", nullable(account)}})},
      {"state", oneOf({"reserved", "dispatch_intent", "dispatched", "not_dispatched"})},
      {"terminal", nullable(terminal)},
      {"billingSettled", boolean},
      {"settledSpendMicrocredits", nullable(number)},
    });
    const Guard admission = fields({
      {"handle", handle},
      {"principalId", text},
      {"policyVersion", number},
      {"windowKey", text},
      {"digest", text},
      {"route", oneOf({"/v1/responses", "/v1/chat/completions"})},
      {"model", text},
      {"account", nullable(account)},
      {"activeGeneration", nullable(number)},
      {"attempts", dictionary(attempt)},
      {"dispatched", boolean},
      {"semantic", oneOf({nullptr, "completed", "incomplete", "failed", "cancelled"})},
      {"kernelSettled", boolean},
      {"delivery", oneOf({nullptr, "delivered", "interrupted", "unobserved"})},
      {"settled", boolean},
    });
    const Guard stateGuard = fields({
      {"scope", scopeGuard},
      {"sequence", number},
      {"accounts", array(account)},
      {"active", nullable(fields({{"accountId", text}, {"fingerprint", text}, {"generation", number}}))},
      {"policies", dictionary(fields({
                     {"principalId", text},
                     {"version", number},
                     {"deleted", boolean},
                     {"windowResetAtMs", number},
                     {"requestLimit", number},
                     {"kernelLimit", number},
                     {"concurrencyLimit", number},
                     {"paidLimitMicrocredits", number},
                   }))},
      {"windows", dictionary(fields({
                    {"reserved", number},
                    {"dispatched", number},
                    {"kernelReserved", number},
                    {"kernelCompleted", number},
                    {"active", number},
                    {"paidReservedMicrocredits", number},
                    {"paidSettledMicrocredits", number},
                  }))},
      {"admissions", dictionary(admission)},
      {"publications", dictionary(fields({
                         {"ownerId", text},
                         {"ownerEpoch", number},
                         {"accountId", text},
                         {"workspaceId", text},
                         {"generation", number},
                         {"nativeRefreshedAtNs", text},
                         {"fingerprint", text},
                         {"accessToken", text},
                         {"idToken", text},
                         {"issuedAtMs", number},
                         {"expiresAtMs", number},
                       }))},
    });
    constexpr double SCHEMA_VERSION = 1;
    const std::string TABLE = "uos_authority_atom";
  }

  DurableAuthorityStore::DurableAuthorityStore(std::shared_ptr<AuthoritySqlStorage> storage_, const AuthorityState& initial, double maximumStateBytes_)
    : _storage(std::move(storage_)), _maximumStateBytes(maximumStateBytes_) {
    if (!rt::Number::isSafeInteger(_maximumStateBytes) || _maximumStateBytes < 1) throw AuthorityError("invalid_authority_storage_bound", 400);
    _scope = initial.scope;
    const std::string encoded = _encode(initial);
    _storage->transactionSync([this, &encoded] {
      _storage->sql().exec("CREATE TABLE IF NOT EXISTS " + TABLE +
                             " (id INTEGER PRIMARY KEY CHECK(id = 1), schema_version INTEGER NOT NULL, revision INTEGER NOT NULL, state_json TEXT NOT NULL)",
                           {});
      const std::vector<json::object> rows = _storage->sql().exec("SELECT schema_version, revision, state_json FROM " + TABLE + " WHERE id = 1", {}).toArray();
      if (rows.empty()) _storage->sql().exec("INSERT INTO " + TABLE + " (id, schema_version, revision, state_json) VALUES (1, ?, 0, ?)", {SCHEMA_VERSION, encoded});
      _read();
    });
  }

  void DurableAuthorityStore::_bounded(const std::string& encoded) const {
    if (static_cast<double>(rt::utf8ByteLength(encoded)) > _maximumStateBytes) throw AuthorityError("authority_state_too_large", 503);
  }

  std::string DurableAuthorityStore::_encode(const AuthorityState& state) const {
    const json::value value = toJson(state);
    if (!stateGuard(&value)) throw AuthorityError("invalid_authority_state", 503);
    if (!sameScope(state.scope, _scope)) throw AuthorityError("namespace_or_epoch_mismatch", 403);
    std::string encoded = rt::jsonStringify(value);
    _bounded(encoded);
    return encoded;
  }

  AuthoritySnapshot DurableAuthorityStore::_read() const {
    const std::vector<json::object> rows = _storage->sql().exec("SELECT schema_version, revision, state_json FROM " + TABLE + " WHERE id = 1", {}).toArray();
    const json::value* schemaVersion = rows.empty() ? nullptr : rt::get(rows[0], "schema_version");
    if (rows.size() != 1 || schemaVersion == nullptr || !rt::isNumber(*schemaVersion) || rt::toNumber(*schemaVersion) != SCHEMA_VERSION)
      throw AuthorityError("authority_schema_mismatch", 503);
    const json::object& row = rows[0];
    const json::value* revision = rt::get(row, "revision");
    const json::value* stateJson = rt::get(row, "state_json");
    if (revision == nullptr || !rt::isNumber(*revision) || !rt::Number::isSafeInteger(*revision) || rt::toNumber(*revision) < 0 || stateJson == nullptr ||
        !stateJson->is_string())
      throw AuthorityError("invalid_authority_record", 503);
    const std::string encodedState(stateJson->get_string());
    _bounded(encodedState);
    json::value state;
    try {
      state = rt::jsonParse(encodedState);
    } catch (...) {
      throw AuthorityError("invalid_authority_state", 503);
    }
    if (!stateGuard(&state)) throw AuthorityError("invalid_authority_state", 503);
    AuthorityState accepted = fromJson<AuthorityState>(state);
    if (!sameScope(accepted.scope, _scope)) throw AuthorityError("namespace_or_epoch_mismatch", 403);
    return AuthoritySnapshot{.revision = rt::toNumber(*revision), .state = std::move(accepted)};
  }

  rt::Promise<AuthoritySnapshot> DurableAuthorityStore::read() { return rt::Promise<AuthoritySnapshot>::resolve(_read()); }

  rt::Promise<bool> DurableAuthorityStore::compareAndSwap(double revision, AuthorityState state) {
    if (!rt::Number::isSafeInteger(revision) || revision < 0 || revision >= rt::Number::MAX_SAFE_INTEGER) throw AuthorityError("invalid_authority_revision", 400);
    const std::string encoded = _encode(state);
    bool committed = false;
    _storage->transactionSync([this, &committed, &encoded, revision] {
      if (_read().revision != revision) {
        committed = false;
        return;
      }
      const std::vector<json::object> rows =
        _storage->sql()
          .exec("UPDATE " + TABLE + " SET revision = ?, state_json = ? WHERE id = 1 AND revision = ? RETURNING revision", {revision + 1, encoded, revision})
          .toArray();
      committed = rows.size() == 1;
    });
    return rt::Promise<bool>::resolve(committed);
  }
}
