#include "uos/authority/contracts.hpp"

#include "uos/rt/number.hpp"

namespace uos::authority::contracts {
  namespace {
    json::value number(double value) { return rt::makeNumber(value); }
    json::value nullableString(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }
    json::value nullableNumber(const std::optional<double>& value) { return value ? number(*value) : json::value(nullptr); }

    template <class T> json::value recordJson(const Record<T>& record) {
      json::object out;
      for (const std::string& key : record.keys()) rt::objectDefine(out, key, toJson(*record.get(key)));
      return out;
    }

    [[noreturn]] void shapeError(std::string_view key, std::string_view expected) {
      throw rt::TypeError("authority value member '" + std::string(key) + "' is not " + std::string(expected));
    }

    const json::object& objectOf(const json::value& value) {
      if (!value.is_object()) throw rt::TypeError("authority value is not an object");
      return value.get_object();
    }
    const json::value& memberOf(const json::object& object, std::string_view key) {
      const json::value* member = rt::get(object, key);
      if (member == nullptr) shapeError(key, "present");
      return *member;
    }
    std::string stringAt(const json::object& object, std::string_view key) {
      const json::value& member = memberOf(object, key);
      if (!member.is_string()) shapeError(key, "a string");
      return std::string(member.get_string());
    }
    double numberAt(const json::object& object, std::string_view key) {
      const json::value& member = memberOf(object, key);
      if (!rt::isNumber(member)) shapeError(key, "a number");
      return rt::toNumber(member);
    }
    bool boolAt(const json::object& object, std::string_view key) {
      const json::value& member = memberOf(object, key);
      if (!member.is_bool()) shapeError(key, "a boolean");
      return member.get_bool();
    }
    std::optional<std::string> nullableStringAt(const json::object& object, std::string_view key) {
      if (memberOf(object, key).is_null()) return std::nullopt;
      return stringAt(object, key);
    }
    std::optional<double> nullableNumberAt(const json::object& object, std::string_view key) {
      if (memberOf(object, key).is_null()) return std::nullopt;
      return numberAt(object, key);
    }
    template <class T> std::optional<T> nullableAt(const json::object& object, std::string_view key) {
      const json::value& member = memberOf(object, key);
      if (member.is_null()) return std::nullopt;
      return fromJson<T>(member);
    }
    template <class T> Record<T> recordAt(const json::object& object, std::string_view key) {
      const json::object& members = objectOf(memberOf(object, key));
      Record<T> out;
      for (const std::string& name : rt::objectKeys(members)) out.define(name, fromJson<T>(*rt::get(members, name)));
      return out;
    }
  }

  AuthorityError::AuthorityError(std::string code_, double status_) : rt::JsError(code_, "AuthorityError"), code(std::move(code_)), status(status_) {}

  bool sameScope(const AuthorityScope& left, const AuthorityScope& right) { return left.namespace_ == right.namespace_ && left.epoch == right.epoch; }

  void assertAmount(double value) {
    if (!rt::Number::isSafeInteger(value) || value < 0) throw AuthorityError("invalid_amount", 400);
  }

  bool sameEvidence(const json::value& left, const json::value& right) { return rt::jsonStringify(left) == rt::jsonStringify(right); }

  bool sameEvidence(const json::value* left, const json::value* right) {
    if (left == nullptr || right == nullptr) return left == nullptr && right == nullptr;
    return sameEvidence(*left, *right);
  }

  json::value toJson(const AuthorityScope& value) {
    json::object out;
    out["namespace"] = value.namespace_;
    out["epoch"] = number(value.epoch);
    return out;
  }

  json::value toJson(const AccountProof& value) {
    json::object out;
    out["kind"] = value.kind;
    out["model"] = value.model;
    out["fingerprint"] = value.fingerprint;
    out["observedAtMs"] = number(value.observedAtMs);
    out["validUntilMs"] = number(value.validUntilMs);
    return out;
  }

  json::value toJson(const AccountBinding& value) {
    json::array models;
    for (const std::string& model : value.models) models.emplace_back(model);
    json::object out;
    out["accountId"] = value.accountId;
    out["fingerprint"] = value.fingerprint;
    out["credentialGeneration"] = number(value.credentialGeneration);
    out["routingGeneration"] = number(value.routingGeneration);
    out["resetGeneration"] = number(value.resetGeneration);
    out["overageVersion"] = number(value.overageVersion);
    out["overageEnabled"] = value.overageEnabled;
    out["enabled"] = value.enabled;
    out["models"] = std::move(models);
    out["proof"] = toJson(value.proof);
    return out;
  }

