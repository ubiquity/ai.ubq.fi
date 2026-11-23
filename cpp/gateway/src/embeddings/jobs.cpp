#include "uos/embeddings/jobs.hpp"

#include "uos/api_key_policy.hpp"
#include "uos/embeddings/handlers.hpp"
#include "uos/embeddings/voyage.hpp"
#include "uos/http.hpp"
#include "uos/kv.hpp"
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

namespace uos::embeddings::jobs {
  namespace {
    using namespace ledger;
    using namespace voyage;
    using namespace openai_telemetry;
    template<class T> rt::Task<T> jsAwait(rt::Task<T> task) { return rt::Promise<T>(std::move(task)).wait(); }
    json::object buildEmbeddingsJobBody(const EmbeddingsJobRecord& job, const std::optional<json::object>& result) {
      json::object body{{"id", job.id}, {"object", "embeddings.job"}, {"status", job.status},
        {"created_at_ms", rt::makeNumber(job.created_at_ms)}, {"updated_at_ms", rt::makeNumber(job.updated_at_ms)}, {"model", job.model},
        {"upstream", job.upstream}, {"upstream_model", job.upstream_model}, {"input_type", job.input_type}, {"dimensions", rt::makeNumber(job.dimensions)},
        {"output_dtype", job.output_dtype}, {"encoding_format", job.encoding_format}, {"truncation", job.truncation},
        {"input_count", rt::makeNumber(job.input_count)}, {"total_chars", rt::makeNumber(job.total_chars)}};
      rt::objectDefine(body, "retry_after_seconds", rt::orNull(job.retry_after_seconds));
      rt::objectDefine(body, "error", job.error ? json::value_from(*job.error) : json::value(nullptr));
      rt::objectDefine(body, "result", result ? json::value(*result) : json::value(nullptr));
      return body;
    }
    rt::Task<std::optional<rt::KvEntryMaybe<>>> cacheEntry(std::shared_ptr<rt::Kv> kv, rt::KvKey key) {
      rt::Reason error;
      std::optional<rt::KvEntryMaybe<>> entry;
      try { entry = co_await jsAwait(kv->get(key)); } catch (...) { error = std::current_exception(); }
      // The fulfilled/rejected get is observed through the source catch reaction.
      (void)error;
      co_return entry;
    }
    rt::Task<std::vector<std::optional<std::vector<double>>>> loadEmbeddingsVectorsFromCache(std::shared_ptr<rt::Kv> kv,
      std::string cacheProfileKey, std::vector<std::string> hashesByIndex, VoyageEmbeddingsDimension dimensions) {
      rt::OrderedSet<std::string> uniqueHashes(hashesByIndex.begin(), hashesByIndex.end());
      std::vector<rt::Promise<std::optional<rt::KvEntryMaybe<>>>> pending;
      for (const auto& hash : uniqueHashes) {
        auto task = cacheEntry(kv, embeddingsCacheKey(cacheProfileKey, hash));
        pending.emplace_back(std::move(task));
      }
      const auto entries = co_await rt::all(std::move(pending)).wait();
      rt::OrderedMap<std::string, std::vector<double>> vectorsByHash;
      std::size_t i = 0;
      for (const auto& hash : uniqueHashes) {
        if (entries[i] && isValidEmbeddingVector(entries[i]->value["embedding"], dimensions)) {
          std::vector<double> vec;
          for (const auto& value : entries[i]->value["embedding"].as<rt::KvArray>()) vec.push_back(value.asNumber());
          vectorsByHash.set(hash, vec);
        }
        ++i;
      }
      std::vector<std::optional<std::vector<double>>> out;
      for (const auto& hash : hashesByIndex) {
        const auto vector = vectorsByHash.get(hash);
        out.push_back(vector ? std::optional<std::vector<double>>(*vector) : std::nullopt);
      }
      co_return out;
    }
    json::object buildOpenAiEmbeddingsResult(const std::string& model, const std::vector<std::optional<std::vector<double>>>& vectorsByIndex,
                                            double usageTotalTokens, const EmbeddingsEncodingFormat& encodingFormat) {
      json::array data;
      for (std::size_t i = 0; i < vectorsByIndex.size(); ++i) {
        const auto& vec = vectorsByIndex[i];
        const json::value embedding = vec && encodingFormat == "base64" ? json::value(floatEmbeddingToBase64(*vec)) : json::value_from(vec.value_or(std::vector<double>()));
        data.push_back(json::object{{"object", "embedding"}, {"index", rt::makeNumber(static_cast<double>(i))}, {"embedding", embedding}});
      }
      return json::object{{"object", "list"}, {"data", data}, {"model", model},
        {"usage", json::object{{"prompt_tokens", rt::makeNumber(usageTotalTokens)}, {"total_tokens", rt::makeNumber(usageTotalTokens)}}}};
    }
    rt::Task<VoyageBudgetResult> reserveVoyageBudgetForJob(rt::Kv& kv, double tokens) {
      for (int attempt = 0; attempt < 6; ++attempt) {
        const auto reserved = co_await jsAwait(tryReserveVoyageBudget(kv, tokens));
        if (reserved.ok || reserved.wait_ms.value_or(0) > 0) co_return reserved;
        auto pending = sleep(5 + attempt * 5);
        co_await pending.wait();
      }
      co_return VoyageBudgetResult{false, 1000};
    }
    rt::Task<void> updateEmbeddingsJobRecord(std::shared_ptr<rt::Kv> kv, rt::KvKey jobKey, rt::KvKey lookupKey,
                                           EmbeddingsJobRecord job, rt::KvValue raw) {
      auto operation = kv->atomic();
      const rt::KvSetOptions options{.expireIn = EMBEDDINGS_JOB_TTL_MS};
      const EmbeddingsJobLookupRecord lookup{job.cache_profile_key};
      operation->set(jobKey, raw, options).set(lookupKey, rt::KvValue(json::value_from(lookup)), options);
      const auto ignored = co_await jsAwait(operation->commit());
      (void)ignored;
    }
    rt::Task<void> deleteEmbeddingsJobInputs(std::shared_ptr<rt::Kv> kv, std::string tokenHash, std::string cacheProfileKey,
                                           std::string jobId, std::vector<std::string> uniqueHashes) {
      std::vector<rt::Promise<void>> pending;
      for (const auto& hash : uniqueHashes) pending.emplace_back(kv->delete_(embeddingsJobInputKey(tokenHash, cacheProfileKey, jobId, hash)));
      const auto ignored = co_await rt::allSettled(std::move(pending)).wait();
      (void)ignored;
    }
    rt::KvValue patchJob(rt::KvValue raw, const json::object& patch) {
      if (!raw.isObject()) raw = rt::KvValue(rt::KvObject());
      for (const auto& member : patch) raw.asObject().set(std::string(member.key()), rt::KvValue(member.value()));
      return raw;
    }
    rt::Response jobResponse(double status, const EmbeddingsJobRecord& job, const std::optional<json::object>& result,
                             std::optional<double> retry = std::nullopt) {
      rt::Headers headers;
      if (retry) headers.set("Retry-After", rt::numberToString(*retry));
      headers.set("x-uos-upstream", job.upstream);
      return http::json(status, buildEmbeddingsJobBody(job, result), headers);
    }
    rt::Response serverError(double status, std::string message, std::string code_, const std::string& type = "server_error") {
      http::OpenaiErrorOptions options;
      options.type = type;
      options.param = std::optional<std::string>();
      return http::openaiError(status, std::move(message), std::move(code_), options);
    }
    struct JobState {
      RunEmbeddingsJobAttemptParams params;
      EmbeddingsJobRecord currentJob;
      rt::KvValue currentRaw;
      std::vector<std::string> uniqueHashes;
      std::optional<double> queueRetryAfterMs{};
      std::optional<std::string> queueFailureKind{};

