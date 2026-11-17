#include "uos/codex/banked_reset_provider.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace uos::codex::banked_reset_provider {
  namespace {
    std::string nonEmptyOption(const std::string& value, const std::string& name) {
      std::string normalized = rt::trim(value);
      if (normalized.empty()) throw CodexUsageResetProviderConfigurationError(name + " must be non-empty.");
      return normalized;
    }

    /** Validate whitespace-only text without changing an opaque protocol value. */
    void requireNonBlank(const std::string& value, const std::string& name) {
      if (rt::trim(value).empty()) throw CodexUsageResetProviderConfigurationError(name + " must be non-empty.");
    }

    bool isHttpSuccess(double status) { return rt::Number::isInteger(status) && status >= 200 && status < 300; }
    bool isNonnegativeSafeInteger(const json::value* value) {
      return value != nullptr && rt::isNumber(*value) && rt::Number::isSafeInteger(*value) && rt::toNumber(*value) >= 0;
    }

    bool isRecord(const json::value* value) { return rt::isRecord(value); }

    rt::Nullable<double> parsedExpiry(const json::value* value) {
      if (value != nullptr && value->is_null()) return nullptr;
      if (value == nullptr || !value->is_string() || rt::trim(std::string_view(value->get_string())).empty()) return rt::undefined;
      const double parsed = rt::Date::parse(std::string_view(value->get_string()));
      if (rt::Number::isSafeInteger(parsed) && parsed >= 0) return parsed;
      return rt::undefined;
    }

    std::string stringOrEmpty(const json::object& item, std::string_view key) {
      const std::optional<std::string_view> text = rt::getString(item, key);
      return text ? std::string(*text) : "";
    }

    std::optional<std::vector<ResetInventoryCredit>> parseDetailedCredits(const json::value* value, double availableCount) {
      if (value == nullptr || !value->is_array()) return std::nullopt;
      std::vector<ResetInventoryCredit> credits;
      std::unordered_set<std::string> ids;
      double detailedAvailableCount = 0;
      for (const json::value& entry : value->get_array()) {
        if (!isRecord(&entry)) return std::nullopt;
        const json::object& item = entry.get_object();
        const std::string id = stringOrEmpty(item, "id");
        const std::string status = stringOrEmpty(item, "status");
        const std::string resetType = stringOrEmpty(item, "reset_type");
        const rt::Nullable<double> expiresAtMs = parsedExpiry(rt::get(item, "expires_at"));
        if (rt::trim(id).empty() || rt::trim(status).empty() || rt::trim(resetType).empty() || expiresAtMs.isUndefined() || ids.contains(id)) return std::nullopt;
        ids.insert(id);
        if (status == "available") detailedAvailableCount += 1;
        credits.push_back(ResetInventoryCredit{.id = id, .status = status, .resetType = resetType, .expiresAtMs = expiresAtMs.toOptional()});
      }
      // The upstream may cap the list. A capped or summary-only result is unsafe
      // for a deterministic account-bound spend, so do not select from it.
      if (detailedAvailableCount != availableCount) return std::nullopt;
      return credits;
    }

    /**
     * Removes the trailing run of "/" characters. Equivalent to
     * `value.replace(/\/+$/, "")`, but linear: that pattern backtracked over the
     * whole trailing run at every offset, so an operator-supplied base URL such as
     * `https://host////...////x` cost O(n^2) during configuration resolution.
     */
    std::string stripTrailingSlashes(const std::string& value) {
      std::size_t end = value.size();
      while (end > 0 && value[end - 1] == '/') end--;
      return value.substr(0, end);
    }

    /** The pinned Codex layouts that expose the rate-limit-reset-credit endpoints. */
    std::optional<std::string> creditPathForCodexLayout(const std::string& pathname) {
      if (pathname == "/backend-api" || pathname == "/backend-api/codex") return "/backend-api/wham/rate-limit-reset-credits";
      if (pathname == "/" || pathname == "/api/codex") return "/api/codex/rate-limit-reset-credits";
      return std::nullopt;
    }

    /** Discard a non-success response body without treating it as a result. */
    void discardResponseBody(const rt::Response& response) {
      try {
        if (const std::optional<rt::ByteStream> body = response.body()) rt::spawn(body->cancel(), rt::ignoreRejection);
      } catch (...) {
        // Best-effort cleanup never changes the result.
      }
    }

    const json::value& upstreamProviderContract() {
      static const json::value contract = toJson(CodexUsageResetProviderContract{
        // `redeem_request_id` is caller-supplied and upstream documents same-key
        // replay as idempotent. We intentionally do not auto-replay an ambiguous
        // request because no retention period or lookup endpoint is documented.
        .idempotency = {.callerSupplied = true, .retentionMs = std::nullopt},
        .lookup = {.byIdempotencyKey = false, .byProviderReceiptId = false},
        .verification = {.independentlyVerifiable = false},
        // The pinned Codex schema defines `reset` and `already_redeemed` as terminal
        // success codes. Transport loss, non-2xx, malformed JSON, and unknown codes
        // remain ambiguous and are never replayed automatically.
        .redeemOutcomeIsFinal = true,
        // The documented response has no provider receipt identifier.
        .receiptIdsSafeToPersistAndLog = false,
        .supportedResetTypes = {"codex_rate_limits"},
      });
      return contract;
    }

    RedeemResetResult unknownResult() { return RedeemResetResultUnknown{.providerReceiptId = std::nullopt}; }

    template <class T> rt::Task<T> resolved(T value) { co_return value; }

    // What createUpstreamCodexUsageResetProvider's closure binds.
    struct Bound {
      CodexUsageResetCreditEndpoints endpoints;
      std::string accountId;
      std::string accessToken;
      std::string userAgent;
      rt::Fetch fetcher;
      std::function<double()> now;
    };

    void assertBoundAccount(const Bound& bound, const ResetAccountContext& input) {
      if (input.accountId != bound.accountId) {
        throw CodexUsageResetProviderConfigurationError("Reset account does not match the provider-bound account.");
      }
    }

    rt::Headers headers(const Bound& bound, bool contentType) {
      rt::Headers result{
        {"Authorization", "Bearer " + bound.accessToken},
        {"ChatGPT-Account-ID", bound.accountId},
        {"user-agent", bound.userAgent},
      };
      if (contentType) result.set("Content-Type", "application/json");
      return result;
    }

    rt::Task<rt::Response> request(std::shared_ptr<const Bound> bound, std::string url, rt::RequestInit init, rt::AbortSignal signal) {
      if (signal.aborted()) {
        if (signal.reason()) rt::rethrow(signal.reason());
        throw rt::DOMException("The request was aborted.", "AbortError");
      }
      init.signal = signal;
      init.redirect = "manual";
      rt::Task<rt::Response> fetching = bound->fetcher(rt::RequestInfo(std::move(url)), std::move(init));
      rt::Response response = co_await std::move(fetching);
      co_return response;
    }

    rt::Task<ResetInventory> readInventory(std::shared_ptr<const Bound> bound, ResetAccountContext input, rt::AbortSignal signal) {
      assertBoundAccount(*bound, input);
      rt::RequestInit init;
      init.method = "GET";
      init.headers = headers(*bound, false);
      rt::Task<rt::Response> requesting = request(bound, bound->endpoints.inventoryUrl, std::move(init), signal);
      const rt::Response response = co_await std::move(requesting);
      if (!isHttpSuccess(response.status())) {
        discardResponseBody(response);
        throw CodexUsageResetProviderHttpError("inventory", response.status());
      }
      json::value payload;
      bool parsed = true;
      try {
        rt::Task<json::value> reading = response.json();
        payload = co_await std::move(reading);
      } catch (...) {
        parsed = false;
      }
      if (!parsed) throw CodexUsageResetProviderConfigurationError("Reset-credit inventory response was not valid JSON.");
      const json::value* availableCount = rt::get(payload, "available_count");
      if (!isRecord(&payload) || !isNonnegativeSafeInteger(availableCount)) {
        throw CodexUsageResetProviderConfigurationError("Reset-credit inventory response was invalid.");
      }
      std::optional<std::vector<ResetInventoryCredit>> credits = parseDetailedCredits(rt::get(payload, "credits"), rt::toNumber(*availableCount));
      if (!credits) {
        throw CodexUsageResetProviderConfigurationError("Reset-credit inventory did not include a complete detailed available-credit list.");
      }
      const double observedAtMs = bound->now();
      if (!rt::Number::isSafeInteger(observedAtMs) || observedAtMs < 0) {
        throw CodexUsageResetProviderConfigurationError("now must return a non-negative safe integer.");
      }
      co_return ResetInventory{
        .availableCount = rt::toNumber(*availableCount),
        .observedAtMs = observedAtMs,
        .credits = std::move(*credits),
      };
    }

    rt::Task<RedeemResetResult> redeem(std::shared_ptr<const Bound> bound, RedeemResetInput input, rt::AbortSignal signal) {
      assertBoundAccount(*bound, input);
      requireNonBlank(input.idempotencyKey, "idempotencyKey");
      requireNonBlank(input.creditId, "creditId");
      json::object body;
      body["redeem_request_id"] = input.idempotencyKey;
      body["credit_id"] = input.creditId;
      rt::RequestInit init;
      init.method = "POST";
      init.headers = headers(*bound, true);
      init.body = rt::BodyInit(rt::jsonStringify(body));
      rt::Task<rt::Response> requesting = request(bound, bound->endpoints.consumeUrl, std::move(init), signal);
      const rt::Response response = co_await std::move(requesting);
      if (!isHttpSuccess(response.status())) {
        discardResponseBody(response);
        co_return unknownResult();
      }
      json::value payload;
      bool parsed = true;
      try {
        rt::Task<json::value> reading = response.json();
        payload = co_await std::move(reading);
      } catch (...) {
        parsed = false;
      }
      if (!parsed) co_return unknownResult();
      const std::optional<std::string_view> codeText = isRecord(&payload) ? rt::getString(payload, "code") : std::nullopt;
      const std::optional<std::string> code = codeText ? std::optional<std::string>(std::string(*codeText)) : std::nullopt;
      if (code == "reset") co_return RedeemResetResultCompleted{.providerReceiptId = "upstream-reset"};
      if (code == "already_redeemed") co_return RedeemResetResultAlreadyRedeemed{.providerReceiptId = "upstream-already-redeemed"};
      if (code == "nothing_to_reset" || code == "no_credit") co_return RedeemResetResultRejected{.reason = *code};
      co_return unknownResult();
    }

    class UpstreamCodexUsageResetProvider final : public CodexUsageResetProvider {
    public:
      explicit UpstreamCodexUsageResetProvider(std::shared_ptr<const Bound> bound_) : CodexUsageResetProvider(upstreamProviderContract()), bound(std::move(bound_)) {}
      rt::Task<ResetInventory> readInventory(ResetAccountContext input, rt::AbortSignal signal) override {
        return banked_reset_provider::readInventory(bound, std::move(input), std::move(signal));
      }
      rt::Task<RedeemResetResult> redeem(RedeemResetInput input, rt::AbortSignal signal) override {
        return banked_reset_provider::redeem(bound, std::move(input), std::move(signal));
      }
      rt::Task<RedeemResetResult> lookup(LookupRedeemResetInput, rt::AbortSignal) override { return resolved(unknownResult()); }
      rt::Task<json::value> verifyApplied(ResetAccountContext, rt::AbortSignal) override { return resolved(json::value(false)); }

    private:
      std::shared_ptr<const Bound> bound;
    };

    bool isPositiveSafeInteger(const json::value* value) {
      return value != nullptr && rt::isNumber(*value) && rt::Number::isSafeInteger(*value) && rt::toNumber(*value) > 0;
    }

    bool hasNonEmptyResetType(const json::value* value) { return value != nullptr && value->is_string() && rt::length(rt::trim(std::string_view(value->get_string()))) > 0; }

    /**
     * Re-reads an injected provider's contract through a nullable view.
     *
     * The production boundary is typed, but an adapter is still an external
     * integration and tests inject arbitrary JavaScript (see the malformed-contract
     * case in tests/codex-banked-reset.test.ts). The receiver is therefore treated
     * as possibly absent here rather than trusting the declaration.
     */
    const json::value* injectedProviderContract(const CodexUsageResetProvider* provider) { return provider != nullptr ? &provider->contract : nullptr; }

    [[noreturn]] void unavailable() { throw CodexUsageResetProviderUnavailableError(); }

    class UnavailableCodexUsageResetProvider final : public CodexUsageResetProvider {
    public:
      UnavailableCodexUsageResetProvider()
        : CodexUsageResetProvider(toJson(CodexUsageResetProviderContract{
            .idempotency = {.callerSupplied = false, .retentionMs = std::nullopt},
            .lookup = {.byIdempotencyKey = false, .byProviderReceiptId = false},
            .verification = {.independentlyVerifiable = false},
            .redeemOutcomeIsFinal = false,
            .receiptIdsSafeToPersistAndLog = false,
            .supportedResetTypes = {},
          })) {}
      rt::Task<ResetInventory> readInventory(ResetAccountContext, rt::AbortSignal) override { unavailable(); }
      rt::Task<RedeemResetResult> redeem(RedeemResetInput, rt::AbortSignal) override { unavailable(); }
      rt::Task<RedeemResetResult> lookup(LookupRedeemResetInput, rt::AbortSignal) override { unavailable(); }
      rt::Task<json::value> verifyApplied(ResetAccountContext, rt::AbortSignal) override { unavailable(); }
    };

    rt::Headers displayHeaders(const UpstreamCodexUsageResetProviderOptions& options) {
      return rt::Headers{
        {"Authorization", "Bearer " + options.accessToken},
        {"ChatGPT-Account-ID", options.accountId},
        {"User-Agent", options.userAgent},
      };
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexResetCreditDiagnostic& value) {
    out = rt::record::Writer().required("status", value.status).orNull("expires_at_ms", value.expires_at_ms).done();
  }

  CodexResetCreditDiagnostic tag_invoke(const json::value_to_tag<CodexResetCreditDiagnostic>&, const json::value& value) {
    const rt::record::Reader in(value, "CodexResetCreditDiagnostic");
    return CodexResetCreditDiagnostic{.status = in.required<std::string>("status"), .expires_at_ms = in.orNull<double>("expires_at_ms")};
  }

  json::value toJson(const CodexUsageResetProviderContract& contract) {
    json::object idempotency;
    idempotency["callerSupplied"] = contract.idempotency.callerSupplied;
    idempotency["retentionMs"] = contract.idempotency.retentionMs ? rt::makeNumber(*contract.idempotency.retentionMs) : json::value(nullptr);
    json::object lookup;
    lookup["byIdempotencyKey"] = contract.lookup.byIdempotencyKey;
    lookup["byProviderReceiptId"] = contract.lookup.byProviderReceiptId;
    json::object verification;
    verification["independentlyVerifiable"] = contract.verification.independentlyVerifiable;
    json::array supportedResetTypes;
    for (const std::string& resetType : contract.supportedResetTypes) supportedResetTypes.emplace_back(resetType);
    json::object out;
    out["idempotency"] = std::move(idempotency);
    out["lookup"] = std::move(lookup);
    out["verification"] = std::move(verification);
    if (contract.redeemOutcomeIsFinal) out["redeemOutcomeIsFinal"] = *contract.redeemOutcomeIsFinal;
    out["receiptIdsSafeToPersistAndLog"] = contract.receiptIdsSafeToPersistAndLog;
    out["supportedResetTypes"] = std::move(supportedResetTypes);
    return out;
  }

  CodexUsageResetProviderConfigurationError::CodexUsageResetProviderConfigurationError(std::string message_)
    : rt::JsError(std::move(message_), "CodexUsageResetProviderConfigurationError") {}

  CodexUsageResetProviderHttpError::CodexUsageResetProviderHttpError(std::string operation_, double status_)
    : rt::JsError("Codex usage-reset " + operation_ + " request failed with status " + rt::numberToString(status_) + ".", "CodexUsageResetProviderHttpError"),
      operation(std::move(operation_)),
      status(status_) {}

  CodexUsageResetCreditEndpoints resolveCodexUsageResetCreditEndpoints(const std::string& codexBaseUrl) {
    std::optional<rt::URL> base;
    try {
      base.emplace(nonEmptyOption(codexBaseUrl, "codexBaseUrl"));
    } catch (const CodexUsageResetProviderConfigurationError&) {
      throw;
    } catch (...) {
      throw CodexUsageResetProviderConfigurationError("codexBaseUrl must be an absolute URL.");
    }
    if (base->protocol() != "https:") {
      throw CodexUsageResetProviderConfigurationError("codexBaseUrl must use HTTPS.");
    }
    if (!base->username().empty() || !base->password().empty() || !base->search().empty() || !base->hash().empty()) {
      throw CodexUsageResetProviderConfigurationError("codexBaseUrl must not include credentials, a query string, or a fragment.");
    }
    std::string pathname = stripTrailingSlashes(base->pathname());
    if (pathname.empty()) pathname = "/";
    const std::optional<std::string> creditPath = creditPathForCodexLayout(pathname);
    if (!creditPath) {
      throw CodexUsageResetProviderConfigurationError("codexBaseUrl must use the /backend-api[/codex] or /api/codex layout.");
    }
    rt::URL inventory(*base);
    inventory.pathname(*creditPath);
    inventory.search("");
    inventory.hash("");
    rt::URL consume(inventory);
    consume.pathname(*creditPath + "/consume");
    return CodexUsageResetCreditEndpoints{.inventoryUrl = inventory.toString(), .consumeUrl = consume.toString()};
  }

  std::shared_ptr<CodexUsageResetProvider> createUpstreamCodexUsageResetProvider(const UpstreamCodexUsageResetProviderOptions& options) {
    CodexUsageResetCreditEndpoints endpoints = resolveCodexUsageResetCreditEndpoints(options.codexBaseUrl);
    std::string accountId = nonEmptyOption(options.accountId, "accountId");
    std::string accessToken = nonEmptyOption(options.accessToken, "accessToken");
    std::string userAgent = nonEmptyOption(options.userAgent, "userAgent");
    rt::Fetch fetcher = options.fetch ? options.fetch : rt::Fetch([](rt::RequestInfo input, rt::RequestInit init) { return rt::fetch(std::move(input), std::move(init)); });
    std::function<double()> now = options.now ? options.now : rt::globalDateNow();
    auto bound = std::make_shared<const Bound>(Bound{
      .endpoints = std::move(endpoints),
      .accountId = std::move(accountId),
      .accessToken = std::move(accessToken),
      .userAgent = std::move(userAgent),
      .fetcher = std::move(fetcher),
      .now = std::move(now),
    });
    return std::make_shared<UpstreamCodexUsageResetProvider>(std::move(bound));
  }

  bool providerSupportsResetType(const CodexUsageResetProvider* provider, const json::value* resetType) {
    try {
      const json::value* contract = injectedProviderContract(provider);
      if (!isRecord(contract)) return false;
      const json::array* supported = rt::getArray(*contract, "supportedResetTypes");
      return supported != nullptr && hasNonEmptyResetType(resetType) && std::any_of(supported->begin(), supported->end(), [resetType](const json::value& value) {
               return rt::strictEquals(value, *resetType) && hasNonEmptyResetType(&value);
             });
    } catch (...) {
      return false;
    }
  }

  bool providerSupportsResetType(const CodexUsageResetProvider* provider, const json::value& resetType) { return providerSupportsResetType(provider, &resetType); }

  bool providerReceiptIdsSafeToPersistAndLog(const CodexUsageResetProvider* provider) {
    try {
      const json::value* contract = injectedProviderContract(provider);
      return isRecord(contract) && rt::getBool(*contract, "receiptIdsSafeToPersistAndLog") == true;
    } catch (...) {
      return false;
    }
  }

  bool providerTreatsRedeemOutcomeAsFinal(const CodexUsageResetProvider* provider) {
    try {
      const json::value* contract = injectedProviderContract(provider);
      return isRecord(contract) && rt::getBool(*contract, "redeemOutcomeIsFinal") == true;
    } catch (...) {
      return false;
    }
  }

  bool providerSupportsLiveRedemption(const CodexUsageResetProvider* provider) {
    try {
      const json::value* contract = injectedProviderContract(provider);
      if (!isRecord(contract)) return false;
      const json::value* idempotency = rt::get(*contract, "idempotency");
      const json::value* lookup = rt::get(*contract, "lookup");
      const json::value* verification = rt::get(*contract, "verification");
      if (!isRecord(idempotency) || !isRecord(lookup) || !isRecord(verification)) {
        return false;
      }
      const json::array* supported = rt::getArray(*contract, "supportedResetTypes");
      const bool supportsResetType =
        supported != nullptr && std::any_of(supported->begin(), supported->end(), [](const json::value& value) { return hasNonEmptyResetType(&value); });
      if (!supportsResetType || !rt::truthy(rt::get(*idempotency, "callerSupplied"))) return false;
      if (rt::getBool(*contract, "redeemOutcomeIsFinal") == true) return true;
      return isPositiveSafeInteger(rt::get(*idempotency, "retentionMs")) && rt::truthy(rt::get(*lookup, "byIdempotencyKey")) &&
             rt::truthy(rt::get(*verification, "independentlyVerifiable"));
    } catch (...) {
      return false;
    }
  }

  CodexUsageResetProviderUnavailableError::CodexUsageResetProviderUnavailableError()
    : rt::JsError("Codex usage-reset provider is unavailable", "CodexUsageResetProviderUnavailableError") {}

  const std::shared_ptr<CodexUsageResetProvider> unavailableCodexUsageResetProvider = std::make_shared<UnavailableCodexUsageResetProvider>();

  rt::Task<std::vector<CodexResetCreditDiagnostic>> readCodexResetCredits(UpstreamCodexUsageResetProviderOptions options, rt::AbortSignal signal) {
    const CodexUsageResetCreditEndpoints endpoints = resolveCodexUsageResetCreditEndpoints(options.codexBaseUrl);
    const rt::Fetch fetcher = options.fetch ? options.fetch : rt::globalFetch();
    rt::RequestInit init;
    init.method = "GET";
    init.headers = displayHeaders(options);
    init.redirect = "manual";
    init.signal = signal;
    rt::Task<rt::Response> fetching = fetcher(rt::RequestInfo(endpoints.inventoryUrl), std::move(init));
    const rt::Response response = co_await std::move(fetching);
    if (!isHttpSuccess(response.status())) {
      discardResponseBody(response);
      throw CodexUsageResetProviderHttpError("inventory", response.status());
    }
    rt::Task<json::value> reading = response.json();
    const json::value payload = co_await std::move(reading);
    const json::value* availableCount = rt::get(payload, "available_count");
    if (!isRecord(&payload) || !isNonnegativeSafeInteger(availableCount)) {
      throw CodexUsageResetProviderConfigurationError("Reset-credit inventory response was invalid.");
    }
    const std::optional<std::vector<ResetInventoryCredit>> credits = parseDetailedCredits(rt::get(payload, "credits"), rt::toNumber(*availableCount));
    if (!credits) {
      throw CodexUsageResetProviderConfigurationError("Reset-credit inventory did not include a complete detailed available-credit list.");
    }
    std::vector<CodexResetCreditDiagnostic> diagnostics;
    for (const ResetInventoryCredit& credit : *credits) {
      if (credit.status == "available") diagnostics.push_back(CodexResetCreditDiagnostic{.status = credit.status, .expires_at_ms = credit.expiresAtMs});
    }
    rt::sort(diagnostics, [](const CodexResetCreditDiagnostic& left, const CodexResetCreditDiagnostic& right) {
      return left.expires_at_ms.value_or(rt::Number::POSITIVE_INFINITY) - right.expires_at_ms.value_or(rt::Number::POSITIVE_INFINITY);
    });
    co_return diagnostics;
  }

  rt::Task<double> readCodexResetAvailableCount(UpstreamCodexUsageResetProviderOptions options, rt::AbortSignal signal) {
    const CodexUsageResetCreditEndpoints endpoints = resolveCodexUsageResetCreditEndpoints(options.codexBaseUrl);
    const rt::Fetch fetcher = options.fetch ? options.fetch : rt::globalFetch();
    rt::RequestInit init;
    init.method = "GET";
    init.headers = displayHeaders(options);
    init.redirect = "manual";
    init.signal = signal;
    rt::Task<rt::Response> fetching = fetcher(rt::RequestInfo(endpoints.inventoryUrl), std::move(init));
    const rt::Response response = co_await std::move(fetching);
    if (!isHttpSuccess(response.status())) {
      discardResponseBody(response);
      throw CodexUsageResetProviderHttpError("inventory", response.status());
    }
    rt::Task<json::value> reading = response.json();
    const json::value payload = co_await std::move(reading);
    const json::value* availableCount = rt::get(payload, "available_count");
    if (!isRecord(&payload) || !isNonnegativeSafeInteger(availableCount)) {
      throw CodexUsageResetProviderConfigurationError("Reset-credit count was invalid.");
    }
    co_return rt::toNumber(*availableCount);
  }
}
