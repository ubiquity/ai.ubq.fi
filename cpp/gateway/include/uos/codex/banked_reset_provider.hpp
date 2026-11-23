#pragma once
/**
 * Boundary for an externally visible Codex usage-reset redemption.
 *
 * The upstream Codex reset-credit API has a documented inventory and consume
 * schema. HTTP 2xx is the transport-success gate; the response `code` is the
 * terminal redemption result.
 */
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::codex::banked_reset_provider {
  /**
   * Stable, non-secret inputs that bind a redemption to one exhausted account
   * and one observed quota generation. `quotaGeneration` must not be a request
   * identifier: it is the durable identity of the exhaustion window.
   */
  struct ResetAccountContext {
    std::string accountId;
    std::string accountIdHash;
    std::string credentialVersion;
    std::string quotaGeneration;
    friend bool operator==(const ResetAccountContext&, const ResetAccountContext&) = default;
  };

  /** A fully detailed opaque reset credit returned for one account. */
  struct ResetInventoryCredit {
    /** Opaque upstream identifier. It is hashed before reaching telemetry or KV. */
    std::string id;
    std::string status;
    std::string resetType;
    /** Null means the provider declares the credit non-expiring. */
    std::optional<double> expiresAtMs;
    friend bool operator==(const ResetInventoryCredit&, const ResetInventoryCredit&) = default;
  };

  /**
   * A provider's observed reset inventory for one account.  An omitted or
   * capped credit list is deliberately not representable: the caller must be
   * able to prove that it selected a specific available credit before it can
   * ever submit a consume request.
   */
  struct ResetInventory {
    double availableCount;
    double observedAtMs;
    std::vector<ResetInventoryCredit> credits;
    friend bool operator==(const ResetInventory&, const ResetInventory&) = default;
  };

  /**
   * Results deliberately preserve ambiguity. Callers must never issue another
   * redemption for `unknown`: reconcilable providers use `lookup()` and
   * `verifyApplied()`, while terminal-only providers retain the ambiguity.
   */
  struct RedeemResetResultCompleted {
    static constexpr std::string_view kind = "completed";
    std::string providerReceiptId;
    friend bool operator==(const RedeemResetResultCompleted&, const RedeemResetResultCompleted&) = default;
  };
  struct RedeemResetResultAccepted {
    static constexpr std::string_view kind = "accepted";
    std::string providerReceiptId;
    friend bool operator==(const RedeemResetResultAccepted&, const RedeemResetResultAccepted&) = default;
  };
  struct RedeemResetResultAlreadyRedeemed {
    static constexpr std::string_view kind = "already_redeemed";
    std::string providerReceiptId;
    friend bool operator==(const RedeemResetResultAlreadyRedeemed&, const RedeemResetResultAlreadyRedeemed&) = default;
  };
  struct RedeemResetResultRejected {
    static constexpr std::string_view kind = "rejected";
    std::string reason;
    friend bool operator==(const RedeemResetResultRejected&, const RedeemResetResultRejected&) = default;
  };
  struct RedeemResetResultUnknown {
    static constexpr std::string_view kind = "unknown";
    std::optional<std::string> providerReceiptId;
    friend bool operator==(const RedeemResetResultUnknown&, const RedeemResetResultUnknown&) = default;
  };
  using RedeemResetResult =
    std::variant<RedeemResetResultCompleted, RedeemResetResultAccepted, RedeemResetResultAlreadyRedeemed, RedeemResetResultRejected, RedeemResetResultUnknown>;

  /**
   * Capabilities established from an exact provider contract, not inferred from
   * a successful HTTP response. An unavailable or unreviewed contract must use
   * false/null/empty values so live redemption remains disabled.
   */
  struct CodexUsageResetProviderContract {
    struct Idempotency {
      /** The provider accepts the caller's deterministic idempotency key. */
      bool callerSupplied;
      /** Documented retention period for that key, or null when unproven. */
      std::optional<double> retentionMs;
    };
    struct Lookup {
      /** Required to reconcile an ambiguous request that never returned a receipt. */
      bool byIdempotencyKey;
      /** Useful after a receipt has been durably recorded. */
      bool byProviderReceiptId;
    };
    struct Verification {
      /** A distinct observation can prove the quota reset actually took effect. */
      bool independentlyVerifiable;
    };
    Idempotency idempotency;
    Lookup lookup;
    Verification verification;
    /**
     * The provider's parsed terminal `redeem` result is authoritative for this
     * one submission. Ambiguous transport or response outcomes never qualify.
     */
    std::optional<bool> redeemOutcomeIsFinal = std::nullopt;
    /**
     * Whether a receipt identifier is non-secret and may be retained in the
     * durable audit record or emitted in logs. When false, a provider that
     * supports reconciliation uses the caller-supplied idempotency key instead.
     */
    bool receiptIdsSafeToPersistAndLog;
    /** Only reset types whose exact semantics have been documented and reviewed. */
    std::vector<std::string> supportedResetTypes;
  };
  // The contract as the plain object it is at run time (member order as declared;
  // an undefined redeemOutcomeIsFinal is omitted).
  json::value toJson(const CodexUsageResetProviderContract& contract);

  // `ResetAccountContext & Readonly<{ idempotencyKey; creditId }>`
  struct RedeemResetInput : ResetAccountContext {
    /** Never log this raw value; persist or emit only a hash. */
    std::string idempotencyKey;
    /** Exact opaque credit selected from the immediately-read complete inventory. */
    std::string creditId;
  };

  // `ResetAccountContext & Readonly<{ idempotencyKey; providerReceiptId }>`
  struct LookupRedeemResetInput : ResetAccountContext {
    /** Never log this raw value; persist or emit only a hash. */
    std::string idempotencyKey;
    std::optional<std::string> providerReceiptId;
  };

  /**
   * Injectable provider boundary. Tests can implement this interface with an
   * injected transport, so automated validation never reaches the real service.
   */
  class CodexUsageResetProvider {
  public:
    virtual ~CodexUsageResetProvider() = default;
    // `readonly contract`, held as the value an injected adapter actually
    // carries: the provider* checks below re-validate it at run time.
    json::value contract;
    virtual rt::Task<ResetInventory> readInventory(ResetAccountContext input, rt::AbortSignal signal) = 0;
    virtual rt::Task<RedeemResetResult> redeem(RedeemResetInput input, rt::AbortSignal signal) = 0;
    virtual rt::Task<RedeemResetResult> lookup(LookupRedeemResetInput input, rt::AbortSignal signal) = 0;
    virtual rt::Task<json::value> verifyApplied(ResetAccountContext input, rt::AbortSignal signal) = 0;

  protected:
    explicit CodexUsageResetProvider(json::value contract_) : contract(std::move(contract_)) {}
  };

  /** Injectable transport used to keep upstream-adapter tests fully offline. */
  using CodexUsageResetFetch = rt::Fetch;

  /** Credentials are bound in this closure, never placed in a durable reset context or record. */
  struct UpstreamCodexUsageResetProviderOptions {
    std::string codexBaseUrl;
    std::string accountId;
    std::string accessToken;
    std::string userAgent;
    // Empty: undefined.
    CodexUsageResetFetch fetch{};
    // Empty: undefined.
    std::function<double()> now{};
  };

  struct CodexUsageResetCreditEndpoints {
    std::string inventoryUrl;
    std::string consumeUrl;
    friend bool operator==(const CodexUsageResetCreditEndpoints&, const CodexUsageResetCreditEndpoints&) = default;
  };

  /** A non-secret configuration error that prevents any reset transport. */
  class CodexUsageResetProviderConfigurationError : public rt::JsError {
  public:
    explicit CodexUsageResetProviderConfigurationError(std::string message_);
  };

  /** A non-secret inventory-status failure. Its response body is never read. */
  class CodexUsageResetProviderHttpError : public rt::JsError {
  public:
    // "inventory"
    const std::string operation;
    const double status;
    CodexUsageResetProviderHttpError(std::string operation_, double status_);
  };

  /**
   * Resolve the approved reset-credit routes from the existing Codex base. An
   * unknown layout fails closed instead of appending a speculative suffix.
   */
  CodexUsageResetCreditEndpoints resolveCodexUsageResetCreditEndpoints(const std::string& codexBaseUrl);

  /**
   * Build the account-bound upstream adapter. It uses the pinned Codex source
   * contract: inventory is `{ credits, available_count }`; consume is
   * `{ redeem_request_id, credit_id? }`; only `reset` and `already_redeemed`
   * are terminal results. A non-2xx, malformed 2xx, or transport loss after
   * consume is ambiguous and never triggers a speculative second consume.
   */
  std::shared_ptr<CodexUsageResetProvider> createUpstreamCodexUsageResetProvider(const UpstreamCodexUsageResetProviderOptions& options);

  // The checks below take the provider as possibly absent (nullptr), as the
  // TypeScript treats an injected receiver.

  /**
   * Runtime validation for an injected provider's advertised capabilities.
   *
   * The production boundary is intentionally typed, but an adapter is still an
   * external integration and tests can inject arbitrary JavaScript. Treat a
   * malformed contract exactly like an unavailable one: fail closed before
   * inventory, lookup, verification, or redemption is called.
   */
  bool providerSupportsResetType(const CodexUsageResetProvider* provider, const json::value& resetType);
  // `resetType` undefined: nullptr.
  bool providerSupportsResetType(const CodexUsageResetProvider* provider, const json::value* resetType);

  /** Returns whether receipt identifiers are explicitly approved for retention. */
  bool providerReceiptIdsSafeToPersistAndLog(const CodexUsageResetProvider* provider);

  /** Returns true only for a provider with documented terminal redeem outcomes. */
  bool providerTreatsRedeemOutcomeAsFinal(const CodexUsageResetProvider* provider);

  /**
   * Returns true for a provider with either a replay-safe, independently
   * reconcilable contract or documented terminal redeem outcomes. The latter is
   * deliberately at-most-once: an ambiguous outcome remains durable `unknown`
   * and is never submitted again.
   */
  bool providerSupportsLiveRedemption(const CodexUsageResetProvider* provider);

  /** A stable, non-secret failure used by the default fail-closed provider. */
  class CodexUsageResetProviderUnavailableError : public rt::JsError {
  public:
    const std::string code = "codex_usage_reset_provider_unavailable";
    CodexUsageResetProviderUnavailableError();
  };

  /**
   * Fail-closed fallback provider. It has no network behavior and is used when
   * the account-bound upstream adapter cannot be constructed.
   */
  extern const std::shared_ptr<CodexUsageResetProvider> unavailableCodexUsageResetProvider;

  /** One redacted diagnostics credit: status and expiry only, never the raw id. */
  struct CodexResetCreditDiagnostic {
    std::string status;
    std::optional<double> expires_at_ms;
    friend bool operator==(const CodexResetCreditDiagnostic&, const CodexResetCreditDiagnostic&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexResetCreditDiagnostic& value);
  CodexResetCreditDiagnostic tag_invoke(const json::value_to_tag<CodexResetCreditDiagnostic>&, const json::value& value);

  /**
   * Read-only diagnostics list; never authorizes a redemption or refreshes
   * credentials. It shares the bounded display-read pattern with the count so an
   * admin view can show expiries without touching the spend path.
   */
  rt::Task<std::vector<CodexResetCreditDiagnostic>> readCodexResetCredits(UpstreamCodexUsageResetProviderOptions options, rt::AbortSignal signal);

  /** Read-only display count; never authorizes a redemption or refreshes credentials. */
  rt::Task<double> readCodexResetAvailableCount(UpstreamCodexUsageResetProviderOptions options, rt::AbortSignal signal);
}
