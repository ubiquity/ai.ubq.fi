#include "uos/provider/surplus.hpp"
#include "uos/rt/memory.hpp"

#include "uos/inference_deadline.hpp"
#include "uos/provider/anthropic_chat_responses.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/surplus_claude_chat_request.hpp"
#include "uos/provider/surplus_claude_request.hpp"
#include "uos/responses_stream.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <cmath>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace uos::provider::surplus {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    using JsonRecord = json::object;
    using responses_stream::ResponsesStreamEvent;
    using surplus_claude_chat_request::SurplusClaudeChatRequest;
    using AttemptHandle = sentinel::upstream_capture::SentinelUpstreamAttemptHandle;
    using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;

    // `${SURPLUS_BASE_URL}/v1/...`
    constexpr std::string_view SURPLUS_MODELS_URL = "https://api.surplusintelligence.ai/v1/models";
    constexpr std::string_view SURPLUS_RESPONSES_URL = "https://api.surplusintelligence.ai/v1/responses";
    constexpr std::string_view SURPLUS_CHAT_URL = "https://api.surplusintelligence.ai/v1/chat/completions";
    constexpr double SURPLUS_MODELS_FAILURE_BACKOFF_MS = 30'000;

    json::value text(std::string_view value) { return json::value(json::string(value)); }
    bool equals(const json::value* value, std::string_view expected) { return value && value->is_string() && value->get_string() == expected; }

    bool isRecord(const json::value* value) { return rt::isRecord(value); }

    std::optional<std::string> nonEmptyText(std::string_view value) {
      std::string trimmed = rt::trim(value);
      if (trimmed.empty()) return std::nullopt;
      return trimmed;
    }
    std::optional<std::string> nonEmptyString(const json::value* value) {
      if (!value || !value->is_string()) return std::nullopt;
      return nonEmptyText(value->get_string());
    }
    std::optional<std::string> nonEmptyString(const std::optional<std::string>& value) { return value ? nonEmptyText(*value) : std::nullopt; }

    bool isNonNegativeSafeInteger(const json::value* value) {
      return value && rt::isNumber(*value) && rt::Number::isSafeInteger(*value) && rt::toNumber(*value) >= 0;
    }

    std::optional<double> nonNegativeNumber(const json::value* value) {
      if (value && rt::isNumber(*value) && rt::Number::isFinite(*value) && rt::toNumber(*value) >= 0) return rt::toNumber(*value);
      if (!value || !value->is_string() || rt::trim(value->get_string()).empty()) return std::nullopt;
      const double parsed = rt::stringToNumber(value->get_string());
      return std::isfinite(parsed) && parsed >= 0 ? std::optional<double>(parsed) : std::nullopt;
    }

    std::unordered_set<std::string> readStringSet(const json::value* value) {
      std::unordered_set<std::string> set;
      if (value && value->is_array())
        for (const json::value& entry : value->get_array())
          if (entry.is_string())
            if (std::string trimmed = rt::trim(entry.get_string()); !trimmed.empty()) set.insert(std::move(trimmed));
      return set;
    }

    // `Readonly<{ supports_tools?: true; supports_parallel_tool_calls?: true }>`
    struct ModelCapabilities {
      std::optional<bool> supports_tools;
      std::optional<bool> supports_parallel_tool_calls;
    };

    ModelCapabilities readModelCapabilities(const JsonRecord& value) {
      const std::unordered_set<std::string> parameters = readStringSet(rt::get(value, "supported_parameters"));
      const std::unordered_set<std::string> features = readStringSet(rt::get(value, "supported_features"));
      ModelCapabilities capabilities;
      if (parameters.contains("tools") && parameters.contains("tool_choice") && features.contains("tools")) capabilities.supports_tools = true;
      if (parameters.contains("parallel_tool_calls")) capabilities.supports_parallel_tool_calls = true;
      return capabilities;
    }

    struct TextModalities {
      bool inputKnown;
      bool outputKnown;
      bool inputText;
      bool outputText;
    };

    bool hasTextModality(std::string_view part) {
      static const rt::Regex SEPARATOR("[+,]");
      for (const std::string& entry : rt::split(part, SEPARATOR))
        if (rt::trim(entry) == "text") return true;
      return false;
    }

    std::vector<std::string> stringEntries(const json::value* value) {
      std::vector<std::string> entries;
      if (value && value->is_array())
        for (const json::value& entry : value->get_array())
          if (entry.is_string()) entries.emplace_back(entry.get_string());
      return entries;
    }

    TextModalities readTextModalities(const json::value* value) {
      if (!isRecord(value)) return {.inputKnown = false, .outputKnown = false, .inputText = false, .outputText = false};
      bool inputKnown = false;
      bool outputKnown = false;
      bool inputText = false;
      bool outputText = false;
      const std::vector<std::string> inputModalities = stringEntries(rt::get(*value, "input_modalities"));
      if (!inputModalities.empty()) {
        inputKnown = true;
        inputText = rt::includes(inputModalities, std::string("text"));
      }
      const std::vector<std::string> outputModalities = stringEntries(rt::get(*value, "output_modalities"));
      if (!outputModalities.empty()) {
        outputKnown = true;
        outputText = rt::includes(outputModalities, std::string("text"));
      }
      if (const std::optional<std::string> modality = nonEmptyString(rt::get(*value, "modality"))) {
        const std::int64_t arrow = rt::lastIndexOf(*modality, "->");
        if (arrow >= 0) {
          inputKnown = true;
          outputKnown = true;
          inputText = hasTextModality(rt::slice(*modality, 0, arrow));
          outputText = hasTextModality(rt::slice(*modality, arrow + 2));
        } else {
          inputKnown = true;
          outputKnown = true;
          inputText = outputText = hasTextModality(*modality);
        }
      }
      return {.inputKnown = inputKnown, .outputKnown = outputKnown, .inputText = inputText, .outputText = outputText};
    }

    bool modelSupportsTextOutput(const JsonRecord& value) {
      const json::value* architecture = rt::get(value, "architecture");
      if (!isRecord(architecture)) return true;
      const TextModalities modalities = readTextModalities(architecture);
      return !modalities.outputKnown || modalities.outputText;
    }

    bool modelSupportsTextInput(const JsonRecord& value) {
      const json::value* architecture = rt::get(value, "architecture");
      if (!isRecord(architecture)) return true;
      const TextModalities modalities = readTextModalities(architecture);
      return !modalities.inputKnown || modalities.inputText;
    }

    // `a ?? b` over two unknown members.
    const json::value* firstPresent(const json::value* first, const json::value* second) { return rt::isNullish(first) ? second : first; }

    // `Readonly<{ input_price_per_token?; output_price_per_token?; cache_read_price_per_token?; cache_write_price_per_token? }>`
    struct Pricing {
      std::optional<double> input_price_per_token;
      std::optional<double> output_price_per_token;
      std::optional<double> cache_read_price_per_token;
      std::optional<double> cache_write_price_per_token;
    };

    Pricing readPricing(const JsonRecord& value) {
      const json::value* pricing = rt::get(value, "pricing");
      if (!isRecord(pricing)) return {};
      const auto member = [pricing](std::string_view key) { return rt::get(*pricing, key); };
      return {
        .input_price_per_token = nonNegativeNumber(firstPresent(member("prompt"), member("input"))),
        .output_price_per_token = nonNegativeNumber(firstPresent(member("completion"), member("output"))),
        .cache_read_price_per_token = nonNegativeNumber(firstPresent(firstPresent(member("input_cache_read"), member("cache_read")), member("cache_read_input"))),
        .cache_write_price_per_token =
          nonNegativeNumber(firstPresent(firstPresent(member("input_cache_write"), member("cache_write")), member("cache_write_input"))),
      };
    }

    std::optional<SurplusModel> readSurplusModel(const json::value& value) {
      if (!isRecord(&value) || !modelSupportsTextInput(value.get_object()) || !modelSupportsTextOutput(value.get_object())) return std::nullopt;
      const JsonRecord& upstream = value.get_object();
      std::optional<std::string> id = nonEmptyString(rt::get(upstream, "id"));
      if (!id) return std::nullopt;
      const json::value* createdValue = rt::get(upstream, "created");
      const double created = isNonNegativeSafeInteger(createdValue) ? rt::toNumber(*createdValue) : 0;
      std::optional<std::string> ownedBy = nonEmptyString(rt::get(upstream, "provider"));
      if (!ownedBy) ownedBy = nonEmptyString(rt::get(upstream, "owned_by"));
      std::optional<std::string> description = nonEmptyString(rt::get(upstream, "description"));
      const json::value* topProvider = rt::get(upstream, "top_provider");
      const json::value* maxOutputTokens = isRecord(topProvider) ? rt::get(*topProvider, "max_completion_tokens") : nullptr;
      const ModelCapabilities capabilities = readModelCapabilities(upstream);
      const Pricing pricing = readPricing(upstream);
      SurplusModel model{
        .id = std::move(*id),
        .object = "model",
        .created = created,
        .owned_by = ownedBy.value_or("surplus"),
        .supported_endpoint_types = {"openai", "openai-response"},
      };
      model.supports_tools = capabilities.supports_tools;
      model.supports_parallel_tool_calls = capabilities.supports_parallel_tool_calls;
      model.description = std::move(description);
      if (isNonNegativeSafeInteger(maxOutputTokens) && rt::toNumber(*maxOutputTokens) > 0) model.max_output_tokens = rt::toNumber(*maxOutputTokens);
      model.input_price_per_token = pricing.input_price_per_token;
      model.output_price_per_token = pricing.output_price_per_token;
      model.cache_read_price_per_token = pricing.cache_read_price_per_token;
      model.cache_write_price_per_token = pricing.cache_write_price_per_token;
      return model;
    }

    std::optional<SurplusModelsSnapshot> surplusModelsCache;
    std::optional<rt::Promise<std::optional<SurplusModelsSnapshot>>> surplusModelsFetchInFlight;
    double surplusModelsFetchGeneration = 0;
    double surplusModelsCacheGeneration = 0;
    double surplusModelsRetryAfterMs = 0;

    rt::AbortSignal boundedModelsSignal(const std::optional<rt::AbortSignal>& signal) {
      const rt::AbortSignal timeout = rt::AbortSignal::timeout(SURPLUS_FETCH_TIMEOUT_MS);
      return signal ? rt::AbortSignal::any({*signal, timeout}) : timeout;
    }

    void markSurplusModelsFetchFailure(double requestGeneration) {
      if (requestGeneration < surplusModelsCacheGeneration) return;
      surplusModelsRetryAfterMs = rt::dateNow() + SURPLUS_MODELS_FAILURE_BACKOFF_MS;
    }

    // The discovery request `fetchSurplusModels` starts (the TS's inline async function).
    rt::Task<std::optional<SurplusModelsSnapshot>> requestSurplusModels(SurplusFetch fetcher, std::optional<rt::AbortSignal> callerSignal, double requestGeneration) {
      const rt::AbortSignal signal = boundedModelsSignal(callerSignal);
      rt::Reason failure;
      try {
        rt::RequestInit init;
        init.method = "GET";
        init.headers = rt::Headers{{"Accept", "application/json"}};
        init.redirect = "manual";
        init.signal = signal;
        const SurplusFetch transport = fetcher ? fetcher : rt::globalFetch();
        const rt::Promise<rt::Response> fetching(transport(std::string(SURPLUS_MODELS_URL), std::move(init)));
        const rt::Response response = co_await fetching.wait();
        if (!response.ok()) {
          markSurplusModelsFetchFailure(requestGeneration);
          co_return surplusModelsCache;
        }
        const rt::Promise<json::value> reading(response.json());
        const json::value payload = co_await reading.wait();
        const json::array* data = isRecord(&payload) ? rt::getArray(payload, "data") : nullptr;
        if (!data) {
          markSurplusModelsFetchFailure(requestGeneration);
          co_return surplusModelsCache;
        }
        std::vector<SurplusModel> models;
        std::unordered_set<std::string> seen;
        for (const json::value& value : *data) {
          std::optional<SurplusModel> model = readSurplusModel(value);
          if (!model || seen.contains(model->id)) continue;
          seen.insert(model->id);
          models.push_back(std::move(*model));
        }
        SurplusModelsSnapshot snapshot{.models = std::move(models), .updated_at_ms = rt::dateNow()};
        if (requestGeneration > surplusModelsCacheGeneration) {
          surplusModelsCacheGeneration = requestGeneration;
          surplusModelsCache = std::move(snapshot);
          const auto& cachedModels = surplusModelsCache->models;
          std::size_t bytes = cachedModels.capacity() * sizeof(cachedModels.front());
          for (const auto& model : cachedModels) {
            bytes += rt::memory::bytes(model.id) + rt::memory::bytes(model.object) + rt::memory::bytes(model.owned_by) +
                     rt::memory::bytes(model.supported_endpoint_types);
            if (model.description) bytes += rt::memory::bytes(*model.description);
          }
          rt::memory::setCache(rt::memory::Cache::surplus_models, cachedModels.size(), bytes);
          surplusModelsRetryAfterMs = 0;
        }
        co_return surplusModelsCache;
      } catch (...) {
        failure = std::current_exception();
      }
      if (signal.aborted() && callerSignal && callerSignal->aborted()) std::rethrow_exception(failure);
      markSurplusModelsFetchFailure(requestGeneration);
      co_return surplusModelsCache;
    }

    std::string requireSurplusApiKey(const rt::Nullable<std::string>& supplied) {
      const std::optional<std::string> apiKey = supplied.isUndefined() ? readSurplusApiKey() : (supplied.hasValue() ? nonEmptyText(*supplied) : std::nullopt);
      if (apiKey) return *apiKey;
      throw SurplusError("Surplus paid fallback is unavailable because SURPLUS_API_KEY is not configured.", "surplus_api_key_missing", 503);
    }

    // `reason ?? fallback` on a caught value: undefined (a null Reason) and null are nullish.
    bool nullishReason(const rt::Reason& reason) {
      if (!reason) return true;
      try {
        std::rethrow_exception(reason);
      } catch (const rt::ThrownValue& thrown) {
        return !thrown.value || thrown.value->is_null();
      } catch (...) {
        return false;
      }
    }
    rt::Reason reasonOr(const rt::Reason& reason, const rt::Reason& fallback) { return nullishReason(reason) ? fallback : reason; }

    // Rejection reasons must be Errors. Deno's DOMException is an Error, so a real
    // abort reason is always passed through unchanged; only a missing or exotic
    // reason becomes the canonical AbortError the call sites already expect.
    rt::Reason abortRejectionReason(const rt::Reason& reason) {
      return rt::isError(reason) ? reason : rt::makeReason(rt::DOMException("Aborted", "AbortError"));
    }

    // A failed upstream operation keeps its original Error so callers can still
    // match on it; anything else is wrapped, which leaves the failure classification
    // in fetchSurplusResponses unchanged.
    rt::Reason requestFailureReason(rt::Reason reason) {
      if (rt::isError(reason)) return reason;
      return rt::makeReason(rt::withCause(rt::JsError("Surplus Responses request failed."), std::move(reason)));
    }

    // The settled flag and the abort listener of one awaitWithAbort call. The listener holds
    // the state weakly; the operation's reactions hold it until the operation settles.
    struct AbortRace {
      rt::AbortSignal signal;
      bool settled = false;
      rt::EventListener onAbort;

      explicit AbortRace(rt::AbortSignal signal_) : signal(std::move(signal_)) {}
      ~AbortRace() {
        if (!settled) signal.removeEventListener("abort", onAbort);
      }

      bool finish() {
        if (settled) return false;
        settled = true;
        signal.removeEventListener("abort", onAbort);
        return true;
      }
    };

    template <class T> rt::Promise<T> awaitWithAbort(rt::Promise<T> operation, rt::AbortSignal signal) {
      const rt::Resolvers<T> resolvers = rt::withResolvers<T>();
      const rt::Resolve<T> resolve = resolvers.resolve;
      const rt::Reject reject = resolvers.reject;
      auto race = std::make_shared<AbortRace>(std::move(signal));
      const std::weak_ptr<AbortRace> weakRace = race;
      race->onAbort = rt::EventListener([weakRace, reject]() {
        const std::shared_ptr<AbortRace> state = weakRace.lock();
        if (!state || !state->finish()) return;
        reject(abortRejectionReason(state->signal.reason()));
      });
      if (race->signal.aborted()) {
        race->onAbort();
        return resolvers.promise;
      }
      race->signal.addEventListener("abort", race->onAbort, {.once = true});
      (void)operation.then(
        [race, resolve](T value) {
          if (race->finish()) resolve(std::move(value));
        },
        [race, reject](rt::Reason error) {
          if (race->finish()) reject(requestFailureReason(std::move(error)));
        });
      return resolvers.promise;
    }

    rt::Headers authenticatedHeaders(const std::string& apiKey) {
      rt::Headers headers{
        {"Authorization", "Bearer " + apiKey},
        {"Accept", "text/event-stream"},
        {"Content-Type", "application/json"},
        {"X-SI-Mask-Secrets", "on"},
      };
      return headers;
    }

    std::optional<std::string> responseRequestId(const rt::Response& response) {
      std::optional<std::string> header = response.headers().get("X-Request-Id");
      if (!header) header = response.headers().get("X-Api-Request-Id");
      if (!header) header = response.headers().get("X-Oneapi-Request-Id");
      return nonEmptyString(header);
    }

    rt::ByteChunk encodeResponsesEvent(const JsonRecord& value) {
      return rt::encodeUtf8("event: " + rt::toString(rt::get(value, "type")) + "\ndata: " + rt::jsonStringify(json::value(value)) + "\n\n");
    }

    // Indices arrive as JSON primitives. Render only those explicitly: `String()` on
    // an object would silently produce "[object Object]" as a part key.
    std::string responsesPartIndex(const json::value* value) {
      return value && (rt::isNumber(*value) || value->is_string()) ? rt::toString(*value) : "0";
    }

    std::string responsesPartKey(const JsonRecord& value) {
      return rt::concat(responsesPartIndex(rt::get(value, "output_index")) + ":", responsesPartIndex(rt::get(value, "content_index")));
    }

    struct SurplusTextPartState {
      rt::OrderedMap<std::string, std::string> textParts;
      rt::OrderedSet<std::string> doneTextParts;
      bool sawMessageDone = false;
    };

    void applyResponsesTextEvent(SurplusTextPartState& state, const ResponsesStreamEvent& event) {
      const JsonRecord& value = event.value;
      if (event.type == "response.output_text.delta") {
        const std::string key = responsesPartKey(value);
        const std::string* previous = state.textParts.get(key);
        const json::value* delta = rt::get(value, "delta");
        const std::string appended = delta && delta->is_string() ? std::string(delta->get_string()) : std::string();
        state.textParts.set(key, rt::concat(previous ? *previous : std::string(), appended));
        return;
      }
      if (event.type == "response.output_text.done") {
        state.doneTextParts.add(responsesPartKey(value));
        return;
      }
      const json::value* item = rt::get(value, "item");
      if (event.type == "response.output_item.done" && isRecord(item) && equals(rt::get(*item, "type"), "message")) {
        state.sawMessageDone = true;
      }
    }

    // Canonical events emitted for the terminal `response.completed` event: the
    // buffered text parts as `response.output_text.done`, then the item and the
    // completed event itself.
    std::vector<rt::ByteChunk> completedResponsesEvents(const SurplusTextPartState& state, const JsonRecord& value) {
      std::vector<rt::ByteChunk> chunks;
      std::vector<std::pair<std::string, std::string>> orderedParts = rt::arrayFrom(state.textParts);
      rt::sort(orderedParts, [](const std::pair<std::string, std::string>& left, const std::pair<std::string, std::string>& right) {
        return rt::localeCompare(left.first, right.first);
      });
      for (const auto& [key, partText] : orderedParts) {
        if (state.doneTextParts.has(key)) continue;
        // `key.split(":").map(Number)`
        const std::vector<std::string> indices = rt::split(key, ":");
        JsonRecord done;
        done["type"] = "response.output_text.done";
        done["output_index"] = rt::makeNumber(rt::stringToNumber(indices.at(0)));
        if (indices.size() > 1) done["content_index"] = rt::makeNumber(rt::stringToNumber(indices.at(1)));
        done["text"] = text(partText);
        chunks.push_back(encodeResponsesEvent(done));
      }
      std::vector<std::string> parts;
      for (const auto& [key, partText] : orderedParts) parts.push_back(partText);
      const std::string joined = rt::join(parts, "");
      JsonRecord content;
      content["type"] = "output_text";
      content["text"] = text(joined);
      content["annotations"] = json::array();
      JsonRecord message;
      message["type"] = "message";
      message["role"] = "assistant";
      message["status"] = "completed";
      message["content"] = json::array{json::value(content)};
      if (!joined.empty() && !state.sawMessageDone) {
        JsonRecord itemDone;
        itemDone["type"] = "response.output_item.done";
        itemDone["output_index"] = 0;
        itemDone["item"] = message;
        chunks.push_back(encodeResponsesEvent(itemDone));
      }
      const json::value* response = rt::get(value, "response");
      if (isRecord(response)) {
        JsonRecord completedResponse = rt::objectSpread(response->get_object());
        if (!joined.empty()) rt::objectDefine(completedResponse, "output", json::array{json::value(message)});
        JsonRecord completed = rt::objectSpread(value);
        rt::objectDefine(completed, "response", json::value(std::move(completedResponse)));
        chunks.push_back(encodeResponsesEvent(completed));
      } else {
        chunks.push_back(encodeResponsesEvent(value));
      }
      return chunks;
    }

    rt::Task<void> normalizedStreamBody(responses_stream::ResponsesStreamIterator source, std::shared_ptr<SurplusTextPartState> state, rt::Yield<rt::ByteChunk> yield) {
      // for await (const event of source) { ... }
      for (;;) {
        rt::IteratorResult<ResponsesStreamEvent> step = co_await source.next();
        if (step.done) co_return;
        rt::Reason failure;
        bool returning = false;
        try {
          const ResponsesStreamEvent& event = *step.value;
          applyResponsesTextEvent(*state, event);
          if (event.type != "response.completed") {
            rt::ByteChunk encoded = encodeResponsesEvent(event.value);
            co_await yield(std::move(encoded));
          } else {
            std::vector<rt::ByteChunk> chunks = completedResponsesEvents(*state, event.value);
            for (std::size_t index = 0; index < chunks.size(); ++index) {
              rt::ByteChunk chunk = std::move(chunks[index]);
              co_await yield(std::move(chunk));
            }
          }
        } catch (const rt::GeneratorReturn&) {
          returning = true;
        } catch (...) {
          failure = std::current_exception();
        }
        if (failure) {
          // Leaving the loop by a throw closes the iterator and keeps the throw.
          try {
            co_await source.return_();
          } catch (...) {
          }
          std::rethrow_exception(failure);
        }
        if (returning) {
          co_await source.return_();
          throw rt::GeneratorReturn{};
        }
      }
    }

    struct NormalizedStreamState {
      rt::AsyncGenerator<rt::ByteChunk> iterator;
      rt::AbortController consumerAbort;
    };

    rt::Task<void> normalizedPull(std::shared_ptr<NormalizedStreamState> state, Controller controller) {
      rt::Reason failure;
      try {
        rt::IteratorResult<rt::ByteChunk> next = co_await state->iterator.next();
        if (next.done) controller.close();
        else controller.enqueue(std::move(*next.value));
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) controller.error(failure);
    }

    rt::Task<void> normalizedCancel(std::shared_ptr<NormalizedStreamState> state, rt::Reason reason) {
      state->consumerAbort.abort(reason);
      // `await iterator.return(reason).catch(() => {})`
      try {
        co_await state->iterator.return_();
      } catch (...) {
      }
      co_await rt::microtask();
    }

    rt::Response normalizeSurplusResponsesStream(const rt::Response& response) {
      const std::optional<rt::ByteStream> body = response.body();
      const std::optional<std::string> contentType = response.headers().get("content-type");
      if (!body || !contentType || !rt::includes(rt::toLowerCase(*contentType), "text/event-stream")) {
        return response;
      }
      const rt::AbortController consumerAbort;
      responses_stream::ResponsesStreamIterator source = responses_stream::readResponsesStream(*body, consumerAbort.signal());
      const auto partState = std::make_shared<SurplusTextPartState>();
      rt::AsyncGenerator<rt::ByteChunk> iterator([source, partState](rt::Yield<rt::ByteChunk> yield) {
        return normalizedStreamBody(source, partState, std::move(yield));
      });
      const auto state = std::make_shared<NormalizedStreamState>(NormalizedStreamState{.iterator = std::move(iterator), .consumerAbort = consumerAbort});
      rt::UnderlyingSource<rt::ByteChunk> underlying;
      underlying.pull = [state](Controller controller) { return normalizedPull(state, std::move(controller)); };
      underlying.cancel = [state](rt::Reason reason) { return normalizedCancel(state, std::move(reason)); };
      rt::ResponseInit init;
      init.status = response.status();
      init.statusText = response.statusText();
      init.headers = response.headers();
      return rt::Response(rt::BodyInit(rt::ByteStream(std::move(underlying))), std::move(init));
    }

    JsonRecord toSurplusResponsesBody(const JsonRecord& body, std::optional<bool> supportsParallelToolCalls) {
      std::optional<JsonRecord> forwardedBody;
      if (const json::array* originalInput = rt::getArray(body, "input")) {
        json::array input;
        bool changed = false;
        for (const json::value& item : *originalInput) {
          if (isRecord(&item) && equals(rt::get(item, "type"), "message") && equals(rt::get(item, "role"), "developer")) {
            JsonRecord system = rt::objectSpread(item.get_object());
            rt::objectDefine(system, "role", json::value("system"));
            input.push_back(json::value(std::move(system)));
            changed = true;
          } else {
            input.push_back(item);
          }
        }
        if (changed) {
          forwardedBody = rt::objectSpread(body);
          rt::objectDefine(*forwardedBody, "input", json::value(std::move(input)));
        }
      }
      if (supportsParallelToolCalls == false && rt::hasOwn(body, "parallel_tool_calls")) {
        if (!forwardedBody) forwardedBody = rt::objectSpread(body);
        rt::objectDelete(*forwardedBody, "parallel_tool_calls");
      }
      const JsonRecord& current = forwardedBody ? *forwardedBody : body;
      const json::value* reasoning = rt::get(current, "reasoning");
      if (!isRecord(reasoning) || !equals(rt::get(*reasoning, "effort"), "ultra")) {
        return current;
      }
      JsonRecord translatedReasoning = rt::objectSpread(reasoning->get_object());
      rt::objectDefine(translatedReasoning, "effort", json::value("max"));
      JsonRecord result = rt::objectSpread(current);
      rt::objectDefine(result, "reasoning", json::value(std::move(translatedReasoning)));
      return result;
    }

    bool isApiKeyQuotaDispatchError(const rt::Reason& reason) {
      if (!reason) return false;
      try {
        std::rethrow_exception(reason);
      } catch (const api_key_policy::ApiKeyQuotaDispatchError&) {
        return true;
      } catch (...) {
        return false;
      }
    }

    // Classify a failed transport attempt: preserve the errors callers already
    // handle, rethrow an abort reason when a signal caused it, and otherwise report
    // the upstream as unreachable.
    rt::Reason surplusTransportFailure(const rt::Reason& error, const std::optional<rt::AbortSignal>& signal, const rt::AbortController& headersDeadline) {
      if (isApiKeyQuotaDispatchError(error)) return error;
      if (signal && signal->aborted()) return reasonOr(signal->reason(), error);
      if (headersDeadline.signal().aborted()) return reasonOr(headersDeadline.signal().reason(), error);
      if (rt::isError(error) && rt::errorName(error) == "AbortError") return error;
      return rt::makeReason(SurplusError("Surplus Responses request could not reach the upstream service.", "surplus_upstream_unreachable", 502));
    }

    std::string surplusRequestUrl(const std::optional<SurplusClaudeChatRequest>& native) {
      return std::string(native ? SURPLUS_CHAT_URL : SURPLUS_RESPONSES_URL);
    }

    rt::Task<rt::Response> deliverSurplusResponse(rt::Response response, JsonRecord body, SurplusAuthenticatedFetchOptions options,
                                                  std::optional<SurplusClaudeChatRequest> native, std::optional<std::string> requestId) {
      if (native) {
        anthropic_chat_responses::AnthropicChatResponsesOptions normalizeOptions;
        normalizeOptions.model = rt::toString(rt::get(body, "model"));
        normalizeOptions.requestId = requestId;
        normalizeOptions.signal = options.signal;
        const json::value* reasoning = rt::get(body, "reasoning");
        const json::value* summary = isRecord(reasoning) ? rt::get(*reasoning, "summary") : nullptr;
        if (summary && summary->is_string()) normalizeOptions.reasoningSummary = std::string(summary->get_string());
        normalizeOptions.reasoningReplayScope = options.reasoningReplayScope;
        normalizeOptions.provider = "surplus";
        rt::OrderedMap<std::string, anthropic_response_wire::ClaudeToolName> toolNames;
        for (const auto& [projected, original] : native->toolNames)
          toolNames.set(projected, anthropic_response_wire::ClaudeToolName{.name = original.name, .namespace_ = original.namespace_});
        normalizeOptions.toolNames = std::move(toolNames);
        normalizeOptions.customToolNames = native->customToolNames;
        const rt::Promise<rt::Response> normalizing(anthropic_chat_responses::normalizeAnthropicChatResponsesResponse(response, std::move(normalizeOptions)));
        rt::Response normalized = co_await normalizing.wait();
        co_return normalized;
      }
      const rt::Response delivered = normalizeSurplusResponsesStream(response);
      if (!surplus_claude_request::isSurplusClaudeModel(rt::get(body, "model"))) co_return delivered;
      rt::Headers warnings(delivered.headers());
      const std::optional<std::string> previous = warnings.get("x-uos-warning");
      const std::string warning = "surplus_claude_native_prompt_cache_unavailable";
      warnings.set("x-uos-warning", previous && !previous->empty() ? *previous + ", " + warning : warning);
      rt::ResponseInit init;
      init.status = delivered.status();
      init.statusText = delivered.statusText();
      init.headers = std::move(warnings);
      co_return rt::Response(rt::bodyInit(delivered.body()), std::move(init));
    }
  }

  std::optional<std::string> readSurplusApiKey() {
    try {
      return nonEmptyString(rt::env::get(SURPLUS_API_KEY_ENV));
    } catch (...) {
      return std::nullopt;
    }
  }

  rt::Task<std::optional<SurplusModelsSnapshot>> fetchSurplusModels() { return fetchSurplusModels(FetchSurplusModelsOptions{}); }

  rt::Task<std::optional<SurplusModelsSnapshot>> fetchSurplusModels(FetchSurplusModelsOptions options) {
    const std::optional<std::string> apiKey =
      options.apiKey.isUndefined() ? readSurplusApiKey() : (options.apiKey.hasValue() ? nonEmptyText(*options.apiKey) : std::nullopt);
    if (options.requireApiKey.value_or(true) && !apiKey) co_return std::nullopt;
    const bool force = options.force.value_or(false);
    if (!force && surplusModelsCache && rt::dateNow() - surplusModelsCache->updated_at_ms < SURPLUS_MODELS_CACHE_TTL_MS) co_return surplusModelsCache;
    if (!force && rt::dateNow() < surplusModelsRetryAfterMs) co_return surplusModelsCache;
    if (options.cachedOnly.value_or(false)) co_return surplusModelsCache;
    // Only the ordinary discovery path shares an upstream request. Callers that
    // supply a signal, fetcher, API key, or force a refresh retain their own
    // request semantics and do not join another caller's request. `cachedOnly` is
    // already excluded by the early return above.
    const bool shouldCoalesce = !force && options.apiKey.isUndefined() && !options.fetcher && !options.signal && !options.requireApiKey;
    if (shouldCoalesce && surplusModelsFetchInFlight) {
      const rt::Promise<std::optional<SurplusModelsSnapshot>> inFlight = *surplusModelsFetchInFlight;
      std::optional<SurplusModelsSnapshot> shared = co_await inFlight.wait();
      co_return shared;
    }

    const double requestGeneration = ++surplusModelsFetchGeneration;
    const rt::Promise<std::optional<SurplusModelsSnapshot>> request(requestSurplusModels(options.fetcher, options.signal, requestGeneration));
    if (!shouldCoalesce) {
      std::optional<SurplusModelsSnapshot> own = co_await request.wait();
      co_return own;
    }
    surplusModelsFetchInFlight = request;
    const auto cleanup = rt::scopeExit([request] {
      if (surplusModelsFetchInFlight && *surplusModelsFetchInFlight == request) surplusModelsFetchInFlight.reset();
    });
    auto result = co_await request.wait();
    co_return result;
  }

  std::optional<SurplusModelsSnapshot> surplusModelsSnapshot() { return surplusModelsCache; }

  void resetSurplusModelsCacheForTest() {
    surplusModelsFetchInFlight.reset();
    surplusModelsFetchGeneration += 1;
    surplusModelsCacheGeneration = surplusModelsFetchGeneration;
    surplusModelsRetryAfterMs = 0;
    surplusModelsCache.reset();
    rt::memory::setCache(rt::memory::Cache::surplus_models, 0, 0);
  }

  rt::Task<SurplusResponsesResult> fetchSurplusResponses(json::value body) { return fetchSurplusResponses(std::move(body), SurplusAuthenticatedFetchOptions{}); }

  rt::Task<SurplusResponsesResult> fetchSurplusResponses(json::value body, SurplusAuthenticatedFetchOptions options) {
    if (!isRecord(&body)) {
      throw SurplusError("Surplus Responses requests must use a canonical JSON object body.", "surplus_request_invalid", 400);
    }
    const JsonRecord record = body.get_object();

    std::optional<double> catalogMaxOutputTokens;
    if (surplusModelsCache) {
      const json::value* model = rt::get(record, "model");
      for (const SurplusModel& candidate : surplusModelsCache->models) {
        if (model && model->is_string() && candidate.id == std::string_view(model->get_string())) {
          catalogMaxOutputTokens = candidate.max_output_tokens;
          break;
        }
      }
    }
    surplus_claude_chat_request::ClaudeChatRequestOptions requestOptions;
    requestOptions.provider = "surplus";
    requestOptions.reasoningReplayScope = options.reasoningReplayScope;
    const std::optional<SurplusClaudeChatRequest> native =
      surplus_claude_chat_request::buildSurplusClaudeChatRequest(record, catalogMaxOutputTokens, requestOptions);
    std::optional<std::string> encodedBody;
    try {
      encodedBody = rt::jsonStringify(json::value(native ? native->body : toSurplusResponsesBody(record, options.supportsParallelToolCalls)));
    } catch (...) {
      throw SurplusError("Surplus Responses requests must use a JSON-serializable body.", "surplus_request_invalid", 400);
    }
    if (!encodedBody) {
      throw SurplusError("Surplus Responses requests must use a JSON-serializable body.", "surplus_request_invalid", 400);
    }

    const std::string apiKey = requireSurplusApiKey(options.apiKey);
    const rt::Headers headers = authenticatedHeaders(apiKey);
    const rt::AbortController headersDeadline;
    std::optional<rt::Response> response;
    std::optional<AttemptHandle> upstreamAttempt;
    {
      const rt::Timeout headersTimer = rt::setTimeout(
        [headersDeadline] { headersDeadline.abort(rt::makeReason(rt::DOMException("Surplus response headers timed out.", "TimeoutError"))); },
        inference_deadline::STREAM_FIRST_EVENT_DEADLINE_MS);
      const auto clearHeadersTimer = rt::scopeExit([headersTimer] { rt::clearTimeout(headersTimer); });
      const rt::AbortSignal signal = options.signal ? rt::AbortSignal::any({*options.signal, headersDeadline.signal()}) : headersDeadline.signal();
      rt::Reason failure;
      try {
        std::optional<api_key_policy::ApiKeyProviderDispatch> dispatch;
        if (options.beforeDispatch) {
          const rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>> admitted(options.beforeDispatch());
          dispatch = co_await admitted.wait();
        }
        if (signal.aborted()) {
          if (dispatch) {
            const rt::Promise<void> cancelled(dispatch->cancelBeforeTransport());
            co_await cancelled.wait();
          } else {
            co_await rt::microtask();
          }
          rt::rethrow(reasonOr(signal.reason(), rt::makeReason(rt::DOMException("The request was aborted.", "AbortError"))));
        }
        if (dispatch) dispatch->markTransportStarted();
        if (options.onDispatch) options.onDispatch();
        if (options.sentinelUpstreamRecorder) upstreamAttempt = options.sentinelUpstreamRecorder->startAttempt("surplus");
        rt::RequestInit init;
        init.method = "POST";
        init.headers = headers;
        init.body = rt::BodyInit(*encodedBody);
        init.redirect = "manual";
        init.signal = signal;
        const SurplusFetch fetcher = options.fetcher ? options.fetcher : rt::globalFetch();
        const rt::Promise<rt::Response> fetching(fetcher(surplusRequestUrl(native), std::move(init)));
        const rt::Promise<rt::Response> responded = awaitWithAbort(fetching, signal);
        response = co_await responded.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        if (upstreamAttempt) upstreamAttempt->recordFetchError();
        rt::rethrow(surplusTransportFailure(failure, options.signal, headersDeadline));
      }
    }

    std::optional<std::string> requestId = responseRequestId(*response);
    const rt::Response recorded = upstreamAttempt ? upstreamAttempt->wrap(*response) : *response;
    const rt::Promise<rt::Response> delivering(deliverSurplusResponse(recorded, record, options, native, requestId));
    rt::Response delivered = co_await delivering.wait();
    SurplusResponsesResult result{.response = std::move(delivered), .request_id = std::move(requestId)};
    if (native) {
      result.prompt_cache_mode = native->promptCacheMode;
      result.explicit_breakpoint_count = native->explicitBreakpointCount;
    }
    co_return result;
  }

  // Members are written in the order readSurplusModel creates them (max_output_tokens
  // before the pricing fields), which is what JSON.stringify of a discovered model prints.
  void tag_invoke(const json::value_from_tag&, json::value& out, const SurplusModel& value) {
    out = Writer()
            .required("id", value.id)
            .required("object", value.object)
            .required("created", value.created)
            .required("owned_by", value.owned_by)
            .required("supported_endpoint_types", value.supported_endpoint_types)
            .orUndefined("supports_tools", value.supports_tools)
            .orUndefined("supports_parallel_tool_calls", value.supports_parallel_tool_calls)
            .orUndefined("description", value.description)
            .orUndefined("max_output_tokens", value.max_output_tokens)
            .orUndefined("input_price_per_token", value.input_price_per_token)
            .orUndefined("output_price_per_token", value.output_price_per_token)
            .orUndefined("cache_read_price_per_token", value.cache_read_price_per_token)
            .orUndefined("cache_write_price_per_token", value.cache_write_price_per_token)
            .done();
  }

  SurplusModel tag_invoke(const json::value_to_tag<SurplusModel>&, const json::value& value) {
    const Reader in(value, "SurplusModel");
    return SurplusModel{
      .id = in.required<std::string>("id"),
      .object = in.required<std::string>("object"),
      .created = in.required<double>("created"),
      .owned_by = in.required<std::string>("owned_by"),
      .supported_endpoint_types = in.required<std::vector<std::string>>("supported_endpoint_types"),
      .supports_tools = in.orUndefined<bool>("supports_tools"),
      .supports_parallel_tool_calls = in.orUndefined<bool>("supports_parallel_tool_calls"),
      .description = in.orUndefined<std::string>("description"),
      .input_price_per_token = in.orUndefined<double>("input_price_per_token"),
      .output_price_per_token = in.orUndefined<double>("output_price_per_token"),
      .cache_read_price_per_token = in.orUndefined<double>("cache_read_price_per_token"),
      .cache_write_price_per_token = in.orUndefined<double>("cache_write_price_per_token"),
      .max_output_tokens = in.orUndefined<double>("max_output_tokens"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SurplusModelsSnapshot& value) {
    out = Writer().required("models", value.models).required("updated_at_ms", value.updated_at_ms).done();
  }

  SurplusModelsSnapshot tag_invoke(const json::value_to_tag<SurplusModelsSnapshot>&, const json::value& value) {
    const Reader in(value, "SurplusModelsSnapshot");
    return SurplusModelsSnapshot{
      .models = in.required<std::vector<SurplusModel>>("models"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }
}