  json::value toJson(const ActiveAccount& value) {
    json::object out;
    out["accountId"] = value.accountId;
    out["fingerprint"] = value.fingerprint;
    out["generation"] = number(value.generation);
    return out;
  }

  json::value toJson(const PrincipalPolicy& value) {
    json::object out;
    out["principalId"] = value.principalId;
    out["version"] = number(value.version);
    out["deleted"] = value.deleted;
    out["windowResetAtMs"] = number(value.windowResetAtMs);
    out["requestLimit"] = number(value.requestLimit);
    out["kernelLimit"] = number(value.kernelLimit);
    out["concurrencyLimit"] = number(value.concurrencyLimit);
    out["paidLimitMicrocredits"] = number(value.paidLimitMicrocredits);
    return out;
  }

  json::value toJson(const PrincipalWindow& value) {
    json::object out;
    out["reserved"] = number(value.reserved);
    out["dispatched"] = number(value.dispatched);
    out["kernelReserved"] = number(value.kernelReserved);
    out["kernelCompleted"] = number(value.kernelCompleted);
    out["active"] = number(value.active);
    out["paidReservedMicrocredits"] = number(value.paidReservedMicrocredits);
    out["paidSettledMicrocredits"] = number(value.paidSettledMicrocredits);
    return out;
  }

  json::value toJson(const AdmissionHandle& value) {
    json::object out;
    out["scope"] = toJson(value.scope);
    out["requestId"] = value.requestId;
    out["fence"] = number(value.fence);
    return out;
  }

  json::value toJson(const AttemptInput& value) {
    json::object out;
    out["attemptId"] = value.attemptId;
    out["provider"] = value.provider;
    out["maximumExposureMicrocredits"] = number(value.maximumExposureMicrocredits);
    out["account"] = toJson(value.account);
    return out;
  }

  json::value toJson(const TerminalEvidence& value) {
    json::object out;
    out["outcome"] = value.outcome;
    out["providerRequestId"] = nullableString(value.providerRequestId);
    out["spendMicrocredits"] = nullableNumber(value.spendMicrocredits);
    out["fallback"] = nullableString(value.fallback);
    return out;
  }

  json::value toJson(const AccessAuthorization& value) {
    json::object out;
    out["ownerId"] = value.ownerId;
    out["ownerEpoch"] = number(value.ownerEpoch);
    out["accountId"] = value.accountId;
    out["workspaceId"] = value.workspaceId;
    out["generation"] = number(value.generation);
    out["nativeRefreshedAtNs"] = value.nativeRefreshedAtNs;
    out["fingerprint"] = value.fingerprint;
    out["accessToken"] = value.accessToken;
    out["idToken"] = value.idToken;
    out["issuedAtMs"] = number(value.issuedAtMs);
    out["expiresAtMs"] = number(value.expiresAtMs);
    return out;
  }

  json::value toJson(const AttemptRecord& value) {
    json::object out;
    out["input"] = toJson(value.input);
    out["state"] = value.state;
    out["terminal"] = toJson(value.terminal);
    out["billingSettled"] = value.billingSettled;
    out["settledSpendMicrocredits"] = nullableNumber(value.settledSpendMicrocredits);
    return out;
  }

  json::value toJson(const AdmissionRecord& value) {
    json::object out;
    out["handle"] = toJson(value.handle);
    out["principalId"] = value.principalId;
    out["policyVersion"] = number(value.policyVersion);
    out["windowKey"] = value.windowKey;
    out["digest"] = value.digest;
    out["route"] = value.route;
    out["model"] = value.model;
    out["account"] = toJson(value.account);
    out["activeGeneration"] = nullableNumber(value.activeGeneration);
    out["attempts"] = recordJson(value.attempts);
    out["dispatched"] = value.dispatched;
    out["semantic"] = nullableString(value.semantic);
    out["kernelSettled"] = value.kernelSettled;
    out["delivery"] = nullableString(value.delivery);
    out["settled"] = value.settled;
    return out;
  }

  // Member order of createAuthorityState's object literal, which every state
  // (and every structuredClone or JSON copy of one) keeps.
  json::value toJson(const AuthorityState& value) {
    json::array accounts;
    for (const AccountBinding& account : value.accounts) accounts.push_back(toJson(account));
    json::object out;
    out["scope"] = toJson(value.scope);
    out["accounts"] = std::move(accounts);
    out["policies"] = recordJson(value.policies);
    out["sequence"] = number(value.sequence);
    out["active"] = toJson(value.active);
    out["windows"] = recordJson(value.windows);
    out["admissions"] = recordJson(value.admissions);
    out["publications"] = recordJson(value.publications);
    return out;
  }