      rt::Task<std::vector<std::string>> computeMissing() {
        std::vector<rt::Promise<std::optional<rt::KvEntryMaybe<>>>> pending;
        for (const auto& hash : uniqueHashes) {
          auto task = cacheEntry(params.kv, embeddingsCacheKey(currentJob.cache_profile_key, hash));
          pending.emplace_back(std::move(task));
        }
        const auto entries = co_await rt::all(std::move(pending)).wait();
        std::vector<std::string> missing;
        for (std::size_t i = 0; i < uniqueHashes.size(); ++i)
          if (!entries[i] || !isValidEmbeddingVector(entries[i]->value["embedding"], currentJob.dimensions)) missing.push_back(uniqueHashes[i]);
        co_return missing;
      }
      rt::Task<void> update() {
        auto task = updateEmbeddingsJobRecord(params.kv, params.jobKey, params.jobLookupKey, currentJob, currentRaw);
        co_await jsAwait(std::move(task));
      }
      rt::Task<void> deleteInputs() {
        auto task = deleteEmbeddingsJobInputs(params.kv, params.tokenHash, currentJob.cache_profile_key, currentJob.id, uniqueHashes);
        co_await jsAwait(std::move(task));
      }
      rt::Task<rt::Response> failJob(std::string message, std::string code_) {
        auto failed = currentJob;
        failed.status = "failed";
        failed.updated_at_ms = rt::dateNow();
        failed.locked_until_ms = std::nullopt;
        failed.retry_after_seconds = std::nullopt;
        failed.error = EmbeddingsJobRecord::Error{message, "server_error", code_};
        const json::object patch{{"status", "failed"}, {"updated_at_ms", rt::makeNumber(failed.updated_at_ms)}, {"locked_until_ms", nullptr},
                                {"retry_after_seconds", nullptr}, {"error", json::value_from(*failed.error)}};
        currentRaw = patchJob(currentRaw, patch);
        currentJob = failed;
        co_await jsAwait(update());
        co_await jsAwait(deleteInputs());
        co_await recordErrorUsage(params.usageContext).wait();
        if (params.usageContext && params.usageContext->responseTelemetry) {
          const auto state = params.usageContext->responseTelemetry;
          state->stream = false; state->completed = false; state->streamTerminalType = "error"; state->failureKind = code_;
        }
        co_return jobResponse(200, failed, std::nullopt);
      }
      rt::Task<rt::Response> queueJob(double waitMs, std::optional<std::string> failureKind = std::nullopt) {
        const double retryAfterSeconds = rt::Math::max(1.0, rt::Math::ceil(waitMs / 1000));
        auto queued = currentJob;
        queued.status = "queued"; queued.updated_at_ms = rt::dateNow(); queued.locked_until_ms = std::nullopt;
        queued.retry_after_seconds = retryAfterSeconds; queued.error = std::nullopt;
        const json::object patch{{"status", "queued"}, {"updated_at_ms", rt::makeNumber(queued.updated_at_ms)}, {"locked_until_ms", nullptr},
                                {"retry_after_seconds", rt::makeNumber(retryAfterSeconds)}, {"error", nullptr}};
        currentRaw = patchJob(currentRaw, patch);
        currentJob = queued;
        co_await jsAwait(update());
        if (failureKind && !failureKind->empty() && params.usageContext && params.usageContext->responseTelemetry) {
          const auto state = params.usageContext->responseTelemetry;
          state->stream = false; state->completed = false; state->streamTerminalType = "deadline"; state->failureKind = *failureKind;
        }
        co_return jobResponse(202, queued, std::nullopt, retryAfterSeconds);
      }
      rt::Task<rt::Response> succeedJob() {
        auto task = loadEmbeddingsVectorsFromCache(params.kv, currentJob.cache_profile_key, currentJob.input_hashes, currentJob.dimensions);
        const auto vectorsByIndex = co_await jsAwait(std::move(task));
        for (const auto& vec : vectorsByIndex) if (!vec) {
          const std::string message = "Embeddings job completed but cache entries were missing.", code_ = "embeddings_job_cache_miss";
          auto result = co_await jsAwait(failJob(message, code_)); co_return result;
        }
        auto succeeded = currentJob;
        succeeded.status = "succeeded"; succeeded.updated_at_ms = rt::dateNow(); succeeded.locked_until_ms = std::nullopt;
        succeeded.retry_after_seconds = std::nullopt; succeeded.error = std::nullopt;
        const json::object patch{{"status", "succeeded"}, {"updated_at_ms", rt::makeNumber(succeeded.updated_at_ms)},
                                {"locked_until_ms", nullptr}, {"retry_after_seconds", nullptr}, {"error", nullptr}};
        currentRaw = patchJob(currentRaw, patch);
        currentJob = succeeded;
        co_await jsAwait(update());
        co_await jsAwait(deleteInputs());
        const auto result = buildOpenAiEmbeddingsResult(succeeded.model, vectorsByIndex, succeeded.usage_total_tokens, succeeded.encoding_format);
        std::optional<UsageTokens> usage;
        if (succeeded.usage_total_tokens > 0) usage = UsageTokens{succeeded.usage_total_tokens, std::nullopt, std::nullopt, 0, succeeded.usage_total_tokens, "reported"};
        co_await recordCompletionUsage(params.usageContext, usage).wait();
        co_return jobResponse(200, succeeded, result);
      }
      rt::Task<std::variant<std::vector<EmbeddingsChunkItem>, rt::Response>> loadInputItems(std::vector<std::string> hashes) {
        std::vector<rt::Promise<std::optional<rt::KvEntryMaybe<>>>> pending;
        for (const auto& hash : hashes) {
          auto task = cacheEntry(params.kv, embeddingsJobInputKey(params.tokenHash, currentJob.cache_profile_key, currentJob.id, hash));
          pending.emplace_back(std::move(task));
        }
        const auto entries = co_await rt::all(std::move(pending)).wait();
        std::vector<EmbeddingsChunkItem> items;
        for (std::size_t i = 0; i < hashes.size(); ++i) {
          const auto normalized = entries[i] ? normalizeEmbeddingsJobInputRecord(entries[i]->value) : std::nullopt;
          if (!normalized) {
            const std::string message = "Embeddings job input expired or was unavailable.", code_ = "embeddings_job_input_missing";
            auto result = co_await jsAwait(failJob(message, code_)); co_return result;
          }
          auto task = decryptEmbeddingsJobInput(params.tokenSeed, *normalized);
          const auto text = co_await jsAwait(std::move(task));
          if (!text) {
            const std::string message = "Embeddings job input could not be decrypted.", code_ = "embeddings_job_input_decrypt_failed";
            auto result = co_await jsAwait(failJob(message, code_)); co_return result;
          }
          items.push_back({hashes[i], *text});
        }
        co_return items;
      }
      struct Queued { double waitMs; std::string failureKind; };
      rt::Task<std::variant<VoyageEmbeddingsResult, rt::Response, Queued>> fetchChunkVectors(std::vector<std::string> texts) {
        rt::Reason error;
        try {
          FetchVoyageEmbeddingsParams input{params.apiKey, currentJob.upstream_model, texts, currentJob.input_type, currentJob.dimensions,
            currentJob.output_dtype, currentJob.truncation, params.deadlineMs, std::nullopt, params.usageContext ? params.usageContext->beforeProviderDispatch : nullptr};
          auto task = fetchVoyageEmbeddings(input);
          const auto upstream = co_await jsAwait(std::move(task));
          co_return upstream;
        } catch (...) { error = std::current_exception(); }
        std::optional<json::value> status;
        std::optional<double> retryAfterMs;
        bool quotaError = false;
        try {
          if (error) std::rethrow_exception(error);
        } catch (const api_key_policy::ApiKeyQuotaDispatchError&) { quotaError = true;
        } catch (const VoyageEmbeddingsError& e) { status = e.status; retryAfterMs = e.retry_after_ms;
        } catch (const upstream_wire::ErrorWithStatus& e) { status = e.status;
        } catch (...) { }
        if (quotaError) { auto response = co_await jsAwait(queueJob(1000)); co_return response; }
        if (!status) if (const auto value = rt::thrownValue(error)) {
          if (const auto field = rt::get(*value, "status")) status = *field;
          retryAfterMs = rt::getNumber(*value, "retry_after_ms");
        }
        if (status && rt::isNumber(*status) && rt::truthy(*status) && EMBEDDINGS_RETRYABLE_UPSTREAM_STATUSES.has(rt::toNumber(*status))) {
          const double number = rt::toNumber(*status);
          co_return Queued{retryAfterMs.value_or(number == 429 ? 60'000 : 1000), "embeddings_job_upstream_http_" + rt::numberToString(number)};
        }
        const auto snippet = upstream_wire::formatErrorSnippet(error);
        const std::string message = snippet.empty() ? "Embeddings upstream request failed." : "Embeddings upstream request failed: " + snippet;
        const std::string code_ = "embeddings_job_upstream_error";
        auto response = co_await jsAwait(failJob(message, code_)); co_return response;
      }
      struct Continue {};
      struct Stop { double waitMs; std::optional<std::string> failureKind; };
      rt::Task<std::variant<Continue, Stop, rt::Response>> processChunk(std::vector<EmbeddingsChunkItem> chunk) {
        if (rt::dateNow() >= params.deadlineMs) co_return Stop{1000, "embeddings_job_deadline"};
        std::vector<std::string> texts;
        for (const auto& item : chunk) texts.push_back(item.text);
        const auto reserved = co_await jsAwait(reserveVoyageBudgetForJob(*params.kv, estimateTokenCount(texts)));
        if (!reserved.ok) co_return Stop{reserved.wait_ms.value_or(0) > 0 ? *reserved.wait_ms : 1000, std::nullopt};
        auto fetchTask = fetchChunkVectors(texts);
        const auto fetched = co_await jsAwait(std::move(fetchTask));
        if (const auto response = std::get_if<rt::Response>(&fetched)) co_return *response;
        if (const auto queued = std::get_if<Queued>(&fetched)) co_return Stop{queued->waitMs, queued->failureKind};
        const auto& upstream = std::get<VoyageEmbeddingsResult>(fetched);
        if (upstream.vectors.size() != chunk.size()) {
          const std::string message = "Embeddings upstream returned a size mismatch.", code_ = "embeddings_job_upstream_mismatch";
          auto response = co_await jsAwait(failJob(message, code_)); co_return response;
        }
        for (const auto& vector : upstream.vectors) if (static_cast<double>(vector.size()) != currentJob.dimensions) {
          const std::string message = "Embeddings upstream returned vector length " + rt::numberToString(static_cast<double>(vector.size())) + "; expected " +
            rt::numberToString(currentJob.dimensions) + ".", code_ = "embeddings_job_upstream_dimension_mismatch";
          auto response = co_await jsAwait(failJob(message, code_)); co_return response;
        }
        if (upstream.totalTokens) {
          currentJob.usage_total_tokens += *upstream.totalTokens;
          const json::object patch{{"usage_total_tokens", rt::makeNumber(currentJob.usage_total_tokens)}};
          currentRaw = patchJob(currentRaw, patch);
        }
        for (std::size_t i = 0; i < chunk.size(); ++i) {
          auto task = writeEmbeddingsCacheEntryBestEffort(params.kv, currentJob.cache_profile_key, chunk[i].hash, upstream.vectors[i], rt::dateNow(), params.deadlineMs);
          const auto ignored = co_await jsAwait(std::move(task));
          (void)ignored;
        }
        co_return Continue{};
      }
    };
    EmbeddingsJobRecord jobOf(const rt::KvValue& raw) { return json::value_to<EmbeddingsJobRecord>(rt::storedJsonView(raw)); }
    rt::Task<void> writeInput(std::shared_ptr<rt::Kv> store, std::string tokenSeed, std::string tokenHash,
                             std::string profile, std::string id, std::string hash, std::string text) {
      const auto record = co_await jsAwait(encryptEmbeddingsJobInput(tokenSeed, text));
      const auto key = embeddingsJobInputKey(tokenHash, profile, id, hash);
      const rt::KvSetOptions options{.expireIn = EMBEDDINGS_JOB_TTL_MS};
      const rt::KvValue value(json::value_from(record));
      const auto ignored = co_await jsAwait(store->set(key, value, options));
      (void)ignored;
    }
    json::object recordView(const json::value& value) {
      if (value.is_object()) return value.get_object();
      json::object out;
      if (value.is_array()) for (std::size_t i = 0; i < value.get_array().size(); ++i) rt::objectDefine(out, rt::numberToString(static_cast<double>(i)), value.get_array()[i]);
      return out;
    }
    rt::Task<rt::Response> handleEmbeddingsJobCreateInternal(rt::Request req, std::optional<std::string> authToken, const UsageContext* usageContext) {
      const auto requestId = rt::crypto::randomUUID();
      const double startedAtMs = rt::dateNow();
      const auto rawBody = co_await jsAwait(request::readJsonBody(req));
      if (!rt::truthy(rawBody) || !utils::isRecord(rawBody)) {
        co_await recordErrorUsage(usageContext).wait();
        co_return http::openaiError(400, "Invalid JSON body", "invalid_request_error");
      }
      const auto parsed = parseEmbeddingsJobRequest(recordView(rawBody));
      if (const auto failure = std::get_if<EmbeddingsParseResultNotOk>(&parsed)) {
        co_await recordErrorUsage(usageContext).wait(); co_return failure->response;
      }
      const auto& value = std::get<EmbeddingsParseResultOk>(parsed).value;
      const auto& profile = value.profile;
      const RecordRequestUsageDetails details{value.model, "embeddings.jobs.create", false, std::nullopt};
      co_await recordRequestUsage(usageContext, details).wait();
      auto kvPending = kv::getKv();
      const auto store = co_await kvPending.wait();
      if (!store) {
        co_await recordErrorUsage(usageContext).wait(); co_return serverError(503, "Embeddings jobs require Deno KV", "server_error");
      }
      const auto apiKey = co_await jsAwait(readVoyageApiKey(store));
      if (!apiKey || apiKey->empty()) {
        co_await recordErrorUsage(usageContext).wait();
        co_return serverError(503, "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)", "server_error");
      }
      std::vector<rt::Promise<std::string>> hashPending;
      for (const auto& text : value.inputs) hashPending.emplace_back(utils::sha256Hex(text));
      const auto hashesByIndex = co_await rt::all(std::move(hashPending)).wait();
      rt::OrderedMap<std::string, std::string> uniqueTextsByHash;
      for (std::size_t i = 0; i < value.inputs.size(); ++i) uniqueTextsByHash.set(hashesByIndex[i], value.inputs[i]);
      const auto uniqueHashes = rt::arrayFrom(uniqueTextsByHash.keys());
      const std::string jobId = "embjob_" + rt::replaceAll(rt::crypto::randomUUID(), "-", "");
      const auto tokenSeed = resolveEmbeddingsJobTokenSeed(jobId, authToken, usageContext);
      const auto tokenHash = co_await jsAwait(utils::sha256Hex(tokenSeed));
      const double now = rt::dateNow();
      // Persist only encrypted input, so later client requests can resume a queued job.
      std::vector<rt::Promise<void>> inputWrites;
      for (const auto& hash : uniqueHashes) {
        auto task = writeInput(store, tokenSeed, tokenHash, profile.cache_profile_key, jobId, hash, *uniqueTextsByHash.get(hash));
        inputWrites.emplace_back(std::move(task));
      }
      co_await rt::all(std::move(inputWrites)).wait();
      EmbeddingsJobRecord job{.id = jobId, .status = "queued", .created_at_ms = now, .updated_at_ms = now, .model = value.model,
        .cache_profile_key = profile.cache_profile_key, .upstream = profile.upstream, .upstream_model = profile.upstream_model,
        .input_type = profile.input_type, .dimensions = profile.dimensions, .output_dtype = profile.output_dtype, .encoding_format = profile.encoding_format,
        .truncation = profile.truncation, .input_hashes = hashesByIndex, .input_count = static_cast<double>(value.inputs.size()), .total_chars = value.total_chars,
        .usage_total_tokens = 0, .retry_after_seconds = std::nullopt, .locked_until_ms = std::nullopt, .error = std::nullopt};
      const auto jobKey = embeddingsJobKey(tokenHash, profile.cache_profile_key, jobId);
      const auto lookupKey = embeddingsJobLookupKey(tokenHash, jobId);
      const EmbeddingsJobLookupRecord lookup{profile.cache_profile_key};
      const rt::KvSetOptions options{.expireIn = EMBEDDINGS_JOB_TTL_MS};
      auto operation = store->atomic();
      operation->set(jobKey, rt::KvValue(json::value_from(job)), options).set(lookupKey, rt::KvValue(json::value_from(lookup)), options);
      const auto persisted = co_await jsAwait(operation->commit());
      if (!persisted.ok) {
        co_await recordErrorUsage(usageContext).wait(); co_return serverError(502, "Embeddings job could not be persisted.", "server_error");
      }
      const auto entry = co_await jsAwait(store->get(jobKey));
      if (!entry.value.truthy()) {
        co_await recordErrorUsage(usageContext).wait(); co_return serverError(502, "Embeddings job could not be persisted.", "server_error");
      }
      RunEmbeddingsJobAttemptParams params{requestId, store, *apiKey, tokenSeed, tokenHash, jobKey, lookupKey, entry,
        jobOf(entry.value), startedAtMs + EMBEDDINGS_TIMEOUT_MS, usageContext, entry.value};
      auto task = runEmbeddingsJobAttempt(params);
      auto response = co_await jsAwait(std::move(task)); co_return response;
    }
    rt::Task<rt::Response> handleEmbeddingsJobGetInternal(rt::Request, std::optional<std::string> authToken, std::string jobId, const UsageContext* usageContext) {
      const auto requestId = rt::crypto::randomUUID();
      const double startedAtMs = rt::dateNow();
      auto pending = kv::getKv();
      const auto store = co_await pending.wait();
      if (!store) {
        co_await recordErrorUsage(usageContext).wait(); co_return serverError(503, "Embeddings jobs require Deno KV", "server_error");
      }
      const auto tokenSeed = resolveEmbeddingsJobTokenSeed(jobId, authToken, usageContext);
      const auto tokenHash = co_await jsAwait(utils::sha256Hex(tokenSeed));
      const auto lookupKey = embeddingsJobLookupKey(tokenHash, jobId);
      const auto lookupEntry = co_await jsAwait(store->get(lookupKey));
      const auto& rawProfile = lookupEntry.value["cache_profile_key"];
      const std::optional<std::string> profile = rt::kvIsObject(lookupEntry.value) && rawProfile.isString() ? std::optional<std::string>(rawProfile.asString()) : std::nullopt;
      if (!profile || profile->empty()) {
        co_await recordErrorUsage(usageContext).wait(); co_return serverError(404, "Embeddings job not found", "not_found", "invalid_request_error");
      }
      const auto jobKey = embeddingsJobKey(tokenHash, *profile, jobId);
      const auto entry = co_await jsAwait(store->get(jobKey));
      if (!entry.value["cache_profile_key"].isString() || entry.value["cache_profile_key"].asString() != *profile) {
        co_await recordErrorUsage(usageContext).wait(); co_return serverError(404, "Embeddings job not found", "not_found", "invalid_request_error");
      }
      const auto job = jobOf(entry.value);
      const RecordRequestUsageDetails details{job.model, "embeddings.jobs.get", false, std::nullopt};
      co_await recordRequestUsage(usageContext, details).wait();
      if (job.status == "succeeded") {
        auto task = loadEmbeddingsVectorsFromCache(store, job.cache_profile_key, job.input_hashes, job.dimensions);
        const auto vectors = co_await jsAwait(std::move(task));
        bool complete = true;
        for (const auto& vec : vectors) if (!vec) complete = false;
        if (!complete) {
          // The cache can be evicted before the job; clients must resubmit in that case.
          auto failed = job;
          failed.status = "failed"; failed.updated_at_ms = rt::dateNow(); failed.locked_until_ms = std::nullopt; failed.retry_after_seconds = std::nullopt;
          failed.error = EmbeddingsJobRecord::Error{"Embeddings job result was unavailable; please resubmit.", "server_error", "embeddings_job_result_missing"};
          const json::object patch{{"status", "failed"}, {"updated_at_ms", rt::makeNumber(failed.updated_at_ms)}, {"locked_until_ms", nullptr},
            {"retry_after_seconds", nullptr}, {"error", json::value_from(*failed.error)}};
          auto updateTask = updateEmbeddingsJobRecord(store, jobKey, lookupKey, failed, patchJob(entry.value, patch));
          co_await jsAwait(std::move(updateTask));
          co_return jobResponse(200, failed, std::nullopt);
        }
        co_return jobResponse(200, job, buildOpenAiEmbeddingsResult(job.model, vectors, job.usage_total_tokens, job.encoding_format));
      }
      if (job.status == "failed") co_return jobResponse(200, job, std::nullopt);
      const auto apiKey = co_await jsAwait(readVoyageApiKey(store));
      if (!apiKey || apiKey->empty()) {
        co_await recordErrorUsage(usageContext).wait();
        co_return serverError(503, "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)", "server_error");
      }
      RunEmbeddingsJobAttemptParams params{requestId, store, *apiKey, tokenSeed, tokenHash, jobKey, lookupKey, entry, job,
        startedAtMs + EMBEDDINGS_TIMEOUT_MS, usageContext, entry.value};
      auto task = runEmbeddingsJobAttempt(params);
      auto response = co_await jsAwait(std::move(task)); co_return response;
    }
    rt::Task<rt::Response> createTelemetry(rt::Request req, std::optional<std::string> token, UsageContext context) {
      auto task = handleEmbeddingsJobCreateInternal(req, token, &context);
      const auto response = co_await jsAwait(std::move(task)); co_return handlers::withVoyageUpstreamHeader(response);
    }
    rt::Task<rt::Response> getTelemetry(rt::Request req, std::optional<std::string> token, std::string jobId, UsageContext context) {
      auto task = handleEmbeddingsJobGetInternal(req, token, jobId, &context);
      const auto response = co_await jsAwait(std::move(task)); co_return handlers::withVoyageUpstreamHeader(response);
    }
  }

