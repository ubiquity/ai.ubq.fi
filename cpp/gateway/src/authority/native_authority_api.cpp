// Port of src/authority/native-authority-api.ts.
#include "uos/authority/native_authority_api.hpp"
#include "uos/rt/json.hpp"

namespace uos::authority::native_authority_api {
  using namespace native_authority_contracts;
  namespace api = native_api_key;
  namespace {
    NativeKey windowKey(const native_contracts::ApiKeyPolicy& policy) { return {"uos_ai", "api_key_usage", "v3", "window", policy.key_id, policy.policy_version, policy.window_start_ms}; }
    NativeKey requestKey(const native_contracts::ApiKeyPolicy& policy, const std::string& requestId) { return {"uos_ai", "api_key_usage", "v3", "request", policy.key_id, policy.policy_version, policy.window_start_ms, requestId}; }
    template<class Plan> std::string kind(const Plan& plan) { return std::visit([](const auto& value) { return std::string(value.kind); }, plan); }
    json::object planJson(const native_contracts::ApiKeyUsageReservationPlan& plan) {
      return std::visit([](const auto& value) {
        json::object result{{"kind", value.kind}};
        if constexpr (requires { value.checks; }) { result["checks"] = json::value_from(value.checks); result["request"] = json::value_from(value.request); result["window"] = json::value_from(value.window); result["expireIn"] = rt::makeNumber(value.expireIn); }
        else if constexpr (requires { value.window; }) result["window"] = json::value_from(value.window);
        else if constexpr (requires { value.reason; }) result["reason"] = value.reason;
        return result;
      }, plan);
    }
  }
  namespace {
    NativeRequest requestWithRaw(native_authority_sql::NativeAuthoritySql& sql, const NativeCapability& cap, const std::string& handle, double now, json::object* rawJson) {
      const auto row = sql.read("request_lifetime", {handle}, now).value.value_or(nullptr);
      const auto expectedKey = cap.keyId ? json::value(*cap.keyId) : json::value(nullptr);
      if (rt::getString(row, "principalId") != cap.principalId || !rt::get(row, "keyId") || !rt::deepEqual(*rt::get(row, "keyId"), expectedKey) || rt::getString(row, "runtimeBindingId") != cap.id) throw NativeAuthorityError("request_ownership_mismatch", 403);
      const auto request = json::value_to<NativeRequest>(row);
      if (rawJson) *rawJson = row.as_object();
      return request;
    }
  }
  NativeRequest NativeAuthorityApi::request(const NativeCapability& cap, const std::string& handle, double now) const { return requestWithRaw(_sql, cap, handle, now, nullptr); }
  void NativeAuthorityApi::save(const NativeRequest& row, const json::object& rawOverrides) {
    const auto previous = _sql.read("request_lifetime", {row.handle}, 0).value.value_or(json::object());
    auto encoded = json::value_from(row);
    for (const auto* field : {"codex", "ownerFence"}) {
      const auto* prior = rt::getObject(previous, field); const auto* next = rt::getObject(encoded, field);
      if (prior && next) encoded.as_object()[field] = rt::objectSpread(*prior, *next);
    }
    const auto merged = rt::objectSpread(previous.as_object(), encoded.as_object(), rawOverrides);
    native_authority_sql::NativeIndex index; index.scope = row.keyId.value_or(row.principalId);
    if (row.api) index.parent = native_authority_sql::nativeKey(requestKey(*row.api, row.requestId));
    _sql.write("request_lifetime", {row.handle}, merged, std::nullopt, index);
  }
  native_runtime_contracts::NativeValidatedRequest NativeAuthorityApi::validated(const NativeCapability& cap, const std::string& handle, double now) const {
    native_runtime_contracts::NativeValidatedRequest result;
    const auto row = requestWithRaw(_sql, cap, handle, now, &result.rawJson);
    if (!row.metadataBound || !row.model || !row.stream) throw NativeAuthorityError("validated_metadata_missing");
    static_cast<NativeRequest&>(result) = row; result.model = *row.model; result.stream = *row.stream; result.reasoning = row.reasoning; return result;
  }
  native_contracts::ApiKeyUsageSnapshot NativeAuthorityApi::_snapshot(const NativeRequest& row, double now) const {
    if (!row.api) return {nullptr, nullptr};
    return {_sql.read("api_request", requestKey(*row.api, row.requestId), now).value.value_or(nullptr), _sql.read("api_window", windowKey(*row.api), now).value.value_or(nullptr)};
  }
  void NativeAuthorityApi::_write(const native_contracts::ApiKeyPolicy& policy, const native_contracts::ApiKeyUsageWritePlan& plan, double now) {
    _sql.write("api_request", requestKey(policy, plan.request.request_id), plan.request, now + plan.expireIn, {.scope = policy.key_id, .parent = native_authority_sql::nativeKey(windowKey(policy))});
    _sql.write("api_window", windowKey(policy), plan.window, now + plan.expireIn, {.scope = policy.key_id});
  }
  void NativeAuthorityApi::_reclaim(const native_contracts::ApiKeyPolicy& policy, double now) {
    std::string after;
    for (;;) {
      const auto page = _sql.page("api_request", now, after, 100, {.parent = native_authority_sql::nativeKey(windowKey(policy))});
      for (const auto& entry : page) {
        const auto row = api::normalizeApiKeyUsageRequestV3(entry.row.value.value_or(nullptr));
        if (!row) throw NativeAuthorityError("request_malformed", 503);
        if (api::isApiKeyUsageReservationExpired(*row, now)) {
          const auto release = api::planApiKeyUsageReservedRelease(policy, row->request_id, "reservation_lease_expired", {json::value_from(*row), _sql.read("api_window", windowKey(policy), now).value.value_or(nullptr)}, now);
          if (const auto* write = std::get_if<native_contracts::ApiKeyUsageWritePlan>(&release)) _write(policy, *write, now);
        }
      }
      if (page.size() < 100) break;
      after = page.back().encoded;
    }
  }
  bool NativeAuthorityApi::_ownsReservation(const NativeRequest& row, const native_contracts::ApiKeyPolicy& policy, double now) const {
    const auto owners = _sql.page("request_lifetime", now, "", 2, {.parent = native_authority_sql::nativeKey(requestKey(policy, row.requestId))});
    if (owners.size() != 1 || !owners.front().row.value) return false;
    const auto& original = *owners.front().row.value;
    return rt::getString(original, "handle") == row.handle && rt::getString(original, "runtimeBindingId") == row.runtimeBindingId && rt::getString(original, "principalId") == row.principalId && rt::deepEqual(*rt::get(original, "keyId"), row.keyId ? json::value(*row.keyId) : json::value(nullptr));
  }
  NativeReply NativeAuthorityApi::reserve(const NativeCapability& cap, const NativeArguments::reserveApiUsage& args, double now) {
    auto row = request(cap, args.handle, now);
    if (!row.apiApplicable) return {{"kind", "not_applicable"}, {"handle", row.handle}};
    if (!cap.keyId || !cap.tokenHash) return {{"kind", "unauthorized"}};
    const auto source = _sql.currentKeyPolicy(cap.tokenHash, cap.keyId, now);
    if (!source || !source->value) return {{"kind", "unauthorized"}};
    const auto& value = *source->value;
    if (value.api.key_id != *cap.keyId || value.api.token_hash != *cap.tokenHash || value.revoked || (value.api.expires_at_ms != -1 && value.api.expires_at_ms <= now)) return {{"kind", "unauthorized"}};
    if (row.metadataBound && !row.api) throw NativeAuthorityError("api_admission_phase_closed");
    auto policy = row.api.value_or(value.api); const auto window = api::currentApiKeyUsageWindow(value.api.usage_reset_at_ms, value.api.window_ms, now);
    if (!row.api) { policy.window_start_ms = window.start; policy.usage_reset_at_ms = window.reset; }
    const auto snapshot = [&] { return native_contracts::ApiKeyUsageSnapshot{_sql.read("api_request", requestKey(policy, row.requestId), now).value.value_or(nullptr), _sql.read("api_window", windowKey(policy), now).value.value_or(nullptr)}; };
    auto plan = api::planApiKeyUsageReservation(policy, row.requestId, row.route, snapshot(), now);
    if (std::holds_alternative<native_contracts::ApiKeyUsageReservationPlanFull>(plan)) { _reclaim(policy, now); plan = api::planApiKeyUsageReservation(policy, row.requestId, row.route, snapshot(), now); }
    const auto* write = std::get_if<native_contracts::ApiKeyUsageWritePlan>(&plan); const bool reuse = std::holds_alternative<native_contracts::ApiKeyUsageReservationPlanReuse>(plan);
    if (!write && !reuse) return {{"kind", kind(plan)}, {"detail", planJson(plan)}};
    if (reuse && !_ownsReservation(row, policy, now)) return {{"kind", "unavailable"}, {"detail", "identity_conflict"}};
    if (write) _write(policy, *write, now);
    row.api = policy; save(row);
    return {{"kind", write ? "reserved" : "reuse"}, {"handle", row.handle}, {"detail", json::object{{"policy", json::value_from(policy)}}}};
  }
  NativeReply NativeAuthorityApi::dispatch(const NativeCapability& cap, const NativeArguments::markApiDispatch& args, double now) {
    auto row = request(cap, args.handle, now);
    if (!row.api) { if (row.dispatch) return {{"kind", "unchanged"}}; row.dispatch = NativeRequestDispatch{args.provider, true, false}; save(row); return {{"kind", "dispatched"}}; }
    const auto plan = api::planApiKeyUsageDispatch(*row.api, row.requestId, row.route, args.provider, _snapshot(row, now), now);
    const auto* write = std::get_if<native_contracts::ApiKeyUsageWritePlan>(&plan);
    if (!write) return {{"kind", kind(plan)}};
    _write(*row.api, *write, now); row.dispatch = NativeRequestDispatch{args.provider, true, false}; save(row); return {{"kind", "dispatched"}};
  }
  bool NativeAuthorityApi::_compensationSafe(const NativeRequest& row) const { return !row.transport && !row.responding && !row.providerObserved && !row.semantic && (!row.delivery || *row.delivery == "unobserved"); }
  void NativeAuthorityApi::assertDispatchReceipt(const NativeCapability& cap, const std::string& handle, double now) const {
    const auto row = request(cap, handle, now); const auto apiRow = api::normalizeApiKeyUsageRequestV3(_snapshot(row, now).request);
    if ((row.api && (!apiRow || apiRow->state != "dispatched")) || !row.dispatch || !row.dispatch->cancellationOwned) throw NativeAuthorityError("api_dispatch_receipt_stale");
  }
  void NativeAuthorityApi::assertTransportReceipt(const NativeCapability& cap, const std::string& handle, const std::string& operationId, double now) const {
    assertDispatchReceipt(cap, handle, now); const auto row = request(cap, handle, now); if (!row.transport || row.transport->operationId != operationId || row.semantic) throw NativeAuthorityError("transport_start_receipt_stale");
  }
  NativeReply NativeAuthorityApi::start(const NativeCapability& cap, const NativeArguments::recordTransportStarted& args, const std::string& operationId, double now) {
    auto row = request(cap, args.handle, now); assertDispatchReceipt(cap, args.handle, now);
    if (row.semantic) throw NativeAuthorityError("request_already_terminal");
    const json::object responding{{"provider", args.provider}, {"attemptHandle", args.attemptHandle ? json::value(*args.attemptHandle) : json::value(nullptr)}};
    if (row.transport && row.transport->provider == args.provider && row.transport->attemptHandle == args.attemptHandle) return {{"kind", "unchanged"}, {"detail", responding}};
    if (!row.dispatch) throw NativeAuthorityError("api_dispatch_missing");
    row.dispatch->fetchStarted = true; row.transport = NativeRequestTransport{args.provider, args.attemptHandle, operationId, now}; row.responding = NativeRequestResponding{args.provider, args.attemptHandle}; save(row); return {{"kind", "started"}, {"detail", responding}};
  }
  NativeReply NativeAuthorityApi::release(const NativeCapability& cap, const NativeArguments::releaseApiUndispatched& args, double now) {
    const auto row = request(cap, args.handle, now); if (!_compensationSafe(row)) throw NativeAuthorityError("prefetch_compensation_closed"); if (!row.api) return {{"kind", "not_applicable"}};
    const auto plan = api::planApiKeyUsageReservedRelease(*row.api, row.requestId, args.reason, _snapshot(row, now), now, row.route);
    const auto* write = std::get_if<native_contracts::ApiKeyUsageWritePlan>(&plan); if (!write) return {{"kind", kind(plan)}}; _write(*row.api, *write, now); return {{"kind", "released"}};
  }
  NativeReply NativeAuthorityApi::cancel(const NativeCapability& cap, const std::string& handle, double now) {
    auto row = request(cap, handle, now); if (!row.dispatch || !row.dispatch->cancellationOwned || row.dispatch->fetchStarted || !_compensationSafe(row)) throw NativeAuthorityError("prefetch_cancellation_not_owned");
    if (!row.api) { row.dispatch->cancellationOwned = false; save(row); return {{"kind", "released"}}; }
    const auto plan = api::planApiKeyUsageDispatchCancellation(*row.api, row.requestId, row.route, row.dispatch->provider, _snapshot(row, now), now);
    const auto* write = std::get_if<native_contracts::ApiKeyUsageWritePlan>(&plan); if (!write) return {{"kind", kind(plan)}}; _write(*row.api, *write, now); row.dispatch->cancellationOwned = false; save(row); return {{"kind", "released"}};
  }
  NativeReply NativeAuthorityApi::status(const NativeCapability& cap, const std::string& handle, double now) const { const auto row = request(cap, handle, now); return {{"kind", "status"}, {"handle", handle}, {"detail", json::object{{"api", _snapshot(row, now).request}}}}; }
  double NativeAuthorityApi::usage(const native_contracts::ApiKeyPolicy& policy, double now) const { const auto window = api::matchingApiKeyUsageWindowV3(_sql.read("api_window", windowKey(policy), now).value.value_or(nullptr), policy); return window ? window->committed_requests : 0; }
  void NativeAuthorityApi::createWindow(const native_contracts::ApiKeyPolicy& policy, double now) {
    if (_sql.read("api_window", windowKey(policy), now).value) throw NativeAuthorityError("api_window_identity_conflict");
    const auto window = api::makeApiKeyUsageWindowV3(policy, now); _sql.write("api_window", windowKey(policy), window, now + api::apiKeyUsageV3RetentionMs(window.window_reset_at_ms, now), {.scope = policy.key_id});
  }
  bool NativeAuthorityApi::_resetReservation(const std::string& keyId, const std::string& encoded, const json::value& value, double now) {
    const auto row = api::normalizeApiKeyUsageRequestV3(value); if (!row || row->key_id != keyId) throw NativeAuthorityError("API key quota ledger is unavailable", 503); if (row->state != "reserved") return false;
    const auto lifetimes = _sql.pageAs<NativeRequest>("request_lifetime", now, "", 2, {.parent = encoded}); const auto lifetime = lifetimes.empty() ? std::nullopt : lifetimes.front().row.value;
    if (lifetimes.size() != 1 || !lifetime || !lifetime->api || native_authority_sql::nativeKey(requestKey(*lifetime->api, lifetime->requestId)) != encoded || lifetime->keyId != keyId) throw NativeAuthorityError("api_reset_original_identity_missing", 503);
    if (!api::isApiKeyUsageReservationExpired(*row, now)) return true;
    const auto release = api::planApiKeyUsageReservedRelease(*lifetime->api, row->request_id, "reservation_lease_expired", {json::value_from(*row), _sql.read("api_window", windowKey(*lifetime->api), now).value.value_or(nullptr)}, now, lifetime->route);
    const auto* write = std::get_if<native_contracts::ApiKeyUsageWritePlan>(&release); if (!write) throw NativeAuthorityError("API key quota ledger is unavailable", 503); _write(*lifetime->api, *write, now); return false;
  }
  bool NativeAuthorityApi::inspectKeyReset(const std::string& keyId, double now) {
    std::string after; bool live = false;
    for (;;) { const auto page = _sql.page("api_request", now, after, 100, {.scope = keyId}); for (const auto& entry : page) { const bool reserved = _resetReservation(keyId, entry.encoded, entry.row.value.value_or(nullptr), now); live = live || reserved; } if (page.size() < 100) break; after = page.back().encoded; }
    return live;
  }
  void NativeAuthorityApi::deleteForKey(const std::string& keyId, double now) {
    for (const std::string table : {"api_request", "api_window", "principal_resolution"}) for (;;) { const auto page = _sql.page(table, now, "", 100, {.scope = keyId}); for (const auto& entry : page) _sql.delete_(table, entry.key); if (page.size() < 100) break; }
  }
}
