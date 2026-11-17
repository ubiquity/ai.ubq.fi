// Codex models fetching and snapshots, split out of src/codex.ts.
#include "uos/models/codex_models_fetch.hpp"

#include "uos/bounded_response_body.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/auth_refresh.hpp"
#include "uos/codex/dispatch.hpp"
#include "uos/kv.hpp"
#include "uos/models/codex_models_availability.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/runtime_config.hpp"
#include "uos/utils.hpp"

#include <array>
#include <exception>
#include <unordered_set>
#include <utility>

namespace uos::models::codex_models_fetch {
  namespace {
    using ::uos::codex::auth::CodexAuthAccountEntry;
    using ::uos::codex::auth::CodexAuthPoolEntry;
    using ::uos::codex::auth::CodexError;
    using ::uos::types::CodexAuthState;
    namespace auth = ::uos::codex::auth;
    namespace auth_refresh = ::uos::codex::auth_refresh;
    namespace dispatch = ::uos::codex::dispatch;

    constexpr double SINGLE_ACCOUNT_CATALOG_MAX_BYTES = 4 * 1024 * 1024;
    constexpr double SINGLE_ACCOUNT_CATALOG_TIMEOUT_MS = 1'000;

    // A caught value that is `null` (the initial `transportError`, or a thrown null).
    bool isNullReason(const rt::Reason& reason) {
      if (!reason) return true;
      const std::optional<json::value> thrown = rt::thrownValue(reason);
      return thrown.has_value() && thrown->is_null();
    }

    // `ifNoneMatch: string | null | undefined`; `signal: AbortSignal | undefined`.
    rt::Task<rt::Response> fetchCodexModelsForAccount(CodexAuthAccountEntry accountEntry, std::string url, std::string clientVersion,
                                                      std::optional<std::string> ifNoneMatch, std::optional<rt::AbortSignal> signal) {
      rt::Promise<CodexAuthState> validating(auth_refresh::getValidAuth(accountEntry));
      rt::Promise<CodexAuthState> awaitingValid(auth_refresh::awaitWithoutCancellingSharedWork(validating, signal));
      CodexAuthState current = co_await awaitingValid.wait();
      rt::Promise<rt::Response> fetching(dispatch::fetchCodexModelsWithAuth(current, url, clientVersion, ifNoneMatch, signal));
      rt::Response res = co_await fetching.wait();
      rt::Promise<void> recording(dispatch::recordCodexResponseHealth(current.account_id, res, current, std::string("reachable")));
      co_await recording.wait();
      if (res.status() == 401) {
        dispatch::cancelResponseBody(res);
        rt::Promise<CodexAuthAccountEntry> locating(auth_refresh::getCurrentAccountEntry(current.account_id, true));
        const CodexAuthAccountEntry currentEntry = co_await locating.wait();
        rt::Promise<CodexAuthState> refreshing(auth_refresh::refreshAuthCoordinated(currentEntry));
        rt::Promise<CodexAuthState> awaitingRefresh(auth_refresh::awaitWithoutCancellingSharedWork(refreshing, signal));
        current = co_await awaitingRefresh.wait();
        rt::Promise<rt::Response> refetching(dispatch::fetchCodexModelsWithAuth(current, url, clientVersion, ifNoneMatch, signal));
        res = co_await refetching.wait();
        rt::Promise<void> rerecording(dispatch::recordCodexResponseHealth(current.account_id, res, current, std::string("reachable")));
        co_await rerecording.wait();
      }
      co_return res;
    }

    /**
     * A transport failure is reported to the caller before provider health is
     * recorded, so a throwing callback can never suppress that bookkeeping.
     */
    void reportCodexModelsTransportFailure(const rt::Reason& error, const std::function<void()>& onProviderTransportFailure) {
      const CodexError* codexError = auth::codexErrorOf(error);
      if (!(codexError != nullptr && (codexError->code == "gateway_timeout" || codexError->code == "codex_upstream_unreachable"))) return;
      try {
        if (onProviderTransportFailure) onProviderTransportFailure();
      } catch (...) {
        rt::console::error("[ai.ubq.fi] Codex models transport failure callback failed:", std::current_exception());
      }
    }

    // `Readonly<{ kind: "response"; response: Response } | { kind: "next_url"; fallback: Response | null }>`
    struct CodexModelsAccountsOutcome {
      bool isResponse = false;
      std::optional<rt::Response> response{};
      std::optional<rt::Response> fallback{};
    };

    /**
     * A missing models endpoint on a multi-URL base list defers to the next
     * candidate URL instead of being reported as the account's answer.
     */
    bool codexModelsUrlNotFound(const rt::Response& res, bool hasFallbackUrl) { return (res.status() == 404 || res.status() == 400) && hasFallbackUrl; }

