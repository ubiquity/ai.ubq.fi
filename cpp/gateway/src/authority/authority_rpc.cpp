#include "uos/authority/authority_rpc.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace uos::authority::authority_rpc {
  using admission_state::Selection;
  using admission_state::Usage;
  using contracts::AuthorityError;
  using contracts::fromJson;
  using contracts::toJson;

  namespace {
    using Guard = std::function<bool(const json::value* value)>;
    using Shape = std::vector<std::pair<std::string, Guard>>;

    const Guard text = [](const json::value* value) { return value != nullptr && value->is_string(); };
    const Guard providerRequestId = [](const json::value* value) {
      return value != nullptr && value->is_string() && rt::length(rt::trim(std::string_view(value->get_string()))) > 0;
    };
    const Guard integer = [](const json::value* value) {
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
    Guard fields(Shape shape) {
      return [shape = std::move(shape)](const json::value* value) {
        if (value == nullptr || !value->is_object()) return false;
        const json::object& record = value->get_object();
        return record.size() == shape.size() &&
               std::all_of(shape.begin(), shape.end(), [&record](const auto& entry) { return rt::hasOwn(record, entry.first) && entry.second(rt::get(record, entry.first)); });
      };
    }
    Guard tuple(std::vector<Guard> guards) {
      return [guards = std::move(guards)](const json::value* value) {
        if (value == nullptr || !value->is_array()) return false;
        const json::array& items = value->get_array();
        if (items.size() != guards.size()) return false;
        for (std::size_t index = 0; index < guards.size(); ++index) {
          if (!guards[index](&items[index])) return false;
        }
        return true;
      };
    }

    const Guard scope = fields({{"namespace", text}, {"epoch", integer}});
    const Guard handleGuard = fields({{"scope", scope}, {"requestId", text}, {"fence", integer}});
    const Guard proofGuard = fields({
      {"kind", oneOf({"quota_exhausted", "credential_invalid", "transient"})},
      {"model", text},
      {"fingerprint", text},
      {"observedAtMs", integer},
      {"validUntilMs", integer},
    });
    const Guard accountGuard = fields({
      {"accountId", text},
      {"fingerprint", text},
      {"credentialGeneration", integer},
      {"routingGeneration", integer},
      {"resetGeneration", integer},
      {"overageVersion", integer},
      {"overageEnabled", boolean},
      {"enabled", boolean},
      {"models", array(text)},
      {"proof", nullable(proofGuard)},
    });
    const Guard attempt =
      fields({{"attemptId", text}, {"provider", oneOf({"codex", "surplus", "openlux"})}, {"maximumExposureMicrocredits", integer}, {"account", nullable(accountGuard)}});
    const Guard terminal = fields({
      {"outcome", oneOf({"completed", "incomplete", "failed", "cancelled"})},
      {"providerRequestId", nullable(text)},
      {"spendMicrocredits", nullable(integer)},
      {"fallback", oneOf({nullptr, "quota_exhausted", "capacity_exhausted"})},
    });
    const std::vector<std::pair<std::string, Guard>> ARGUMENTS{
      {"admit", tuple({fields({
                  {"scope", scope},
                  {"requestId", text},
                  {"principalId", text},
                  {"policyVersion", integer},
                  {"digest", text},
                  {"route", oneOf({"/v1/responses", "/v1/chat/completions"})},
                  {"model", text},
                  {"nowMs", integer},
                })})},
      {"reserveAttempt", tuple({handleGuard, attempt, integer})},
      {"beginDispatch", tuple({handleGuard, text, integer})},
      {"selection", tuple({handleGuard})},
      {"reselectSubscription", tuple({handleGuard, integer})},
      {"confirmDispatched", tuple({handleGuard, text})},
      {"releaseBeforeDispatch", tuple({handleGuard, text})},
      {"observeProof", tuple({handleGuard, text, proofGuard})},
      {"observeTerminal",
       [](const json::value* value) {
         static const Guard three = tuple({handleGuard, text, terminal});
         static const Guard four = tuple({handleGuard, text, terminal, [](const json::value* flag) { return flag == nullptr || boolean(flag); }});
         return three(value) || four(value);
       }},
      {"observeDelivery", tuple({handleGuard, oneOf({"delivered", "interrupted", "unobserved"})})},
      {"observeUsage", tuple({handleGuard, text, fields({{"providerRequestId", providerRequestId}, {"spendMicrocredits", integer}})})},
      {"resetEligible", tuple({handleGuard, integer})},
      {"settle", tuple({handleGuard})},
      {"cancelUndispatched", tuple({handleGuard})},
    };

    const Guard* argumentsGuard(std::string_view operation) {
      for (const auto& [name, guard] : ARGUMENTS) {
        if (name == operation) return &guard;
      }
      return nullptr;
    }

    bool isCommand(const json::value& value) {
      if (!value.is_object()) return false;
      const json::object& command = value.get_object();
      const json::value* operation = rt::get(command, "operation");
      if (command.size() != 2 || operation == nullptr || !operation->is_string()) return false;
      const Guard* guard = argumentsGuard(operation->get_string());
      return guard != nullptr && rt::hasOwn(command, "arguments") && (*guard)(rt::get(command, "arguments"));
    }

    json::value selectionJson(const Selection& selected) {
      json::object out;
      out["account"] = toJson(selected.account);
      out["model"] = selected.model;
      out["route"] = selected.route;
      return out;
    }

    // `if ("error" in reply) throw new AuthorityError(reply.error.code, reply.error.status); return reply.value;`
    std::optional<json::value> unwrap(const AuthorityReply& reply) {
      if (rt::hasOwn(reply, "error")) {
        const json::value* error = rt::get(reply, "error");
        const json::value* code = rt::get(error, "code");
        const std::optional<double> status = rt::getNumber(rt::get(error, "status"));
        throw AuthorityError(code != nullptr && code->is_string() ? std::string(code->get_string()) : rt::toString(code), status.value_or(409));
      }
      const json::value* value = rt::get(reply, "value");
      return value != nullptr ? std::optional<json::value>(*value) : std::nullopt;
    }

    const json::value& required(const std::optional<json::value>& value) {
      if (!value) throw rt::TypeError("authority reply has no value");
      return *value;
    }

    json::object command(std::string operation, json::array arguments) {
      json::object out;
      out["operation"] = std::move(operation);
      out["arguments"] = std::move(arguments);
      return out;
    }
  }

  AuthorityRpcServer::AuthorityRpcServer(std::shared_ptr<AuthorityOperations> authority_, double maximumCommandBytes_)
    : _authority(std::move(authority_)), _maximumCommandBytes(maximumCommandBytes_) {
    if (!rt::Number::isSafeInteger(_maximumCommandBytes) || _maximumCommandBytes < 1) throw AuthorityError("invalid_authority_command_bound", 400);
  }

  rt::Task<AuthorityReply> AuthorityRpcServer::invoke(AuthorityCommand command) {
    rt::Reason failure;
    std::optional<json::value> value;
    try {
      if (!isCommand(command)) throw AuthorityError("invalid_authority_operation", 400);
      if (static_cast<double>(rt::utf8ByteLength(rt::jsonStringify(command))) > _maximumCommandBytes) throw AuthorityError("authority_command_too_large", 413);
      rt::Task<std::optional<json::value>> executing = _execute(command.get_object());
      value = co_await std::move(executing);
    } catch (...) {
      failure = std::current_exception();
    }
    AuthorityReply reply;
    if (!failure) {
      if (value) reply["value"] = std::move(*value);
      co_return reply;
    }
    json::object error;
    try {
      std::rethrow_exception(failure);
    } catch (const AuthorityError& authorityError) {
      error["code"] = authorityError.code;
      error["status"] = rt::makeNumber(authorityError.status);
    } catch (...) {
      error["code"] = "authority_unavailable";
      error["status"] = 503;
    }
    reply["error"] = std::move(error);
    co_return reply;
  }

  rt::Task<std::optional<json::value>> AuthorityRpcServer::_execute(json::object command) {
    const std::string operation(command.at("operation").get_string());
    const json::array& arguments = command.at("arguments").get_array();
    if (operation == "admit") {
      rt::Task<AdmissionHandle> call = _authority->admit(fromJson<AdmitInput>(arguments[0]));
      const AdmissionHandle result = co_await std::move(call);
      co_return toJson(result);
    }
    if (operation == "reserveAttempt") {
      rt::Task<void> call = _authority->reserveAttempt(fromJson<AdmissionHandle>(arguments[0]), fromJson<AttemptInput>(arguments[1]), rt::toNumber(arguments[2]));
      co_await std::move(call);
      co_return std::nullopt;
    }
    if (operation == "beginDispatch") {
      rt::Task<bool> call = _authority->beginDispatch(fromJson<AdmissionHandle>(arguments[0]), std::string(arguments[1].get_string()), rt::toNumber(arguments[2]));
      const bool result = co_await std::move(call);
      co_return json::value(result);
    }
    if (operation == "selection") {
      rt::Task<Selection> call = _authority->selection(fromJson<AdmissionHandle>(arguments[0]));
      const Selection result = co_await std::move(call);
      co_return selectionJson(result);
    }
    if (operation == "reselectSubscription") {
      rt::Task<std::optional<AccountBinding>> call = _authority->reselectSubscription(fromJson<AdmissionHandle>(arguments[0]), rt::toNumber(arguments[1]));
      const std::optional<AccountBinding> result = co_await std::move(call);
      co_return toJson(result);
    }
    if (operation == "confirmDispatched") {
      rt::Task<void> call = _authority->confirmDispatched(fromJson<AdmissionHandle>(arguments[0]), std::string(arguments[1].get_string()));
      co_await std::move(call);
      co_return std::nullopt;
    }
    if (operation == "releaseBeforeDispatch") {
      rt::Task<void> call = _authority->releaseBeforeDispatch(fromJson<AdmissionHandle>(arguments[0]), std::string(arguments[1].get_string()));
      co_await std::move(call);
      co_return std::nullopt;
    }
    if (operation == "observeProof") {
      rt::Task<void> call =
        _authority->observeProof(fromJson<AdmissionHandle>(arguments[0]), std::string(arguments[1].get_string()), fromJson<AccountProof>(arguments[2]));
      co_await std::move(call);
      co_return std::nullopt;
    }
    if (operation == "observeTerminal") {
      const bool finalAttempt = arguments.size() > 3 && arguments[3].is_bool() ? arguments[3].get_bool() : true;
      rt::Task<void> call = _authority->observeTerminal(fromJson<AdmissionHandle>(arguments[0]), std::string(arguments[1].get_string()),
                                                        fromJson<TerminalEvidence>(arguments[2]), finalAttempt);
      co_await std::move(call);
      co_return std::nullopt;
    }
    if (operation == "observeDelivery") {
      rt::Task<void> call = _authority->observeDelivery(fromJson<AdmissionHandle>(arguments[0]), std::string(arguments[1].get_string()));
      co_await std::move(call);
      co_return std::nullopt;
    }
    if (operation == "observeUsage") {
      const json::object& usage = arguments[2].get_object();
      const Usage evidence{.providerRequestId = std::string(usage.at("providerRequestId").get_string()), .spendMicrocredits = rt::toNumber(usage.at("spendMicrocredits"))};
      rt::Task<void> call = _authority->observeUsage(fromJson<AdmissionHandle>(arguments[0]), std::string(arguments[1].get_string()), evidence);
      co_await std::move(call);
      co_return std::nullopt;
    }
    if (operation == "resetEligible") {
      rt::Task<bool> call = _authority->resetEligible(fromJson<AdmissionHandle>(arguments[0]), rt::toNumber(arguments[1]));
      const bool result = co_await std::move(call);
      co_return json::value(result);
    }
    if (operation == "settle") {
      rt::Task<std::string> call = _authority->settle(fromJson<AdmissionHandle>(arguments[0]));
      const std::string result = co_await std::move(call);
      co_return json::value(result);
    }
    if (operation == "cancelUndispatched") {
      rt::Task<void> call = _authority->cancelUndispatched(fromJson<AdmissionHandle>(arguments[0]));
      co_await std::move(call);
      co_return std::nullopt;
    }
    throw AuthorityError("invalid_authority_operation", 400);
  }

  AuthorityRpcClient::AuthorityRpcClient(std::shared_ptr<AuthorityRpc> rpc_) : _rpc(std::move(rpc_)) {}

  rt::Task<std::optional<json::value>> AuthorityRpcClient::_call(std::string operation, json::array arguments) {
    rt::Task<AuthorityReply> invoking = _rpc->invoke(command(std::move(operation), std::move(arguments)));
    const AuthorityReply reply = co_await std::move(invoking);
    std::optional<json::value> value = unwrap(reply);
    co_return value;
  }

  rt::Task<AdmissionHandle> AuthorityRpcClient::admit(AdmitInput input) {
    json::array arguments{toJson(input)};
    rt::Task<std::optional<json::value>> call = _call("admit", std::move(arguments));
    const std::optional<json::value> value = co_await std::move(call);
    co_return fromJson<AdmissionHandle>(required(value));
  }

  rt::Task<void> AuthorityRpcClient::reserveAttempt(AdmissionHandle handle, AttemptInput input, double nowMs) {
    json::array arguments{toJson(handle), toJson(input), rt::makeNumber(nowMs)};
    rt::Task<std::optional<json::value>> call = _call("reserveAttempt", std::move(arguments));
    co_await std::move(call);
  }

  rt::Task<bool> AuthorityRpcClient::beginDispatch(AdmissionHandle handle, std::string attemptId, double nowMs) {
    json::array arguments{toJson(handle), json::value(attemptId), rt::makeNumber(nowMs)};
    rt::Task<std::optional<json::value>> call = _call("beginDispatch", std::move(arguments));
    const std::optional<json::value> value = co_await std::move(call);
    co_return value && rt::truthy(*value);
  }

  rt::Task<Selection> AuthorityRpcClient::selection(AdmissionHandle handle) {
    json::array arguments{toJson(handle)};
    rt::Task<std::optional<json::value>> call = _call("selection", std::move(arguments));
    const std::optional<json::value> value = co_await std::move(call);
    const json::value& selected = required(value);
    const json::value* account = rt::get(selected, "account");
    const std::optional<std::string_view> model = rt::getString(selected, "model");
    const std::optional<std::string_view> route = rt::getString(selected, "route");
    if (account == nullptr || !model || !route) throw rt::TypeError("authority reply is not a selection");
    co_return Selection{
      .account = account->is_null() ? std::nullopt : std::optional<AccountBinding>(fromJson<AccountBinding>(*account)),
      .model = std::string(*model),
      .route = std::string(*route),
    };
  }

  rt::Task<std::optional<AccountBinding>> AuthorityRpcClient::reselectSubscription(AdmissionHandle handle, double nowMs) {
    json::array arguments{toJson(handle), rt::makeNumber(nowMs)};
    rt::Task<std::optional<json::value>> call = _call("reselectSubscription", std::move(arguments));
    const std::optional<json::value> value = co_await std::move(call);
    if (!value || value->is_null()) co_return std::nullopt;
    co_return fromJson<AccountBinding>(*value);
  }

  rt::Task<void> AuthorityRpcClient::confirmDispatched(AdmissionHandle handle, std::string attemptId) {
    json::array arguments{toJson(handle), json::value(attemptId)};
    rt::Task<std::optional<json::value>> call = _call("confirmDispatched", std::move(arguments));
    co_await std::move(call);
  }

  rt::Task<void> AuthorityRpcClient::releaseBeforeDispatch(AdmissionHandle handle, std::string attemptId) {
    json::array arguments{toJson(handle), json::value(attemptId)};
    rt::Task<std::optional<json::value>> call = _call("releaseBeforeDispatch", std::move(arguments));
    co_await std::move(call);
  }

  rt::Task<void> AuthorityRpcClient::observeProof(AdmissionHandle handle, std::string attemptId, AccountProof proof) {
    json::array arguments{toJson(handle), json::value(attemptId), toJson(proof)};
    rt::Task<std::optional<json::value>> call = _call("observeProof", std::move(arguments));
    co_await std::move(call);
  }

  rt::Task<void> AuthorityRpcClient::observeTerminal(AdmissionHandle handle, std::string attemptId, TerminalEvidence evidence, bool finalAttempt) {
    json::array arguments{toJson(handle), json::value(attemptId), toJson(evidence), json::value(finalAttempt)};
    rt::Task<std::optional<json::value>> call = _call("observeTerminal", std::move(arguments));
    co_await std::move(call);
  }

  rt::Task<void> AuthorityRpcClient::observeDelivery(AdmissionHandle handle, DeliveryOutcome outcome) {
    json::array arguments{toJson(handle), json::value(outcome)};
    rt::Task<std::optional<json::value>> call = _call("observeDelivery", std::move(arguments));
    co_await std::move(call);
  }

  rt::Task<void> AuthorityRpcClient::observeUsage(AdmissionHandle handle, std::string attemptId, Usage usage) {
    json::object evidence;
    evidence["providerRequestId"] = usage.providerRequestId;
    evidence["spendMicrocredits"] = rt::makeNumber(usage.spendMicrocredits);
    json::array arguments{toJson(handle), json::value(attemptId), json::value(std::move(evidence))};
    rt::Task<std::optional<json::value>> call = _call("observeUsage", std::move(arguments));
    co_await std::move(call);
  }

  rt::Task<bool> AuthorityRpcClient::resetEligible(AdmissionHandle handle, double nowMs) {
    json::array arguments{toJson(handle), rt::makeNumber(nowMs)};
    rt::Task<std::optional<json::value>> call = _call("resetEligible", std::move(arguments));
    const std::optional<json::value> value = co_await std::move(call);
    co_return value && rt::truthy(*value);
  }

  rt::Task<std::string> AuthorityRpcClient::settle(AdmissionHandle handle) {
    json::array arguments{toJson(handle)};
    rt::Task<std::optional<json::value>> call = _call("settle", std::move(arguments));
    const std::optional<json::value> value = co_await std::move(call);
    co_return rt::toString(value ? &*value : nullptr);
  }

  rt::Task<void> AuthorityRpcClient::cancelUndispatched(AdmissionHandle handle) {
    json::array arguments{toJson(handle)};
    rt::Task<std::optional<json::value>> call = _call("cancelUndispatched", std::move(arguments));
    co_await std::move(call);
  }
}
