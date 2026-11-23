#include "uos/embeddings/voyage.hpp"

#include "uos/http.hpp"
#include "uos/openai.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/utils.hpp"

#include <bit>
#include <cstdint>
#include <exception>
#include <utility>

namespace uos::embeddings::voyage {
  namespace {
    using namespace ledger;
    template<class T> rt::Task<T> jsAwait(rt::Task<T> task) { return rt::Promise<T>(std::move(task)).wait(); }

    std::optional<double> normalizeEmbeddingsCacheTimestampMs(const rt::KvValue& value) {
      if (!value.isNumber() || !rt::Number::isFinite(value.asNumber())) return std::nullopt;
      const double ts = rt::Math::trunc(value.asNumber());
      return ts < 0 ? std::nullopt : std::optional<double>(ts);
    }
    struct EmbeddingsCacheEvictResult { double evicted_embeddings; double deleted_stale_index_keys; };
    std::string readEmbeddingsCacheErrorName(const rt::Reason& error) {
      if (rt::isError(error)) return rt::errorName(error).value_or("");
      const auto value = rt::thrownValue(error);
      return value ? std::string(rt::getString(*value, "name").value_or("")) : "";
    }
    std::string readEmbeddingsCacheErrorMessage(const rt::Reason& error) {
      if (rt::isError(error)) return rt::errorMessage(error).value_or("");
      const auto value = rt::thrownValue(error);
      return value && value->is_string() ? std::string(value->get_string()) : "";
    }
    bool isEmbeddingsCacheQuotaError(const rt::Reason& error) {
      const std::string combined = rt::toLowerCase(readEmbeddingsCacheErrorName(error) + " " + readEmbeddingsCacheErrorMessage(error));
      const auto has = [&](std::string_view text) { return combined.find(text) != std::string::npos; };
      return has("quota") || (has("insufficient") && has("storage")) || (has("insufficient") && has("space")) || has("no space") ||
             has("storage limit") || (has("storage") && has("exceeded"));
    }
    rt::console::Value cacheErrorValue(const rt::Reason& error) {
      if (const auto value = rt::thrownValue(error)) return rt::console::Value(*value);
      return rt::console::Value(error);
    }
    rt::Task<EmbeddingsCacheWriteResult> writeEmbeddingsCacheEntry(rt::Kv& kv, std::string cacheProfileKey, std::string hash,
                                                                 std::vector<double> embedding, double createdAtMs) {
      const auto byHashKey = embeddingsCacheIndexByHashKey(cacheProfileKey, hash);
      const auto cacheKey = embeddingsCacheKey(cacheProfileKey, hash);
      // Only one concurrent writer wins the create-index CAS; the others reuse its index.
      for (int attempt = 0; attempt < 5; ++attempt) {
        const auto entry = co_await jsAwait(kv.get(byHashKey));
        const auto existingCreatedAtMs = normalizeEmbeddingsCacheTimestampMs(entry.value);
        const double timestamp = existingCreatedAtMs.value_or(createdAtMs);
        const auto indexKey = embeddingsCacheIndexKey(cacheProfileKey, timestamp, hash);
        const json::object value{{"embedding", json::value_from(embedding)}, {"created_at", rt::Date(timestamp).toISOString()}};
        auto operation = kv.atomic();
        operation->check(entry).set(cacheKey, rt::KvValue(value)).set(indexKey, rt::KvValue(1))
          .set(embeddingsCacheGlobalIndexKey(timestamp, cacheProfileKey, hash), rt::KvValue(1));
        if (!existingCreatedAtMs) operation->set(byHashKey, rt::KvValue(createdAtMs));
        const auto commit = co_await jsAwait(operation->commit());
        if (commit.ok) co_return EmbeddingsCacheWriteResult{!existingCreatedAtMs};
      }
      co_return EmbeddingsCacheWriteResult{false};
    }
    struct CacheCandidate { rt::KvKey globalIndexKey; std::string cacheProfileKey; double createdAtMs; std::string hash; };
    rt::Task<std::vector<CacheCandidate>> listEmbeddingsCacheEvictionCandidates(rt::Kv& kv, double count) {
      std::vector<CacheCandidate> keys;
      const rt::KvListSelector selector{.prefix = embeddingsCacheGlobalIndexPrefix};
      const rt::KvListOptions options{.limit = count};
      auto iterator = kv.list(selector, options);
      while (auto entry = co_await iterator->next()) {
        const auto& key = entry->key;
        if (key.size() < 3) continue;
        const auto& hash = key[key.size() - 1];
        const auto& profile = key[key.size() - 2];
        const auto& timestamp = key[key.size() - 3];
        if (!hash.isString() || hash.string().empty() || !profile.isString() || profile.string().empty() ||
            !timestamp.isNumber() || !rt::Number::isFinite(timestamp.number())) continue;
        keys.push_back({key, profile.string(), rt::Math::trunc(timestamp.number()), hash.string()});
      }
      co_return keys;
    }
    struct CacheEvictionOutcome { bool evicted; bool deletedStaleIndexKey; };
    rt::Task<CacheEvictionOutcome> evictEmbeddingsCacheCandidate(rt::Kv& kv, CacheCandidate candidate, rt::KvEntryMaybe<> pointerEntry) {
      const auto pointer = normalizeEmbeddingsCacheTimestampMs(pointerEntry.value);
      const auto cacheKey = embeddingsCacheKey(candidate.cacheProfileKey, candidate.hash);
      const auto profileIndexKey = embeddingsCacheIndexKey(candidate.cacheProfileKey, candidate.createdAtMs, candidate.hash);
      if (pointer && *pointer != candidate.createdAtMs) {
        auto operation = kv.atomic();
        operation->check(pointerEntry).delete_(candidate.globalIndexKey).delete_(profileIndexKey);
        const auto commit = co_await jsAwait(operation->commit());
        co_return CacheEvictionOutcome{false, commit.ok};
      }
      if (!pointer) {
        const auto valueEntry = co_await jsAwait(kv.get(cacheKey));
        const auto& value = valueEntry.value;
        const std::optional<std::string> createdAtIso = rt::kvIsObject(value) && value["created_at"].isString()
          ? std::optional<std::string>(value["created_at"].asString()) : std::nullopt;
        const std::string expectedIso = rt::Date(candidate.createdAtMs).toISOString();
        auto operation = kv.atomic();
        operation->check(pointerEntry).delete_(candidate.globalIndexKey).delete_(profileIndexKey);
        if (createdAtIso != expectedIso) {
          const auto commit = co_await jsAwait(operation->commit());
          co_return CacheEvictionOutcome{false, commit.ok};
        }
        operation->delete_(cacheKey);
        const auto commit = co_await jsAwait(operation->commit());
        co_return CacheEvictionOutcome{commit.ok, false};
      }
      auto operation = kv.atomic();
      operation->check(pointerEntry).delete_(candidate.globalIndexKey).delete_(profileIndexKey).delete_(cacheKey)
        .delete_(embeddingsCacheIndexByHashKey(candidate.cacheProfileKey, candidate.hash));
      const auto commit = co_await jsAwait(operation->commit());
      co_return CacheEvictionOutcome{commit.ok, false};
    }
    rt::Task<EmbeddingsCacheEvictResult> evictOldestEmbeddingsCacheEntries(std::shared_ptr<rt::Kv> kv, double count) {
      auto listing = listEmbeddingsCacheEvictionCandidates(*kv, count);
      const auto candidates = co_await jsAwait(std::move(listing));
      if (candidates.empty()) co_return EmbeddingsCacheEvictResult{0, 0};
      std::vector<rt::Promise<rt::KvEntryMaybe<>>> pending;
      for (const auto& candidate : candidates) {
        auto read = kv->get(embeddingsCacheIndexByHashKey(candidate.cacheProfileKey, candidate.hash));
        pending.emplace_back(rt::detail::keepAlive(kv, std::move(read)));
      }
      auto reads = rt::all(std::move(pending));
      const auto byHashEntries = co_await reads.wait();
      double evictedEmbeddings = 0;
      double deletedStaleIndexKeys = 0;
      for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto task = evictEmbeddingsCacheCandidate(*kv, candidates[i], byHashEntries[i]);
        const auto outcome = co_await jsAwait(std::move(task));
        if (outcome.evicted) ++evictedEmbeddings;
        if (outcome.deletedStaleIndexKey) ++deletedStaleIndexKeys;
      }
      co_return EmbeddingsCacheEvictResult{evictedEmbeddings, deletedStaleIndexKeys};
    }
  }

  rt::Task<EmbeddingsCacheWriteResult> writeEmbeddingsCacheEntryBestEffort(std::shared_ptr<rt::Kv> kv, std::string cacheProfileKey, std::string hash,
                                                                       std::vector<double> embedding, double createdAtMs, double deadlineMs) {
    double evictBatch = ledger::EMBEDDINGS_CACHE_EVICT_BATCH;
    for (int attempt = 0; attempt <= ledger::EMBEDDINGS_CACHE_QUOTA_MAX_RETRIES; ++attempt) {
      if (rt::dateNow() >= deadlineMs) co_return EmbeddingsCacheWriteResult{false};
      rt::Reason error;
      try {
        auto writing = writeEmbeddingsCacheEntry(*kv, cacheProfileKey, hash, embedding, createdAtMs);
        const auto result = co_await jsAwait(std::move(writing));
        co_return result;
      } catch (...) { error = std::current_exception(); }
      if (!isEmbeddingsCacheQuotaError(error)) {
        rt::console::warn("[ai.ubq.fi] embeddings_cache write failed:", cacheErrorValue(error));
        co_return EmbeddingsCacheWriteResult{false};
      }
      // Evict globally so a new profile cannot be starved by another profile's entries.
      rt::Reason evictError;
      try {
        auto evicting = evictOldestEmbeddingsCacheEntries(kv, evictBatch);
        const auto evicted = co_await jsAwait(std::move(evicting));
        rt::console::warn("[ai.ubq.fi] embeddings_cache quota eviction requesting_profile=" + cacheProfileKey + " scope=global evicted=" +
          rt::numberToString(evicted.evicted_embeddings) + " stale_index_deleted=" + rt::numberToString(evicted.deleted_stale_index_keys) +
          " batch=" + rt::numberToString(evictBatch));
        if (evicted.evicted_embeddings <= 0 && evicted.deleted_stale_index_keys <= 0) co_return EmbeddingsCacheWriteResult{false};
      } catch (...) { evictError = std::current_exception(); }
      if (evictError) {
        rt::console::warn("[ai.ubq.fi] embeddings_cache quota eviction failed:", cacheErrorValue(evictError));
        co_return EmbeddingsCacheWriteResult{false};
      }
      evictBatch = rt::Math::min(ledger::EMBEDDINGS_CACHE_EVICT_MAX_BATCH, evictBatch * 2);
    }
    co_return EmbeddingsCacheWriteResult{false};
  }

  std::string resolveEmbeddingsJobTokenSeed(const std::string& jobId, const std::optional<std::string>& authToken,
                                           const openai_telemetry::UsageContext* usageContext) {
    if (usageContext && usageContext->keyId && !usageContext->keyId->empty()) return "uos_api_key_id:" + *usageContext->keyId;
    if (usageContext && usageContext->kernelRepo) return "uos_kernel_repo:" + usageContext->kernelRepo->owner + "/" + usageContext->kernelRepo->repo;
    if (authToken && !authToken->empty()) return *authToken;
    return jobId;
  }
  std::optional<std::string> getEnv(const std::string& key) {
    try { return rt::env::get(key); } catch (...) { return std::nullopt; }
  }
  rt::Promise<void> sleep(double ms) {
    return rt::newPromise<void>([ms](const rt::Resolve<void>& resolve, const rt::Reject&) { rt::setTimeout([resolve] { resolve(); }, ms); });
  }
  rt::Promise<bool> sleepUnlessAborted(double ms, rt::AbortSignal signal) {
    if (signal.aborted()) return rt::Promise<bool>::resolve(false);
    struct State {
      rt::AbortSignal signal;
      rt::Timeout timeout;
      rt::EventListener onAbort;

      explicit State(rt::AbortSignal signal_) : signal(std::move(signal_)) {}
      ~State() { signal.removeEventListener("abort", onAbort); }
    };
    auto state = std::make_shared<State>(std::move(signal));
    return rt::newPromise<bool>([ms, state](const rt::Resolve<bool>& resolve, const rt::Reject&) {
      state->timeout = rt::setTimeout([state, resolve] {
        state->signal.removeEventListener("abort", state->onAbort);
        resolve(true);
      }, ms);
      const std::weak_ptr<State> weak = state;
      state->onAbort = rt::EventListener([weak, resolve] {
        if (const auto current = weak.lock()) {
          rt::clearTimeout(current->timeout);
          resolve(false);
        }
      });
      state->signal.addEventListener("abort", state->onAbort, {.once = true});
      if (state->signal.aborted()) state->onAbort();
    });
  }
  double estimateTokens(const std::string& text) { return text.empty() ? 0 : rt::Math::ceil(static_cast<double>(rt::utf8ByteLength(text)) / 4); }
  double estimateTokenCount(const std::vector<std::string>& texts) {
    double count = 0;
    for (const auto& text : texts) count += estimateTokens(text);
    return count;
  }
  std::vector<std::vector<EmbeddingsChunkItem>> chunkByTokenBudget(const std::vector<EmbeddingsChunkItem>& items, double maxItems, double maxTokens) {
    std::vector<std::vector<EmbeddingsChunkItem>> out;
    const double itemLimit = rt::Math::max(1.0, rt::Math::trunc(maxItems));
    const double tokenLimit = rt::Math::max(1.0, rt::Math::trunc(maxTokens));
    std::vector<EmbeddingsChunkItem> current;
    double currentTokens = 0;
    for (const auto& item : items) {
      const double tokens = estimateTokens(item.text);
      if (static_cast<double>(current.size()) >= itemLimit || (currentTokens + tokens > tokenLimit && !current.empty())) {
        out.push_back(current);
        current.clear();
        currentTokens = 0;
      }
      current.push_back(item);
      currentTokens += tokens;
    }
    if (!current.empty()) out.push_back(current);
    return out;
  }

  namespace {
    std::optional<ledger::VoyageRateLimitState> normalizeVoyageRateLimitState(const rt::KvValue& value) {
      if (!rt::kvIsObject(value)) return std::nullopt;
      const auto window = normalizeEmbeddingsCacheTimestampMs(value["window_start_ms"]);
      const auto requests = normalizeEmbeddingsCacheTimestampMs(value["requests"]);
      const auto tokens = normalizeEmbeddingsCacheTimestampMs(value["tokens"]);
      if (!window || !requests || !tokens) return std::nullopt;
      return ledger::VoyageRateLimitState{*window, *requests, *tokens};
    }
    bool wouldExceedVoyageRateLimit(double limit, double used) { return limit > 0 && used > limit; }
    rt::Task<VoyageBudgetResult> tryReserveVoyageBudgetWithRetries(rt::Kv& kv, double tokens) {
      VoyageBudgetResult reserved{false, 0};
      for (int attempt = 0; attempt < 6; ++attempt) {
        reserved = co_await jsAwait(tryReserveVoyageBudget(kv, tokens));
        if (reserved.ok) co_return reserved;
        if (reserved.wait_ms.value_or(0) > 0) break;
        auto pending = sleep(5 + attempt * 5);
        co_await pending.wait();
      }
      co_return reserved;
    }
  }
  rt::Task<VoyageBudgetResult> tryReserveVoyageBudget(rt::Kv& kv, double tokens) {
    const double now = rt::dateNow();
    const auto entry = co_await jsAwait(kv.get(ledger::VOYAGE_RATE_LIMIT_KEY));
    const auto current = normalizeVoyageRateLimitState(entry.value);
    const auto state = !current || now - current->window_start_ms >= 60'000 ? ledger::VoyageRateLimitState{now, 0, 0} : *current;
    if (wouldExceedVoyageRateLimit(ledger::VOYAGE_RATE_LIMIT_RPM, state.requests + 1) ||
        wouldExceedVoyageRateLimit(ledger::VOYAGE_RATE_LIMIT_TPM, state.tokens + tokens))
      co_return VoyageBudgetResult{false, rt::Math::max(0.0, 60'000 - (now - state.window_start_ms))};
    const ledger::VoyageRateLimitState next{state.window_start_ms, state.requests + 1, state.tokens + tokens};
    auto operation = kv.atomic();
    operation->check(entry).set(ledger::VOYAGE_RATE_LIMIT_KEY, rt::KvValue(json::value_from(next)));
    const auto commit = co_await jsAwait(operation->commit());
    co_return commit.ok ? VoyageBudgetResult{true} : VoyageBudgetResult{false, 0};
  }
  rt::Task<VoyageBudgetResult> applyVoyageRateLimit(rt::Kv& kv, double tokens, double deadlineMs) {
    for (;;) {
      const double now = rt::dateNow();
      if (now >= deadlineMs) co_return VoyageBudgetResult{false, 0};
      const auto reserved = co_await jsAwait(tryReserveVoyageBudgetWithRetries(kv, tokens));
      if (reserved.ok) co_return reserved;
      const double waitMs = reserved.wait_ms.value_or(0);
      if (waitMs <= 0) {
        const double sleepMs = rt::Math::min(25.0, rt::Math::max(0.0, deadlineMs - rt::dateNow()));
        if (sleepMs > 0) { auto pending = sleep(sleepMs); co_await pending.wait(); }
        continue;
      }
      if (now + waitMs > deadlineMs) co_return VoyageBudgetResult{false, waitMs};
      auto pending = sleep(waitMs);
      co_await pending.wait();
    }
  }

  namespace {
    rt::Response fieldError(std::string message, const std::string& param, std::string code_ = "invalid_request_error") {
      http::OpenaiErrorOptions options;
      options.param = std::optional<std::string>(param);
      return http::openaiError(400, std::move(message), std::move(code_), options);
    }
    struct FieldFailure { std::string message; };
    std::variant<ledger::EmbeddingsEncodingFormat, FieldFailure> parseEmbeddingsEncodingFormat(const json::value* value) {
      if (!value) return std::string("float");
      if (!value->is_string()) return FieldFailure{"encoding_format must be a string"};
      const std::string text(value->get_string());
      if (text == "float" || text == "base64") return text;
      return FieldFailure{"encoding_format must be one of: \"float\", \"base64\""};
    }
    std::variant<ledger::VoyageEmbeddingsDimension, FieldFailure> parseEmbeddingsDimensions(const json::value* value) {
      if (!value) return ledger::VOYAGE_DEFAULT_DIMENSIONS;
      if (!rt::Number::isFinite(*value) || !rt::Number::isInteger(*value)) return FieldFailure{"dimensions must be an integer"};
      const double dimensions = rt::toNumber(*value);
      if (!ledger::VOYAGE_SUPPORTED_DIMENSIONS.has(dimensions)) return FieldFailure{"dimensions must be one of: 256, 512, 1024, 2048"};
      return dimensions;
    }
    std::string buildEmbeddingsCacheProfileKey(const ledger::VoyageEmbeddingsInputType& inputType, ledger::VoyageEmbeddingsDimension dimensions,
                                               const ledger::EmbeddingsEncodingFormat& encodingFormat, bool truncation) {
      const json::array key{"voyage-profile-v2", ledger::VOYAGE_EMBEDDINGS_MODEL, inputType, rt::makeNumber(dimensions),
                            ledger::VOYAGE_OUTPUT_DTYPE, encodingFormat, truncation};
      return rt::jsonStringify(key);
    }
    ledger::ResolvedEmbeddingsProfile buildResolvedEmbeddingsProfile(const ledger::VoyageEmbeddingsInputType& inputType,
      ledger::VoyageEmbeddingsDimension dimensions, const ledger::EmbeddingsEncodingFormat& encodingFormat, bool truncation) {
      return {.input_type = inputType, .dimensions = dimensions, .output_dtype = std::string(ledger::VOYAGE_OUTPUT_DTYPE),
        .encoding_format = encodingFormat, .truncation = truncation,
        .cache_profile_key = buildEmbeddingsCacheProfileKey(inputType, dimensions, encodingFormat, truncation)};
    }
    std::optional<rt::Response> embeddingsUserFieldError(const json::object& rawBody, bool isJob) {
      if (isJob || !rt::hasOwn(rawBody, "user")) return std::nullopt;
      const auto user = rt::get(rawBody, "user");
      if (!user || user->is_null() || user->is_string()) return std::nullopt;
      return fieldError("user must be a string", "user");
    }
    std::variant<ledger::VoyageEmbeddingsInputType, rt::Response> resolveEmbeddingsInputTypeField(const json::value* raw, bool isJob) {
      if (!raw) {
        if (isJob) return fieldError("input_type is required and must be one of: \"query\", \"document\"", "input_type");
        return std::string("document");
      }
      if (raw->is_string() && (raw->get_string() == "query" || raw->get_string() == "document")) return std::string(raw->get_string());
      return fieldError("input_type must be one of: \"query\", \"document\"", "input_type");
    }
    std::variant<bool, rt::Response> parseEmbeddingsTruncationField(const json::value* raw) {
      if (raw && !raw->is_bool()) return fieldError("truncation must be a boolean", "truncation");
      return raw ? raw->get_bool() : true;
    }
    std::variant<std::vector<std::string>, rt::Response> parseEmbeddingsInputList(const json::value* input) {
      if (input && input->is_string()) return std::vector<std::string>{std::string(input->get_string())};
      if (!input || !input->is_array()) return fieldError("input must be a string or an array of strings", "input");
      std::vector<std::string> inputs;
      for (const auto& item : input->get_array()) {
        if (!item.is_string()) return fieldError("input must be a string or an array of strings", "input");
        inputs.emplace_back(item.get_string());
      }
      return inputs;
    }
    std::variant<double, rt::Response> measureEmbeddingsInputs(const std::vector<std::string>& inputs) {
      if (inputs.empty()) return fieldError("input must be a non-empty string or a non-empty array", "input");
      if (static_cast<double>(inputs.size()) > ledger::EMBEDDINGS_MAX_INPUTS_PER_REQUEST)
        return fieldError("Too many inputs: " + rt::numberToString(static_cast<double>(inputs.size())) + " (max " +
          rt::numberToString(ledger::EMBEDDINGS_MAX_INPUTS_PER_REQUEST) + ")", "input");
      double totalChars = 0;
      for (const auto& text : inputs) {
        const double len = static_cast<double>(rt::length(text));
        if (len > ledger::EMBEDDINGS_MAX_CHARS_PER_INPUT)
          return fieldError("Input too large: " + rt::numberToString(len) + " chars (max " + rt::numberToString(ledger::EMBEDDINGS_MAX_CHARS_PER_INPUT) + ")", "input");
        totalChars += len;
        if (totalChars > ledger::EMBEDDINGS_MAX_TOTAL_CHARS)
          return fieldError("Request too large: " + rt::numberToString(totalChars) + " chars total (max " + rt::numberToString(ledger::EMBEDDINGS_MAX_TOTAL_CHARS) + ")", "input");
        const double tokens = estimateTokens(text);
        if (tokens > ledger::VOYAGE_RATE_LIMIT_TPM)
          return fieldError("Input too large for embeddings provider: ~" + rt::numberToString(tokens) + " tokens (max " + rt::numberToString(ledger::VOYAGE_RATE_LIMIT_TPM) + ").", "input");
      }
      return totalChars;
    }
    ledger::EmbeddingsParseResult parseEmbeddingsRequest(const json::object& rawBody, const std::string& contract) {
      const bool isJob = contract == "uos_job";
      const auto& allowed = isJob ? ledger::UOS_EMBEDDINGS_JOB_ALLOWED_KEYS : ledger::UOS_SYNC_EMBEDDINGS_ALLOWED_KEYS;
      if (const auto unknown = openai::findUnknownKey(rawBody, allowed); unknown && !unknown->empty())
        return ledger::EmbeddingsParseResultNotOk{http::openaiError(400, "Unrecognized request argument supplied: " + *unknown, "invalid_request_error")};
      const auto model = utils::getString(rt::get(rawBody, "model"));
      if (!model || rt::trim(*model).empty()) return ledger::EmbeddingsParseResultNotOk{fieldError("model is required and must be a non-empty string", "model")};
      if (*model != ledger::VOYAGE_EMBEDDINGS_MODEL) return ledger::EmbeddingsParseResultNotOk{fieldError("Unsupported embedding model: " + *model, "model", "model_not_found")};
      const auto dimensions = parseEmbeddingsDimensions(rt::get(rawBody, "dimensions"));
      if (const auto error = std::get_if<FieldFailure>(&dimensions)) return ledger::EmbeddingsParseResultNotOk{fieldError(error->message, "dimensions")};
      const auto encoding = parseEmbeddingsEncodingFormat(rt::get(rawBody, "encoding_format"));
      if (const auto error = std::get_if<FieldFailure>(&encoding)) return ledger::EmbeddingsParseResultNotOk{fieldError(error->message, "encoding_format")};
      const auto& encodingFormat = std::get<ledger::EmbeddingsEncodingFormat>(encoding);
      if (isJob && encodingFormat != "float") return ledger::EmbeddingsParseResultNotOk{fieldError("encoding_format must be \"float\" for embeddings jobs", "encoding_format")};
      const auto inputType = resolveEmbeddingsInputTypeField(rt::get(rawBody, "input_type"), isJob);
      if (const auto error = std::get_if<rt::Response>(&inputType)) return ledger::EmbeddingsParseResultNotOk{*error};
      const auto truncation = parseEmbeddingsTruncationField(rt::get(rawBody, "truncation"));
      if (const auto error = std::get_if<rt::Response>(&truncation)) return ledger::EmbeddingsParseResultNotOk{*error};
      if (const auto error = embeddingsUserFieldError(rawBody, isJob)) return ledger::EmbeddingsParseResultNotOk{*error};
      const auto inputs = parseEmbeddingsInputList(rt::get(rawBody, "input"));
      if (const auto error = std::get_if<rt::Response>(&inputs)) return ledger::EmbeddingsParseResultNotOk{*error};
      const auto& parsedInputs = std::get<std::vector<std::string>>(inputs);
      const auto measured = measureEmbeddingsInputs(parsedInputs);
      if (const auto error = std::get_if<rt::Response>(&measured)) return ledger::EmbeddingsParseResultNotOk{*error};
      const auto profile = buildResolvedEmbeddingsProfile(std::get<ledger::VoyageEmbeddingsInputType>(inputType),
        std::get<ledger::VoyageEmbeddingsDimension>(dimensions), encodingFormat, std::get<bool>(truncation));
      return ledger::EmbeddingsParseResultOk{{*model, parsedInputs, std::get<double>(measured), profile}};
    }
  }
  ledger::EmbeddingsParseResult parseUosEmbeddingsRequest(const json::object& rawBody) { return parseEmbeddingsRequest(rawBody, "uos_sync"); }
  ledger::EmbeddingsParseResult parseEmbeddingsJobRequest(const json::object& rawBody) { return parseEmbeddingsRequest(rawBody, "uos_job"); }
  bool isValidEmbeddingVector(const json::value* value, ledger::VoyageEmbeddingsDimension dimensions) {
    if (!value || !value->is_array() || static_cast<double>(value->get_array().size()) != dimensions) return false;
    for (const auto& item : value->get_array()) if (!rt::Number::isFinite(item)) return false;
    return true;
  }
  bool isValidEmbeddingVector(const json::value& value, ledger::VoyageEmbeddingsDimension dimensions) { return isValidEmbeddingVector(&value, dimensions); }
  bool isValidEmbeddingVector(const rt::KvValue& value, ledger::VoyageEmbeddingsDimension dimensions) {
    if (!value.isArray() || static_cast<double>(value.as<rt::KvArray>().size()) != dimensions) return false;
    for (const auto& item : value.as<rt::KvArray>()) if (!item.isNumber() || !rt::Number::isFinite(item.asNumber())) return false;
    return true;
  }
  std::string floatEmbeddingToBase64(const std::vector<double>& embedding) {
    rt::Bytes bytes;
    bytes.reserve(embedding.size() * 4);
    for (double value : embedding) {
      const std::uint32_t bits = rt::Number::isNaN(value) ? 0x7fc00000U : std::bit_cast<std::uint32_t>(static_cast<float>(value));
      for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::uint8_t>((bits >> shift) & 255));
    }
    return rt::toBase64(bytes);
  }
  std::optional<ledger::EmbeddingsJobInputRecord> normalizeEmbeddingsJobInputRecord(const json::value* value) {
    if (!utils::isRecord(value) || rt::getNumber(*value, "v") != 1) return std::nullopt;
    const auto iv = utils::getString(rt::get(*value, "iv_b64"));
    const auto data = utils::getString(rt::get(*value, "data_b64"));
    const auto created = rt::getNumber(*value, "created_at_ms");
    if (!iv || iv->empty() || !data || data->empty() || !created || !rt::Number::isFinite(*created) || rt::Math::trunc(*created) < 0) return std::nullopt;
    return ledger::EmbeddingsJobInputRecord{1, *iv, *data, rt::Math::trunc(*created)};
  }
  std::optional<ledger::EmbeddingsJobInputRecord> normalizeEmbeddingsJobInputRecord(const json::value& value) { return normalizeEmbeddingsJobInputRecord(&value); }
  std::optional<ledger::EmbeddingsJobInputRecord> normalizeEmbeddingsJobInputRecord(const rt::KvValue& value) {
    const auto view = rt::storedJsonView(value);
    return normalizeEmbeddingsJobInputRecord(view);
  }
  namespace {
    rt::Task<rt::crypto::CryptoKey> importEmbeddingsJobKey(std::string tokenSeed) {
      const auto material = rt::encodeUtf8("uos_embeddings_job_v2:" + tokenSeed);
      const auto digest = rt::crypto::subtle::digest("SHA-256", material);
      co_await rt::microtask();
      auto key = rt::crypto::subtle::importKey("raw", digest, "AES-GCM", false, {"encrypt", "decrypt"});
      co_await rt::microtask();
      co_return key;
    }
  }
  rt::Task<ledger::EmbeddingsJobInputRecord> encryptEmbeddingsJobInput(std::string tokenSeed, std::string text) {
    const auto key = co_await jsAwait(importEmbeddingsJobKey(tokenSeed));
    rt::Bytes iv(12);
    rt::crypto::getRandomValues(iv);
    const auto bytes = rt::encodeUtf8(text);
    const rt::crypto::Algorithm algorithm{.name = "AES-GCM", .iv = iv};
    const auto encrypted = rt::crypto::subtle::encrypt(algorithm, key, bytes);
    co_await rt::microtask();
    co_return ledger::EmbeddingsJobInputRecord{1, utils::bytesToBase64(iv), utils::bytesToBase64(encrypted), rt::dateNow()};
  }
  rt::Task<std::optional<std::string>> decryptEmbeddingsJobInput(std::string tokenSeed, ledger::EmbeddingsJobInputRecord record) {
    const auto iv = utils::base64ToBytes(record.iv_b64);
    const auto data = utils::base64ToBytes(record.data_b64);
    if (!iv || !data) co_return std::nullopt;
    try {
      const auto key = co_await jsAwait(importEmbeddingsJobKey(tokenSeed));
      const rt::crypto::Algorithm algorithm{.name = "AES-GCM", .iv = *iv};
      const auto decrypted = rt::crypto::subtle::decrypt(algorithm, key, *data);
      co_await rt::microtask();
      co_return rt::decodeUtf8(decrypted);
    } catch (...) { }
    co_return std::nullopt;
  }
  namespace {
    std::optional<double> extractRetryAfterMs(const std::optional<std::string>& value) {
      if (!value || value->empty()) return std::nullopt;
      const auto trimmed = rt::trim(*value);
      if (trimmed.empty()) return std::nullopt;
      const double seconds = rt::stringToNumber(trimmed);
      if (rt::Number::isFinite(seconds) && seconds > 0) return rt::Math::min(60'000.0, rt::Math::trunc(seconds * 1000));
      const double asDate = rt::Date::parse(trimmed);
      if (!rt::Number::isNaN(asDate)) {
        const double delta = asDate - rt::dateNow();
        if (delta > 0) return rt::Math::min(60'000.0, rt::Math::trunc(delta));
      }
      return std::nullopt;
    }
    std::vector<double> parseVoyageEmbeddingVector(const json::value* embedding) {
      if (!embedding || !embedding->is_array()) throw rt::JsError("Voyage embeddings response missing embedding vector.");
      std::vector<double> vec;
      for (const auto& v : embedding->get_array()) {
        if (!rt::Number::isFinite(v)) throw rt::JsError("Voyage embeddings response contained non-numeric values.");
        vec.push_back(rt::toNumber(v));
      }
      return vec;
    }
    VoyageEmbeddingsResult parseVoyageEmbeddingsPayload(const json::value& payload) {
      if (!utils::isRecord(payload) || !rt::getArray(payload, "data")) throw rt::JsError("Voyage embeddings returned invalid JSON.");
      std::optional<double> totalTokens;
      const auto usage = rt::get(payload, "usage");
      if (utils::isRecord(usage)) {
        const auto total = rt::getNumber(*usage, "total_tokens");
        if (total && rt::Number::isFinite(*total)) totalTokens = rt::Math::max(0.0, rt::Math::trunc(*total));
      }
      std::vector<std::vector<double>> vectors;
      for (const auto& item : *rt::getArray(payload, "data")) vectors.push_back(parseVoyageEmbeddingVector(utils::isRecord(item) ? rt::get(item, "embedding") : nullptr));
      return {vectors, totalTokens};
    }
  }
  rt::Task<std::optional<std::string>> readVoyageApiKey(std::shared_ptr<rt::Kv> kv) {
    const auto envKey = rt::trim(getEnv("VOYAGEAI_API_KEY").value_or(""));
    if (!envKey.empty()) co_return envKey;
    if (!kv) co_return std::nullopt;
    const auto entry = co_await jsAwait(kv->get(ledger::VOYAGE_API_KEY_KV_KEY));
    const auto kvKey = entry.value.isString() ? rt::trim(entry.value.asString()) : "";
    co_return kvKey.empty() ? std::nullopt : std::optional<std::string>(kvKey);
  }
  rt::Task<VoyageEmbeddingsResult> fetchVoyageEmbeddings(FetchVoyageEmbeddingsParams params) {
    rt::AbortController controller;
    const auto signal = params.downstreamSignal ? rt::AbortSignal::any({controller.signal(), *params.downstreamSignal}) : controller.signal();
    const double timeoutMs = rt::Math::max(1.0, rt::Math::min(ledger::EMBEDDINGS_TIMEOUT_MS, params.deadlineMs - rt::dateNow()));
    const auto timeout = rt::setTimeout([controller] { controller.abort(); }, timeoutMs);
    const auto cleanup = rt::scopeExit([timeout] { rt::clearTimeout(timeout); });
    std::optional<api_key_policy::ApiKeyProviderDispatch> dispatch;
    if (params.beforeProviderDispatch) {
      const std::string provider = "voyage";
      dispatch = co_await jsAwait(params.beforeProviderDispatch(provider));
    }
    if (signal.aborted()) {
      if (dispatch) co_await jsAwait(dispatch->cancelBeforeTransport());
      else co_await rt::microtask();
      const auto reasonValue = rt::thrownValue(signal.reason());
      if (signal.reason() && !(reasonValue && reasonValue->is_null())) std::rethrow_exception(signal.reason());
      throw rt::DOMException("The request was aborted.", "AbortError");
    }
    if (dispatch) dispatch->markTransportStarted();
    const json::value input = params.inputs.size() == 1 ? json::value(params.inputs[0]) : json::value_from(params.inputs);
    const json::object body{{"model", params.model}, {"input", input}, {"input_type", params.inputType}, {"output_dimension", rt::makeNumber(params.dimensions)},
                            {"output_dtype", params.outputDtype}, {"truncation", params.truncation}};
    rt::RequestInit init{.method = "POST", .headers = rt::Headers{{"Authorization", "Bearer " + params.apiKey}, {"Content-Type", "application/json"}},
                         .body = rt::jsonStringify(body), .signal = signal};
    const std::string url(ledger::VOYAGE_EMBEDDINGS_URL);
    rt::Promise<rt::Response> pending(rt::fetch(url, std::move(init)));
    const auto resp = co_await pending.wait();
    if (!resp.ok()) throw VoyageEmbeddingsError("Voyage embeddings failed (" + rt::numberToString(resp.status()) + ").", resp.status(), extractRetryAfterMs(resp.headers().get("Retry-After")));
    json::value payload;
    try { payload = co_await jsAwait(resp.json()); } catch (...) { payload = nullptr; }
    // `.catch(() => null)` is an additional promise reaction before the await.
    co_await rt::microtask();
    co_return parseVoyageEmbeddingsPayload(payload);
  }
}