  json::value toJson(const AdmitInput& value) {
    json::object out;
    out["scope"] = toJson(value.scope);
    out["requestId"] = value.requestId;
    out["principalId"] = value.principalId;
    out["policyVersion"] = number(value.policyVersion);
    out["digest"] = value.digest;
    out["route"] = value.route;
    out["model"] = value.model;
    out["nowMs"] = number(value.nowMs);
    return out;
  }

  template <> AuthorityScope fromJson<AuthorityScope>(const json::value& value) {
    const json::object& object = objectOf(value);
    return AuthorityScope{.namespace_ = stringAt(object, "namespace"), .epoch = numberAt(object, "epoch")};
  }

  template <> AccountProof fromJson<AccountProof>(const json::value& value) {
    const json::object& object = objectOf(value);
    return AccountProof{
      .kind = stringAt(object, "kind"),
      .model = stringAt(object, "model"),
      .fingerprint = stringAt(object, "fingerprint"),
      .observedAtMs = numberAt(object, "observedAtMs"),
      .validUntilMs = numberAt(object, "validUntilMs"),
    };
  }

  template <> AccountBinding fromJson<AccountBinding>(const json::value& value) {
    const json::object& object = objectOf(value);
    const json::value& modelsValue = memberOf(object, "models");
    if (!modelsValue.is_array()) shapeError("models", "an array");
    std::vector<std::string> models;
    for (const json::value& model : modelsValue.get_array()) {
      if (!model.is_string()) shapeError("models", "an array of strings");
      models.emplace_back(model.get_string());
    }
    return AccountBinding{
      .accountId = stringAt(object, "accountId"),
      .fingerprint = stringAt(object, "fingerprint"),
      .credentialGeneration = numberAt(object, "credentialGeneration"),
      .routingGeneration = numberAt(object, "routingGeneration"),
      .resetGeneration = numberAt(object, "resetGeneration"),
      .overageVersion = numberAt(object, "overageVersion"),
      .overageEnabled = boolAt(object, "overageEnabled"),
      .enabled = boolAt(object, "enabled"),
      .models = std::move(models),
      .proof = nullableAt<AccountProof>(object, "proof"),
    };
  }

  template <> ActiveAccount fromJson<ActiveAccount>(const json::value& value) {
    const json::object& object = objectOf(value);
    return ActiveAccount{.accountId = stringAt(object, "accountId"), .fingerprint = stringAt(object, "fingerprint"), .generation = numberAt(object, "generation")};
  }

  template <> PrincipalPolicy fromJson<PrincipalPolicy>(const json::value& value) {
    const json::object& object = objectOf(value);
    return PrincipalPolicy{
      .principalId = stringAt(object, "principalId"),
      .version = numberAt(object, "version"),
      .deleted = boolAt(object, "deleted"),
      .windowResetAtMs = numberAt(object, "windowResetAtMs"),
      .requestLimit = numberAt(object, "requestLimit"),
      .kernelLimit = numberAt(object, "kernelLimit"),
      .concurrencyLimit = numberAt(object, "concurrencyLimit"),
      .paidLimitMicrocredits = numberAt(object, "paidLimitMicrocredits"),
    };
  }

  template <> PrincipalWindow fromJson<PrincipalWindow>(const json::value& value) {
    const json::object& object = objectOf(value);
    return PrincipalWindow{
      .reserved = numberAt(object, "reserved"),
      .dispatched = numberAt(object, "dispatched"),
      .kernelReserved = numberAt(object, "kernelReserved"),
      .kernelCompleted = numberAt(object, "kernelCompleted"),
      .active = numberAt(object, "active"),
      .paidReservedMicrocredits = numberAt(object, "paidReservedMicrocredits"),
      .paidSettledMicrocredits = numberAt(object, "paidSettledMicrocredits"),
    };
  }

  template <> AdmissionHandle fromJson<AdmissionHandle>(const json::value& value) {
    const json::object& object = objectOf(value);
    return AdmissionHandle{.scope = fromJson<AuthorityScope>(memberOf(object, "scope")), .requestId = stringAt(object, "requestId"), .fence = numberAt(object, "fence")};
  }

  template <> AttemptInput fromJson<AttemptInput>(const json::value& value) {
    const json::object& object = objectOf(value);
    return AttemptInput{
      .attemptId = stringAt(object, "attemptId"),
      .provider = stringAt(object, "provider"),
      .maximumExposureMicrocredits = numberAt(object, "maximumExposureMicrocredits"),
      .account = nullableAt<AccountBinding>(object, "account"),
    };
  }