  rt::Task<rt::Response> runEmbeddingsJobAttempt(RunEmbeddingsJobAttemptParams params) {
    const double now = rt::dateNow();
    if (params.job.locked_until_ms && rt::truthy(*params.job.locked_until_ms) && *params.job.locked_until_ms > now) {
      const double seconds = rt::Math::max(1.0, rt::Math::ceil((*params.job.locked_until_ms - now) / 1000));
      co_return jobResponse(202, params.job, std::nullopt, seconds);
    }
    auto locked = params.job;
    locked.status = "running"; locked.locked_until_ms = now + EMBEDDINGS_JOB_LOCK_MS; locked.updated_at_ms = now; locked.retry_after_seconds = std::nullopt;
    const json::object patch{{"status", "running"}, {"locked_until_ms", rt::makeNumber(*locked.locked_until_ms)},
                            {"updated_at_ms", rt::makeNumber(now)}, {"retry_after_seconds", nullptr}};
    const auto lockedRaw = patchJob(params.jobRaw.value_or(rt::KvValue(json::value_from(params.job))), patch);
    auto operation = params.kv->atomic();
    const rt::KvSetOptions options{.expireIn = EMBEDDINGS_JOB_TTL_MS};
    const EmbeddingsJobLookupRecord lookup{locked.cache_profile_key};
    operation->check(params.jobEntry).set(params.jobKey, lockedRaw, options).set(params.jobLookupKey, rt::KvValue(json::value_from(lookup)), options);
    const auto commit = co_await jsAwait(operation->commit());
    if (!commit.ok) co_return jobResponse(202, params.job, std::nullopt, 1);
    const rt::OrderedSet<std::string> unique(locked.input_hashes.begin(), locked.input_hashes.end());
    JobState state{params, locked, lockedRaw, rt::arrayFrom(unique.values())};
    const auto missingBefore = co_await jsAwait(state.computeMissing());
    if (missingBefore.empty()) { auto response = co_await jsAwait(state.succeedJob()); co_return response; }
    auto loadTask = state.loadInputItems(missingBefore);
    const auto loaded = co_await jsAwait(std::move(loadTask));
    if (const auto response = std::get_if<rt::Response>(&loaded)) co_return *response;
    const auto chunks = chunkByTokenBudget(std::get<std::vector<EmbeddingsChunkItem>>(loaded), EMBEDDINGS_MAX_INPUTS_PER_REQUEST, VOYAGE_RATE_LIMIT_TPM);
    for (const auto& chunk : chunks) {
      auto task = state.processChunk(chunk);
      const auto outcome = co_await jsAwait(std::move(task));
      if (const auto response = std::get_if<rt::Response>(&outcome)) co_return *response;
      if (const auto stop = std::get_if<JobState::Stop>(&outcome)) { state.queueRetryAfterMs = stop->waitMs; state.queueFailureKind = stop->failureKind; break; }
    }
    const auto missingAfter = co_await jsAwait(state.computeMissing());
    if (missingAfter.empty()) { auto response = co_await jsAwait(state.succeedJob()); co_return response; }
    auto task = state.queueJob(state.queueRetryAfterMs.value_or(60'000), state.queueFailureKind);
    auto response = co_await jsAwait(std::move(task)); co_return response;
  }
  rt::Task<rt::Response> handleEmbeddingsJobCreate(rt::Request req, std::optional<std::string> authToken, const UsageContext* usageContext) {
    const std::optional<UsageContext> context = usageContext ? std::optional<UsageContext>(*usageContext) : std::nullopt;
    const std::function<rt::Task<rt::Response>(UsageContext)> run = [req, authToken](UsageContext next) { return createTelemetry(req, authToken, next); };
    auto task = runWithResponseTelemetry(context, run);
    auto response = co_await jsAwait(std::move(task)); co_return response;
  }
  rt::Task<rt::Response> handleEmbeddingsJobGet(rt::Request req, std::optional<std::string> authToken, std::string jobId, const UsageContext* usageContext) {
    const std::optional<UsageContext> context = usageContext ? std::optional<UsageContext>(*usageContext) : std::nullopt;
    const std::function<rt::Task<rt::Response>(UsageContext)> run = [req, authToken, jobId](UsageContext next) { return getTelemetry(req, authToken, jobId, next); };
    auto task = runWithResponseTelemetry(context, run);
    auto response = co_await jsAwait(std::move(task)); co_return response;
  }
}
