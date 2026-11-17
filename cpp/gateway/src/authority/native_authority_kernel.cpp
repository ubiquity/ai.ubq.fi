// Port of src/authority/native-authority-kernel.ts.
#include "uos/authority/native_authority_kernel.hpp"
#include "uos/api_keys.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/string.hpp"

namespace uos::authority::native_authority_kernel {
  using namespace native_authority_contracts;
  using namespace native_kernel_contracts;
  namespace kernel = native_kernel;
  namespace {
    KernelQuotaRead<> read(const native_authority_sql::NativeAuthoritySql& sql, const std::string& table, const NativeKey& key, double now) {
      const auto row = sql.read(table, key, now); return {row.value.value_or(nullptr), row.revision, rt::orNull(row.expiresAtMs)};
    }
    template<class Plan> std::string kind(const Plan& plan) { return std::visit([](const auto& value) { return std::string(value.kind); }, plan); }
    template<class Plan> json::object planJson(const Plan& plan) {
      return std::visit([](const auto& value) {
        json::object result{{"kind", value.kind}};
        if constexpr (requires { value.status; }) { result["status"] = rt::makeNumber(value.status); result["message"] = value.message; }
        if constexpr (requires { value.reservation; }) result["reservation"] = json::value_from(value.reservation);
        if constexpr (requires { value.window; }) result["window"] = json::value_from(value.window);
        if constexpr (requires { value.limit; }) result["limit"] = rt::makeNumber(value.limit);
        if constexpr (requires { value.marker; }) result["marker"] = json::value_from(value.marker);
        if constexpr (requires { value.expireIn; }) result["expireIn"] = rt::makeNumber(value.expireIn);
        if constexpr (requires { value.reason; }) result["reason"] = value.reason;
        return result;
      }, plan);
    }
    std::string scopeKey(const KernelQuotaIdentity& identity) { return native_authority_sql::nativeKey({identity.owner, identity.repo.value_or("")}); }
    KernelQuotaReservationIdentity reservationIdentity(const KernelQuotaIdentity& identity, const std::string& requestId, const std::string& route, double createdAtMs) {
      return {identity, requestId, route, createdAtMs};
    }
  }
  KernelQuotaDefaults NativeAuthorityKernel::_defaults(double now) const {
    const auto limit = _sql.read("kernel_default", {"default", "kernel_policy_limit_requests"}, now).value.value_or(nullptr);
    const auto window = _sql.read("kernel_default", {"default", "kernel_policy_window_ms"}, now).value.value_or(nullptr);
    return {kernel::normalizeUsageLimit(&limit, api_keys::API_KEY_NO_USAGE_LIMIT, _initial.kernelConstants.noUsageLimit), kernel::normalizeWindow(&window, api_keys::USAGE_RESET_PERIOD_MS)};
  }
  NativeAuthorityKernel::Window NativeAuthorityKernel::_readWindow(const KernelQuotaIdentity& identity, double now) const {
    const auto row = read(_sql, "kernel_window", kernel::kernelScopedWindowKey(identity.scope, identity.owner, identity.repo), now);
    const auto value = kernel::normalizeKernelQuotaWindowV2(row.value, identity.scope, identity.owner, identity.repo);
    if (!row.value.is_null() && !value) throw NativeAuthorityError("kernel_window_malformed", 503);
    return {row, value};
  }
  KernelQuotaRead<> NativeAuthorityKernel::_reservation(const KernelQuotaReservationIdentity& identity, double now) const { return read(_sql, "kernel_reservation", kernel::kernelReservationKey(identity.scope, identity.owner, identity.repo, identity.windowCreatedAtMs, identity.requestId), now); }
  void NativeAuthorityKernel::_writeReservation(const KernelQuotaIdentity& identity, const KernelQuotaReservationRowV2& row, double expiry) {
    _sql.write("kernel_reservation", kernel::kernelReservationKey(identity.scope, identity.owner, identity.repo, row.window_created_at_ms, row.request_id), row, expiry, {.scope = scopeKey(identity), .parent = native_authority_sql::nativeKey(kernel::kernelScopedWindowKey(identity.scope, identity.owner, identity.repo))});
  }
  double NativeAuthorityKernel::_reclaimWindow(const KernelQuotaIdentity& identity, double now) {
    double count = 0; std::string after;
    for (;;) {
      const auto page = _sql.page("kernel_reservation", now, after, 100, {.parent = native_authority_sql::nativeKey(kernel::kernelScopedWindowKey(identity.scope, identity.owner, identity.repo))});
      for (const auto& item : page) {
        const auto row = kernel::normalizeKernelQuotaReservationRowV2(item.row.value.value_or(nullptr), identity.scope, identity.owner, identity.repo);
        if (!row) throw NativeAuthorityError("kernel_reservation_malformed", 503);
        const auto target = kernel::decideKernelQuotaReclaim(*row, now);
        if (target) { _settle(reservationIdentity(identity, row->request_id, row->route, row->window_created_at_ms), *target, "reservation_lease_expired", now); ++count; }
      }
      if (page.size() < 100) break;
      after = page.back().encoded;
    }
    return count;
  }
  NativeReply NativeAuthorityKernel::reserve(const NativeCapability& cap, const std::string& handle, double now) {
    auto request = _api.request(cap, handle, now);
    if (!request.kernelApplicable) return {{"kind", "not_applicable"}};
    if (request.apiApplicable && !request.api) throw NativeAuthorityError("api_admission_required");
    if (!cap.kernel) throw NativeAuthorityError("kernel_scope_not_applicable", 403);
    if (request.kernel) return {{"kind", "refused"}, {"detail", json::object{{"kind", "refused"}, {"status", 503}, {"message", "Kernel quota request already has a reservation"}}}};
    const auto repo = read(_sql, "kernel_policy", kernel::kernelRepoPolicyKey(cap.kernel->owner, cap.kernel->repo), now);
    const auto org = read(_sql, "kernel_policy", kernel::kernelOrgPolicyKey(cap.kernel->owner), now);
    const auto scope = kernel::decideKernelQuotaScope({repo, org}, cap.kernel->owner, cap.kernel->repo, _initial.kernelConstants).limit_scope;
    const auto identity = nativeIdentity(scope, cap);
    const auto eligibility = kernel::decideKernelQuotaEligibility(identity, {scope == "repo" ? repo : org, scope == "org" ? std::optional<KernelQuotaRead<>>(repo) : std::nullopt, read(_sql, "kernel_cutover", kernel::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, now), scope == "repo" ? "present" : "absent"}, _defaults(now), _initial.kernelConstants, now);
    const auto* eligible = std::get_if<KernelQuotaEligibilityPlanEligible>(&eligibility);
    if (!eligible) return {{"kind", kind(eligibility)}, {"detail", planJson(eligibility)}};
    const auto decide = [&] {
      const auto current = _readWindow(identity, now);
      const auto window = kernel::windowForEffectivePolicy(current.value, identity.scope, identity.owner, identity.repo, eligible->effective.windowMs, now).window;
      return kernel::decideKernelQuotaReserve(identity, eligible->effective, window, read(_sql, "kernel_reservation", kernel::kernelReservationKey(scope, identity.owner, identity.repo, window.created_at_ms, request.requestId), now), request.requestId, request.route, _initial.kernelConstants.noUsageLimit, now);
    };
    auto plan = decide();
    if (std::holds_alternative<KernelQuotaReservePlanFull>(plan) && _reclaimWindow(identity, now) > 0) plan = decide();
    const auto* reserve = std::get_if<KernelQuotaReservePlanReserve>(&plan);
    if (!reserve) return {{"kind", kind(plan)}, {"detail", planJson(plan)}};
    _sql.write("kernel_window", kernel::kernelScopedWindowKey(scope, identity.owner, identity.repo), reserve->window, now + reserve->expireIn, {.scope = scopeKey(identity)});
    _writeReservation(identity, reserve->reservation, now + reserve->expireIn);
    request.kernel = reservationIdentity(identity, request.requestId, request.route, reserve->window.created_at_ms); _api.save(request);
    return {{"kind", "reserved"}, {"detail", json::value_from(reserve->reservation)}};
  }
  KernelQuotaReservationIdentity NativeAuthorityKernel::_identity(const NativeCapability& cap, const std::string& handle, double now) const {
    const auto identity = _api.request(cap, handle, now).kernel;
    if (!identity) throw NativeAuthorityError("kernel_reservation_missing");
    return *identity;
  }
  NativeReply NativeAuthorityKernel::intent(const NativeCapability& cap, const NativeArguments::recordKernelTerminalIntent& args, double now) {
    const auto identity = _identity(cap, args.handle, now); const auto plan = kernel::decideKernelQuotaTerminalIntent(identity, _reservation(identity, now), args.terminalState, now);
    if (const auto* value = std::get_if<KernelQuotaTerminalPlanIntent>(&plan)) { _writeReservation(identity, value->reservation, now + value->expireIn); return {{"kind", "intent"}, {"detail", json::value_from(value->reservation)}}; }
    return {{"kind", "unchanged"}, {"detail", std::get<KernelQuotaTerminalPlanUnchanged>(plan).outcome}};
  }
  NativeReply NativeAuthorityKernel::_settle(const KernelQuotaReservationIdentity& identity, const std::string& target, const std::string& reason, double now) {
    const auto entry = _reservation(identity, now); const auto row = kernel::normalizeKernelQuotaReservationRowV2(entry.value, identity.scope, identity.owner, identity.repo);
    if (!row) return {{"kind", "unchanged"}, {"detail", entry.value.is_null() ? "missing" : "invalid"}};
    const auto precondition = kernel::kernelReservationPrecondition(*row, identity.requestId, identity.route, identity.windowCreatedAtMs, target);
    if (precondition) return {{"kind", "unchanged"}, {"detail", *precondition}};
    const auto current = _readWindow(identity, now);
    const auto plan = kernel::decideKernelQuotaSettlement(*row, {current.value, !current.row.value.is_null()}, target, reason, now);
    if (const auto* settle = std::get_if<KernelQuotaSettlePlanSettle>(&plan)) {
      _writeReservation(identity, settle->reservation, now + settle->expireIn);
      if (settle->window) _sql.write("kernel_window", kernel::kernelScopedWindowKey(identity.scope, identity.owner, identity.repo), *settle->window, now + settle->expireIn, {.scope = scopeKey(identity)});
      return {{"kind", "settle"}, {"detail", json::value_from(settle->reservation)}};
    }
    return {{"kind", "unchanged"}, {"detail", std::get<KernelQuotaSettlePlanUnchanged>(plan).outcome}};
  }
  NativeReply NativeAuthorityKernel::settle(const NativeCapability& cap, const NativeArguments::settleKernelUsage& args, double now) { return _settle(_identity(cap, args.handle, now), args.terminalState, args.reason, now); }
  NativeReply NativeAuthorityKernel::renew(const NativeCapability& cap, const std::string& handle, double now) {
    const auto identity = _identity(cap, handle, now); const auto plan = kernel::decideKernelQuotaRenewal(identity, _reservation(identity, now), now); _writeReservation(identity, plan.reservation, now + plan.expireIn); return {{"kind", "renewed"}, {"detail", json::value_from(plan.reservation)}};
  }
  NativeReply NativeAuthorityKernel::reclaim(const NativeCapability& cap, const NativeArguments::reclaimExpiredKernelReservations& args, double now) {
    std::string scope = "org", after;
    if (args.cursor && !args.cursor->empty()) {
      const auto cursor = _sql.readAs<NativeCursor>("cursor", {*args.cursor}, now).value;
      if (!cursor || cursor->capabilityId != cap.id || cursor->parent != cap.keyId || !rt::startsWith(cursor->family, "kernel_reclaim:")) throw NativeAuthorityError("cursor_ownership_mismatch", 403);
      scope = cursor->family == "kernel_reclaim:repo" ? "repo" : "org"; after = cursor->after;
    }
    const auto identity = nativeIdentity(scope, cap); const auto page = _sql.page("kernel_reservation", now, after, args.limit, {.scope = scopeKey(identity)}); double reclaimed = 0;
    for (const auto& item : page) {
      const auto row = kernel::normalizeKernelQuotaReservationRowV2(item.row.value.value_or(nullptr), scope, identity.owner, identity.repo);
      if (!row) throw NativeAuthorityError("kernel_reservation_malformed", 503);
      const auto target = kernel::decideKernelQuotaReclaim(*row, now);
      if (target) { _settle(reservationIdentity(identity, row->request_id, row->route, row->window_created_at_ms), *target, "reservation_lease_expired", now); ++reclaimed; }
    }
    std::optional<std::string> cursor;
    if (static_cast<double>(page.size()) == args.limit || scope == "org") {
      cursor = rt::crypto::randomUUID(); const bool continuing = static_cast<double>(page.size()) == args.limit;
      _sql.write("cursor", {*cursor}, NativeCursor{*cursor, cap.id, "kernel_reclaim:" + (continuing ? scope : "repo"), continuing ? page.back().encoded : "", cap.keyId, std::nullopt});
    }
    return {{"kind", "reclaimed"}, {"cursor", cursor ? json::value(*cursor) : json::value(nullptr)}, {"detail", json::object{{"reclaimed", rt::makeNumber(reclaimed)}, {"scanned", page.size()}}}};
  }
  NativeReply NativeAuthorityKernel::cutover(double now) {
    const auto plan = kernel::decideKernelDefaultWindowCutover(read(_sql, "kernel_cutover", kernel::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, now), rt::crypto::randomUUID(), now);
    if (const auto* acquire = std::get_if<KernelQuotaCutoverPlanAcquire>(&plan)) _sql.write("kernel_cutover", kernel::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, acquire->marker, now + acquire->expireIn);
    return {{"kind", kind(plan)}, {"detail", planJson(plan)}};
  }
  NativeReply NativeAuthorityKernel::_setPolicy(const KernelQuotaIdentity& identity, const NativeArguments::setKernelQuota& args, const KernelQuotaEffectivePolicy& current, std::optional<KernelQuotaWindowV2> window, double now) {
    const auto timing = kernel::resolveKernelPolicyTiming(args.options, current, _initial.kernelConstants.noExpirationMs); const bool reset = kernel::kernelQuotaPolicyResetRequested(args.options, current.windowMs, timing.windowMs);
    if (std::holds_alternative<kernel::KernelPolicyWindowGuardReclaim>(kernel::decideKernelPolicyWindowGuard(window, window.has_value(), reset, now))) { _reclaimWindow(identity, now); window = _readWindow(identity, now).value; if (kernel::hasLiveKernelReservations(window, now)) return {{"kind", "conflict"}}; }
    const auto policy = kernel::makeKernelQuotaPolicy(identity, current, args.usageLimitRequests, timing, _initial.kernelConstants.noUsageLimit, now); const auto plan = kernel::decideKernelQuotaPolicyWrite(identity, policy, window, reset, now);
    _sql.write("kernel_policy", kernel::kernelScopedPolicyKey(identity.scope, identity.owner, identity.repo), plan.policy); _sql.write("kernel_window", kernel::kernelScopedWindowKey(identity.scope, identity.owner, identity.repo), plan.window); return {{"kind", "written"}, {"detail", json::value_from(plan.policy)}};
  }
  NativeReply NativeAuthorityKernel::_policy(const NativeCapability& cap, const std::string& scope, const std::optional<NativeArguments::setKernelQuota>& args, double now) {
    const auto identity = nativeIdentity(scope, cap); const auto keys = kernel::kernelPolicyKeys(identity.scope, identity.owner, identity.repo);
    const auto entry = read(_sql, "kernel_policy", keys.policyKey, now), marker = read(_sql, "kernel_cutover", kernel::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, now);
    const auto current = kernel::policyFor(entry, identity.scope, identity.owner, identity.repo, _defaults(now), _initial.kernelConstants);
    if (kernel::kernelQuotaDefaultCutoverBlocks(current, marker)) return {{"kind", "conflict"}};
    if (identity.scope == "repo" && entry.value.is_null()) { const auto org = nativeIdentity("org", cap); if (kernel::hasLiveKernelReservations(_readWindow(org, now).value, now)) { _reclaimWindow(org, now); if (kernel::hasLiveKernelReservations(_readWindow(org, now).value, now)) return {{"kind", "conflict"}}; } }
    auto window = _readWindow(identity, now).value;
    if (args) return _setPolicy(identity, *args, current, window, now);
    const auto pre = kernel::decideKernelQuotaPolicyDeletePrecondition({entry, marker});
    if (pre != "proceed") return {{"kind", pre}};
    if (kernel::hasLiveKernelReservations(window, now)) { _reclaimWindow(identity, now); window = _readWindow(identity, now).value; if (kernel::hasLiveKernelReservations(window, now)) return {{"kind", "conflict"}}; }
    const auto next = kernel::decideKernelQuotaPolicyDeleteWindow(identity, current, _defaults(now), window, now); _sql.delete_("kernel_policy", keys.policyKey); _sql.write("kernel_window", keys.windowKey, next); return {{"kind", "deleted"}};
  }
  NativeReply NativeAuthorityKernel::policy(const NativeCapability& cap, const NativeArguments::setKernelQuota& args, double now) { return _policy(cap, args.scope, args, now); }
  NativeReply NativeAuthorityKernel::policy(const NativeCapability& cap, const NativeArguments::deleteKernelQuota& args, double now) { return _policy(cap, args.scope, std::nullopt, now); }
}