  template <> TerminalEvidence fromJson<TerminalEvidence>(const json::value& value) {
    const json::object& object = objectOf(value);
    return TerminalEvidence{
      .outcome = stringAt(object, "outcome"),
      .providerRequestId = nullableStringAt(object, "providerRequestId"),
      .spendMicrocredits = nullableNumberAt(object, "spendMicrocredits"),
      .fallback = nullableStringAt(object, "fallback"),
    };
  }

  template <> AccessAuthorization fromJson<AccessAuthorization>(const json::value& value) {
    const json::object& object = objectOf(value);
    return AccessAuthorization{
      .ownerId = stringAt(object, "ownerId"),
      .ownerEpoch = numberAt(object, "ownerEpoch"),
      .accountId = stringAt(object, "accountId"),
      .workspaceId = stringAt(object, "workspaceId"),
      .generation = numberAt(object, "generation"),
      .nativeRefreshedAtNs = stringAt(object, "nativeRefreshedAtNs"),
      .fingerprint = stringAt(object, "fingerprint"),
      .accessToken = stringAt(object, "accessToken"),
      .idToken = stringAt(object, "idToken"),
      .issuedAtMs = numberAt(object, "issuedAtMs"),
      .expiresAtMs = numberAt(object, "expiresAtMs"),
    };
  }

  template <> AttemptRecord fromJson<AttemptRecord>(const json::value& value) {
    const json::object& object = objectOf(value);
    return AttemptRecord{
      .input = fromJson<AttemptInput>(memberOf(object, "input")),
      .state = stringAt(object, "state"),
      .terminal = nullableAt<TerminalEvidence>(object, "terminal"),
      .billingSettled = boolAt(object, "billingSettled"),
      .settledSpendMicrocredits = nullableNumberAt(object, "settledSpendMicrocredits"),
    };
  }

  template <> AdmissionRecord fromJson<AdmissionRecord>(const json::value& value) {
    const json::object& object = objectOf(value);
    return AdmissionRecord{
      .handle = fromJson<AdmissionHandle>(memberOf(object, "handle")),
      .principalId = stringAt(object, "principalId"),
      .policyVersion = numberAt(object, "policyVersion"),
      .windowKey = stringAt(object, "windowKey"),
      .digest = stringAt(object, "digest"),
      .route = stringAt(object, "route"),
      .model = stringAt(object, "model"),
      .account = nullableAt<AccountBinding>(object, "account"),
      .activeGeneration = nullableNumberAt(object, "activeGeneration"),
      .attempts = recordAt<AttemptRecord>(object, "attempts"),
      .dispatched = boolAt(object, "dispatched"),
      .semantic = nullableStringAt(object, "semantic"),
      .kernelSettled = boolAt(object, "kernelSettled"),
      .delivery = nullableStringAt(object, "delivery"),
      .settled = boolAt(object, "settled"),
    };
  }

  template <> AuthorityState fromJson<AuthorityState>(const json::value& value) {
    const json::object& object = objectOf(value);
    const json::value& accountsValue = memberOf(object, "accounts");
    if (!accountsValue.is_array()) shapeError("accounts", "an array");
    std::vector<AccountBinding> accounts;
    for (const json::value& account : accountsValue.get_array()) accounts.push_back(fromJson<AccountBinding>(account));
    return AuthorityState{
      .scope = fromJson<AuthorityScope>(memberOf(object, "scope")),
      .sequence = numberAt(object, "sequence"),
      .accounts = std::move(accounts),
      .active = nullableAt<ActiveAccount>(object, "active"),
      .policies = recordAt<PrincipalPolicy>(object, "policies"),
      .windows = recordAt<PrincipalWindow>(object, "windows"),
      .admissions = recordAt<AdmissionRecord>(object, "admissions"),
      .publications = recordAt<AccessAuthorization>(object, "publications"),
    };
  }

  template <> AdmitInput fromJson<AdmitInput>(const json::value& value) {
    const json::object& object = objectOf(value);
    return AdmitInput{
      .scope = fromJson<AuthorityScope>(memberOf(object, "scope")),
      .requestId = stringAt(object, "requestId"),
      .principalId = stringAt(object, "principalId"),
      .policyVersion = numberAt(object, "policyVersion"),
      .digest = stringAt(object, "digest"),
      .route = stringAt(object, "route"),
      .model = stringAt(object, "model"),
      .nowMs = numberAt(object, "nowMs"),
    };
  }
}