    /** A usable JSON catalog one account advertised, with its rows and slugs. */
    struct CodexAccountCatalogContribution {
      std::string accountId;
      json::object body;
      json::array models;
      std::vector<std::string> slugs;
    };

    std::optional<std::string> codexModelRowSlug(const json::value& value) {
      if (!::uos::utils::isRecord(value)) return std::nullopt;
      std::optional<std::string> slug = ::uos::utils::getString(rt::get(value, "slug"));
      if (!slug.has_value()) slug = ::uos::utils::getString(rt::get(value, "id"));
      if (!slug.has_value()) slug = ::uos::utils::getString(rt::get(value, "model"));
      if (!slug.has_value()) slug = ::uos::utils::getString(rt::get(value, "name"));
      if (!slug.has_value()) return std::nullopt;
      std::string trimmed = rt::trim(*slug);
      if (trimmed.empty()) return std::nullopt;
      return trimmed;
    }

    CodexAccountCatalogContribution contributionFromBody(const std::string& accountId, const json::object& body) {
      const json::array* rows = rt::getArray(body, "models");
      const json::array models = rows != nullptr ? *rows : json::array();
      std::vector<std::string> slugs;
      std::unordered_set<std::string> seen;
      for (const json::value& model : models) {
        const std::optional<std::string> slug = codexModelRowSlug(model);
        if (!slug.has_value() || seen.contains(*slug)) continue;
        seen.insert(*slug);
        slugs.push_back(*slug);
      }
      return CodexAccountCatalogContribution{.accountId = accountId, .body = body, .models = models, .slugs = slugs};
    }

    json::array slugArray(const std::vector<std::string>& slugs) {
      json::array array;
      for (const std::string& slug : slugs) array.push_back(json::value(slug));
      return array;
    }

    /** Record fresh single-account evidence without consuming or rewriting its upstream response. */
    rt::Task<void> recordSingleAccountCodexCatalog(rt::Response res, std::string accountId, std::string clientVersion) {
      if (!res.ok()) co_return;
      ::uos::bounded_response_body::ReadBoundedResponseBodyOptions options;
      options.maxBytes = SINGLE_ACCOUNT_CATALOG_MAX_BYTES;
      options.timeoutMs = SINGLE_ACCOUNT_CATALOG_TIMEOUT_MS;
      rt::Promise<::uos::bounded_response_body::BoundedResponseBody> reading(::uos::bounded_response_body::readBoundedResponseBody(res.clone(), std::move(options)));
      const ::uos::bounded_response_body::BoundedResponseBody observation = co_await reading.wait();
      if (!observation.complete) co_return;
      std::optional<json::value> body;
      try {
        body = rt::jsonParse(rt::decodeUtf8(observation.bytes));
      } catch (...) {
      }
      if (!body.has_value()) co_return;
      if (!::uos::utils::isRecord(*body) || rt::getArray(*body, "models") == nullptr) co_return;
      const CodexAccountCatalogContribution contribution = contributionFromBody(accountId, body->get_object());
      std::vector<codex_models_availability::CodexAccountCatalogInput> entries;
      entries.push_back(codex_models_availability::CodexAccountCatalogInput{.accountId = accountId, .clientVersion = clientVersion, .slugs = slugArray(contribution.slugs)});
      rt::Promise<void> recording(codex_models_availability::recordCodexAccountCatalogs(std::move(entries)));
      co_await recording.wait();
    }

    /**
     * Try every account for one models URL in order. A sibling account absorbs an
     * expired bearer or quota answer; a 404/400 on a multi-URL base list defers to
     * the next URL with the response kept for the final diagnostic. Any other
     * answer is final.
     */
    rt::Task<CodexModelsAccountsOutcome> fetchCodexModelsFromAccounts(std::string url, std::vector<CodexAuthAccountEntry> accountEntries, std::string clientVersion,
                                                                      bool hasFallbackUrl, FetchCodexModelsOptions options) {
      std::optional<rt::Response> fallback;
      for (std::size_t index = 0; index < accountEntries.size(); ++index) {
        const CodexAuthAccountEntry& accountEntry = accountEntries[index];
        const bool hasFallbackAccount = index + 1 < accountEntries.size();
        std::optional<rt::Response> res;
        rt::Reason failure;
        try {
          rt::Promise<rt::Response> fetching(fetchCodexModelsForAccount(accountEntry, url, clientVersion, options.ifNoneMatch, options.signal));
          res = co_await fetching.wait();
        } catch (...) {
          failure = std::current_exception();
        }
        if (!res.has_value()) {
          reportCodexModelsTransportFailure(failure, options.onProviderTransportFailure);
          rt::Promise<void> recording(dispatch::recordCodexThrownHealth(accountEntry.auth.account_id, failure));
          co_await recording.wait();
          if (!hasFallbackAccount) std::rethrow_exception(failure);
          continue;
        }
        if (hasFallbackAccount && (res->status() == 401 || res->status() == 429)) {
          dispatch::cancelResponseBody(*res);
          fallback = res;
          continue;
        }
        if (codexModelsUrlNotFound(*res, hasFallbackUrl)) {
          fallback = res;
          break;
        }
        rt::Promise<void> recording(recordSingleAccountCodexCatalog(*res, accountEntry.auth.account_id, clientVersion));
        co_await recording.wait();
        co_return CodexModelsAccountsOutcome{.isResponse = true, .response = res, .fallback = std::nullopt};
      }
      co_return CodexModelsAccountsOutcome{.isResponse = false, .response = std::nullopt, .fallback = fallback};
    }

