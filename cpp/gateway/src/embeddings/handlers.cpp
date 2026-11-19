#include "uos/embeddings/handlers.hpp"

#include "uos/api_key_policy.hpp"
#include "uos/embeddings/ledger.hpp"
#include "uos/embeddings/voyage.hpp"
#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/openai.hpp"
#include "uos/request.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/utils.hpp"

#include <exception>
#include <utility>

namespace uos::embeddings::handlers {
  rt::Response withVoyageUpstreamHeader(rt::Response response) {
    auto headers = response.headers();
    headers.set("x-uos-upstream", "voyage");
    const rt::ResponseInit init{.status = response.status(), .statusText = response.statusText(), .headers = headers};
    return rt::Response(rt::bodyInit(response.body()), init);
  }
  namespace {
    using namespace ledger;
    using namespace voyage;
    using namespace openai_telemetry;
    template<class T> rt::Task<T> jsAwait(rt::Task<T> task) { return rt::Promise<T>(std::move(task)).wait(); }
    rt::Response serverError(double status, std::string message, std::string code_) {
      http::OpenaiErrorOptions options;
      options.type = "server_error";
      options.param = std::optional<std::string>();
      return http::openaiError(status, std::move(message), std::move(code_), options);
    }
    json::object recordView(const json::value& value) {
      if (value.is_object()) return value.get_object();
      json::object out;
      if (value.is_array()) for (std::size_t i = 0; i < value.get_array().size(); ++i)
        rt::objectDefine(out, rt::numberToString(static_cast<double>(i)), value.get_array()[i]);
      return out;
    }
    std::pair<std::optional<json::value>, std::optional<double>> errorFields(const rt::Reason& error) {
      try {
        if (error) std::rethrow_exception(error);
      } catch (const VoyageEmbeddingsError& e) { return {e.status, e.retry_after_ms};
      } catch (const upstream_wire::ErrorWithStatus& e) { return {e.status, std::nullopt};
      } catch (...) { }
      if (const auto value = rt::thrownValue(error)) {
        const auto status = rt::get(*value, "status");
        return {status ? std::optional<json::value>(*status) : std::nullopt, rt::getNumber(*value, "retry_after_ms")};
      }
      return {};
    }
    struct MissingItem { std::string hash; std::string text; std::vector<std::size_t> indices; };
    struct RequestState {
      rt::Request req;
      const UsageContext* usageContext;
      std::string requestId;
      double startedAtMs;
      rt::AbortSignal downstreamSignal;
      ParsedEmbeddingsRequest parsed;
      std::shared_ptr<rt::Kv> kv;
      std::vector<std::string> hashes;
      std::optional<EmbeddingsIdempotencyLease> idempotencyLease{};
      bool idempotencyDispatched = false;
      bool idempotencyHasConfirmedSuccess = false;
      double deadlineMs;
      std::string apiKey{};
      rt::OrderedMap<std::string, MissingItem> buckets{};
      std::vector<std::optional<std::vector<double>>> vectorsByIndex{};
      double voyageTotalTokens = 0;
      bool sawVoyageTokenUsage = false;
      std::vector<MissingItem> missing{};

