// Port of src/authority/native-authority-sql.ts.
#include "uos/authority/native_authority_sql.hpp"
#include "uos/api_keys.hpp"
#include "uos/authority/native_api_key_policy.hpp"
#include "uos/authority/native_paid_policy.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace uos::authority::native_authority_sql {
  using namespace native_authority_contracts;
  namespace {
    constexpr std::array<std::string_view, 37> TABLES{
      "retained_history_grant", "runtime_grant", "runtime_request", "key_control_grant", "principal_resolution", "api_key_id", "api_key_hash", "api_key_audit", "key_control_receipt", "kernel_default", "authority_config", "capability", "api_policy", "api_window", "api_request", "kernel_policy", "kernel_window", "kernel_reservation", "kernel_cutover", "codex_pool", "codex_routing", "codex_active", "codex_capacity", "codex_owner", "codex_reset", "paid_request", "paid_window", "paid_pending", "paid_gate", "paid_lease", "paid_backfill_lease", "paid_rollup", "paid_deletion_guard", "request_lifetime", "attempt_lifetime", "cursor", "operation_receipt"
    };
    std::string tableName(const std::string& table) {
      if (std::find(TABLES.begin(), TABLES.end(), table) == TABLES.end()) throw NativeAuthorityError("invalid_table", 400);
      return "uos_native_" + table;
    }
    std::string keyJson(const NativeKey& key) {
      json::array result;
      for (const auto& part : key) {
        std::visit([&](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, rt::KvBytes>) { json::array bytes; for (auto byte : value) bytes.push_back(byte); result.push_back(json::object{{"bytes", bytes}}); }
          else if constexpr (std::is_same_v<T, rt::BigInt>) result.push_back(json::object{{"bigint", value.toString()}});
          else if constexpr (std::is_same_v<T, double>) { if (value == 0 && std::signbit(value)) result.push_back(json::object{{"negativeZero", true}}); else result.push_back(rt::makeNumber(value)); }
          else result.push_back(json::value_from(value));
        }, part.value);
      }
      return rt::jsonStringify(result);
    }
    NativeKey parseKey(const std::string& encoded) {
      const auto values = rt::jsonParse(encoded);
      if (!values.is_array()) throw NativeAuthorityError("malformed_native_key", 503);
      NativeKey result;
      for (const auto& part : values.as_array()) {
        if (part.is_string()) result.emplace_back(std::string(part.as_string()));
        else if (rt::isNumber(part)) result.emplace_back(rt::toNumber(part));
        else if (part.is_bool()) result.emplace_back(part.as_bool());
        else {
          bool bytesValid = false;
          if (const auto* bytes = rt::getArray(part, "bytes")) {
            rt::KvBytes value; bytesValid = true;
            for (const auto& byte : *bytes) {
              if (!rt::Number::isInteger(byte) || rt::toNumber(byte) < 0 || rt::toNumber(byte) > 255) { bytesValid = false; break; }
              value.push_back(static_cast<std::uint8_t>(rt::toNumber(byte)));
            }
            if (bytesValid) result.emplace_back(std::move(value));
          }
          if (bytesValid) continue;
          if (const auto bigint = rt::getString(part, "bigint")) result.emplace_back(rt::BigInt::fromString(*bigint));
          else if (rt::getBool(part, "negativeZero") == true) result.emplace_back(-0.0);
          else throw NativeAuthorityError("malformed_native_key", 503);
        }
      }
      return result;
    }
    NativeSqlStorage::Binding binding(const std::optional<std::string>& value) { return value ? NativeSqlStorage::Binding(*value) : NativeSqlStorage::Binding(nullptr); }
    NativeSqlStorage::Binding binding(const std::optional<double>& value) { return value ? NativeSqlStorage::Binding(*value) : NativeSqlStorage::Binding(nullptr); }
  }
  std::string nativeKey(const NativeKey& key) {
    for (const auto& part : key) {
      if (const auto* number = std::get_if<double>(&part.value); number && std::isnan(*number)) throw NativeAuthorityError("nonportable_native_key", 500);
      if (const auto* bigint = std::get_if<rt::BigInt>(&part.value)) { const auto text = bigint->toString(16); const auto digits = text.size() - (bigint->isNegative() ? 1 : 0);
    if ((digits + 1) / 2 > 255) throw NativeAuthorityError("native_bigint_key_too_large", 500); }
    }
    return rt::toHex(rt::packKey(key));
  }
  std::vector<json::object> NativeAuthoritySql::_exec(std::string query, std::vector<NativeSqlStorage::Binding> bindings) const { return _storage.sql.exec(std::move(query), std::move(bindings)).toArray(); }
  NativeAuthoritySql::NativeAuthoritySql(NativeSqlStorage storage, std::string namespaceValue, std::string epochValue) : _storage(std::move(storage)), namespace_(std::move(namespaceValue)), epoch(std::move(epochValue)) {
    _storage.transactionSync([&] {
      _exec("CREATE TABLE IF NOT EXISTS uos_native_authority_meta (id INTEGER PRIMARY KEY CHECK(id=1), schema_version INTEGER NOT NULL, namespace TEXT NOT NULL, epoch TEXT NOT NULL, sequence INTEGER NOT NULL, initialized INTEGER NOT NULL DEFAULT 0)");
      for (const auto table : TABLES) {
        const auto name = tableName(std::string(table));
        _exec("CREATE TABLE IF NOT EXISTS " + name + " (row_key TEXT PRIMARY KEY, key_json TEXT NOT NULL, row_json TEXT NOT NULL, revision TEXT NOT NULL, expires_at_ms INTEGER NULL, scope_key TEXT NULL, parent_key TEXT NULL, due_at_ms INTEGER NULL)");
        _exec("CREATE INDEX IF NOT EXISTS " + name + "_scope ON " + name + "(scope_key, row_key)");
        _exec("CREATE INDEX IF NOT EXISTS " + name + "_parent ON " + name + "(parent_key, row_key)");
        _exec("CREATE INDEX IF NOT EXISTS " + name + "_expiry ON " + name + "(expires_at_ms)");
      }
      _exec("CREATE INDEX IF NOT EXISTS uos_native_attempt_billing ON uos_native_attempt_lifetime(scope_key,json_extract(row_json,'$.billingRequestId'))");
      if (_exec("SELECT * FROM uos_native_authority_meta WHERE id=1").empty()) _exec("INSERT INTO uos_native_authority_meta VALUES(1,3,?,?,0,0)", {namespace_, epoch});
      assertEpoch();
    });
  }
  void NativeAuthoritySql::assertEpoch() const {
    const auto rows = _exec("SELECT * FROM uos_native_authority_meta WHERE id=1");
    if (rows.empty() || rt::getNumber(rows.front(), "schema_version") != 3 || rt::getString(rows.front(), "namespace") != namespace_ || rt::getString(rows.front(), "epoch") != epoch || (!rt::get(rows.front(), "sequence") || !rt::Number::isSafeInteger(*rt::get(rows.front(), "sequence")))) throw NativeAuthorityError("authority_schema_or_epoch_mismatch", 503);
  }
  NativeRead<json::value> NativeAuthoritySql::read(const std::string& table, const NativeKey& key, double now) const {
    const auto name = tableName(table);
    const auto rows = _exec("SELECT row_json,revision,expires_at_ms FROM " + name + " WHERE row_key=? AND (expires_at_ms IS NULL OR expires_at_ms>?)", {nativeKey(key), now});
    if (rows.empty()) return {std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    const auto& row = rows.front(); const auto encoded = rt::getString(row, "row_json"), revision = rt::getString(row, "revision"); const auto* expiry = rt::get(row, "expires_at_ms");
    if (!encoded || !revision || !expiry || (!expiry->is_null() && !rt::Number::isSafeInteger(*expiry))) throw NativeAuthorityError("malformed_native_row", 503);
    const auto value = rt::jsonParse(*encoded);
    if (table != "kernel_default" && !nativeObject(value)) throw NativeAuthorityError("malformed_native_row", 503);
    return {value, std::string(*revision), std::string(*revision), expiry->is_null() ? std::nullopt : rt::getNumber(*expiry)};
  }
  void NativeAuthoritySql::write(const std::string& table, const NativeKey& key, const json::value& value, std::optional<double> expiresAtMs, const NativeIndex& index) {
    if ((table != "kernel_default" && !nativeObject(value)) || (expiresAtMs && (!rt::Number::isSafeInteger(*expiresAtMs) || *expiresAtMs < 0))) throw NativeAuthorityError("invalid_native_write", 500);
    const auto next = _exec("UPDATE uos_native_authority_meta SET sequence=sequence+1 WHERE id=1 AND sequence<9007199254740991 RETURNING sequence");
    const auto sequence = next.empty() ? std::nullopt : rt::getNumber(next.front(), "sequence");
    if (!sequence) throw NativeAuthorityError("authority_revision_exhausted", 503);
    const auto name = tableName(table);
    _exec("INSERT INTO " + name + " (row_key,key_json,row_json,revision,expires_at_ms,scope_key,parent_key,due_at_ms) VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(row_key) DO UPDATE SET row_json=excluded.row_json,revision=excluded.revision,expires_at_ms=excluded.expires_at_ms,scope_key=excluded.scope_key,parent_key=excluded.parent_key,due_at_ms=excluded.due_at_ms", {nativeKey(key), keyJson(key), rt::jsonStringify(value), "r" + rt::padStart(rt::numberToString(*sequence), 20, "0"), binding(expiresAtMs), binding(index.scope), binding(index.parent), binding(index.due)});
  }
  void NativeAuthoritySql::delete_(const std::string& table, const NativeKey& key) { const auto name = tableName(table); _exec("DELETE FROM " + name + " WHERE row_key=?", {nativeKey(key)}); }
  std::vector<NativePageEntry<>> NativeAuthoritySql::page(const std::string& table, double now, const std::string& after, double limit, const NativeIndex& filter) const {
    if (!rt::Number::isSafeInteger(limit) || limit < 1 || limit > 100) throw NativeAuthorityError("invalid_page_limit", 400);
    const auto match = filter.parent ? filter.parent : filter.scope;
    std::vector<NativeSqlStorage::Binding> bindings{after, now};
    if (match) bindings.emplace_back(*match);
    bindings.emplace_back(limit);
    const auto rows = _exec("SELECT row_key,key_json FROM " + tableName(table) + " WHERE row_key>? AND (expires_at_ms IS NULL OR expires_at_ms>?)" + (match ? " AND " + std::string(filter.parent ? "parent_key" : "scope_key") + "=?" : "") + " ORDER BY row_key LIMIT ?", bindings);
    std::vector<NativePageEntry<>> result;
    for (const auto& row : rows) {
      const auto encoded = rt::getString(row, "row_key"), encodedKey = rt::getString(row, "key_json");
      if (!encoded || !encodedKey) throw NativeAuthorityError("malformed_native_key", 503);
      const auto key = parseKey(std::string(*encodedKey)); result.push_back({key, std::string(*encoded), read(table, key, now)});
    }
    return result;
  }
  void NativeAuthoritySql::initialize(const std::function<void()>& callback) {
    transaction([&] {
      const auto rows = _exec("SELECT initialized FROM uos_native_authority_meta WHERE id=1");
      const auto initialized = rows.empty() ? std::nullopt : rt::getNumber(rows.front(), "initialized");
      if (initialized == 1) return;
    if (initialized != 0) throw NativeAuthorityError("authority_initialization_malformed", 503);
      callback(); _exec("UPDATE uos_native_authority_meta SET initialized=1 WHERE id=1");
    });
  }
  std::optional<double> NativeAuthoritySql::earliestPaidDue(double now) const {
    const auto rows = _exec("SELECT MIN(due_at_ms) AS due FROM uos_native_paid_pending WHERE expires_at_ms IS NULL OR expires_at_ms>?", {now}); const auto* due = rows.empty() ? nullptr : rt::get(rows.front(), "due");
    if (due && due->is_null()) return std::nullopt;
    if ((!due || !rt::Number::isSafeInteger(*due))) throw NativeAuthorityError("pending_due_malformed", 503);
    return rt::getNumber(*due);
  }
  std::optional<NativeAttempt> NativeAuthoritySql::paidAttempt(const std::string& keyId, const std::string& billingRequestId, double now) const {
    const auto rows = _exec("SELECT row_json FROM uos_native_attempt_lifetime WHERE scope_key=? AND json_extract(row_json,'$.billingRequestId')=? AND (expires_at_ms IS NULL OR expires_at_ms>?) LIMIT 2", {keyId, billingRequestId, now});
    if (rows.size() > 1) throw NativeAuthorityError("paid_attempt_identity_conflict", 503);
    if (rows.empty() || !rt::get(rows.front(), "row_json")) return std::nullopt;
    const auto encoded = rt::getString(rows.front(), "row_json");
    if (!encoded) throw NativeAuthorityError("paid_attempt_malformed", 503);
    const auto row = rt::jsonParse(*encoded); const auto handle = rt::getString(row, "handle");
    if (!nativeObject(row) || !handle) throw NativeAuthorityError("paid_attempt_malformed", 503);
    return readAs<NativeAttempt>("attempt_lifetime", {std::string(*handle)}, now).value;
  }
  NativeCapability NativeAuthoritySql::capability(const std::string& id, double now) const {
    const auto cap = read("capability", {id}, now).value;
    if (!cap || rt::truthy(rt::get(*cap, "revoked")) || rt::getString(*cap, "epoch") != epoch) throw NativeAuthorityError("capability_revoked_or_wrong_epoch", 403);
    return json::value_to<NativeCapability>(*cap);
  }
  std::optional<native_principal_contracts::NativePrincipalResolution> NativeAuthoritySql::principalReference(const std::string& id, const std::string& keyId, double now) const {
    const auto rows = _exec("SELECT key_json FROM uos_native_principal_resolution WHERE scope_key=? AND json_extract(row_json,'$.id')=? AND (expires_at_ms IS NULL OR expires_at_ms>?) LIMIT 2", {keyId, id, now});
    if (rows.size() > 1) throw NativeAuthorityError("principal_reference_identity_conflict", 503);
    if (rows.empty() || !rt::get(rows.front(), "key_json")) return std::nullopt;
    const auto encoded = rt::getString(rows.front(), "key_json");
    if (!encoded) throw NativeAuthorityError("principal_reference_malformed", 503);
    return readAs<native_principal_contracts::NativePrincipalResolution>("principal_resolution", parseKey(std::string(*encoded)), now).value;
  }
  std::optional<NativeRead<NativeKeyPolicy>> NativeAuthoritySql::currentKeyPolicy(const std::optional<std::string>& tokenHash, const std::optional<std::string>& keyId, double now) const {
    if (!tokenHash || !keyId) return std::nullopt;
    const auto source = read("api_policy", api_keys::apiKeyHashKey(*tokenHash), now); const auto value = source.value.value_or(nullptr);
    const auto* sourceApi = rt::get(value, "api");
    if (!sourceApi || rt::getString(*sourceApi, "key_id") != *keyId) return std::nullopt;
    const auto raw = read("api_key_hash", api_keys::apiKeyHashKey(*tokenHash), now).value.value_or(nullptr); const auto* revoked = rt::get(raw, "revoked_at_ms");
    if (rt::getString(raw, "id") != *keyId || !revoked || !revoked->is_null() || !native_paid_policy::hasStrictPaidFallbackPolicy(raw)) return std::nullopt;
    auto api = native_api_key_policy::apiKeyPolicyFromHashRecord(*tokenHash, raw, now);
    if (!api) return std::nullopt;
    api->usage_reset_at_ms = rt::toNumber(rt::get(raw, "usage_reset_at_ms")); api->window_start_ms = api->usage_reset_at_ms - api->window_ms;
    auto policy = json::value_to<NativeKeyPolicy>(value); policy.api = *api;
    return NativeRead<NativeKeyPolicy>{policy, source.revision, source.versionstamp, source.expiresAtMs};
  }
}