    /**
     * The last failed attempt is the only diagnostic evidence left, so its bounded
     * snippet becomes the caller's error body.
     */
    rt::Task<ValidateCodexAuthJsonResultNotOk> codexModelsFailureResult(rt::Response fallback) {
      std::string text;
      try {
        rt::Task<std::string> reading = fallback.text();
        text = co_await std::move(reading);
      } catch (...) {
        text = "";
      }
      co_return ValidateCodexAuthJsonResultNotOk{.status = static_cast<double>(fallback.status()),
                                                 .body = rt::slice(!text.empty() ? text : fallback.statusText(), 0, 8'000)};
    }

    // `{ res: Response | null; auth: CodexAuthState; refreshed: boolean }`
    struct CodexModelsRefreshRetry {
      std::optional<rt::Response> res;
      CodexAuthState auth;
      bool refreshed;
    };

    /**
     * A stateless 401 retry keeps the original response *and* credentials whenever
     * the refresh or its retry fails, while a successful refresh is reported even
     * if the retry fetch itself throws.
     */
    rt::Task<CodexModelsRefreshRetry> retryCodexModelsAfterAuthRefresh(CodexAuthState auth, std::string url, std::string clientVersion) {
      try {
        rt::Promise<CodexAuthState> refreshing(auth_refresh::refreshAuthStateless(auth));
        const CodexAuthState next = co_await refreshing.wait();
        rt::Promise<rt::Response> fetching(dispatch::fetchCodexModelsWithAuth(next, url, clientVersion));
        const rt::Response res = co_await fetching.wait();
        co_return CodexModelsRefreshRetry{.res = res, .auth = next, .refreshed = true};
      } catch (...) {
        // Ignore and let the caller return the original 401 response.
      }
      co_return CodexModelsRefreshRetry{.res = std::nullopt, .auth = auth, .refreshed = false};
    }

    // `{ models: unknown; body: string }`
    struct CodexModelsResponseBody {
      json::value models;
      std::string body;
    };

    /**
     * Read a models body from a successful upstream response. A non-JSON body is
     * reported as `null` models with the raw text preserved for diagnostics; the
     * already-buffered stream is cancelled on a best-effort basis.
     */
    rt::Task<CodexModelsResponseBody> readCodexModelsResponseBody(rt::Response res, std::optional<std::string> contentType) {
      std::string text;
      try {
        rt::Task<std::string> reading = res.text();
        text = co_await std::move(reading);
      } catch (...) {
        text = "";
      }
      json::value models = nullptr;
      if (contentType.has_value() && rt::includes(std::string_view(*contentType), std::string_view("application/json")) && !text.empty()) {
        try {
          models = rt::jsonParse(text);
        } catch (...) {
          models = nullptr;
        }
      }
      try {
        const std::optional<rt::ByteStream> body = res.body();
        if (body.has_value()) {
          rt::Task<void> cancelling = body->cancel();
          co_await std::move(cancelling);
        } else {
          co_await rt::microtask();
        }
      } catch (...) {
        // The body is already buffered; cancellation is best effort.
      }
      co_return CodexModelsResponseBody{.models = models, .body = text};
    }

    /**
     * Read one successful models body as JSON. A body without a `models` array is
     * not a catalog, so it contributes nothing to the union instead of poisoning it.
     */
    rt::Task<std::optional<json::object>> readCodexModelsJsonBody(rt::Response res) {
      std::string text;
      try {
        rt::Task<std::string> reading = res.text();
        text = co_await std::move(reading);
      } catch (...) {
        text = "";
      }
      try {
        const std::optional<rt::ByteStream> body = res.body();
        if (body.has_value()) {
          rt::Task<void> cancelling = body->cancel();
          co_await std::move(cancelling);
        } else {
          co_await rt::microtask();
        }
      } catch (...) {
        // The body is already buffered; cancellation is best effort.
      }
      if (text.empty()) co_return std::nullopt;
      try {
        const json::value parsed = rt::jsonParse(text);
        if (::uos::utils::isRecord(parsed) && rt::getArray(parsed, "models") != nullptr) co_return parsed.get_object();
      } catch (...) {
      }
      co_return std::nullopt;
    }

    // `Readonly<{ catalog: CodexAccountCatalogContribution | null; fallback: Response | null }>`
    struct CodexAccountCatalogAttempt {
      std::optional<CodexAccountCatalogContribution> catalog;
      std::optional<rt::Response> fallback;
    };

    /**
     * Walk one account's candidate URLs with the existing 401-refresh, 429 and
     * 404/400 fallback semantics, but never with `If-None-Match`: a 304 from one
     * account cannot express another account's rows.
     */
    rt::Task<CodexAccountCatalogAttempt> fetchCodexModelsCatalogForAccount(CodexAuthAccountEntry accountEntry, std::vector<std::string> urls,
                                                                           std::string clientVersion, FetchCodexModelsOptions options) {
      std::optional<rt::Response> fallback;
      for (const std::string& url : urls) {
        std::optional<rt::Response> res;
        rt::Reason failure;
        try {
          rt::Promise<rt::Response> fetching(fetchCodexModelsForAccount(accountEntry, url, clientVersion, std::nullopt, options.signal));
          res = co_await fetching.wait();
        } catch (...) {
          failure = std::current_exception();
        }
        if (!res.has_value()) {
          reportCodexModelsTransportFailure(failure, options.onProviderTransportFailure);
          rt::Promise<void> recording(dispatch::recordCodexThrownHealth(accountEntry.auth.account_id, failure));
          co_await recording.wait();
          std::rethrow_exception(failure);
        }
        if (res->ok()) {
          rt::Promise<std::optional<json::object>> reading(readCodexModelsJsonBody(*res));
          const std::optional<json::object> body = co_await reading.wait();
          if (body.has_value()) {
            if (fallback.has_value()) dispatch::cancelResponseBody(*fallback);
            co_return CodexAccountCatalogAttempt{.catalog = contributionFromBody(accountEntry.auth.account_id, *body), .fallback = std::nullopt};
          }
          fallback = res;
          continue;
        }
        fallback = res;
        if (codexModelsUrlNotFound(*res, urls.size() > 1)) continue;
        break;
      }
      co_return CodexAccountCatalogAttempt{.catalog = std::nullopt, .fallback = fallback};
    }

    /**
     * Union every pool account's catalog in pool order. Rows deduplicate by slug and
     * the first-in-pool-order row is kept verbatim; the first contributing body
     * supplies the other top-level keys. When no account returns a usable catalog,
     * the last upstream failure is returned unchanged.
     */
    struct CodexModelsUnionCollection {
      std::vector<CodexAccountCatalogContribution> contributions;
      std::optional<rt::Response> fallback;
      // `transportError: unknown`, initially null (a null Reason).
      rt::Reason transportError;
    };

    /** Ask every pool account in pool order, keeping the last failure for the all-failed answer. */
    rt::Task<CodexModelsUnionCollection> collectCodexModelsUnion(CodexAuthPoolEntry poolEntry, std::vector<std::string> urls, std::string clientVersion,
                                                                 FetchCodexModelsOptions options) {
      std::vector<CodexAccountCatalogContribution> contributions;
      std::optional<rt::Response> fallback;
      rt::Reason transportError;
      for (const CodexAuthState& account : poolEntry.pool.accounts) {
        std::optional<CodexAccountCatalogAttempt> attempt;
        rt::Reason failure;
        try {
          rt::Promise<CodexAccountCatalogAttempt> fetching(
            fetchCodexModelsCatalogForAccount(CodexAuthAccountEntry{poolEntry, account, std::nullopt}, urls, clientVersion, options));
          attempt = co_await fetching.wait();
        } catch (...) {
          failure = std::current_exception();
        }
        if (!attempt.has_value()) {
          transportError = failure;
          continue;
        }
        if (attempt->catalog.has_value()) {
          contributions.push_back(*attempt->catalog);
          continue;
        }
        if (attempt->fallback.has_value()) {
          if (fallback.has_value()) dispatch::cancelResponseBody(*fallback);
          fallback = attempt->fallback;
        }
      }
      co_return CodexModelsUnionCollection{.contributions = std::move(contributions), .fallback = fallback, .transportError = transportError};
    }

    /** The unchanged upstream failure when no account produced a usable catalog. */
    rt::Response codexModelsUnionFailureResponse(const std::optional<rt::Response>& fallback, const rt::Reason& transportError) {
      if (fallback.has_value()) return *fallback;
      if (isNullReason(transportError)) {
        rt::ResponseInit init;
        init.status = 404;
        return rt::Response(rt::BodyInit(std::string("Codex upstream models endpoint not found.")), init);
      }
      if (rt::isError(transportError)) std::rethrow_exception(transportError);
      throw rt::withCause(rt::JsError("Codex models fetch failed with a non-Error value."), transportError);
    }

    /** Merge the contributing rows by slug, keeping the first-in-pool-order row verbatim. */
    rt::Response buildCodexModelsUnionResponse(const std::vector<CodexAccountCatalogContribution>& contributions) {
      json::array models;
      std::unordered_set<std::string> seen;
      for (const CodexAccountCatalogContribution& contribution : contributions) {
        for (const json::value& model : contribution.models) {
          const std::optional<std::string> slug = codexModelRowSlug(model);
          if (!slug.has_value() || seen.contains(*slug)) continue;
          seen.insert(*slug);
          models.push_back(model);
        }
      }
      json::object body = rt::objectSpread(contributions[0].body);
      rt::objectDefine(body, "models", json::value(std::move(models)));
      rt::ResponseInit init;
      init.status = 200;
      init.headers = rt::Headers{{"Content-Type", "application/json"}};
      return rt::Response(rt::BodyInit(rt::jsonStringify(body)), init);
    }

    rt::Task<rt::Response> fetchCodexModelsUnion(CodexAuthPoolEntry poolEntry, std::vector<std::string> urls, std::string clientVersion,
                                                 FetchCodexModelsOptions options) {
      rt::Promise<CodexModelsUnionCollection> collecting(collectCodexModelsUnion(poolEntry, urls, clientVersion, options));
      const CodexModelsUnionCollection collected = co_await collecting.wait();
      if (collected.contributions.empty()) co_return codexModelsUnionFailureResponse(collected.fallback, collected.transportError);

      std::vector<codex_models_availability::CodexAccountCatalogInput> entries;
      for (const CodexAccountCatalogContribution& contribution : collected.contributions) {
        entries.push_back(codex_models_availability::CodexAccountCatalogInput{
          .accountId = contribution.accountId, .clientVersion = clientVersion, .slugs = slugArray(contribution.slugs)});
      }
      rt::Promise<void> recording(codex_models_availability::recordCodexAccountCatalogs(std::move(entries)));
      co_await recording.wait();
      co_return buildCodexModelsUnionResponse(collected.contributions);
    }

    // `options.clientVersion?.trim() ?? null` validated: the requested version or CODEX_CLIENT_VERSION.
    std::string resolveClientVersion(const std::optional<std::string>& requested) {
      const std::optional<std::string> requestedVersion = requested.has_value() ? std::optional<std::string>(rt::trim(*requested)) : std::nullopt;
      return rt::truthy(requestedVersion) && codex_models::parseCodexClientVersion(*requestedVersion).has_value() ? *requestedVersion
                                                                                                            : std::string(auth::CODEX_CLIENT_VERSION);
    }
  }

  rt::Task<rt::Response> fetchCodexModels() { return fetchCodexModels(FetchCodexModelsOptions{}); }

  rt::Task<rt::Response> fetchCodexModels(FetchCodexModelsOptions options) {
    rt::Promise<CodexAuthPoolEntry> loading(auth::getAuthPoolEntry());
    const CodexAuthPoolEntry poolEntry = co_await loading.wait();
    const std::string clientVersion = resolveClientVersion(options.clientVersion);
    const std::vector<std::string> urls = dispatch::codexModelsBaseUrls(clientVersion);

    // One configured account keeps the historical single-account contract exactly,
    // conditional requests and 304 revalidation included. A pool asks every
    // account and unions their answers, because one account's catalog cannot
    // express what its siblings are entitled to serve.
    if (poolEntry.pool.accounts.size() <= 1) {
      const std::vector<CodexAuthAccountEntry> accountEntries = dispatch::randomizedAuthEntries(poolEntry);
      std::optional<rt::Response> lastResponse;

      for (const std::string& url : urls) {
        rt::Promise<CodexModelsAccountsOutcome> fetching(fetchCodexModelsFromAccounts(url, accountEntries, clientVersion, urls.size() > 1, options));
        const CodexModelsAccountsOutcome outcome = co_await fetching.wait();
        if (outcome.isResponse) co_return *outcome.response;
        if (outcome.fallback.has_value()) lastResponse = outcome.fallback;
      }

      if (lastResponse.has_value()) co_return *lastResponse;
      rt::ResponseInit init;
      init.status = 404;
      co_return rt::Response(rt::BodyInit(std::string("Codex upstream models endpoint not found.")), init);
    }

    rt::Promise<rt::Response> unioning(fetchCodexModelsUnion(poolEntry, urls, clientVersion, options));
    rt::Response response = co_await unioning.wait();
    co_return response;
  }

  rt::Task<std::optional<codex_models::CodexModelsSnapshot>> loadCodexModelsSnapshot() {
    rt::Promise<std::optional<runtime_config::RuntimeConfigV2>> loading(runtime_config::loadRuntimeConfig());
    const std::optional<runtime_config::RuntimeConfigV2> config = co_await loading.wait();
    if (!config.has_value()) co_return std::nullopt;
    co_return runtime_config::toJson(config->codex_models).as_object();
  }

  std::optional<std::string> preserveCodexDefaultModel(const codex_models::CodexModelsSnapshot& snapshot, const std::optional<std::string>& candidate) {
    const std::optional<std::string> target = candidate.has_value() ? std::optional<std::string>(rt::trim(*candidate)) : std::nullopt;
    if (!rt::truthy(target)) return std::nullopt;
    // `snapshot.models.some(...)` on a non-array throws JavaScriptCore's TypeError (Bun 1.4 text).
    const json::value* rawModels = rt::get(snapshot, "models");
    if (rawModels == nullptr) throw rt::TypeError("undefined is not an object (evaluating 'snapshot.models.some')");
    if (rawModels->is_null()) throw rt::TypeError("null is not an object (evaluating 'snapshot.models.some')");
    const json::array* models = rawModels->if_array();
    if (models == nullptr) {
      throw rt::TypeError(
        "snapshot.models.some is not a function. (In 'snapshot.models.some((model) => {\n    if (!isRecord(model))\n      return !1;\n    return (getString(model.slug) ?? getString(model.id) ?? getString(model.model) ?? getString(model.name))?.trim() === target;\n  })', 'snapshot.models.some' is undefined)");
    }
    bool found = false;
    for (const json::value& model : *models) {
      if (!::uos::utils::isRecord(model)) continue;
      std::optional<std::string> id = ::uos::utils::getString(rt::get(model, "slug"));
      if (!id.has_value()) id = ::uos::utils::getString(rt::get(model, "id"));
      if (!id.has_value()) id = ::uos::utils::getString(rt::get(model, "model"));
      if (!id.has_value()) id = ::uos::utils::getString(rt::get(model, "name"));
      if (id.has_value() && rt::trim(*id) == *target) {
        found = true;
        break;
      }
    }
    return found ? target : std::nullopt;
  }

  rt::Task<std::optional<codex_models::CodexModelsSnapshot>> loadFullCodexModelsSnapshot() { return loadFullCodexModelsSnapshot(std::nullopt); }

  namespace {
    // `typeof value === "object" && value !== null` over a stored value (array holes are skipped by `some`).
    bool isStoredRecord(const rt::KvValue& value) {
      return !value.isNullish() && !value.isBool() && !value.isNumber() && !value.isString() && !value.isBigInt();
    }
  }

  rt::Task<std::optional<codex_models::CodexModelsSnapshot>> loadFullCodexModelsSnapshot(std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    std::shared_ptr<rt::Kv> kv;
    if (!kvOverride.has_value()) {
      rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      kv = co_await opening.wait();
    } else {
      kv = *kvOverride;
    }
    if (!kv) co_return std::nullopt;
    // This optional snapshot may be unavailable while the runtime catalog still serves.
    rt::KvGetOptions strong;
    strong.consistency = rt::KvConsistencyLevel::strong;
    rt::Promise<rt::KvEntryMaybe<>> getting(kv->get(auth::CODEX_MODELS_KV_KEY, strong));
    const rt::Promise<std::optional<rt::KvEntryMaybe<>>> caught =
      getting.then([](const rt::KvEntryMaybe<>& entry) { return std::optional<rt::KvEntryMaybe<>>(entry); },
                   [](const rt::Reason&) { return std::optional<rt::KvEntryMaybe<>>(); });
    const std::optional<rt::KvEntryMaybe<>> entry = co_await caught.wait();
    if (!entry.has_value()) co_return std::nullopt;
    const rt::KvValue& snapshot = entry->value;
    if (!snapshot.truthy()) co_return std::nullopt;
    const rt::KvValue& models = snapshot["models"];
    if (!models.isArray() || models.asArray().empty()) co_return std::nullopt;
    for (const rt::KvValue& model : models.asArray()) {
      if (model.is<rt::KvHole>()) continue;
      if (!isStoredRecord(model)) co_return std::nullopt;
    }
    const rt::KvValue& source = snapshot["source"];
    if (!source.isString() || rt::trim(source.asString()).empty()) co_return std::nullopt;
    const rt::KvValue& updatedAtMs = snapshot["updated_at_ms"];
    if (!updatedAtMs.isNumber() || !rt::Number::isSafeInteger(updatedAtMs.asNumber()) || updatedAtMs.asNumber() <= 0) co_return std::nullopt;
    // The snapshot is returned as JSON: a stored value with parts JSON cannot hold counts as unavailable.
    std::optional<json::value> snapshotJson;
    try {
      snapshotJson = snapshot.toJson();
    } catch (const rt::TypeError&) {
    }
    if (!snapshotJson.has_value() || !snapshotJson->is_object()) co_return std::nullopt;
    co_return snapshotJson->get_object();
  }

  rt::Task<bool> storeCodexModelsSnapshot(codex_models::CodexModelsSnapshot snapshot) {
    rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
    const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
    if (!kv) co_return false;
    rt::KvGetOptions strong;
    strong.consistency = rt::KvConsistencyLevel::strong;
    for (int attempt = 0; attempt < 3; attempt += 1) {
      rt::Promise<rt::KvEntryMaybe<>> currentReading(kv->get(auth::CODEX_MODELS_KV_KEY, strong));
      rt::Promise<rt::KvEntryMaybe<>> runtimeReading(kv->get(runtime_config::RUNTIME_CONFIG_V2_KEY, strong));
      std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads{currentReading, runtimeReading};
      rt::Promise<std::vector<rt::KvEntryMaybe<>>> both = rt::all(std::move(reads));
      const std::vector<rt::KvEntryMaybe<>> entries = co_await both.wait();
      const rt::KvEntryMaybe<>& currentEntry = entries[0];
      const rt::KvEntryMaybe<>& runtimeEntry = entries[1];
      const std::optional<runtime_config::RuntimeConfigV2> currentRuntime = runtime_config::normalizeRuntimeConfig(runtimeEntry.value);
      const std::optional<codex_models::CodexModelsSnapshot> previous =
        currentEntry.value.isObject() ? std::optional<codex_models::CodexModelsSnapshot>(currentEntry.value.toJson().as_object()) : std::nullopt;
      const codex_models::CodexModelsSnapshot nextSnapshot = codex_models::mergeCodexModelPromptCacheCapabilities(snapshot, previous);
      runtime_config::BuildRuntimeConfigOptions buildOptions;
      buildOptions.defaultModel =
        preserveCodexDefaultModel(nextSnapshot, currentRuntime.has_value() ? std::optional<std::string>(currentRuntime->default_model) : std::nullopt);
      buildOptions.defaultReasoningEffort =
        currentRuntime.has_value() ? std::optional<std::string>(currentRuntime->default_reasoning_effort) : std::nullopt;
      const runtime_config::RuntimeConfigV2 runtimeConfig = runtime_config::buildRuntimeConfig(nextSnapshot, buildOptions);

      // Guard: refuse to overwrite a populated catalog with empty models.
      // A failed upstream /models probe must not nuke the serving catalog.
      const rt::KvValue& currentModels = currentEntry.value["models"];
      const json::array* nextModels = rt::getArray(nextSnapshot, "models");
      if (currentEntry.value.truthy() && currentModels.isArray() && !currentModels.asArray().empty() && (nextModels == nullptr || nextModels->empty())) {
        co_return false;
      }
      const rt::KvValue nextSnapshotValue(nextSnapshot);
      const rt::KvValue runtimeConfigValue(runtime_config::toJson(runtimeConfig));
      auto operation = kv->atomic();
      operation->check(currentEntry).check(runtimeEntry).set(auth::CODEX_MODELS_KV_KEY, nextSnapshotValue).set(runtime_config::RUNTIME_CONFIG_V2_KEY, runtimeConfigValue);
      rt::Task<rt::KvCommitResult> committing = operation->commit();
      const rt::KvCommitResult commit = co_await std::move(committing);
      if (!commit.ok) continue;
      runtime_config::cacheRuntimeConfig(runtimeConfig);
      co_return true;
    }
    co_return false;
  }

  namespace {
    /**
     * The item ids the gateway's own Chat-only Responses translators synthesize.
     *
     * The producer mints `responseId` as `resp_` plus up to 40 alphanumerics, so a
     * gateway item id is `<responseId>_<kind>_<indexes>` in the legacy shape or
     * `<kind>_<responseId>_<indexes>` once the kind carries its official prefix.
     * Only those exact producer shapes are recognized, so a genuine OpenAI id is
     * never rewritten. A replayed legacy id fails the upstream's prefix validation,
     * and a replayed synthetic reasoning item cannot be looked up under
     * `store: false`.
     */
    const std::array<rt::Regex, 2>& gatewayItemIds() {
      static const std::array<rt::Regex, 2> patterns{
        rt::Regex("^resp_[A-Za-z0-9]{1,40}_(?:rs|msg|fc|ctc)(?:_\\d+){1,2}$", ""),
        rt::Regex("^(?:rs|msg|fc|ctc)_resp_[A-Za-z0-9]{1,40}(?:_\\d+){1,2}$", ""),
      };
      return patterns;
    }

    bool isGatewaySyntheticItemId(const std::string& id) {
      for (const rt::Regex& pattern : gatewayItemIds())
        if (pattern.test(id)) return true;
      return false;
    }

    /** Only an encrypted payload lets a stateless upstream resolve a replayed reasoning item. */
    bool hasEncryptedReasoningReplay(const json::object& item) {
      const std::optional<std::string_view> encrypted = rt::getString(item, "encrypted_content");
      return encrypted.has_value() && rt::length(*encrypted) > 0;
    }

    /**
     * One replayed input item for the Codex seam, with gateway-local provenance
     * removed. A gateway-synthesized reasoning item carries no encrypted payload,
     * so a `store: false` upstream cannot resolve it and it is dropped. Every other
     * gateway-synthesized item keeps its content, order and `call_id` and loses
     * only the nonportable synthetic `id`; unrelated items pass through untouched.
     */
    // The item as JSON, or std::nullopt (null) when it is dropped.
    std::optional<json::value> replayItemForCodex(const ::uos::types::ResponseInputItem& item) {
      const json::object* value = std::get_if<json::object>(&item);
      // A typed message item carries no `id` member, so it passes through untouched.
      if (value == nullptr) return json::value_from(item);
      const std::optional<std::string> id = ::uos::utils::getString(rt::get(*value, "id"));
      if (!rt::truthy(id) || !isGatewaySyntheticItemId(*id)) return json::value(*value);
      if (rt::getString(*value, "type") == std::optional<std::string_view>("reasoning") && !hasEncryptedReasoningReplay(*value)) return std::nullopt;
      json::object copy = rt::objectSpread(*value);
      rt::objectDelete(copy, "id");
      return json::value(std::move(copy));
    }
  }

  json::object buildCodexRequest(const std::string& model, const std::vector<::uos::types::ResponseInputItem>& input) {
    return buildCodexRequest(model, input, BuildCodexRequestOptions{});
  }

  json::object buildCodexRequest(const std::string& model, const std::vector<::uos::types::ResponseInputItem>& input, const BuildCodexRequestOptions& options) {
    json::array items;
    for (const ::uos::types::ResponseInputItem& item : input) {
      std::optional<json::value> replayed = replayItemForCodex(item);
      if (replayed.has_value()) items.push_back(std::move(*replayed));
    }
    json::object body;
    body["model"] = model;
    body["input"] = std::move(items);
    body["store"] = false;
    body["stream"] = true;

    if (!(options.reasoning == rt::undefined)) body["reasoning"] = options.reasoning.isNull() ? json::value(nullptr) : json::value(*options.reasoning);
    if (!(options.instructions == rt::undefined)) body["instructions"] = options.instructions.isNull() ? json::value(nullptr) : json::value(*options.instructions);

    return body;
  }

  rt::Task<ValidateCodexAuthJsonResult> validateCodexAuthJson(::uos::types::CodexAuthState auth) {
    return validateCodexAuthJson(std::move(auth), ValidateCodexAuthJsonOptions{});
  }

  rt::Task<ValidateCodexAuthJsonResult> validateCodexAuthJson(::uos::types::CodexAuthState auth, ValidateCodexAuthJsonOptions options) {
    const std::string clientVersion = resolveClientVersion(options.clientVersion);
    const std::vector<std::string> urls = dispatch::codexModelsBaseUrls(clientVersion);
    bool refreshed = false;
    std::optional<rt::Response> lastResponse;

    for (const std::string& url : urls) {
      rt::Promise<rt::Response> fetching(dispatch::fetchCodexModelsWithAuth(auth, url, clientVersion));
      rt::Response res = co_await fetching.wait();
      if (res.status() == 401) {
        rt::Promise<CodexModelsRefreshRetry> retrying(retryCodexModelsAfterAuthRefresh(auth, url, clientVersion));
        const CodexModelsRefreshRetry retry = co_await retrying.wait();
        if (retry.res.has_value()) {
          res = *retry.res;
          auth = retry.auth;
        }
        if (retry.refreshed) refreshed = true;
      }
      if (codexModelsUrlNotFound(res, urls.size() > 1)) {
        lastResponse = res;
        continue;
      }

      const std::optional<std::string> contentType = res.headers().get("Content-Type");
      if (res.ok()) {
        rt::Promise<CodexModelsResponseBody> reading(readCodexModelsResponseBody(res, contentType));
        const CodexModelsResponseBody body = co_await reading.wait();
        ValidateCodexAuthJsonResultOk ok{.auth = auth,
                                         .refreshed = refreshed,
                                         .status = static_cast<double>(res.status()),
                                         .contentType = contentType,
                                         .models = body.models,
                                         .modelsBody = body.body,
                                         .etag = res.headers().get("ETag"),
                                         .clientVersion = clientVersion};
        co_return ok;
      }

      rt::Promise<ValidateCodexAuthJsonResultNotOk> failing(codexModelsFailureResult(res));
      ValidateCodexAuthJsonResultNotOk failure = co_await failing.wait();
      co_return failure;
    }

    rt::Response fallback;
    if (lastResponse.has_value()) {
      fallback = *lastResponse;
    } else {
      rt::ResponseInit init;
      init.status = 404;
      fallback = rt::Response(rt::BodyInit(std::string("Codex upstream models endpoint not found.")), init);
    }
    rt::Promise<ValidateCodexAuthJsonResultNotOk> failing(codexModelsFailureResult(fallback));
    ValidateCodexAuthJsonResultNotOk failure = co_await failing.wait();
    co_return failure;
  }
}
