#pragma once
/** Private constructor capabilities. These are not public HTTP/admin operations. */
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::authority::contracts {
  // A TS `Record<string, T>` held as a plain JS object: own keys enumerate in JS
  // property order (array-index keys ascending, then insertion order),
  // `obj[key] = value` is [[Set]] (an absent "__proto__" key creates nothing)
  // and Object.fromEntries / JSON.parse / structuredClone define own keys.
  template <class T> class Record {
  public:
    // `Object.hasOwn(obj, key) ? obj[key] : undefined`
    T* get(std::string_view key) noexcept { return entries_.get(key); }
    const T* get(std::string_view key) const noexcept { return entries_.get(key); }
    bool has(std::string_view key) const noexcept { return entries_.has(key); }
    // `obj[key] = value`
    void set(const std::string& key, T value) {
      if (key == "__proto__" && !entries_.has(key)) return;
      entries_.set(key, std::move(value));
    }
    // CreateDataProperty (Object.fromEntries, JSON.parse, structuredClone).
    void define(const std::string& key, T value) { entries_.set(key, std::move(value)); }
    // `Object.keys(obj).length`
    std::size_t size() const noexcept { return entries_.size(); }
    // `Object.keys(obj)`, JS property order.
    std::vector<std::string> keys() const {
      std::vector<std::string> indexes;
      std::vector<std::string> names;
      for (const auto& [key, value] : entries_) (rt::isArrayIndex(key) ? indexes : names).push_back(key);
      std::stable_sort(indexes.begin(), indexes.end(), [](const std::string& a, const std::string& b) { return a.size() != b.size() ? a.size() < b.size() : a < b; });
      indexes.insert(indexes.end(), names.begin(), names.end());
      return indexes;
    }
    // `Object.values(obj)` as references into the record, JS property order.
    std::vector<T*> values() {
      std::vector<T*> out;
      for (const std::string& key : keys()) out.push_back(entries_.get(key));
      return out;
    }
    std::vector<const T*> values() const {
      std::vector<const T*> out;
      for (const std::string& key : keys()) out.push_back(entries_.get(key));
      return out;
    }
    // Structural equality (`toEqual`): the same own keys with equal values.
    friend bool operator==(const Record& left, const Record& right) {
      if (left.size() != right.size()) return false;
      for (const auto& [key, value] : left.entries_) {
        const T* other = right.get(key);
        if (other == nullptr || !(*other == value)) return false;
      }
      return true;
    }

  private:
    rt::OrderedMap<std::string, T> entries_;
  };

  struct AuthorityScope {
    std::string namespace_;
    double epoch;
    friend bool operator==(const AuthorityScope&, const AuthorityScope&) = default;
  };
  // "/v1/responses" | "/v1/chat/completions"
  using GatewayRoute = std::string;
  // "codex" | "surplus" | "openlux"
  using DispatchProvider = std::string;
  // "delivered" | "interrupted" | "unobserved"
  using DeliveryOutcome = std::string;
  // "completed" | "incomplete" | "failed" | "cancelled"
  using SemanticTerminal = std::string;
  struct AccountProof {
    // "quota_exhausted" | "credential_invalid" | "transient"
    std::string kind;
    std::string model;
    std::string fingerprint;
    double observedAtMs;
    double validUntilMs;
    friend bool operator==(const AccountProof&, const AccountProof&) = default;
  };
  struct AccountBinding {
    std::string accountId;
    std::string fingerprint;
    double credentialGeneration;
    double routingGeneration;
    double resetGeneration;
    double overageVersion;
    bool overageEnabled;
    bool enabled;
    std::vector<std::string> models;
    std::optional<AccountProof> proof;
    friend bool operator==(const AccountBinding&, const AccountBinding&) = default;
  };
  struct ActiveAccount {
    std::string accountId;
    std::string fingerprint;
    double generation;
    friend bool operator==(const ActiveAccount&, const ActiveAccount&) = default;
  };
  struct PrincipalPolicy {
    std::string principalId;
    double version;
    bool deleted;
    double windowResetAtMs;
    double requestLimit;
    double kernelLimit;
    double concurrencyLimit;
    double paidLimitMicrocredits;
    friend bool operator==(const PrincipalPolicy&, const PrincipalPolicy&) = default;
  };
  struct PrincipalWindow {
    double reserved;
    double dispatched;
    double kernelReserved;
    double kernelCompleted;
    double active;
    double paidReservedMicrocredits;
    double paidSettledMicrocredits;
    friend bool operator==(const PrincipalWindow&, const PrincipalWindow&) = default;
  };
  struct AdmissionHandle {
    AuthorityScope scope;
    std::string requestId;
    double fence;
    friend bool operator==(const AdmissionHandle&, const AdmissionHandle&) = default;
  };
  struct AttemptInput {
    std::string attemptId;
    DispatchProvider provider;
    double maximumExposureMicrocredits;
    std::optional<AccountBinding> account;
    friend bool operator==(const AttemptInput&, const AttemptInput&) = default;
  };
  struct TerminalEvidence {
    SemanticTerminal outcome;
    std::optional<std::string> providerRequestId;
    /** Null means billing remains pending; zero is authoritative zero usage. */
    std::optional<double> spendMicrocredits;
    // "quota_exhausted" | "capacity_exhausted" | null
    std::optional<std::string> fallback;
    friend bool operator==(const TerminalEvidence&, const TerminalEvidence&) = default;
  };
  struct AccessAuthorization {
    std::string ownerId;
    double ownerEpoch;
    std::string accountId;
    std::string workspaceId;
    double generation;
    std::string nativeRefreshedAtNs;
    std::string fingerprint;
    std::string accessToken;
    std::string idToken;
    double issuedAtMs;
    double expiresAtMs;
    friend bool operator==(const AccessAuthorization&, const AccessAuthorization&) = default;
  };
  struct AttemptRecord {
    AttemptInput input;
    // "reserved" | "dispatch_intent" | "dispatched" | "not_dispatched"
    std::string state;
    std::optional<TerminalEvidence> terminal;
    bool billingSettled;
    std::optional<double> settledSpendMicrocredits;
    friend bool operator==(const AttemptRecord&, const AttemptRecord&) = default;
  };
  struct AdmissionRecord {
    AdmissionHandle handle;
    std::string principalId;
    double policyVersion;
    std::string windowKey;
    std::string digest;
    GatewayRoute route;
    std::string model;
    std::optional<AccountBinding> account;
    std::optional<double> activeGeneration;
    Record<AttemptRecord> attempts;
    bool dispatched;
    std::optional<SemanticTerminal> semantic;
    bool kernelSettled;
    std::optional<DeliveryOutcome> delivery;
    bool settled;
    friend bool operator==(const AdmissionRecord&, const AdmissionRecord&) = default;
  };
  struct AuthorityState {
    AuthorityScope scope;
    double sequence;
    std::vector<AccountBinding> accounts;
    std::optional<ActiveAccount> active;
    Record<PrincipalPolicy> policies;
    Record<PrincipalWindow> windows;
    Record<AdmissionRecord> admissions;
    Record<AccessAuthorization> publications;
    friend bool operator==(const AuthorityState&, const AuthorityState&) = default;
  };
  struct AuthoritySnapshot {
    double revision;
    AuthorityState state;
    friend bool operator==(const AuthoritySnapshot&, const AuthoritySnapshot&) = default;
  };
  /** The adapter must provide a coherent strong snapshot and atomic CAS over ALL state. */
  class AuthorityStore {
  public:
    virtual ~AuthorityStore() = default;
    virtual rt::Promise<AuthoritySnapshot> read() = 0;
    virtual rt::Promise<bool> compareAndSwap(double revision, AuthorityState state) = 0;
  };
  struct AdmitInput {
    AuthorityScope scope;
    std::string requestId;
    std::string principalId;
    double policyVersion;
    std::string digest;
    GatewayRoute route;
    std::string model;
    double nowMs;
    friend bool operator==(const AdmitInput&, const AdmitInput&) = default;
  };
  class AuthorityError : public rt::JsError {
  public:
    const std::string code;
    const double status;
    explicit AuthorityError(std::string code_, double status_ = 409);
  };
  bool sameScope(const AuthorityScope& left, const AuthorityScope& right);
  void assertAmount(double value);
  // `JSON.stringify(left) === JSON.stringify(right)` over two unknown values;
  // nullptr is undefined, whose JSON.stringify is undefined itself.
  bool sameEvidence(const json::value& left, const json::value& right);
  bool sameEvidence(const json::value* left, const json::value* right);

  template <class T> T* lookup(Record<T>& values, std::string_view key) { return values.get(key); }
  template <class T> const T* lookup(const Record<T>& values, std::string_view key) { return values.get(key); }

  // The JSON view of the plain objects above (what JSON.stringify and the
  // RPC/storage guards see): member order is the order the TypeScript builds
  // each object in, numbers are canonical JS numbers and null members are
  // written as null.
  json::value toJson(const AuthorityScope& value);
  json::value toJson(const AccountProof& value);
  json::value toJson(const AccountBinding& value);
  json::value toJson(const ActiveAccount& value);
  json::value toJson(const PrincipalPolicy& value);
  json::value toJson(const PrincipalWindow& value);
  json::value toJson(const AdmissionHandle& value);
  json::value toJson(const AttemptInput& value);
  json::value toJson(const TerminalEvidence& value);
  json::value toJson(const AccessAuthorization& value);
  json::value toJson(const AttemptRecord& value);
  json::value toJson(const AdmissionRecord& value);
  json::value toJson(const AuthorityState& value);
  json::value toJson(const AdmitInput& value);
  template <class T> json::value toJson(const std::optional<T>& value) { return value ? toJson(*value) : json::value(nullptr); }

  // The typed object a validated JSON value stands for. The value must already
  // satisfy the matching shape guard (authority-rpc / durable-store);
  // rt::TypeError when it does not.
  template <class T> T fromJson(const json::value& value);
  template <> AuthorityScope fromJson<AuthorityScope>(const json::value& value);
  template <> AccountProof fromJson<AccountProof>(const json::value& value);
  template <> AccountBinding fromJson<AccountBinding>(const json::value& value);
  template <> ActiveAccount fromJson<ActiveAccount>(const json::value& value);
  template <> PrincipalPolicy fromJson<PrincipalPolicy>(const json::value& value);
  template <> PrincipalWindow fromJson<PrincipalWindow>(const json::value& value);
  template <> AdmissionHandle fromJson<AdmissionHandle>(const json::value& value);
  template <> AttemptInput fromJson<AttemptInput>(const json::value& value);
  template <> TerminalEvidence fromJson<TerminalEvidence>(const json::value& value);
  template <> AccessAuthorization fromJson<AccessAuthorization>(const json::value& value);
  template <> AttemptRecord fromJson<AttemptRecord>(const json::value& value);
  template <> AdmissionRecord fromJson<AdmissionRecord>(const json::value& value);
  template <> AuthorityState fromJson<AuthorityState>(const json::value& value);
  template <> AdmitInput fromJson<AdmitInput>(const json::value& value);
}
