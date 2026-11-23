// Port of src/authority/native-authority-rpc.ts.
#include "uos/authority/native_authority_rpc.hpp"
#include "uos/authority/native_api_key.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace uos::authority::native_authority_rpc {
  namespace {
    using native_authority_contracts::NativeAuthorityError;
    using native_authority_contracts::NativeCommand;
    using native_authority_contracts::NativeReply;
    using native_authority_contracts::nativeObject;
    using native_principal_contracts::NativeControlCommand;
    using Guard = std::function<bool(const json::value*)>;
    using Fields = std::vector<std::pair<std::string, Guard>>;
    const Guard text = [](const json::value* value) {
      if (!value || !value->is_string()) return false;
      const std::string raw(value->as_string());
      if (raw.empty() || rt::length(raw) > 1000) return false;
      for (const auto cp : rt::codePoints(raw)) if (rt::leadCodeUnit(cp) < 32) return false;
      return true;
    };
    const Guard integer = [](const json::value* value) { const auto n = rt::getNumber(value); return n && rt::Number::isSafeInteger(*n) && *n >= 0; };
    const Guard finite = [](const json::value* value) { const auto n = rt::getNumber(value); return n && rt::Number::isFinite(*n) && *n >= 0; };
    const Guard boolean = [](const json::value* value) { return value && value->is_bool(); };
    Guard oneOf(std::vector<json::value> values) { return [values = std::move(values)](const json::value* value) { return value && std::any_of(values.begin(), values.end(), [&](const auto& item) { return rt::strictEquals(*value, item); }); }; }
    Guard nullable(Guard guard) { return [guard = std::move(guard)](const json::value* value) { return value && (value->is_null() || guard(value)); }; }
    Guard optional(Guard guard) { return [guard = std::move(guard)](const json::value* value) { return !value || guard(value); }; }
    Guard shape(Fields fields) {
      return [fields = std::move(fields)](const json::value* value) {
        if (!nativeObject(value)) return false;
        for (const auto& key : rt::objectKeys(value->as_object())) if (std::none_of(fields.begin(), fields.end(), [&](const auto& item) { return item.first == key; })) return false;
        for (const auto& [key, guard] : fields) if (!guard(rt::get(*value, key))) return false;
        return true;
      };
    }
    Guard provider() {
      std::vector<json::value> values;
      for (const auto& item : native_api_key::API_KEY_USAGE_PROVIDERS) values.emplace_back(item);
      return oneOf(std::move(values));
    }
    const std::vector<std::pair<std::string, Guard>>& guards() {
      static const auto value = [] {
        const Guard terminal = oneOf({"committed", "released"});
        const Guard scope = oneOf({"repo", "org"});
        const Guard signedInteger = [](const json::value* item) { const auto n = rt::getNumber(item); return n && rt::Number::isSafeInteger(*n) && *n >= -1; };
        const Guard pageLimit = [](const json::value* item) { const auto n = rt::getNumber(item); return integer(item) && n && *n >= 1 && *n <= 100; };
        const Guard usage = shape({{"input_tokens", nullable(integer)}, {"cached_input_tokens", nullable(integer)}, {"output_tokens", nullable(integer)}});
        const Guard headerPair = [](const json::value* item) {
          const auto* array = item ? item->if_array() : nullptr;
          if (!array || array->size() != 2 || !text(rt::get(*array, 0))) return false;
          const auto* second = rt::get(*array, 1);
          return second && second->is_string() && rt::length(std::string(second->as_string())) <= 4000;
        };
        const Guard response = shape({{"status", oneOf({429})}, {"headers", [headerPair](const json::value* item) {
          const auto* array = item ? item->if_array() : nullptr;
          return array && array->size() <= 64 && std::all_of(array->begin(), array->end(), [&](const auto& pair) { return headerPair(&pair); });
        }}, {"body", [](const json::value* item) { return item && item->is_string() && rt::length(std::string(item->as_string())) <= 48000; }}});
        return std::vector<std::pair<std::string, Guard>>{
          {"openAuthenticatedRequest", shape({{"requestId", text}, {"route", text}, {"path", text}})},
          {"reserveApiUsage", shape({{"handle", text}})},
          {"bindValidatedRequestMetadata", shape({{"handle", text}, {"model", text}, {"stream", boolean}, {"reasoning", nullable(text)}})},
          {"recordTransportStarted", shape({{"handle", text}, {"provider", provider()}, {"attemptHandle", optional(text)}})},
          {"markApiDispatch", shape({{"handle", text}, {"provider", provider()}})},
          {"releaseApiUndispatched", shape({{"handle", text}, {"reason", text}})},
          {"recordApiDispatchCancellation", shape({{"handle", text}})},
          {"reserveKernelUsage", shape({{"handle", text}})},
          {"recordKernelTerminalIntent", shape({{"handle", text}, {"terminalState", terminal}})},
          {"settleKernelUsage", shape({{"handle", text}, {"terminalState", terminal}, {"reason", text}})},
          {"renewKernelReservation", shape({{"handle", text}})},
          {"reclaimExpiredKernelReservations", shape({{"cursor", optional(text)}, {"limit", pageLimit}})},
          {"setKernelQuota", shape({{"scope", scope}, {"usageLimitRequests", signedInteger}, {"options", shape({{"resetUsage", optional(boolean)}, {"windowMs", optional([](const json::value* item) { const auto n = rt::getNumber(item); return integer(item) && n && *n > 0; })}, {"expiresAtMs", optional(signedInteger)}})}})},
          {"deleteKernelQuota", shape({{"scope", scope}})},
          {"acquireKernelDefaultCutover", shape({})},
          {"selectCodexAccount", shape({{"handle", text}})},
          {"recordCodexAttemptSignal", shape({{"handle", text}, {"signal", oneOf({"success", "credential_invalid", "upstream_timeout", "quota"})}, {"response", optional(response)}, {"recoveryProbe", optional(boolean)}})},
          {"releaseCodexProbe", shape({{"handle", text}})},
          {"electCodexResetProbe", shape({{"handle", text}, {"proofId", text}})},
          {"refreshCodexAdmission", shape({{"handle", text}})},
          {"reservePaidAttempt", shape({{"handle", text}, {"hopId", text}})},
          {"recordPaidAttemptLifecycle", shape({{"attemptHandle", text}, {"patch", shape({{"provider_request_id", optional(nullable(text))}, {"dispatch_state", optional(oneOf({"reserved", "dispatched", "not_dispatched"}))}, {"terminal_state", optional(oneOf({"pending", "completed", "incomplete", "failed", "cancelled"}))}})}, {"fetchStarted", optional(oneOf({true}))}})},
          {"releasePaidBeforeFetch", shape({{"attemptHandle", text}})},
          {"recordVerifiedPaidBilling", shape({{"attemptHandle", text}, {"log", shape({{"request_id", text}, {"quota", finite}, {"prompt_tokens", integer}, {"cached_prompt_tokens", optional(integer)}, {"cache_write_prompt_tokens", optional(integer)}, {"completion_tokens", integer}, {"model", text}, {"created_at", finite}})}, {"surplusBuyerCostMicro", optional(integer)}})},
          {"deferPaidReconciliation", shape({{"attemptHandle", text}})},
          {"claimPaidReconciliation", shape({{"cursor", optional(text)}, {"limit", pageLimit}})},
          {"finishPaidReconciliation", shape({{"lease", text}})},
          {"foldPaidBackfillPage", shape({{"cursor", optional(text)}, {"limit", pageLimit}})},
          {"beginPaidKeyDeletion", shape({})},
          {"recordSemanticTerminal", shape({{"handle", text}, {"attemptHandle", optional(text)}, {"outcome", oneOf({"completed", "incomplete", "failed", "cancelled"})}, {"usage", optional(usage)}})},
          {"recordDeliveryObservation", shape({{"handle", text}, {"observation", oneOf({"delivered", "interrupted", "unobserved"})}})},
          {"readRequestStatus", shape({{"handle", text}, {"cursor", optional(text)}, {"limit", pageLimit}})}
        };
      }();
      return value;
    }
    const Guard* findGuard(const std::vector<std::pair<std::string, Guard>>& entries, std::string_view method) {
      for (const auto& [key, guard] : entries) if (key == method) return &guard;
      return nullptr;
    }
    bool envelopeKeys(const json::value& value) {
      for (const auto& key : rt::objectKeys(value.as_object())) if (key != "epoch" && key != "operationId" && key != "method" && key != "args") return false;
      return true;
    }
    Guard controlIntent(bool reset) {
      return [reset](const json::value* value) {
        if (!nativeObject(value)) return false;
        for (const auto& key : rt::objectKeys(value->as_object())) {
          if (key == "name" || key == "expires_at_ms" || key == "usage_limit_requests" || key == "window_ms" || key == "paid_fallback_enabled" || key == "paid_fallback_limit_credits" || key == "banked_resets_enabled" || (reset && key == "reset_usage")) continue;
          return false;
        }
        return true;
      };
    }
    const std::vector<std::pair<std::string, Guard>>& controlGuards() {
      static const std::vector<std::pair<std::string, Guard>> value{
        {"resolvePrincipal", shape({})}, {"createKey", shape({{"input", controlIntent(false)}})}, {"updateKey", shape({{"input", controlIntent(true)}})},
        {"revokeKey", shape({})}, {"unrevokeKey", shape({})}, {"deleteKey", shape({})}
      };
      return value;
    }
    rt::Task<NativeRpcResult> executeRuntime(std::shared_ptr<NativeBoundRuntimePort> port, json::value command) {
      try {
        auto parsed = parseNativeCommand(command);
        auto task = port->execute(parsed);
        rt::Promise<NativeReply> pending(std::move(task));
        const auto reply = co_await pending.wait();
        co_return NativeRpcResultResult{reply};
      } catch (const NativeAuthorityError& error) { co_return NativeRpcResultError{{error.code, error.status}}; }
    }
    rt::Task<NativeRpcResult> bindRouting(std::shared_ptr<NativeBoundRuntimePort> port, std::string handle, std::string operationId) {
      try {
        const json::value handleValue(handle), operationValue(operationId);
        if (!text(&handleValue) || !text(&operationValue)) throw NativeAuthorityError("invalid_private_routing_binding", 400);
        auto task = port->bindTrustedRoutingDecision(handle, operationId);
        rt::Promise<NativeReply> pending(std::move(task));
        const auto reply = co_await pending.wait();
        co_return NativeRpcResultResult{reply};
      } catch (const NativeAuthorityError& error) { co_return NativeRpcResultError{{error.code, error.status}}; }
    }
    rt::Task<NativeRpcResult> executeRetained(std::function<rt::Task<NativeReply>(NativeCommand)> execute, json::value command) {
      try {
        auto parsed = parseNativeCommand(command);
        auto task = execute(parsed);
        rt::Promise<NativeReply> pending(std::move(task));
        const auto reply = co_await pending.wait();
        co_return NativeRpcResultResult{reply};
      } catch (const NativeAuthorityError& error) { co_return NativeRpcResultError{{error.code, error.status}}; }
    }
    rt::Task<NativeControlRpcResult> control(native_principal_contracts::NativePrincipalControlRunner authority, native_principal_contracts::NativeControlBinding binding, json::value command) {
      try {
        auto parsed = parseNativeControlCommand(command);
        auto task = authority.control(binding, parsed);
        rt::Promise<native_principal_contracts::NativeControlReply> pending(std::move(task));
        const auto reply = co_await pending.wait();
        co_return NativeRpcResultResult{json::value_from(reply)};
      } catch (const NativeAuthorityError& error) { co_return NativeRpcResultError{{error.code, error.status}}; }
    }
    json::value unwrap(const NativeRpcResult& reply) {
      if (const auto* error = std::get_if<NativeRpcResultError>(&reply)) throw NativeAuthorityError(error->error.code, error->error.status);
      return std::get<NativeRpcResultResult>(reply).result;
    }
    rt::Task<NativeReply> callRuntime(NativeRpcTransport transport, std::string epoch, std::string method, json::object args, std::string operationId) {
      const json::object value{{"epoch", epoch}, {"operationId", operationId}, {"method", method}, {"args", args}};
      const auto command = parseNativeCommand(value);
      const auto encoded = json::value_from(command);
      auto task = transport.execute(encoded);
      rt::Promise<NativeRpcResult> pending(std::move(task));
      const auto response = co_await pending.wait();
      const auto result = unwrap(response);
      if (!nativeObject(result) || !rt::getString(result, "kind")) throw NativeAuthorityError("invalid_authority_reply", 503);
      co_return result.as_object();
    }
    rt::Task<json::value> callBinding(NativeRpcTransport transport, std::string handle, std::string operationId) {
      if (!transport.bindTrustedRoutingDecision) throw NativeAuthorityError("private_routing_port_unavailable", 503);
      auto task = transport.bindTrustedRoutingDecision(handle, operationId);
      rt::Promise<NativeRpcResult> pending(std::move(task));
      const auto reply = co_await pending.wait();
      co_return unwrap(reply);
    }
    rt::Task<json::value> callControl(NativeControlRpcTransport transport, std::string epoch, std::string method, json::object args, std::string operationId) {
      const json::object value{{"epoch", epoch}, {"operationId", operationId}, {"method", method}, {"args", args}};
      const auto command = parseNativeControlCommand(value);
      const auto encoded = json::value_from(command);
      auto task = transport.control(encoded);
      rt::Promise<NativeRpcResult> pending(std::move(task));
      const auto reply = co_await pending.wait();
      co_return unwrap(reply);
    }
  }
  std::string nativeCommandBytes(const json::value& value) { return nativeCommandBytes(&value); }
  std::string nativeCommandBytes(const json::value* value) {
    if (!value) throw NativeAuthorityError("invalid_command_value", 400);
    struct Frame { const json::value* input; json::value result; std::vector<std::string> keys; std::size_t next = 0; bool prepared = false; };
    std::vector<Frame> stack;
    stack.push_back(Frame{value, nullptr, {}, 0, false});
    json::value output;
    while (!stack.empty()) {
      if (stack.size() > rt::kJsonMaxDepth) throw rt::RangeError("Maximum call stack size exceeded.");
      auto& frame = stack.back();
      if (!frame.prepared) {
        frame.prepared = true;
        const auto& input = *frame.input;
        if (input.is_null() || input.is_string() || input.is_bool() || (rt::isNumber(input) && rt::Number::isFinite(input))) frame.result = input;
        else if (input.is_array()) frame.result = json::array();
        else if (nativeObject(input)) {
          frame.result = json::object();
          frame.keys = rt::objectKeys(input.as_object());
          rt::sort(frame.keys, [](const auto& left, const auto& right) { return static_cast<double>(rt::localeCompare(left, right)); });
        } else throw NativeAuthorityError("invalid_command_value", 400);
      }
      const auto& input = *frame.input;
      const json::value* child = nullptr;
      if (input.is_array() && frame.next < input.as_array().size()) child = &input.as_array()[frame.next++];
      else if (input.is_object() && frame.next < frame.keys.size()) child = rt::get(input, frame.keys[frame.next++]);
      if (child) { stack.push_back(Frame{child, nullptr, {}, 0, false}); continue; }
      auto result = std::move(frame.result);
      stack.pop_back();
      if (stack.empty()) output = std::move(result);
      else {
        auto& parent = stack.back();
        if (parent.result.is_array()) parent.result.as_array().push_back(std::move(result));
        else rt::objectDefine(parent.result.as_object(), parent.keys[parent.next - 1], result);
      }
    }
    return rt::jsonStringify(output);
  }
  native_authority_contracts::NativeCommand parseNativeCommand(const json::value& value) { return parseNativeCommand(&value); }
  native_authority_contracts::NativeCommand parseNativeCommand(const json::value* value) {
    const auto method = value ? rt::getString(*value, "method") : std::nullopt;
    const auto* guard = method ? findGuard(guards(), *method) : nullptr;
    if (!nativeObject(value) || value->as_object().size() != 4 || !text(rt::get(*value, "epoch")) || !text(rt::get(*value, "operationId")) || !guard)
      throw NativeAuthorityError("invalid_authority_operation", 400);
    if (!envelopeKeys(*value) || !(*guard)(rt::get(*value, "args"))) throw NativeAuthorityError("invalid_authority_arguments", 400);
    if (rt::utf8ByteLength(nativeCommandBytes(*value)) > 65536) throw NativeAuthorityError("authority_command_too_large", 413);
    return {std::string(*rt::getString(*value, "epoch")), std::string(*rt::getString(*value, "operationId")), std::string(*method), *rt::getObject(*value, "args")};
  }
  native_principal_contracts::NativeControlCommand parseNativeControlCommand(const json::value& value) { return parseNativeControlCommand(&value); }
  native_principal_contracts::NativeControlCommand parseNativeControlCommand(const json::value* value) {
    const auto method = value ? rt::getString(*value, "method") : std::nullopt;
    const auto* guard = method ? findGuard(controlGuards(), *method) : nullptr;
    if (!nativeObject(value) || value->as_object().size() != 4 || !text(rt::get(*value, "epoch")) || !text(rt::get(*value, "operationId")) || !guard || !envelopeKeys(*value))
      throw NativeAuthorityError("invalid_control_operation", 400);
    if (!(*guard)(rt::get(*value, "args"))) throw NativeAuthorityError("invalid_control_arguments", 400);
    if (rt::utf8ByteLength(nativeCommandBytes(*value)) > 65536) throw NativeAuthorityError("control_command_too_large", 413);
    return {std::string(*rt::getString(*value, "epoch")), std::string(*rt::getString(*value, "operationId")), std::string(*method), *rt::getObject(*value, "args")};
  }
  NativeAuthorityRpcServer::NativeAuthorityRpcServer(const NativeOperationRunner& authority, const native_runtime_contracts::NativeRuntimeBinding& binding)
    : _port(std::make_shared<NativeBoundRuntimePort>(authority.bindAuthenticatedRuntime(binding))) {}
  rt::Task<NativeRpcResult> NativeAuthorityRpcServer::execute(json::value command) { return executeRuntime(_port, std::move(command)); }
  rt::Task<NativeRpcResult> NativeAuthorityRpcServer::bindTrustedRoutingDecision(std::string handle, std::string operationId) { return bindRouting(_port, std::move(handle), std::move(operationId)); }
  NativeRpcTransport NativeAuthorityRpcServer::transport() const {
    const auto port = _port;
    return {[port](json::value command) { return executeRuntime(port, std::move(command)); }, [port](std::string handle, std::string operationId) { return bindRouting(port, std::move(handle), std::move(operationId)); }};
  }
  NativeRetainedHistoryServer::NativeRetainedHistoryServer(const NativeRetainedHistoryRunner& authority, const native_runtime_contracts::NativeRetainedHistoryBinding& binding)
    : _execute(authority.bindRetainedHistory(binding).execute) {}
  rt::Task<NativeRpcResult> NativeRetainedHistoryServer::execute(json::value command) { return executeRetained(_execute, std::move(command)); }
  NativeRpcTransport NativeRetainedHistoryServer::transport() const {
    const auto execute = _execute;
    return {[execute](json::value command) { return executeRetained(execute, std::move(command)); }, {}};
  }
  NativeAuthorityClient::NativeAuthorityClient(NativeRpcTransport transport, std::string epoch) : _transport(std::move(transport)), _epoch(std::move(epoch)) {}
  rt::Task<json::value> NativeAuthorityClient::bindTrustedRoutingDecision(std::string handle) { return bindTrustedRoutingDecision(std::move(handle), rt::crypto::randomUUID()); }
  rt::Task<json::value> NativeAuthorityClient::bindTrustedRoutingDecision(std::string handle, std::string operationId) { return callBinding(_transport, std::move(handle), std::move(operationId)); }
  rt::Task<native_authority_contracts::NativeReply> NativeAuthorityClient::call(std::string method, const json::object& args) { return call(std::move(method), args, rt::crypto::randomUUID()); }
  rt::Task<native_authority_contracts::NativeReply> NativeAuthorityClient::call(std::string method, const json::object& args, std::string operationId) { return callRuntime(_transport, _epoch, std::move(method), args, std::move(operationId)); }
  NativePrincipalControlServer::NativePrincipalControlServer(native_principal_contracts::NativePrincipalControlRunner authority, native_principal_contracts::NativeControlBinding binding)
    : _authority(std::move(authority)), _binding(std::move(binding)) {}
  rt::Task<NativeControlRpcResult> NativePrincipalControlServer::control(json::value command) { return native_authority_rpc::control(_authority, _binding, std::move(command)); }
  NativeControlRpcTransport NativePrincipalControlServer::transport() const {
    const auto authority = _authority; const auto binding = _binding;
    return {[authority, binding](json::value command) { return native_authority_rpc::control(authority, binding, std::move(command)); }};
  }
  NativePrincipalControlClient::NativePrincipalControlClient(NativeControlRpcTransport transport, std::string epoch) : _transport(std::move(transport)), _epoch(std::move(epoch)) {}
  rt::Task<json::value> NativePrincipalControlClient::call(std::string method, const json::object& args) { return call(std::move(method), args, rt::crypto::randomUUID()); }
  rt::Task<json::value> NativePrincipalControlClient::call(std::string method, const json::object& args, std::string operationId) { return callControl(_transport, _epoch, std::move(method), args, std::move(operationId)); }
}
