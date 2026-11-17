// Port of src/authority/native-authority.ts.
#include "uos/authority/native_authority.hpp"
#include "uos/authority/native_authority_codex.hpp"
#include "uos/authority/native_authority_kernel.hpp"
#include "uos/authority/native_authority_principals.hpp"
#include "uos/authority/native_authority_runtime.hpp"
#include "uos/api_keys.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include <algorithm>
#include <initializer_list>

namespace uos::authority::native_authority {
  using namespace native_authority_contracts;
  namespace runtime = native_runtime_contracts;
  namespace rpc = native_authority_rpc;
  namespace principal = native_principal_contracts;
  namespace paid = native_paid;
  namespace {
    bool oneOf(const std::string& value, std::initializer_list<std::string_view> values) { return std::find(values.begin(), values.end(), value) != values.end(); }
    bool permits(const NativeCapability& cap, std::string_view role) { return std::find(cap.roles.begin(), cap.roles.end(), role) != cap.roles.end(); }
    std::string text(const json::value& value, const std::string& key) { return std::string(rt::getString(value, key).value_or("")); }
    std::string text(const json::object& value, const std::string& key) { return std::string(rt::getString(value, key).value_or("")); }
    template<class T> T arguments(const NativeCommand& command) { return json::value_to<T>(json::value(command.args)); }
    const json::value& member(const json::value& value, const std::string& key) { const auto* result = rt::get(value, key); if (!result) throw rt::TypeError("undefined is not an object"); return *result; }
    const json::value& member(const json::object& value, const std::string& key) { const auto* result = rt::get(value, key); if (!result) throw rt::TypeError("undefined is not an object"); return *result; }
  }
  struct NativeAuthority::State {
    NativeAuthorityInitial initial;
    std::function<double()> _clock;
    std::shared_ptr<principal::NativePrincipalControlInitial> controlInitial;
    std::shared_ptr<runtime::NativeRuntimeInitial> runtimeInitial;
    native_authority_sql::NativeAuthoritySql _sql;
    native_authority_api::NativeAuthorityApi _api;
    native_authority_kernel::NativeAuthorityKernel _kernel;
    native_authority_codex::NativeAuthorityCodex _codex;
    native_authority_paid::NativeAuthorityPaid _paid;
    std::unique_ptr<native_authority_principals::NativeAuthorityPrincipals> _principals;
    std::unique_ptr<native_authority_runtime::NativeAuthorityRuntime> _runtime;
    State(NativeSqlStorage storage, NativeAuthorityInitial seed, std::function<double()> clock,
          std::shared_ptr<principal::NativePrincipalControlInitial> control, std::shared_ptr<runtime::NativeRuntimeInitial> runtimeSeed)
      : initial(std::move(seed)), _clock(std::move(clock)), controlInitial(std::move(control)), runtimeInitial(std::move(runtimeSeed)),
        _sql(std::move(storage), initial.namespace_, initial.epoch), _api(_sql), _kernel(_sql, _api, initial), _codex(_sql, _api), _paid(_sql, _api) {
      if (runtimeInitial) _runtime = std::make_unique<native_authority_runtime::NativeAuthorityRuntime>(_sql, _api, *runtimeInitial);
      if (controlInitial) _principals = std::make_unique<native_authority_principals::NativeAuthorityPrincipals>(_sql, _api, _paid, *controlInitial, _clock);
      _sql.initialize([&] {
        _sql.write("authority_config", {"config"}, NativeAuthorityConfig{initial.providerHops});
        _sql.write("kernel_default", {"default", "kernel_policy_limit_requests"}, initial.kernelDefaults.limit);
        _sql.write("kernel_default", {"default", "kernel_policy_window_ms"}, initial.kernelDefaults.windowMs);
        for (const auto& key : initial.keys) _sql.write("api_policy", api_keys::apiKeyHashKey(key.api.token_hash), key);
        for (const auto& policy : initial.kernelPolicies) _sql.write("kernel_policy", native_kernel::kernelScopedPolicyKey(policy.scope, policy.owner, policy.repo), policy);
        _sql.write("codex_pool", native_codex_contracts::CODEX_AUTH_POOL_KV_KEY, initial.pool);
        if (initial.routing) _sql.write("codex_routing", native_codex_contracts::CODEX_ACCOUNT_ROUTING_KV_KEY, native_codex_contracts::toJson(*initial.routing));
        if (initial.active) _sql.write("codex_active", native_codex_contracts::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, native_codex_contracts::toJson(*initial.active));
        _sql.write("codex_capacity", native_codex_contracts::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY, initial.capacity);
        for (const auto& owner : initial.owners) _sql.write("codex_owner", {owner.routingAccountIdHash}, owner);
        for (const auto& proof : initial.resetProofs) _sql.write("codex_reset", {proof.id}, proof);
        if (_principals) _principals->initialize();
        if (_runtime) _runtime->initialize();
      });
    }
    rt::Task<principal::NativeControlReply> control(principal::NativeControlBinding binding, principal::NativeControlCommand command) {
      if (!_principals) throw NativeAuthorityError("principal_control_unavailable", 503);
      auto task = _principals->control(binding, command); rt::Promise<principal::NativeControlReply> pending(std::move(task));
      auto reply = co_await pending.wait(); co_return reply;
    }
    rt::Task<NativeReply> _executeRetained(runtime::NativeRetainedHistoryBinding binding, NativeCommand input) {
      if (!_runtime) throw NativeAuthorityError("retained_history_unavailable", 503);
      const auto command = rpc::parseNativeCommand(json::value_from(input));
      const auto canonical = rpc::nativeCommandBytes(json::value_from(command));
      auto task = _runtime->verifyRetained(binding, canonical, _clock()); rt::Promise<runtime::VerifiedNativeRetainedHistory> pending(std::move(task));
      const auto verified = co_await pending.wait();
      co_return _sql.transaction([&]() -> NativeReply {
        const auto now = _clock(); const auto cap = _runtime->authorizeRetained(binding, verified, command, now);
        if (!permits(cap, text(rpc::NATIVE_METHOD_ROLES, command.method))) throw NativeAuthorityError("authority_capability_denied", 403);
        auto scoped = json::value_from(verified); scoped.as_object()["intentDigest"] = nullptr;
        const auto scope = rpc::nativeCommandBytes(scoped);
        const NativeKey key{_sql.epoch, "retained-history", binding.grantId, cap.id, command.operationId};
        const auto prior = _sql.read("operation_receipt", key, now).value;
        if (prior && rt::truthy(*prior)) {
          if (text(*prior, "command") != canonical || text(*prior, "retainedScope") != scope) throw NativeAuthorityError("retained_operation_replay_conflict");
          return member(*prior, "reply").as_object();
        }
        NativeReply reply;
        if (command.method == "releaseApiUndispatched") reply = _api.release(cap, arguments<NativeArguments::releaseApiUndispatched>(command), now);
        else if (command.method == "releasePaidBeforeFetch") reply = _paid.release(cap, text(command.args, "attemptHandle"), now);
        else if (command.method == "recordPaidAttemptLifecycle") reply = _paid.lifecycle(cap, arguments<NativeArguments::recordPaidAttemptLifecycle>(command), now);
        else if (command.method == "recordVerifiedPaidBilling") reply = _paid.billing(cap, arguments<NativeArguments::recordVerifiedPaidBilling>(command), now);
        else if (command.method == "deferPaidReconciliation") reply = _paid.defer(cap, text(command.args, "attemptHandle"), now);
        else if (command.method == "claimPaidReconciliation") reply = _paid.claim(cap, arguments<NativeArguments::claimPaidReconciliation>(command), now);
        else if (command.method == "finishPaidReconciliation") reply = _paid.finish(cap, text(command.args, "lease"), now);
        else throw NativeAuthorityError("retained_operation_denied", 403);
        _sql.write("operation_receipt", key, json::object{{"command", canonical}, {"reply", reply}, {"retainedScope", scope}}, std::nullopt, {.scope = cap.keyId.value_or(cap.id)});
        return reply;
      });
    }
    NativeCapability _authorize(const runtime::NativeRuntimeAuthorization& auth, const NativeCommand& command, double now) {
      _sql.assertEpoch();
      if (command.epoch != _sql.epoch) throw NativeAuthorityError("authority_epoch_mismatch", 403);
      if (!_runtime) throw NativeAuthorityError("native_runtime_unavailable", 503);
      const auto cap = _runtime->authorize(auth, now);
      if (!permits(cap, text(rpc::NATIVE_METHOD_ROLES, command.method))) throw NativeAuthorityError("authority_capability_denied", 403);
      if (oneOf(command.method, {"bindValidatedRequestMetadata", "reserveKernelUsage", "selectCodexAccount", "electCodexResetProbe", "refreshCodexAdmission"})) _runtime->assertCurrentApi(cap, now);
      if (rt::hasOwn(command.args, "handle")) _api.request(cap, text(command.args, "handle"), now);
      if (rt::hasOwn(command.args, "attemptHandle")) _paid.attempt(cap, text(command.args, "attemptHandle"), now);
      return cap;
    }
    rt::Task<NativeReply> _bindTrustedRoutingDecision(runtime::NativeRuntimeBinding binding, std::string handle, std::string operationId) {
      if (!_runtime) throw NativeAuthorityError("native_runtime_unavailable", 503);
      const json::value value(json::object{{"revision", 3}, {"method", "privateRouting"}, {"handle", handle}, {"operationId", operationId}, {"epoch", _sql.epoch}});
      const auto intent = rpc::nativeCommandBytes(value);
      auto verification = _runtime->verify(binding, intent, _clock()); rt::Promise<runtime::NativeRuntimeAuthorization> verified(std::move(verification));
      const auto auth = co_await verified.wait();
      const auto request = _sql.transaction([&] { return _api.validated(_runtime->authorize(auth, _clock()), handle, _clock()); });
      auto routing = _runtime->routing(binding, request, intent, _clock()); rt::Promise<runtime::NativeTrustedRoutingDecision> routed(std::move(routing));
      const auto decision = co_await routed.wait();
      co_return _sql.transaction([&]() -> NativeReply {
        const auto now = _clock(); const auto cap = _runtime->authorize(auth, now);
        if (!permits(cap, "inference")) throw NativeAuthorityError("authority_capability_denied", 403);
        const auto current = _api.validated(cap, handle, now);
        const NativeKey key{_sql.epoch, cap.principalId, cap.id, "private-routing", operationId};
        const auto prior = _sql.read("operation_receipt", key, now).value;
        if (prior && rt::truthy(*prior) && text(*prior, "command") != intent) throw NativeAuthorityError("authority_operation_replay_conflict");
        const auto reply = _runtime->bindRouting(cap, current, decision, now);
        if (prior && rt::truthy(*prior)) return member(*prior, "reply").as_object();
        _sql.write("operation_receipt", key, json::object{{"command", intent}, {"reply", reply}}, std::nullopt, {.scope = cap.keyId.value_or(cap.principalId)});
        return reply;
      });
    }
    NativeRequest _requestForDispatch(const NativeCapability& cap, const std::string& handle, const std::string& provider, double now) {
      auto request = _api.validated(cap, handle, now);
      if (_runtime) _runtime->assertCurrentApi(cap, now);
      if (!request.routingDecision) throw NativeAuthorityError("trusted_routing_missing");
      const auto& providers = request.routingDecision->providers;
      if (std::find(providers.begin(), providers.end(), provider) == providers.end()) throw NativeAuthorityError("request_provider_not_authorized", 403);
      if (provider == "chatgpt_codex") _codex.fence(cap, handle, now);
      return request;
    }
    void _terminalUsage(const NativeRequest& request, const std::optional<std::string>& provider, const NativeTerminalUsage& observations, double now) {
      if (!request.model) throw NativeAuthorityError("validated_metadata_missing");
      const native_paid_contracts::TerminalUsageRollupInput input{request.model, provider, request.requestId, request.createdAtMs, observations.input_tokens, observations.cached_input_tokens, observations.output_tokens};
      const auto usage = paid::paidFallbackTerminalUsageV3(input);
      if (usage) {
        const auto row = paid::paidFallbackTerminalRollupV3(input, *usage, request.requestId, now);
        const auto key = paid::paidFallbackUsageRollupKey(row.bucket_start_at_ms, row.model, row.provider, paid::paidFallbackUsageRollupShard(request.requestId));
        const auto prior = _sql.read("paid_rollup", key, now).value;
        const auto existing = paid::isPaidFallbackUsageRollup(prior ? &*prior : nullptr) ? std::optional<native_paid_contracts::PaidFallbackUsageRollup>(json::value_to<native_paid_contracts::PaidFallbackUsageRollup>(*prior)) : std::nullopt;
        _sql.write("paid_rollup", key, paid::mergePaidFallbackUsageRollup(existing, row));
      }
    }
    NativeReply _observation(const NativeCapability& cap, const NativeCommand& command, double now) {
      auto request = _api.request(cap, text(command.args, "handle"), now);
      if (command.method == "recordDeliveryObservation") {
        const auto observation = text(command.args, "observation");
        if (request.delivery) return {{"kind", *request.delivery == observation ? "unchanged" : "observation_conflict"}, {"detail", *request.delivery}};
        request.delivery = observation; _api.save(request); return {{"kind", "recorded"}};
      }
      const auto args = arguments<NativeArguments::recordSemanticTerminal>(command);
      if (request.semantic) return {{"kind", *request.semantic == args.outcome ? "unchanged" : "terminal_mismatch"}, {"detail", *request.semantic}};
      const auto responding = args.attemptHandle && !args.attemptHandle->empty() ? std::optional<native_authority_paid::NativePaidResponse>(_paid.response(cap, *args.attemptHandle, now)) : std::nullopt;
      if (responding && responding->attempt.requestHandle != request.handle) throw NativeAuthorityError("terminal_attempt_mismatch", 403);
      const auto responseOwner = responding ? std::optional<NativeRequestResponding>({responding->provider, responding->attempt.handle}) : request.responding;
      request.semantic = args.outcome; request.responding = responseOwner; _api.save(request);
      if (args.usage) _terminalUsage(request, responseOwner ? std::optional<std::string>(responseOwner->provider) : std::nullopt, *args.usage, now);
      return {{"kind", "recorded"}};
    }
    NativeReply _status(const NativeCapability& cap, const NativeArguments::readRequestStatus& args, double now) {
      const auto request = _api.request(cap, args.handle, now); std::string after;
      if (args.cursor && !args.cursor->empty()) {
        const auto cursor = _sql.read("cursor", {*args.cursor}, now).value.value_or(nullptr);
        if (text(cursor, "capabilityId") != cap.id || text(cursor, "family") != "request_attempts" || text(cursor, "parent") != request.handle) throw NativeAuthorityError("cursor_ownership_mismatch", 403);
        after = text(cursor, "after");
      }
      const auto page = _sql.page("attempt_lifetime", now, after, args.limit, {.parent = request.handle});
      json::value cursor = nullptr;
      if (static_cast<double>(page.size()) == args.limit) {
        const auto id = rt::crypto::randomUUID(); cursor = id;
        _sql.write("cursor", {id}, json::object{{"id", id}, {"capabilityId", cap.id}, {"family", "request_attempts"}, {"parent", request.handle}, {"after", page.back().encoded}, {"lease", nullptr}});
      }
      json::array attempts;
      for (const auto& entry : page) {
        if (!entry.row.value || !rt::truthy(*entry.row.value)) throw NativeAuthorityError("attempt_malformed", 503);
        const auto& attempt = *entry.row.value;
        attempts.push_back(json::object{{"attemptHandle", member(attempt, "handle")}, {"hopId", member(attempt, "hopId")}, {"row", _sql.read("paid_request", paid::paidFallbackRequestV3Key(text(attempt, "keyId"), text(attempt, "billingRequestId")), now).value.value_or(nullptr)}});
      }
      const auto status = _api.status(cap, request.handle, now);
      const json::object lifetime{{"requestId", request.requestId}, {"route", request.route}, {"model", request.model ? json::value(*request.model) : json::value(nullptr)}, {"semantic", request.semantic ? json::value(*request.semantic) : json::value(nullptr)}, {"delivery", request.delivery ? json::value(*request.delivery) : json::value(nullptr)}};
      return {{"kind", "status"}, {"handle", request.handle}, {"cursor", cursor}, {"detail", json::object{{"lifetime", lifetime}, {"api", member(status, "detail")}, {"attempts", attempts}}}};
    }
    void _validateReceipt(const NativeCapability& cap, const NativeCommand& command, const json::value& receipt, const std::string& canonical, double now) {
      if (text(receipt, "command") != canonical) throw NativeAuthorityError("authority_operation_replay_conflict");
      if (oneOf(command.method, {"openAuthenticatedRequest", "reserveApiUsage", "reservePaidAttempt", "bindValidatedRequestMetadata"}) && _runtime) _runtime->assertCurrentApi(cap, now);
      const auto kind = text(member(receipt, "reply"), "kind");
      if (command.method == "markApiDispatch" && kind == "dispatched") _api.assertDispatchReceipt(cap, text(command.args, "handle"), now);
      if (command.method == "refreshCodexAdmission" && kind == "current") {
        const auto* auth = rt::get(receipt, "codexAuthorization");
        if (!auth || !rt::truthy(*auth)) throw NativeAuthorityError("cached_codex_authorization_missing");
        _codex.fence(cap, text(command.args, "handle"), now, json::value_to<NativeCodexAuthorization>(*auth));
      }
      if (command.method == "recordTransportStarted") {
        const auto args = arguments<NativeArguments::recordTransportStarted>(command);
        _requestForDispatch(cap, args.handle, args.provider, now); _paid.assertStart(cap, args, now);
        if (kind == "started") _api.assertTransportReceipt(cap, args.handle, command.operationId, now);
        else _api.assertDispatchReceipt(cap, args.handle, now);
      }
    }
    rt::Task<NativeReply> _execute(runtime::NativeRuntimeBinding binding, NativeCommand input) {
      const auto command = rpc::parseNativeCommand(json::value_from(input)); const auto observedAt = _clock();
      if (!_runtime) throw NativeAuthorityError("native_runtime_unavailable", 503);
      const auto canonical = rpc::nativeCommandBytes(json::value_from(command));
      auto verification = _runtime->verify(binding, canonical, observedAt); rt::Promise<runtime::NativeRuntimeAuthorization> verified(std::move(verification));
      const auto auth = co_await verified.wait();
      _sql.transaction([&] { _authorize(auth, command, observedAt); });
      std::optional<runtime::NativeTrustedRoutingDecision> routing;
      if (oneOf(command.method, {"selectCodexAccount", "reservePaidAttempt", "markApiDispatch", "recordTransportStarted", "refreshCodexAdmission"}) && rt::hasOwn(command.args, "handle")) {
        const auto handle = text(command.args, "handle");
        const auto request = _sql.transaction([&] { return _api.validated(_authorize(auth, command, _clock()), handle, _clock()); });
        auto routingTask = _runtime->routing(binding, request, canonical, _clock()); rt::Promise<runtime::NativeTrustedRoutingDecision> pending(std::move(routingTask));
        routing = co_await pending.wait();
      }
      // Classification consumes the actual bounded response outside the synchronous storage transaction.
      std::optional<native_codex_429::Codex429Classification> parsed;
      if (command.method == "recordCodexAttemptSignal") {
        const auto args = arguments<NativeArguments::recordCodexAttemptSignal>(command);
        if (args.signal == "quota" && args.response) {
          rt::Headers headers; for (const auto& pair : args.response->headers) headers.append(pair[0], pair[1]);
          const rt::Response response(rt::BodyInit(args.response->body), rt::ResponseInit{.status = args.response->status, .headers = headers});
          auto classification = native_codex_429::classifyCodex429(response, observedAt); rt::Promise<native_codex_429::Codex429Classification> pending(std::move(classification));
          parsed = co_await pending.wait();
        }
      }
      co_return _sql.transaction([&]() -> NativeReply {
        const auto now = _clock(); const auto cap = _authorize(auth, command, now);
        if (routing && rt::hasOwn(command.args, "handle")) _runtime->assertRouting(cap, _api.validated(cap, text(command.args, "handle"), now), *routing, now);
        // Current dispatch/owner/epoch checks precede receipt lookup; a lost reply cannot revive revoked authority.
        if (command.method == "markApiDispatch") _requestForDispatch(cap, text(command.args, "handle"), text(command.args, "provider"), now);
        const NativeKey key{_sql.epoch, cap.principalId, cap.id, command.operationId};
        const auto receipt = _sql.read("operation_receipt", key, now).value;
        if (receipt && rt::truthy(*receipt)) { _validateReceipt(cap, command, *receipt, canonical, now); return member(*receipt, "reply").as_object(); }
        NativeReply reply;
        if (command.method == "openAuthenticatedRequest") reply = _runtime->open(cap, arguments<NativeArguments::openAuthenticatedRequest>(command), now);
        else if (command.method == "bindValidatedRequestMetadata") reply = _runtime->metadata(cap, arguments<NativeArguments::bindValidatedRequestMetadata>(command), now);
        else if (command.method == "reserveApiUsage") reply = _api.reserve(cap, arguments<NativeArguments::reserveApiUsage>(command), now);
        else if (command.method == "recordTransportStarted") { const auto args = arguments<NativeArguments::recordTransportStarted>(command); _requestForDispatch(cap, args.handle, args.provider, now); _paid.start(cap, args, now); reply = _api.start(cap, args, command.operationId, now); }
        else if (command.method == "markApiDispatch") reply = _api.dispatch(cap, arguments<NativeArguments::markApiDispatch>(command), now);
        else if (command.method == "releaseApiUndispatched") reply = _api.release(cap, arguments<NativeArguments::releaseApiUndispatched>(command), now);
        else if (command.method == "recordApiDispatchCancellation") reply = _api.cancel(cap, text(command.args, "handle"), now);
        else if (command.method == "reserveKernelUsage") reply = _kernel.reserve(cap, text(command.args, "handle"), now);
        else if (command.method == "recordKernelTerminalIntent") reply = _kernel.intent(cap, arguments<NativeArguments::recordKernelTerminalIntent>(command), now);
        else if (command.method == "settleKernelUsage") reply = _kernel.settle(cap, arguments<NativeArguments::settleKernelUsage>(command), now);
        else if (command.method == "renewKernelReservation") reply = _kernel.renew(cap, text(command.args, "handle"), now);
        else if (command.method == "reclaimExpiredKernelReservations") reply = _kernel.reclaim(cap, arguments<NativeArguments::reclaimExpiredKernelReservations>(command), now);
        else if (command.method == "setKernelQuota") reply = _kernel.policy(cap, arguments<NativeArguments::setKernelQuota>(command), now);
        else if (command.method == "deleteKernelQuota") reply = _kernel.policy(cap, arguments<NativeArguments::deleteKernelQuota>(command), now);
        else if (command.method == "acquireKernelDefaultCutover") reply = _kernel.cutover(now);
        else if (command.method == "selectCodexAccount") { const auto request = _api.validated(cap, text(command.args, "handle"), now); if (!request.routingDecision || request.routingDecision->codex != "eligible") throw NativeAuthorityError("request_codex_not_authorized", 403); reply = _codex.select(cap, request.handle, now); }
        else if (command.method == "recordCodexAttemptSignal") reply = _codex.signal(cap, arguments<NativeArguments::recordCodexAttemptSignal>(command), parsed ? &*parsed : nullptr, now);
        else if (command.method == "releaseCodexProbe") reply = _codex.release(cap, text(command.args, "handle"), now);
        else if (command.method == "electCodexResetProbe") reply = _codex.elect(cap, arguments<NativeArguments::electCodexResetProbe>(command), now);
        else if (command.method == "refreshCodexAdmission") reply = _codex.refresh(cap, text(command.args, "handle"), now);
        else if (command.method == "reservePaidAttempt") reply = _paid.reserve(cap, arguments<NativeArguments::reservePaidAttempt>(command), now);
        else if (command.method == "recordPaidAttemptLifecycle") reply = _paid.lifecycle(cap, arguments<NativeArguments::recordPaidAttemptLifecycle>(command), now);
        else if (command.method == "releasePaidBeforeFetch") reply = _paid.release(cap, text(command.args, "attemptHandle"), now);
        else if (command.method == "recordVerifiedPaidBilling") reply = _paid.billing(cap, arguments<NativeArguments::recordVerifiedPaidBilling>(command), now);
        else if (command.method == "deferPaidReconciliation") reply = _paid.defer(cap, text(command.args, "attemptHandle"), now);
        else if (command.method == "claimPaidReconciliation") reply = _paid.claim(cap, arguments<NativeArguments::claimPaidReconciliation>(command), now);
        else if (command.method == "finishPaidReconciliation") reply = _paid.finish(cap, text(command.args, "lease"), now);
        else if (command.method == "foldPaidBackfillPage") reply = _paid.backfill(cap, arguments<NativeArguments::foldPaidBackfillPage>(command), now);
        else if (command.method == "beginPaidKeyDeletion") reply = _paid.deletion(cap, now);
        else if (command.method == "recordSemanticTerminal" || command.method == "recordDeliveryObservation") reply = _observation(cap, command, now);
        else if (command.method == "readRequestStatus") reply = _status(cap, arguments<NativeArguments::readRequestStatus>(command), now);
        else throw NativeAuthorityError("invalid_authority_operation", 400);
        const auto committed = rt::jsonParse(rt::jsonStringify(reply)).as_object();
        json::object saved{{"command", canonical}, {"reply", committed}};
        if (command.method == "refreshCodexAdmission" && text(committed, "kind") == "current") {
          const auto handle = text(command.args, "handle"); _codex.authorization(cap, handle, now);
          const auto request = _sql.read("request_lifetime", {handle}, now).value.value();
          saved["codexAuthorization"] = json::object{{"account", member(request, "codex")}, {"owner", member(request, "ownerFence")}};
        }
        _sql.write("operation_receipt", key, saved, std::nullopt, {.scope = cap.keyId.value_or(cap.principalId)});
        return committed;
      });
    }
  };
  NativeAuthority::NativeAuthority(NativeSqlStorage storage, NativeAuthorityInitial initial, std::function<double()> clock,
                                   std::optional<principal::NativePrincipalControlInitial> control, std::optional<runtime::NativeRuntimeInitial> runtimeSeed)
    : NativeAuthority(std::move(storage), std::move(initial), std::move(clock),
                      control ? std::make_shared<principal::NativePrincipalControlInitial>(std::move(*control)) : nullptr,
                      runtimeSeed ? std::make_shared<runtime::NativeRuntimeInitial>(std::move(*runtimeSeed)) : nullptr) {}
  NativeAuthority::NativeAuthority(NativeSqlStorage storage, NativeAuthorityInitial initial, std::function<double()> clock,
                                   std::shared_ptr<principal::NativePrincipalControlInitial> control, std::shared_ptr<runtime::NativeRuntimeInitial> runtimeSeed) {
    if (initial.kernelConstants.noUsageLimit != api_keys::API_KEY_NO_USAGE_LIMIT || initial.kernelConstants.noExpirationMs != api_keys::API_KEY_NO_EXPIRATION_MS) throw NativeAuthorityError("native_constants_mismatch", 500);
    if (!clock) clock = [] { return rt::dateNow(); };
    _state = std::make_shared<State>(std::move(storage), std::move(initial), std::move(clock), std::move(control), std::move(runtimeSeed));
  }
  rt::Task<principal::NativeControlReply> NativeAuthority::control(principal::NativeControlBinding binding, principal::NativeControlCommand command) {
    return rt::async([state = _state, binding = std::move(binding), command = std::move(command)] { return state->control(binding, command); });
  }
  rpc::NativeBoundRuntimePort NativeAuthority::bindAuthenticatedRuntime(runtime::NativeRuntimeBinding binding) const {
    if (!_state->_runtime) throw NativeAuthorityError("native_runtime_unavailable", 503);
    const auto state = _state;
    return {[state, binding](NativeCommand command) { return rt::async([state, binding, command = std::move(command)] { return state->_execute(binding, command); }); },
            [state, binding](std::string handle, std::string operationId) { return rt::async([state, binding, handle = std::move(handle), operationId = std::move(operationId)] { return state->_bindTrustedRoutingDecision(binding, handle, operationId); }); }};
  }
  rpc::NativeBoundRuntimePort NativeAuthority::bindAuthenticatedRuntime(const json::value& binding) const {
    if (!_state->_runtime) throw NativeAuthorityError("native_runtime_unavailable", 503);
    if (!nativeObject(binding) || binding.as_object().size() != 3 || !rt::getString(binding, "grantId") || !rt::getString(binding, "contextBinding") || !rt::hasOwn(binding.as_object(), "privateContext")) throw NativeAuthorityError("invalid_private_runtime_binding", 403);
    return bindAuthenticatedRuntime(json::value_to<runtime::NativeRuntimeBinding>(binding));
  }
  rpc::NativeRetainedHistoryPort NativeAuthority::bindRetainedHistory(runtime::NativeRetainedHistoryBinding binding) const {
    if (binding.kind != "retained_history") throw NativeAuthorityError("invalid_retained_history_binding", 403);
    const auto state = _state;
    return {[state, binding](NativeCommand command) { return rt::async([state, binding, command = std::move(command)] { return state->_executeRetained(binding, command); }); }};
  }
  rpc::NativeRetainedHistoryPort NativeAuthority::bindRetainedHistory(const json::value& binding) const {
    if (!nativeObject(binding) || binding.as_object().size() != 4 || rt::getString(binding, "kind") != "retained_history" || !rt::getString(binding, "grantId") || !rt::getString(binding, "contextBinding") || !rt::hasOwn(binding.as_object(), "privateContext")) throw NativeAuthorityError("invalid_retained_history_binding", 403);
    return bindRetainedHistory(json::value_to<runtime::NativeRetainedHistoryBinding>(binding));
  }
  NativeAuthority::operator rpc::NativeOperationRunner() const { return {[self = *this](runtime::NativeRuntimeBinding binding) { return self.bindAuthenticatedRuntime(std::move(binding)); }}; }
  NativeAuthority::operator rpc::NativeRetainedHistoryRunner() const { return {[self = *this](runtime::NativeRetainedHistoryBinding binding) { return self.bindRetainedHistory(std::move(binding)); }}; }
  NativeAuthority::operator principal::NativePrincipalControlRunner() const { return {[self = *this](principal::NativeControlBinding binding, principal::NativeControlCommand command) mutable { return self.control(std::move(binding), std::move(command)); }}; }
}
