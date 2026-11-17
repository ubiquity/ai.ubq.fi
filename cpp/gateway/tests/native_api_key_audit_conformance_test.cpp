// Port of tests/native-api-key-audit-conformance.test.ts: JSON-representable shared decisions,
// lazy private source ordering, UTF-16 cuts, actor mapping and timestamp-only reads.
// JS-only getter/proxy/symbol/class-identity vectors have no C++ DTO representation.
// Links: tests/native_api_key_audit_conformance_test.cpp src/authority/native_api_key_audit.cpp src/utils.cpp
#include "uos/authority/native_api_key_audit.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

#include <limits>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt; namespace json = uos::json; namespace audit = uos::authority::native_api_key_audit; namespace auth = uos::auth::index;
  const json::value& vectors() { static const auto value = rt::jsonParse(
#include "tests/native_api_key_audit_vectors.inc"
  ); return value; }
  json::value decoded(const json::value& value) {
    if (value.is_object()) {
      if (const auto marker = rt::getString(value, "$num")) {
        if (*marker == "NaN") return rt::makeNumber(std::numeric_limits<double>::quiet_NaN());
        if (*marker == "Infinity") return rt::makeNumber(std::numeric_limits<double>::infinity());
        if (*marker == "-Infinity") return rt::makeNumber(-std::numeric_limits<double>::infinity());
        if (rt::getBool(value, "negativeZero").value_or(false)) return rt::makeNumber(-0.0);
      }
      json::object output; for (const auto& member : value.as_object()) rt::objectDefine(output, member.key(), decoded(member.value())); return output;
    }
    if (value.is_array()) { json::array output; for (const auto& item : value.as_array()) output.push_back(decoded(item)); return output; }
    return value;
  }
}
TEST("shared API-key audit scalar sanitizer vectors preserve safe field order and UTF-16 cuts") {
  for (const auto& row : *rt::getArray(vectors(), "sanitizers")) {
    const auto input = decoded(*rt::get(row, "input"));
    const auto expected = decoded(*rt::get(row, "expected"));
    EXPECT(rt::deepEqual(audit::sanitizeApiKeyChangeFields(input.as_object()), expected));
  }
  const json::object scrambled{{"revoked_at_ms", 0}, {"reset_usage", false}, {"paid_fallback_limit_credits", nullptr}, {"paid_fallback_enabled", true}, {"window_ms", 60000}, {"usage_limit_requests", -1}, {"expires_at_ms", -1}, {"name", "ordered"}};
  EXPECT_EQ(rt::objectKeys(audit::sanitizeApiKeyChangeFields(scrambled)), (std::vector<std::string>{"name", "expires_at_ms", "usage_limit_requests", "window_ms", "paid_fallback_enabled", "paid_fallback_limit_credits", "reset_usage", "revoked_at_ms"}));
}
TEST("shared API-key audit event vectors preserve lazy UUID clock release order and event key") {
  for (const auto& row : *rt::getArray(vectors(), "events")) {
    const auto& raw = *rt::get(row, "input");
    audit::ApiKeyChangeEventInput input{.action = std::string(*rt::getString(raw, "action")), .targetKeyId = std::string(*rt::getString(raw, "targetKeyId"))};
    if (const auto name = rt::getString(raw, "targetKeyName")) input.targetKeyName = std::string(*name);
    if (const auto now = rt::getNumber(raw, "nowMs")) input.nowMs = *now;
    if (const auto* changed = rt::getObject(raw, "changed")) input.changed = *changed;
    if (const auto* context = rt::getObject(raw, "context")) input.context = audit::ApiKeyAuditContext{.request_id = std::string(*rt::getString(*context, "request_id"))};
    std::vector<std::string> trace;
    const audit::ApiKeyAuditSources sources{[&] { trace.push_back("uuid"); return "event-id"; }, [&] { trace.push_back("clock"); return 1800000000000.0; }, [&] { trace.push_back("release"); return "fixture-release"; }};
    const auto event = audit::buildApiKeyChangeEvent(input, sources);
    EXPECT(rt::deepEqual(json::value_from(event), *rt::get(row, "event")));
    EXPECT(rt::deepEqual(json::value_from(trace), *rt::get(row, "trace")));
    EXPECT(rt::deepEqual(rt::kvValueFromKey(audit::apiKeyChangeEventKey(event)).toJson(), *rt::get(row, "key")));
    const auto roundTrip = json::value_to<audit::ApiKeyChangeEvent>(json::value_from(event)); EXPECT(roundTrip == event);
  }
}
TEST("shared API-key audit sources stop at the first failing source") {
  for (const std::string failure : {"uuid", "clock", "release"}) {
    std::vector<std::string> trace;
    const auto source = [&](const std::string& label) { trace.push_back(label); if (label == failure) throw rt::JsError("synthetic-" + label); };
    const audit::ApiKeyAuditSources sources{[&] { source("uuid"); return "id"; }, [&] { source("clock"); return 1.0; }, [&] { source("release"); return "release"; }};
    const audit::ApiKeyChangeEventInput input{.action = "create", .targetKeyId = "key"};
    EXPECT_THROWS_MATCHING(audit::buildApiKeyChangeEvent(input, sources), "synthetic-" + failure);
    EXPECT_EQ(trace.back(), failure); EXPECT_EQ(trace.size(), failure == "uuid" ? std::size_t(1) : failure == "clock" ? std::size_t(2) : std::size_t(3));
  }
}
TEST("shared API-key audit actor attribution never persists credential material") {
  EXPECT_EQ(audit::apiKeyChangeActorFromAuth().kind, "unknown");
  const std::optional<auth::AdminAuthResult> refusal(auth::AdminAuthResultNotOk{rt::Response()}); EXPECT_EQ(audit::apiKeyChangeActorFromAuth(refusal).kind, "unknown");
  const std::vector<auth::AdminAuthMethod> methods{auth::AdminAuthMethodPasskeySession{"user", "handle", true, 1}, auth::AdminAuthMethodDisabled{}, auth::AdminAuthMethodAdminAllowlist{}, auth::AdminAuthMethodDenoDeployToken{}};
  for (const auto& method : methods) for (const bool super : {false, true}) {
    const std::optional<auth::AdminAuthResult> authenticated(auth::AdminAuthResultOk{"synthetic-secret-never-persisted", method, super}); const auto actor = audit::apiKeyChangeActorFromAuth(authenticated);
    EXPECT_EQ(actor.is_super_admin, super); EXPECT(!rt::includes(rt::jsonStringify(json::value_from(actor)), "synthetic-secret"));
    if (std::holds_alternative<auth::AdminAuthMethodPasskeySession>(method)) { EXPECT_EQ(actor.kind, "passkey_user"); EXPECT_EQ(actor.principal_id, std::optional<std::string>("user")); }
    else { EXPECT_EQ(actor.kind, std::holds_alternative<auth::AdminAuthMethodDisabled>(method) ? "local_admin" : "shared_admin_credential"); EXPECT(!actor.principal_id); }
  }
  auth::PassportAuthMethod passport{"passport-user:subject", {"subject", "handle", true, {}, "sso", std::nullopt, std::nullopt, std::nullopt}};
  const std::optional<auth::AdminAuthResult> authenticated(auth::AdminAuthResultOk{"jwt-must-not-persist", passport, false}); const auto actor = audit::apiKeyChangeActorFromAuth(authenticated);
  EXPECT_EQ(actor.kind, "passkey_user"); EXPECT_EQ(actor.principal_id, std::optional<std::string>("subject")); EXPECT_EQ(actor.principal_label, std::optional<std::string>("handle"));
}
TEST("shared API-key audit read vectors retain timestamp-only visibility and bounded limits") {
  for (const auto& row : *rt::getArray(vectors(), "limits")) { const auto* raw = rt::get(row, "input"); const auto input = raw && raw->is_string() ? std::optional(std::string(raw->as_string())) : std::nullopt; const auto actual = audit::parseApiKeyChangeLimit(input); const auto expected = rt::getNumber(row, "expected"); EXPECT_EQ(actual, expected); }
  const double cutoff = *rt::getNumber(vectors(), "cutoff"); for (const auto& row : *rt::getArray(vectors(), "retention")) { const auto input = decoded(*rt::get(row, "input")); EXPECT_EQ(audit::isRetainedApiKeyChangeEvent(input, cutoff), *rt::getBool(row, "expected")); }
  EXPECT(!audit::isRetainedApiKeyChangeEvent(static_cast<const json::value*>(nullptr), cutoff));
}