      rt::Task<std::optional<rt::Response>> acquireIdempotencyLease() {
        const auto key = req.headers().get("Idempotency-Key");
        if (!key) co_return std::nullopt;
        if (key->empty() || static_cast<double>(rt::length(*key)) > EMBEDDINGS_IDEMPOTENCY_MAX_KEY_CHARS || hasAsciiControlCharacter(*key)) {
          http::OpenaiErrorOptions options;
          options.param = std::optional<std::string>();
          co_return http::openaiError(400, "Idempotency-Key must contain 1-" + rt::numberToString(EMBEDDINGS_IDEMPOTENCY_MAX_KEY_CHARS) +
            " non-control characters.", "invalid_request_error", options);
        }
        const auto principal = usageContext && !usageContext->idempotencyPrincipal.isNullish() ? rt::trim(*usageContext->idempotencyPrincipal) : "";
        if (!kv || principal.empty()) co_return embeddingsIdempotencyUnavailableResponse();
        auto fingerprintTask = buildEmbeddingsIdempotencyFingerprint(parsed.profile, hashes);
        const auto fingerprint = co_await jsAwait(std::move(fingerprintTask));
        AcquireEmbeddingsIdempotencyLeaseParams params{kv, principal, *key, fingerprint, requestId};
        auto task = acquireEmbeddingsIdempotencyLease(params);
        const auto acquired = co_await jsAwait(std::move(task));
        if (const auto replay = std::get_if<EmbeddingsIdempotencyAcquireResultReplay>(&acquired)) co_return replay->response;
        if (const auto error = std::get_if<EmbeddingsIdempotencyAcquireResultError>(&acquired)) co_return error->response;
        idempotencyLease = std::get<EmbeddingsIdempotencyAcquireResultAcquired>(acquired).lease;
        co_return std::nullopt;
      }
      rt::Task<rt::Response> releaseBeforeDispatch(rt::Response response) {
        if (!idempotencyLease) co_return response;
        auto task = releaseEmbeddingsIdempotencyReservation(*idempotencyLease, false);
        const bool released = co_await jsAwait(std::move(task));
        co_return released ? response : embeddingsIdempotencyUnavailableResponse();
      }
      rt::Task<rt::Response> failIndeterminate() {
        if (idempotencyLease) {
          auto task = markEmbeddingsIdempotencyIndeterminate(*idempotencyLease);
          co_await jsAwait(std::move(task));
        }
        co_return embeddingsIdempotencyIndeterminateResponse();
      }
      rt::Task<rt::Response> releaseAfterExplicitUpstreamFailure(rt::Response response) {
        if (!idempotencyLease) co_return response;
        if (idempotencyHasConfirmedSuccess) { auto result = co_await jsAwait(failIndeterminate()); co_return result; }
        auto task = releaseEmbeddingsIdempotencyReservation(*idempotencyLease, true);
        const bool released = co_await jsAwait(std::move(task));
        if (released) co_return response;
        auto markTask = markEmbeddingsIdempotencyIndeterminate(*idempotencyLease);
        co_await jsAwait(std::move(markTask));
        co_return embeddingsIdempotencyIndeterminateResponse();
      }
      rt::Task<rt::Response> cancelledResponse() {
        recordStreamTerminalType(usageContext, "cancelled");
        co_await recordErrorUsage(usageContext).wait();
        if (idempotencyLease && idempotencyDispatched) { auto result = co_await jsAwait(failIndeterminate()); co_return result; }
        auto response = serverError(499, "Request was cancelled.", "request_cancelled");
        const auto result = co_await jsAwait(releaseBeforeDispatch(response));
        co_return result;
      }
      rt::Task<void> loadCachedVectorsAndMissing() {
        if (!kv) {
          for (const auto& [hash, bucket] : buckets) { (void)hash; missing.push_back(bucket); }
          co_return;
        }
        const auto unique = rt::arrayFrom(buckets.values());
        std::vector<rt::Promise<rt::KvEntryMaybe<>>> pending;
        for (const auto& item : unique) {
          auto read = kv->get(embeddingsCacheKey(parsed.profile.cache_profile_key, item.hash));
          pending.emplace_back(rt::detail::keepAlive(kv, std::move(read)));
        }
        auto reads = rt::all(std::move(pending));
        const auto entries = co_await reads.wait();
        for (std::size_t i = 0; i < unique.size(); ++i) {
          const auto& cached = entries[i].value["embedding"];
          if (isValidEmbeddingVector(cached, parsed.profile.dimensions)) {
            std::vector<double> vector;
            for (const auto& value : cached.as<rt::KvArray>()) vector.push_back(value.asNumber());
            for (auto idx : unique[i].indices) vectorsByIndex[idx] = vector;
          } else missing.push_back(unique[i]);
        }
      }
      rt::Task<rt::Response> releasedFailureResponse(rt::Response response) {
        co_await recordErrorUsage(usageContext).wait();
        if (idempotencyLease && idempotencyDispatched) { auto result = co_await jsAwait(failIndeterminate()); co_return result; }
        auto result = co_await jsAwait(releaseBeforeDispatch(response));
        co_return result;
      }
      rt::Task<rt::Response> unreleasedFailureResponse(rt::Response response) {
        co_await recordErrorUsage(usageContext).wait();
        if (idempotencyLease && idempotencyDispatched) { auto result = co_await jsAwait(failIndeterminate()); co_return result; }
        co_return response;
      }
      rt::Task<rt::Response> rateLimitResponse(double waitMs) {
        const double retryAfterSeconds = rt::Math::max(1.0, rt::Math::ceil(waitMs / 1000));
        const json::object error{{"message", "Rate limit exceeded; retry after ~" + rt::numberToString(retryAfterSeconds) + "s"},
          {"type", "rate_limit_error"}, {"code", "rate_limit_exceeded"}, {"param", nullptr}};
        const auto response = http::json(429, json::object{{"error", error}}, rt::Headers{{"Retry-After", rt::numberToString(retryAfterSeconds)}});
        auto result = co_await jsAwait(releasedFailureResponse(response));
        co_return result;
      }
      rt::Task<std::optional<rt::Response>> ensureIdempotencyDispatched() {
        if (!idempotencyLease || idempotencyDispatched) co_return std::nullopt;
        auto task = markEmbeddingsIdempotencyDispatched(*idempotencyLease);
        const bool marked = co_await jsAwait(std::move(task));
        if (!marked) { co_await recordErrorUsage(usageContext).wait(); co_return embeddingsIdempotencyUnavailableResponse(); }
        idempotencyDispatched = true;
        co_return std::nullopt;
      }
      rt::Task<rt::Response> handleQuotaDispatchFailure(api_key_policy::ApiKeyQuotaDispatchError error) {
        co_await recordErrorUsage(usageContext).wait();
        if (idempotencyLease) {
          auto task = releaseEmbeddingsIdempotencyReservation(*idempotencyLease, idempotencyDispatched);
          const bool released = co_await jsAwait(std::move(task));
          if (!released) co_return embeddingsIdempotencyUnavailableResponse();
          idempotencyDispatched = false;
        }
        co_return upstream_wire::apiKeyQuotaDispatchErrorResponse(error);
      }
      rt::Task<rt::Response> upstreamFailureResponse(double status, std::string message, double waitMs) {
        rt::Response response;
        if (status == 429) {
          const double seconds = rt::Math::max(1.0, rt::Math::ceil(waitMs / 1000));
          const json::object error{{"message", message}, {"type", "rate_limit_error"}, {"code", "rate_limit_exceeded"}, {"param", nullptr}};
          response = http::json(429, json::object{{"error", error}}, rt::Headers{{"Retry-After", rt::numberToString(seconds)}});
        } else response = serverError(502, message, "upstream_error");
        auto result = co_await jsAwait(releaseAfterExplicitUpstreamFailure(response));
        co_return result;
      }
      rt::Task<std::optional<rt::Response>> resolveUpstreamFailure(rt::Reason error, int attempt, double backoffMs) {
        if (downstreamSignal.aborted()) { auto result = co_await jsAwait(cancelledResponse()); co_return result; }
        std::optional<api_key_policy::ApiKeyQuotaDispatchError> quotaError;
        try { if (error) std::rethrow_exception(error); } catch (const api_key_policy::ApiKeyQuotaDispatchError& e) { quotaError.emplace(e); } catch (...) { }
        if (quotaError) { auto task = handleQuotaDispatchFailure(*quotaError); auto result = co_await jsAwait(std::move(task)); co_return result; }
        const auto fields = errorFields(error);
        const auto& [status, retryAfterMs] = fields;
        const bool hasStatus = status && rt::truthy(*status);
        const bool retryable = status && rt::isNumber(*status) && EMBEDDINGS_RETRYABLE_UPSTREAM_STATUSES.has(rt::toNumber(*status));
        const auto snippet = upstream_wire::formatErrorSnippet(error);
        const auto message = snippet.empty() ? "Embeddings upstream request failed." : "Embeddings upstream request failed: " + snippet;
        if (!hasStatus || !retryable) {
          upstream_wire::logRedactedUpstreamError("[ai.ubq.fi] embeddings request_id=" + requestId + " upstream_error:", error);
          co_await recordErrorUsage(usageContext).wait();
          if (!hasStatus) { auto result = co_await jsAwait(failIndeterminate()); co_return result; }
          auto response = serverError(502, message, "upstream_error");
          auto result = co_await jsAwait(releaseAfterExplicitUpstreamFailure(response));
          co_return result;
        }
        const double waitMs = rt::Math::max(0.0, retryAfterMs.value_or(backoffMs));
        if (attempt >= 2) {
          upstream_wire::logRedactedUpstreamError("[ai.ubq.fi] embeddings request_id=" + requestId + " upstream_error:", error);
          co_await recordErrorUsage(usageContext).wait();
          auto result = co_await jsAwait(upstreamFailureResponse(rt::toNumber(*status), message, waitMs));
          co_return result;
        }
        if (rt::dateNow() + waitMs >= deadlineMs) {
          co_await recordErrorUsage(usageContext).wait();
          auto result = co_await jsAwait(upstreamFailureResponse(rt::toNumber(*status), message, waitMs));
          co_return result;
        }
        auto pending = sleepUnlessAborted(waitMs, downstreamSignal);
        const bool slept = co_await pending.wait();
        if (!slept) { auto result = co_await jsAwait(cancelledResponse()); co_return result; }
        co_return std::nullopt;
      }
      void recordChunkUpstreamSuccess(const std::optional<double>& totalTokens) {
        if (idempotencyLease) idempotencyHasConfirmedSuccess = true;
        if (totalTokens) { sawVoyageTokenUsage = true; voyageTotalTokens += *totalTokens; }
      }
      rt::Task<std::variant<VoyageEmbeddingsResult, rt::Response>> fetchChunkVectors(std::vector<std::string> texts) {
        int attempt = 0;
        double backoffMs = 250;
        for (;;) {
          if (downstreamSignal.aborted()) { auto result = co_await jsAwait(cancelledResponse()); co_return result; }
          const auto dispatchFailure = co_await jsAwait(ensureIdempotencyDispatched());
          if (dispatchFailure) co_return *dispatchFailure;
          rt::Reason error;
          try {
            FetchVoyageEmbeddingsParams params{apiKey, parsed.profile.upstream_model, texts, parsed.profile.input_type, parsed.profile.dimensions,
              parsed.profile.output_dtype, parsed.profile.truncation, deadlineMs, downstreamSignal, usageContext ? usageContext->beforeProviderDispatch : nullptr};
            auto task = fetchVoyageEmbeddings(params);
            const auto upstream = co_await jsAwait(std::move(task));
            recordChunkUpstreamSuccess(upstream.totalTokens);
            co_return upstream;
          } catch (...) { error = std::current_exception(); }
          const auto failure = co_await jsAwait(resolveUpstreamFailure(error, attempt, backoffMs));
          if (failure) co_return *failure;
          backoffMs = rt::Math::min(2000.0, backoffMs * 2);
          ++attempt;
        }
      }
      rt::Task<void> writeChunkCacheEntries(std::vector<MissingItem> items, std::vector<std::vector<double>> vectors) {
        for (std::size_t i = 0; i < items.size(); ++i) {
          for (auto idx : items[i].indices) vectorsByIndex[idx] = vectors[i];
          if (kv) {
            auto task = writeEmbeddingsCacheEntryBestEffort(kv, parsed.profile.cache_profile_key, items[i].hash, vectors[i], rt::dateNow(), deadlineMs);
            const auto ignored = co_await jsAwait(std::move(task));
            (void)ignored;
          }
        }
      }
      rt::Task<std::optional<rt::Response>> processChunk(std::vector<MissingItem> items) {
        std::vector<std::string> texts;
        for (const auto& item : items) texts.push_back(item.text);
        if (kv) {
          const auto reserved = co_await jsAwait(applyVoyageRateLimit(*kv, estimateTokenCount(texts), deadlineMs));
          if (!reserved.ok) { auto result = co_await jsAwait(rateLimitResponse(reserved.wait_ms.value_or(0))); co_return result; }
        }
        auto fetchTask = fetchChunkVectors(texts);
        const auto fetched = co_await jsAwait(std::move(fetchTask));
        if (const auto response = std::get_if<rt::Response>(&fetched)) co_return *response;
        const auto& vectors = std::get<VoyageEmbeddingsResult>(fetched).vectors;
        if (vectors.size() != items.size()) {
          auto response = serverError(502, "Embeddings upstream returned a size mismatch.", "upstream_error");
          auto result = co_await jsAwait(unreleasedFailureResponse(response)); co_return result;
        }
        for (const auto& vector : vectors) if (static_cast<double>(vector.size()) != parsed.profile.dimensions) {
          auto response = serverError(502, "Embeddings upstream returned vector length " + rt::numberToString(static_cast<double>(vector.size())) +
            "; expected " + rt::numberToString(parsed.profile.dimensions) + ".", "upstream_dimension_mismatch");
          auto result = co_await jsAwait(unreleasedFailureResponse(response)); co_return result;
        }
        auto writeTask = writeChunkCacheEntries(items, vectors);
        co_await jsAwait(std::move(writeTask));
        co_return std::nullopt;
      }
      rt::Task<std::optional<rt::Response>> fillMissingVectors() {
        std::vector<EmbeddingsChunkItem> items;
        for (const auto& item : missing) items.push_back({item.hash, item.text});
        const auto chunks = chunkByTokenBudget(items, EMBEDDINGS_MAX_INPUTS_PER_REQUEST, VOYAGE_RATE_LIMIT_TPM);
        std::size_t offset = 0;
        for (const auto& chunk : chunks) {
          if (rt::dateNow() >= deadlineMs) {
            auto response = serverError(502, "Embeddings request timed out.", "timeout");
            auto result = co_await jsAwait(releasedFailureResponse(response)); co_return result;
          }
          std::vector<MissingItem> chunkItems(missing.begin() + static_cast<std::ptrdiff_t>(offset), missing.begin() + static_cast<std::ptrdiff_t>(offset + chunk.size()));
          offset += chunk.size();
          auto task = processChunk(chunkItems);
          const auto failure = co_await jsAwait(std::move(task));
          if (failure) co_return *failure;
        }
        co_return std::nullopt;
      }
      rt::Task<std::optional<rt::Response>> storeSuccessOrFailure(rt::Response response) {
        if (!idempotencyLease) co_return std::nullopt;
        auto task = storeEmbeddingsIdempotencySuccess(*idempotencyLease, response);
        const bool stored = co_await jsAwait(std::move(task));
        if (stored) co_return std::nullopt;
        if (idempotencyDispatched) { auto result = co_await jsAwait(failIndeterminate()); co_return result; }
        auto failure = embeddingsIdempotencyUnavailableResponse();
        auto result = co_await jsAwait(releaseBeforeDispatch(failure)); co_return result;
      }
      rt::Task<rt::Response> completeEmbeddingsResponse() {
        json::array data;
        for (std::size_t i = 0; i < vectorsByIndex.size(); ++i) {
          const auto& vec = vectorsByIndex[i];
          if (!vec) {
            auto response = serverError(502, "Embeddings gateway failed to construct a complete response.", "server_error");
            auto result = co_await jsAwait(unreleasedFailureResponse(response)); co_return result;
          }
          const json::value embedding = parsed.profile.encoding_format == "base64" ? json::value(floatEmbeddingToBase64(*vec)) : json::value_from(*vec);
          data.push_back(json::object{{"object", "embedding"}, {"index", rt::makeNumber(static_cast<double>(i))}, {"embedding", embedding}});
        }
        std::optional<UsageTokens> usageTokens;
        if (sawVoyageTokenUsage) usageTokens = UsageTokens{voyageTotalTokens, std::nullopt, std::nullopt, 0, voyageTotalTokens, "reported"};
        const json::object body{{"object", "list"}, {"data", data}, {"model", parsed.model},
          {"usage", json::object{{"prompt_tokens", rt::makeNumber(usageTokens ? usageTokens->inputTokens.value_or(0) : 0)},
                                {"total_tokens", rt::makeNumber(usageTokens ? usageTokens->totalTokens.value_or(0) : 0)}}}};
        const auto response = http::json(200, body);
        const auto storeFailure = co_await jsAwait(storeSuccessOrFailure(response));
        if (storeFailure) co_return *storeFailure;
        co_await recordCompletionUsage(usageContext, usageTokens).wait();
        co_return response;
      }
    };
    rt::Task<rt::Response> handleEmbeddingsRequest(rt::Request req, const UsageContext* context, HandleUosEmbeddingsOptions options) {
      const std::string requestId = rt::crypto::randomUUID();
      const double startedAtMs = rt::dateNow();
      const auto downstreamSignal = openai::downstreamSignalFor(req, context);
      const auto rawBody = co_await jsAwait(request::readJsonBody(req));
      if (!rt::truthy(rawBody) || !utils::isRecord(rawBody)) co_return http::openaiError(400, "Invalid JSON body", "invalid_request_error");
      const auto parsed = parseUosEmbeddingsRequest(recordView(rawBody));
      if (const auto failure = std::get_if<EmbeddingsParseResultNotOk>(&parsed)) co_return failure->response;
      std::shared_ptr<rt::Kv> store;
      if (options.kv) store = *options.kv;
      else { auto pending = kv::getKv(); store = co_await pending.wait(); }
      std::vector<rt::Promise<std::string>> pending;
      for (const auto& input : std::get<EmbeddingsParseResultOk>(parsed).value.inputs) pending.emplace_back(utils::sha256Hex(input));
      const auto hashes = co_await rt::all(std::move(pending)).wait();
      RequestState state{req, context, requestId, startedAtMs, downstreamSignal, std::get<EmbeddingsParseResultOk>(parsed).value, store, hashes,
        std::nullopt, false, false, startedAtMs + EMBEDDINGS_TIMEOUT_MS};
      const auto leaseFailure = co_await jsAwait(state.acquireIdempotencyLease());
      if (leaseFailure) co_return *leaseFailure;
      const RecordRequestUsageDetails details{state.parsed.model, "embeddings", false, std::nullopt};
      co_await recordRequestUsage(context, details).wait();
      const auto apiKey = co_await jsAwait(readVoyageApiKey(store));
      if (!apiKey || apiKey->empty()) {
        co_await recordErrorUsage(context).wait();
        auto response = serverError(503, "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)", "server_error");
        auto result = co_await jsAwait(state.releaseBeforeDispatch(response)); co_return result;
      }
      state.apiKey = *apiKey;
      for (std::size_t i = 0; i < state.parsed.inputs.size(); ++i) {
        if (auto existing = state.buckets.get(hashes[i])) existing->indices.push_back(i);
        else state.buckets.set(hashes[i], MissingItem{hashes[i], state.parsed.inputs[i], {i}});
      }
      state.vectorsByIndex.resize(state.parsed.inputs.size());
      co_await jsAwait(state.loadCachedVectorsAndMissing());
      if (!state.missing.empty()) {
        const auto failure = co_await jsAwait(state.fillMissingVectors());
        if (failure) co_return *failure;
      }
      auto response = co_await jsAwait(state.completeEmbeddingsResponse());
      co_return response;
    }
    rt::Task<rt::Response> runTelemetry(rt::Request req, UsageContext context, HandleUosEmbeddingsOptions options) {
      auto task = handleEmbeddingsRequest(req, &context, options);
      const auto response = co_await jsAwait(std::move(task));
      co_return withVoyageUpstreamHeader(response);
    }
  }
  rt::Task<rt::Response> handleUosEmbeddings(rt::Request req, const UsageContext* usageContext) {
    HandleUosEmbeddingsOptions options;
    return handleUosEmbeddings(req, usageContext, options);
  }
  rt::Task<rt::Response> handleUosEmbeddings(rt::Request req, const UsageContext* usageContext, HandleUosEmbeddingsOptions options) {
    const std::optional<UsageContext> context = usageContext ? std::optional<UsageContext>(*usageContext) : std::nullopt;
    const std::function<rt::Task<rt::Response>(UsageContext)> run = [req, options](UsageContext next) { return runTelemetry(req, next, options); };
    auto task = runWithResponseTelemetry(context, run);
    const auto response = co_await jsAwait(std::move(task));
    co_return response;
  }
}
