/** Human names for the two Cerebras ids, shared with the Codex catalog rows. */

/**
 * The official DeepSeek ids are interchangeable aliases of one served model,
 * so both are advertised. Unlike the Cerebras entry, an id discovered from
 * another provider is REPLACED rather than skipped: once a request for that id
 * is dispatched to DeepSeek, a catalog row still naming the paid-fallback
 * provider (and its `["none"]` reasoning tiers) would misdescribe the route.
 */

/**
 * The eight LithosAI ids. Unlike the DeepSeek aliases these are eight distinct
 * selectable ids, so each id is advertised in its own right; a row another
 * provider published for one of them is REPLACED, because once a request for
 * that id dispatches to LithosAI a row still naming the other provider would
 * misdescribe the route it now takes.
 */

/** The gateway-owned DeepSeek waterfall id, listed whenever one hop is usable. */

/** The Typesafe System One decision model, served through the same upstream. */

/**
 * Everything this gateway can dispatch to OpenRouter: the cached public model
 * catalogue (OpenAI-compatible chat and responses wires) plus the Typesafe
 * System One decision model behind `/v1/systemone`. The dispatch path reads the
 * same snapshot, so a model listed here is a model that works, and a new
 * upstream model appears as soon as the cached list refreshes.
 */

/**
 * OpenRouter's served rows are published by the upstream and refresh on the
 * cache TTL, so the listing routes append them after the operator whitelist
 * instead of letting a curated list hide ids the upstream already serves.
 */

/**
 * Responses permits a function-call result to carry the same input content
 * blocks as a message. Normalize that known standard shape so cache
 * breakpoints are neither passed through unchecked nor omitted from telemetry.
 * Other Codex Responses extension items remain opaque passthrough values.
 */
#include "uos/input_normalization.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/deepseek/waterfall.hpp"
#include "uos/models/codex_models.hpp"
#include "uos/models/metadata.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/provider/gemini.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/recent_model_context.hpp"
#include "uos/request_policy.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"
#include <algorithm>
#include <initializer_list>
#include <unordered_set>
namespace uos::input_normalization {
  namespace {
    using namespace ::uos::types;
    namespace wf = deepseek::waterfall;
    namespace md = models::metadata;
    namespace ds = deepseek::index;
    namespace li = provider::lithos;
    namespace ce = provider::cerebras;
    template<class T> NormalizationResult<T> success(T value) { return NormalizationResultOk<T>{std::move(value)}; }
    NormalizationResult<void> successVoid() { return NormalizationResultOk<void>{}; }
    NormalizationResultNotOk invalidNormalizedField(const std::string& param, std::string message) { return {std::move(message), param}; }
    template<class T> const NormalizationResultNotOk* failure(const NormalizationResult<T>& result) { return std::get_if<NormalizationResultNotOk>(&result); }
    template<class T> const auto& resultValue(const NormalizationResult<T>& result) { return std::get<NormalizationResultOk<T>>(result).value; }
    std::optional<std::string> stringAt(const json::value& value, std::string_view field) { return utils::getString(rt::get(value, field)); }
    bool equals(const json::value* value, std::string_view expected) { return value && value->is_string() && value->as_string() == expected; }
    std::string findUnknownContentField(const json::object& value, std::initializer_list<std::string_view> allowed) {
      for (const auto& key : rt::objectKeys(value)) if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) return key;
      return "";
    }
    NormalizationResult<rt::Nullable<std::string>> parseImageDetail(const json::value* value, const std::string& param) {
      if (!value) return success(rt::Nullable<std::string>{});
      if (value->is_null()) return success(rt::Nullable<std::string>(nullptr));
      if (equals(value, "auto") || equals(value, "low") || equals(value, "high") || equals(value, "original")) return success(rt::Nullable<std::string>(utils::getString(value).value()));
      return invalidNormalizedField(param, param + " must be one of auto, low, high, or original");
    }
    NormalizationResult<std::optional<std::string>> parseInputFileDetail(const json::value* value, const std::string& param) {
      if (!value) return success(std::optional<std::string>{});
      if (equals(value, "auto") || equals(value, "low") || equals(value, "high")) return success(utils::getString(value));
      return invalidNormalizedField(param, param + " must be one of auto, low, or high");
    }
    NormalizationResult<std::optional<PromptCacheBreakpoint>> normalizePromptCacheBreakpoint(const json::value* value, const std::string& param) {
      if (!value) return success(std::optional<PromptCacheBreakpoint>{});
      if (!value->is_object()) return invalidNormalizedField(param, param + " must be an object");
      const auto unknown = findUnknownContentField(value->as_object(), {"mode"});
      if (!unknown.empty()) return invalidNormalizedField(param + "." + unknown, "Unknown cache breakpoint field: " + unknown);
      if (!equals(rt::get(*value, "mode"), "explicit")) return invalidNormalizedField(param + ".mode", param + ".mode must be explicit");
      return success(std::optional(PromptCacheBreakpoint{}));
    }
    NormalizationResult<MessageContentItem> normalizeChatTextContentPart(const json::object& part, const std::string& param, bool isAssistant, const std::string& textItemType) {
      const auto unknown = findUnknownContentField(part, {"type", "text", "prompt_cache_breakpoint"});
      if (!unknown.empty()) return invalidNormalizedField(param + "." + unknown, "Unknown content field: " + unknown);
      const auto text = utils::getString(rt::get(part, "text"));
      if (!text) return invalidNormalizedField(param + ".text", param + ".text must be a string");
      if (isAssistant && rt::get(part, "prompt_cache_breakpoint")) return invalidNormalizedField(param + ".prompt_cache_breakpoint", "prompt_cache_breakpoint is not supported for assistant output content in this gateway");
      const auto breakpoint = normalizePromptCacheBreakpoint(rt::get(part, "prompt_cache_breakpoint"), param + ".prompt_cache_breakpoint");
      if (const auto* error = failure(breakpoint)) return *error;
      if (textItemType == "input_text") return success(MessageContentItem(MessageContentItemInputText{*text, resultValue(breakpoint)}));
      return success(MessageContentItem(MessageContentItemOutputText{*text}));
    }
    NormalizationResult<MessageContentItem> normalizeChatRefusalContentPart(const json::object& part, const std::string& param, bool isAssistant, std::size_t partCount) {
      const auto unknown = findUnknownContentField(part, {"type", "refusal", "prompt_cache_breakpoint"});
      if (!unknown.empty()) return invalidNormalizedField(param + "." + unknown, "Unknown content field: " + unknown);
      if (!isAssistant) return invalidNormalizedField(param + ".type", param + ".type is only valid for assistant messages");
      if (partCount != 1) return invalidNormalizedField(param + ".type", "assistant refusal content must be the only part");
      const auto refusal = utils::getString(rt::get(part, "refusal"));
      if (!refusal) return invalidNormalizedField(param + ".refusal", param + ".refusal must be a string");
      if (rt::get(part, "prompt_cache_breakpoint")) return invalidNormalizedField(param + ".prompt_cache_breakpoint", "prompt_cache_breakpoint is not supported for refusal content in this gateway");
      return success(MessageContentItem(MessageContentItemOutputText{*refusal}));
    }
    NormalizationResult<MessageContentItem> normalizeChatImageContentPart(const json::object& part, const std::string& param, const std::string& role) {
      const auto unknown = findUnknownContentField(part, {"type", "image_url", "prompt_cache_breakpoint"});
      if (!unknown.empty()) return invalidNormalizedField(param + "." + unknown, "Unknown content field: " + unknown);
      if (role != "user") return invalidNormalizedField(param + ".type", param + ".type is only valid for user messages");
      const auto* image = rt::getObject(part, "image_url");
      if (!image) return invalidNormalizedField(param + ".image_url", param + ".image_url must be an object");
      const auto imageUnknown = findUnknownContentField(*image, {"url", "detail"});
      if (!imageUnknown.empty()) return invalidNormalizedField(param + ".image_url." + imageUnknown, "Unknown image_url field: " + imageUnknown);
      const auto url = utils::getString(rt::get(*image, "url"));
      if (!url || rt::trim(*url).empty()) return invalidNormalizedField(param + ".image_url.url", param + ".image_url.url must contain a URL");
      const auto detail = parseImageDetail(rt::get(*image, "detail"), param + ".image_url.detail");
      if (const auto* error = failure(detail)) return *error;
      const auto breakpoint = normalizePromptCacheBreakpoint(rt::get(part, "prompt_cache_breakpoint"), param + ".prompt_cache_breakpoint");
      if (const auto* error = failure(breakpoint)) return *error;
      return success(MessageContentItem(MessageContentItemInputImage{.image_url = rt::trim(*url), .detail = resultValue(detail), .prompt_cache_breakpoint = resultValue(breakpoint)}));
    }
    NormalizationResult<MessageContentItem> normalizeChatFileContentPart(const json::object& part, const std::string& param, const std::string& role) {
      const auto unknown = findUnknownContentField(part, {"type", "file", "prompt_cache_breakpoint"});
      if (!unknown.empty()) return invalidNormalizedField(param + "." + unknown, "Unknown content field: " + unknown);
      if (role != "user") return invalidNormalizedField(param + ".type", param + ".type is only valid for user messages");
      const auto* file = rt::getObject(part, "file");
      if (!file) return invalidNormalizedField(param + ".file", param + ".file must be an object");
      const auto fileUnknown = findUnknownContentField(*file, {"file_id", "file_data", "filename"});
      if (!fileUnknown.empty()) return invalidNormalizedField(param + ".file." + fileUnknown, "Unknown file field: " + fileUnknown);
      for (const auto field : {"file_id", "file_data", "filename"}) {
        const auto* value = rt::get(*file, field);
        if (value && !value->is_string()) return invalidNormalizedField(param + ".file." + field, param + ".file." + field + " must be a string");
      }
      const auto id = utils::getString(rt::get(*file, "file_id"));
      const auto data = utils::getString(rt::get(*file, "file_data"));
      if ((!id || rt::trim(*id).empty()) && (!data || rt::trim(*data).empty())) return invalidNormalizedField(param + ".file.file_id", param + ".file must include file_id or file_data");
      const auto breakpoint = normalizePromptCacheBreakpoint(rt::get(part, "prompt_cache_breakpoint"), param + ".prompt_cache_breakpoint");
      if (const auto* error = failure(breakpoint)) return *error;
      MessageContentItemInputFile item{.file_id = id, .file_data = data, .filename = rt::orUndefined(utils::getString(rt::get(*file, "filename"))), .prompt_cache_breakpoint = resultValue(breakpoint)};
      return success(MessageContentItem(item));
    }
    NormalizationResult<MessageContentItem> normalizeChatContentPart(const json::object& part, const std::string& param, const std::string& role, bool isAssistant, const std::string& textItemType, std::size_t partCount) {
      const auto type = utils::getString(rt::get(part, "type"));
      if (type == "text") return normalizeChatTextContentPart(part, param, isAssistant, textItemType);
      if (type == "refusal") return normalizeChatRefusalContentPart(part, param, isAssistant, partCount);
      if (type == "image_url") return normalizeChatImageContentPart(part, param, role);
      if (type == "file") return normalizeChatFileContentPart(part, param, role);
      if (type == "input_audio" && rt::hasOwn(part, "prompt_cache_breakpoint")) return invalidNormalizedField(param + ".prompt_cache_breakpoint", "prompt_cache_breakpoint is not supported for input_audio content in this gateway");
      return invalidNormalizedField(param + ".type", param + ".type is not supported");
    }
    NormalizationResult<std::vector<MessageContentItem>> normalizeChatContentItems(const std::string& role, const json::value* content, const std::string& param) {
      const bool isAssistant = role == "assistant";
      const std::string textItemType = isAssistant ? "output_text" : "input_text";
      if (content && content->is_string()) {
        MessageContentItem item = isAssistant ? MessageContentItem(MessageContentItemOutputText{utils::getString(content).value()}) : MessageContentItem(MessageContentItemInputText{utils::getString(content).value()});
        return success(std::vector<MessageContentItem>{std::move(item)});
      }
      if (content && content->is_null() && isAssistant) return success(std::vector<MessageContentItem>{});
      if (!content || !content->is_array()) return invalidNormalizedField(param, param + " must be a string or an array");
      std::vector<MessageContentItem> items;
      const auto& parts = content->as_array();
      for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto partParam = param + "[" + rt::numberToString(static_cast<double>(index)) + "]";
        if (!parts[index].is_object()) return invalidNormalizedField(partParam, partParam + " must be an object");
        const auto normalized = normalizeChatContentPart(parts[index].as_object(), partParam, role, isAssistant, textItemType, parts.size());
        if (const auto* error = failure(normalized)) return *error;
        items.push_back(resultValue(normalized));
      }
      return success(items);
    }
    std::string messageContentToText(const std::vector<MessageContentItem>& items) {
      std::vector<std::string> text;
      for (const auto& item : items) {
        const auto value = json::value_from(item);
        const auto type = stringAt(value, "type");
        const auto content = stringAt(value, "text");
        if ((type == "input_text" || type == "output_text") && content && !rt::trim(*content).empty()) text.push_back(*content);
      }
      return rt::join(text, "\n");
    }
    std::optional<std::string> chatRoleToCodexRole(const std::string& role) {
      if (role == "system" || role == "developer" || role == "tool") return "developer";
      if (role == "user" || role == "assistant") return role;
      return std::nullopt;
    }
    std::optional<double> normalizeUnixSeconds(const json::value* value) {
      if (!value || !rt::isNumber(*value) || !rt::Number::isFinite(rt::toNumber(*value))) return std::nullopt;
      const double seconds = rt::Math::trunc(rt::toNumber(*value));
      return seconds >= 0 ? std::optional(seconds) : std::nullopt;
    }
    std::optional<json::object> normalizeModelEntry(const json::value& value, double fallbackCreated) {
      if (!utils::isRecord(value)) return std::nullopt;
      auto id = stringAt(value, "id");
      for (const auto field : {"slug", "model", "name"}) if (!id) id = stringAt(value, field);
      if (!id || id->empty()) return std::nullopt;
      return json::object{{"id", *id}, {"object", "model"}, {"created", rt::makeNumber(normalizeUnixSeconds(rt::get(value, "created")).value_or(fallbackCreated))}, {"owned_by", stringAt(value, "owned_by").value_or("openai")}};
    }
    template<class Range> json::array configuredModels(const Range& ids, std::string_view owner) {
      json::array result;
      for (const auto id : ids) result.push_back(json::object{{"id", id}, {"object", "model"}, {"created", 0}, {"owned_by", owner}});
      return result;
    }
    template<class Range> json::array strings(const Range& values) {
      json::array result;
      for (const auto& value : values) result.push_back(json::value(std::string(value)));
      return result;
    }
    void contextFields(json::object& result, const md::ResolvedModelMetadata& resolved, bool includePercent = true) {
      rt::objectSet(result, "context_window_tokens", rt::orNull(resolved.context_window_tokens));
      rt::objectSet(result, "max_context_window_tokens", rt::orNull(resolved.max_context_window_tokens));
      rt::objectSet(result, "auto_compact_token_limit_tokens", rt::orNull(resolved.auto_compact_token_limit_tokens));
      if (includePercent && resolved.effective_context_window_percent) rt::objectSet(result, "effective_context_window_percent", *resolved.effective_context_window_percent);
      rt::objectSet(result, "context_source", json::value(resolved.context_source));
    }
    json::array configuredDeepSeekModelCapabilities() {
      if (!ds::readDeepSeekApiKey()) return {};
      json::array result;
      for (const auto id : ds::DEEPSEEK_OFFICIAL_MODEL_IDS) {
        md::ModelMetadataSources sources;
        sources.provider = md::ModelMetadataHint{.context_window_tokens = ds::DEEPSEEK_CONTEXT_WINDOW_TOKENS, .max_context_window_tokens = ds::DEEPSEEK_CONTEXT_WINDOW_TOKENS};
        const auto resolved = md::resolveModelMetadata(id, sources);
        const auto* displayName = ds::DEEPSEEK_DISPLAY_NAMES.get(id);
        json::object row{{"id", id}, {"object", "uos.model_capabilities"}, {"owned_by", "deepseek"}, {"display_name", displayName ? *displayName : std::string(id)}, {"upstream_provider", "deepseek"}, {"supported_endpoints", json::array{"/v1/chat/completions", "/v1/responses"}}, {"supported_reasoning_levels", strings(ds::DEEPSEEK_REASONING_LEVELS)}, {"default_reasoning_effort", ds::DEEPSEEK_DEFAULT_REASONING_EFFORT}, {"reasoning_effort_wire_map", json::object{{"ultra", "max"}}}};
        contextFields(row, resolved);
        result.push_back(std::move(row));
      }
      return result;
    }
    json::array configuredLithosModelCapabilities() {
      if (!li::readLithosApiKey()) return {};
      json::array result;
      for (const auto id : li::LITHOS_MODEL_IDS) {
        md::ModelMetadataSources sources; sources.provider = request_policy::LITHOS_PROVIDER_HINT();
        const auto resolved = md::resolveModelMetadata(id, sources);
        const auto* displayName = li::LITHOS_DISPLAY_NAMES.get(id);
        json::object row{{"id", id}, {"object", "uos.model_capabilities"}, {"owned_by", "lithos"}, {"display_name", displayName ? *displayName : std::string(id)}, {"upstream_provider", "lithos"}, {"supported_endpoints", json::array{"/v1/chat/completions", "/v1/responses"}}, {"supported_reasoning_levels", strings(li::LITHOS_REASONING_LEVELS)}, {"default_reasoning_effort", li::LITHOS_DEFAULT_REASONING_EFFORT}, {"reasoning_effort_wire_map", json::object{}}};
        contextFields(row, resolved);
        result.push_back(std::move(row));
      }
      return result;
    }
    bool anyWaterfallConfigured() {
      return std::any_of(wf::DEEPSEEK_WATERFALL_ORDER.begin(), wf::DEEPSEEK_WATERFALL_ORDER.end(), wf::deepSeekWaterfallProviderConfigured);
    }
    json::array configuredDeepSeekWaterfallModels() {
      return anyWaterfallConfigured() ? json::array{json::object{{"id", wf::DEEPSEEK_WATERFALL_MODEL_ID}, {"object", "model"}, {"created", rt::makeNumber(wf::DEEPSEEK_WATERFALL_CREATED)}, {"owned_by", "ubiquity"}}} : json::array{};
    }
    json::array configuredDeepSeekWaterfallModelCapabilities() {
      if (!anyWaterfallConfigured()) return {};
      const auto window = wf::DEEPSEEK_WATERFALL_CONTEXT_WINDOW_TOKENS;
      return {json::object{
        {"id", wf::DEEPSEEK_WATERFALL_MODEL_ID},
        {"object", "uos.model_capabilities"},
        {"owned_by", "ubiquity"},
        {"display_name", wf::DEEPSEEK_WATERFALL_DISPLAY_NAME},
        {"upstream_provider", "ubiquity"},
        {"supported_endpoints", json::array{"/v1/responses"}},
        {"supported_reasoning_levels", strings(wf::DEEPSEEK_WATERFALL_REASONING_LEVELS)},
        {"default_reasoning_effort", wf::DEEPSEEK_WATERFALL_DEFAULT_REASONING_LEVEL},
        {"reasoning_effort_wire_map", json::object{}},
        {"context_window_tokens", rt::makeNumber(window)},
        {"max_context_window_tokens", rt::makeNumber(window)},
        {"auto_compact_token_limit_tokens", rt::makeNumber(recent_model_context::deriveAutoCompactTokenLimit(window))},
        {"effective_context_window_percent", rt::makeNumber(wf::DEEPSEEK_WATERFALL_EFFECTIVE_CONTEXT_WINDOW_PERCENT)},
        {"context_source", "openrouter"},
        {"reasoning_source", "provider_discovery"},
      }};
    }
    json::array replaceConfigured(const json::array& models, const json::array& configured) {
      if (configured.empty()) return models;
      std::unordered_set<std::string> ids;
      for (const auto& model : configured) ids.insert(stringAt(model, "id").value());
      json::array result;
      for (const auto& model : models) if (!ids.contains(stringAt(model, "id").value_or(""))) result.push_back(model);
      for (const auto& model : configured) result.push_back(model);
      return result;
    }
    struct OpenRouterServedModel { std::string id; std::vector<std::string> endpoints; std::string ownedBy; };
    std::vector<OpenRouterServedModel> openRouterServedModels() {
      if (!provider::openrouter::readOpenRouterApiKey()) return {};
      std::vector<OpenRouterServedModel> catalogue;
      const auto snapshot = models::openrouter_models::openRouterModelsSnapshot();
      if (snapshot) for (const auto& model : snapshot->models) {
        const auto endpoints = models::openrouter_models::openRouterModelEndpointPaths(model);
        const auto owner = rt::split(model.id, "/", 1);
        if (!endpoints.empty()) catalogue.push_back({model.id, endpoints, owner.empty() || owner[0].empty() ? "openrouter" : owner[0]});
      }
      if (std::none_of(catalogue.begin(), catalogue.end(), [](const auto& model) { return model.id == OPENROUTER_SERVED_MODEL_ID; })) catalogue.push_back({std::string(OPENROUTER_SERVED_MODEL_ID), {"/v1/decisions", "/v1/systemone"}, "typesafe"});
      return catalogue;
    }
    NormalizationResult<MessageContentItem> normalizeResponseTextContentItem(const json::object& value, const std::string& partType, const std::string& param, const std::string& role) {
      if (partType == "output_text" && role != "assistant") return invalidNormalizedField(param + ".type", param + ".type is only valid for assistant messages");
      const auto unknown = partType == "output_text" ? findUnknownContentField(value, {"type", "text", "annotations", "logprobs"}) : findUnknownContentField(value, {"type", "text", "prompt_cache_breakpoint"});
      if (!unknown.empty()) return invalidNormalizedField(param + "." + unknown, "Unknown content field: " + unknown);
      const auto text = utils::getString(rt::get(value, "text"));
      if (!text) return invalidNormalizedField(param + ".text", param + ".text must be a string");
      for (const auto field : {"annotations", "logprobs"}) {
        const auto* entry = rt::get(value, field);
        if (partType == "output_text" && entry && !entry->is_array()) return invalidNormalizedField(param + "." + field, param + "." + field + " must be an array");
      }
      // Replayed output metadata is accepted but does not belong in provider input.
      if (partType == "output_text") return success(MessageContentItem(MessageContentItemOutputText{*text}));
      const auto breakpoint = normalizePromptCacheBreakpoint(rt::get(value, "prompt_cache_breakpoint"), param + ".prompt_cache_breakpoint");
      if (const auto* error = failure(breakpoint)) return *error;
      return success(MessageContentItem(MessageContentItemInputText{*text, resultValue(breakpoint)}));
    }
    NormalizationResult<MessageContentItem> normalizeResponseImageContentItem(const json::object& value, const std::string& param) {
      const auto unknown = findUnknownContentField(value, {"type", "image_url", "file_id", "detail", "prompt_cache_breakpoint"});
      if (!unknown.empty()) return invalidNormalizedField(param + "." + unknown, "Unknown content field: " + unknown);
      const auto imageUrl = rt::trim(utils::getString(rt::get(value, "image_url")).value_or(""));
      const auto fileId = rt::trim(utils::getString(rt::get(value, "file_id")).value_or(""));
      if (imageUrl.empty() == fileId.empty()) return invalidNormalizedField(param + ".image_url", param + " must include exactly one of image_url or file_id");
      for (const auto field : {"image_url", "file_id"}) {
        const auto* entry = rt::get(value, field);
        if (entry && !entry->is_string()) return invalidNormalizedField(param + "." + field, param + "." + field + " must be a string");
      }
      const auto detail = parseImageDetail(rt::get(value, "detail"), param + ".detail");
      if (const auto* error = failure(detail)) return *error;
      const auto breakpoint = normalizePromptCacheBreakpoint(rt::get(value, "prompt_cache_breakpoint"), param + ".prompt_cache_breakpoint");
      if (const auto* error = failure(breakpoint)) return *error;
      MessageContentItemInputImage item{.detail = resultValue(detail), .prompt_cache_breakpoint = resultValue(breakpoint)};
      if (!imageUrl.empty()) item.image_url = imageUrl; else item.file_id = fileId;
      return success(MessageContentItem(item));
    }
    NormalizationResult<MessageContentItem> normalizeResponseFileContentItem(const json::object& value, const std::string& param) {
      const auto unknown = findUnknownContentField(value, {"type", "file_id", "file_data", "file_url", "filename", "detail", "prompt_cache_breakpoint"});
      if (!unknown.empty()) return invalidNormalizedField(param + "." + unknown, "Unknown content field: " + unknown);
      bool present = false;
      for (const auto field : {"file_id", "file_data", "file_url"}) {
        const auto string = utils::getString(rt::get(value, field));
        if (string && !rt::trim(*string).empty()) present = true;
      }
      if (!present) return invalidNormalizedField(param + ".file_id", param + " must include file_id, file_data, or file_url");
      for (const auto field : {"file_id", "file_data", "file_url"}) {
        const auto* entry = rt::get(value, field);
        if (entry && !entry->is_string()) return invalidNormalizedField(param + "." + field, param + "." + field + " must be a string");
      }
      const auto* filename = rt::get(value, "filename");
      if (filename && !filename->is_null() && !filename->is_string()) return invalidNormalizedField(param + ".filename", param + ".filename must be a string or null");
      const auto detail = parseInputFileDetail(rt::get(value, "detail"), param + ".detail");
      if (const auto* error = failure(detail)) return *error;
      const auto breakpoint = normalizePromptCacheBreakpoint(rt::get(value, "prompt_cache_breakpoint"), param + ".prompt_cache_breakpoint");
      if (const auto* error = failure(breakpoint)) return *error;
      MessageContentItemInputFile item{.detail = resultValue(detail), .prompt_cache_breakpoint = resultValue(breakpoint)};
      const auto nonempty = [&](std::string_view field) { auto str = utils::getString(rt::get(value, field)); return str && !str->empty() ? str : std::nullopt; };
      item.file_id = nonempty("file_id"); item.file_data = nonempty("file_data"); item.file_url = nonempty("file_url");
      if (filename) item.filename = filename->is_null() ? rt::Nullable<std::string>(nullptr) : rt::Nullable<std::string>(utils::getString(filename).value());
      return success(MessageContentItem(item));
    }
    NormalizationResult<json::object> normalizeChatToolCall(const json::value& value, const std::string& param) {
      if (!value.is_object()) return invalidNormalizedField(param, param + " must be an object");
      const auto unknown = findUnknownContentField(value.as_object(), {"id", "type", "function"});
      if (!unknown.empty()) return invalidNormalizedField(param + "." + unknown, "Unknown tool call field: " + unknown);
      if (!equals(rt::get(value, "type"), "function")) return invalidNormalizedField(param + ".type", param + ".type must be function");
      const auto callId = rt::trim(stringAt(value, "id").value_or(""));
      if (callId.empty()) return invalidNormalizedField(param + ".id", param + ".id must be a non-empty string");
      const auto* function = rt::getObject(value, "function");
      if (!function) return invalidNormalizedField(param + ".function", param + ".function must be an object");
      const auto unknownFunctionField = findUnknownContentField(*function, {"name", "arguments"});
      if (!unknownFunctionField.empty()) return invalidNormalizedField(param + ".function." + unknownFunctionField, "Unknown tool call function field: " + unknownFunctionField);
      const auto name = rt::trim(utils::getString(rt::get(*function, "name")).value_or(""));
      if (name.empty()) return invalidNormalizedField(param + ".function.name", param + ".function.name must be a non-empty string");
      const auto arguments = utils::getString(rt::get(*function, "arguments"));
      if (!arguments) return invalidNormalizedField(param + ".function.arguments", param + ".function.arguments must be a string");
      // Arguments are opaque: do not parse, validate, or reserialize them.
      return success(json::object{{"type", "function_call"}, {"call_id", callId}, {"name", name}, {"arguments", *arguments}});
    }
    NormalizationResult<json::value> normalizeChatToolOutput(const json::value* value, const std::string& param) {
      if (value && value->is_string()) return success(*value);
      if (!value || !value->is_array()) return invalidNormalizedField(param, param + " must be a string or an array");
      json::array output;
      for (std::size_t index = 0; index < value->as_array().size(); ++index) {
        const auto& part = value->as_array()[index];
        const auto partParam = param + "[" + rt::numberToString(static_cast<double>(index)) + "]";
        if (!part.is_object()) return invalidNormalizedField(partParam, partParam + " must be an object");
        if (!equals(rt::get(part, "type"), "text")) return invalidNormalizedField(partParam + ".type", partParam + ".type must be a text content part");
        const auto normalized = normalizeChatTextContentPart(part.as_object(), partParam, false, "input_text");
        if (const auto* error = failure(normalized)) return *error;
        output.push_back(json::value_from(resultValue(normalized)));
      }
      return success(json::value(output));
    }
    NormalizationResult<NormalizedChatMessage> normalizeChatToolMessage(const json::object& value, const std::string& param) {
      if (rt::hasOwn(value, "tool_calls")) return invalidNormalizedField(param + ".tool_calls", "tool_calls are only valid for assistant messages");
      const auto callId = rt::trim(utils::getString(rt::get(value, "tool_call_id")).value_or(""));
      if (callId.empty()) return invalidNormalizedField(param + ".tool_call_id", param + ".tool_call_id must be a non-empty string");
      const auto output = normalizeChatToolOutput(rt::get(value, "content"), param + ".content");
      if (const auto* error = failure(output)) return *error;
      return success(NormalizedChatMessage{std::nullopt, std::nullopt, {json::object{{"type", "function_call_output"}, {"call_id", callId}, {"output", resultValue(output)}}}});
    }
    NormalizationResult<void> appendChatToolCallInputs(std::vector<ResponseInputItem>& input, const json::value* toolCalls, const std::string& param) {
      if (!toolCalls || !toolCalls->is_array()) return invalidNormalizedField(param + ".tool_calls", param + ".tool_calls must be an array");
      for (std::size_t index = 0; index < toolCalls->as_array().size(); ++index) {
        const auto normalized = normalizeChatToolCall(toolCalls->as_array()[index], param + ".tool_calls[" + rt::numberToString(static_cast<double>(index)) + "]");
        if (const auto* error = failure(normalized)) return *error;
        input.push_back(resultValue(normalized));
      }
      return successVoid();
    }
    NormalizationResult<NormalizedChatMessage> normalizeChatAssistantMessage(const json::object& value, const std::string& param, const std::string& role) {
      const bool hasToolCalls = rt::hasOwn(value, "tool_calls");
      const auto* refusalValue = rt::get(value, "refusal");
      const auto refusal = rt::isNullish(refusalValue) ? std::nullopt : utils::getString(refusalValue);
      if (!refusal && !rt::isNullish(refusalValue)) return invalidNormalizedField(param + ".refusal", param + ".refusal must be a string or null");
      const auto* rawContent = rt::get(value, "content");
      const auto content = !rawContent && hasToolCalls ? success(std::vector<MessageContentItem>{}) : normalizeChatContentItems(role, rawContent, param + ".content");
      if (const auto* error = failure(content)) return *error;
      auto messageContent = resultValue(content);
      if (refusal) messageContent.push_back(MessageContentItemOutputText{*refusal});
      std::vector<ResponseInputItem> input;
      if (!messageContent.empty()) input.push_back(ResponseMessageItem{role, std::move(messageContent)});
      if (hasToolCalls) {
        const auto calls = appendChatToolCallInputs(input, rt::get(value, "tool_calls"), param);
        if (const auto* error = failure(calls)) return *error;
      }
      if (input.empty()) return invalidNormalizedField(param + ".content", "assistant messages require content, refusal, or tool_calls");
      return success(NormalizedChatMessage{std::nullopt, std::nullopt, std::move(input)});
    }
    NormalizationResult<NormalizedChatMessage> normalizeChatPlainMessage(const json::object& value, const std::string& param, const std::string& role, const std::string& roleRaw) {
      const auto content = normalizeChatContentItems(role, rt::get(value, "content"), param + ".content");
      if (const auto* error = failure(content)) return *error;
      if (rt::hasOwn(value, "tool_calls")) return invalidNormalizedField(param + ".tool_calls", "tool_calls are only valid for assistant messages");
      if (roleRaw == "system" || roleRaw == "developer") return success(NormalizedChatMessage{messageContentToText(resultValue(content)), resultValue(content), {}});
      return success(NormalizedChatMessage{std::nullopt, std::nullopt, {ResponseMessageItem{role, resultValue(content)}}});
    }
  }
  NormalizationResult<void> validatePromptCacheControls(const json::object& rawRecord) {
    if (rt::hasOwn(rawRecord, "prompt_cache_key") && !rt::get(rawRecord, "prompt_cache_key")->is_string()) return invalidNormalizedField("prompt_cache_key", "prompt_cache_key must be a string");
    if (rt::hasOwn(rawRecord, "prompt_cache_options")) {
      const auto* options = rt::getObject(rawRecord, "prompt_cache_options");
      if (!options) return invalidNormalizedField("prompt_cache_options", "prompt_cache_options must be an object");
      const auto unknown = findUnknownContentField(*options, {"mode", "ttl"});
      if (!unknown.empty()) return invalidNormalizedField("prompt_cache_options." + unknown, "Unknown prompt cache option: " + unknown);
      const auto* mode = rt::get(*options, "mode");
      if (mode && !equals(mode, "implicit") && !equals(mode, "explicit")) return invalidNormalizedField("prompt_cache_options.mode", "prompt_cache_options.mode must be implicit or explicit");
      const auto* ttl = rt::get(*options, "ttl");
      if (ttl && !equals(ttl, "30m")) return invalidNormalizedField("prompt_cache_options.ttl", "prompt_cache_options.ttl must be 30m");
    }
    const auto* retention = rt::get(rawRecord, "prompt_cache_retention");
    if (retention && !equals(retention, "in_memory") && !equals(retention, "24h")) return invalidNormalizedField("prompt_cache_retention", "prompt_cache_retention must be in_memory or 24h");
    return successVoid();
  }
  std::string normalizeModelForCodex(std::string_view model) { return rt::trim(model); }
  std::optional<json::object> normalizeModelList(const json::value* payload) {
    if (!utils::isRecord(payload)) return std::nullopt;
    double fallbackCreated = 0;
    const auto timestamp = rt::getNumber(*payload, "updated_at_ms");
    if (timestamp && rt::Number::isFinite(*timestamp)) fallbackCreated = rt::Math::max(0.0, rt::Math::trunc(*timestamp / 1000));
    const auto* data = rt::getArray(*payload, "data");
    if (!data) data = rt::getArray(*payload, "models");
    if (!data) return std::nullopt;
    json::array result;
    for (const auto& entry : *data) if (auto model = normalizeModelEntry(entry, fallbackCreated)) result.push_back(*model);
    return json::object{{"object", "list"}, {"data", std::move(result)}};
  }
  std::optional<json::object> normalizeModelList(const json::value& payload) { return normalizeModelList(&payload); }
  std::optional<json::object> configuredCerebrasModelCapabilities() { return configuredCerebrasModelCapabilities(std::string(ce::CEREBRAS_GPT_OSS_120B_MODEL)); }
  std::optional<json::object> configuredCerebrasModelCapabilities(const std::string& model) {
    if (!ce::readCerebrasApiKey()) return std::nullopt;
    const auto id = ce::cerebrasUpstreamModelFor(model);
    if (!id) return std::nullopt;
    const auto hint = request_policy::cerebrasProviderHint(*id);
    md::ModelMetadataSources sources; sources.provider = hint;
    const auto resolved = md::resolveModelMetadata(*id, sources);
    const auto* name = CEREBRAS_MODEL_DISPLAY_NAMES.get(*id);
    json::object result{{"id", *id}, {"object", "uos.model_capabilities"}, {"owned_by", "cerebras"}, {"display_name", name ? *name : *id}, {"upstream_provider", "cerebras"}, {"supported_endpoints", json::array{"/v1/chat/completions"}}, {"supported_reasoning_levels", hint.supported_reasoning_levels.isNullish() ? json::array{} : *hint.supported_reasoning_levels}, {"default_reasoning_effort", hint.default_reasoning_effort && !hint.default_reasoning_effort->is_null() ? *hint.default_reasoning_effort : json::value("medium")}, {"reasoning_effort_wire_map", json::object{}}};
    contextFields(result, resolved);
    return result;
  }
  json::array withConfiguredCerebrasModel(const json::array& models, bool enabled) {
    const auto configured = enabled && ce::readCerebrasApiKey() ? configuredModels(ce::CEREBRAS_MODELS, "cerebras") : json::array{};
    auto result = models;
    for (const auto& candidate : configured) if (std::none_of(models.begin(), models.end(), [&](const auto& model) { const auto* id = rt::get(model, "id"); return id && rt::strictEquals(*id, *rt::get(candidate, "id")); })) result.push_back(candidate);
    return result;
  }
  json::array withConfiguredGeminiModels(const json::array& models, bool enabled) {
    const auto configured = enabled && provider::gemini::geminiCliConfigured() ? configuredModels(provider::gemini::GEMINI_MODEL_IDS, "google") : json::array{};
    return replaceConfigured(models, configured);
  }
  json::array withConfiguredGeminiCapabilities(const json::array& data, bool enabled) {
    if (!enabled || !provider::gemini::geminiCliConfigured()) return data;
    json::array rows;
    for (const auto id : provider::gemini::GEMINI_MODEL_IDS) {
      const std::string effort = rt::endsWith(id, "-high") ? "high" : rt::endsWith(id, "-medium") ? "medium" : "low";
      const std::string display = std::string("Gemini 3.8 Flash (") + (effort == "high" ? "High" : effort == "medium" ? "Medium" : "Low") + ")";
      json::object row{{"id", id}, {"object", "uos.model_capabilities"}, {"owned_by", "google"}, {"display_name", display},
        {"upstream_provider", "gemini"}, {"supported_endpoints", json::array{"/v1/responses", "/v1/chat/completions"}},
        {"supported_reasoning_levels", json::array{effort}}, {"default_reasoning_effort", effort}, {"reasoning_effort_wire_map", json::object{}},
        {"context_window_tokens", rt::makeNumber(provider::gemini::GEMINI_CONTEXT_WINDOW_TOKENS)},
        {"max_context_window_tokens", rt::makeNumber(provider::gemini::GEMINI_CONTEXT_WINDOW_TOKENS)},
        {"auto_compact_token_limit_tokens", rt::makeNumber(provider::gemini::GEMINI_CONTEXT_WINDOW_TOKENS * 0.8)},
        {"context_source", "provider_documented"}, {"reasoning_source", "model_id"}};
      rows.push_back(std::move(row));
    }
    return replaceConfigured(data, rows);
  }
  json::array withConfiguredDeepSeekModels(const json::array& models, bool enabled) { return replaceConfigured(models, enabled && ds::readDeepSeekApiKey() ? configuredModels(ds::DEEPSEEK_OFFICIAL_MODEL_IDS, "deepseek") : json::array{}); }
  json::array withConfiguredDeepSeekCapabilities(const json::array& data, bool enabled) { return replaceConfigured(data, enabled ? configuredDeepSeekModelCapabilities() : json::array{}); }
  json::array withConfiguredLithosModels(const json::array& models, bool enabled) { return replaceConfigured(models, enabled && li::readLithosApiKey() ? configuredModels(li::LITHOS_MODEL_IDS, "lithos") : json::array{}); }
  json::array withConfiguredDeepSeekWaterfallModels(const json::array& models, bool enabled) { return replaceConfigured(models, enabled ? configuredDeepSeekWaterfallModels() : json::array{}); }
  json::array withConfiguredDeepSeekWaterfallCapabilities(const json::array& data, bool enabled) { return replaceConfigured(data, enabled ? configuredDeepSeekWaterfallModelCapabilities() : json::array{}); }
  json::array configuredOpenRouterModels() {
    json::array result;
    for (const auto& model : openRouterServedModels()) result.push_back(json::object{{"id", model.id}, {"object", "model"}, {"created", 0}, {"owned_by", model.ownedBy}});
    return result;
  }
  json::array configuredOpenRouterModelCapabilities() {
    json::array result;
    for (const auto& model : openRouterServedModels()) {
      const auto resolved = md::resolveModelMetadata(model.id);
      const bool systemOne = model.id == OPENROUTER_SERVED_MODEL_ID;
      const auto defaultEffort = systemOne ? std::optional<std::string>("none") : resolved.default_reasoning_effort;
      json::object row{{"id", model.id}, {"object", "uos.model_capabilities"}, {"owned_by", model.ownedBy}, {"display_name", systemOne ? "Jev (Typesafe System One decisions)" : model.id}, {"upstream_provider", "openrouter"}, {"supported_endpoints", strings(model.endpoints)}, {"supported_reasoning_levels", systemOne || !resolved.supported_reasoning_levels ? json::array{"none"} : strings(*resolved.supported_reasoning_levels)}};
      if (defaultEffort || !resolved.supported_reasoning_levels) rt::objectSet(row, "default_reasoning_effort", json::value(defaultEffort.value_or("none")));
      rt::objectSet(row, "reasoning_effort_wire_map", json::object{});
      rt::objectSet(row, "context_window_tokens", systemOne ? rt::Nullable<double>(nullptr) : rt::orNull(resolved.context_window_tokens));
      rt::objectSet(row, "max_context_window_tokens", systemOne ? rt::Nullable<double>(nullptr) : rt::orNull(resolved.max_context_window_tokens));
      rt::objectSet(row, "auto_compact_token_limit_tokens", systemOne ? rt::Nullable<double>(nullptr) : rt::orNull(resolved.auto_compact_token_limit_tokens));
      rt::objectSet(row, "context_source", json::value(systemOne ? "unknown" : resolved.context_source));
      rt::objectSet(row, "reasoning_source", json::value(systemOne ? "unknown" : resolved.reasoning_source));
      result.push_back(std::move(row));
    }
    return result;
  }
  json::array withConfiguredLithosCapabilities(const json::array& data, bool enabled) { return replaceConfigured(data, enabled ? configuredLithosModelCapabilities() : json::array{}); }
  std::optional<json::object> normalizeModelCapabilitiesEntry(const json::value* value) {
    if (!value || !value->is_object()) return std::nullopt;
    const auto id = request_policy::modelIdFromSnapshotRecord(value->as_object());
    if (!id || id->empty()) return std::nullopt;
    const auto reasoning = request_policy::getCodexModelReasoning(value->as_object());
    const auto promptCache = models::codex_models::normalizePromptCacheCapabilities(rt::get(*value, "prompt_cache"));
    md::ModelMetadataSources sources; sources.codex = rt::orNull(md::codexSnapshotMetadataHint(value->as_object())); sources.codexSubscription = md::codexSubscriptionMetadataHint();
    const auto resolved = md::resolveModelMetadata(*id, sources);
    json::object wireEfforts;
    for (const auto& [key, effort] : reasoning.wireEfforts) rt::objectDefine(wireEfforts, key, json::value(effort));
    json::object row{{"id", *id}, {"object", "uos.model_capabilities"}, {"owned_by", stringAt(*value, "owned_by").value_or("openai")}, {"display_name", rt::orNull(stringAt(*value, "display_name")).toJson().value()}, {"upstream_provider", "codex_chatgpt"}, {"supported_endpoints", json::array{"/v1/chat/completions", "/v1/responses"}}, {"supported_reasoning_levels", strings(reasoning.levels)}, {"default_reasoning_effort", rt::orNull(reasoning.defaultLevel).toJson().value()}, {"reasoning_effort_wire_map", std::move(wireEfforts)}};
    contextFields(row, resolved);
    rt::objectSet(row, "reasoning_source", json::value(resolved.reasoning_source));
    if (promptCache) rt::objectSet(row, "prompt_cache", json::value_from(*promptCache));
    return row;
  }
  std::optional<json::object> normalizeModelCapabilitiesEntry(const json::value& value) { return normalizeModelCapabilitiesEntry(&value); }
  NormalizationResult<MessageContentItem> normalizeResponseContentItem(const json::value* value, const std::string& param, const std::string& role) {
    if (!value || !value->is_object()) return invalidNormalizedField(param, param + " must be an object");
    const auto partType = stringAt(*value, "type");
    if (!partType || partType->empty()) return invalidNormalizedField(param + ".type", param + ".type must be a string");
    if (*partType == "input_text" || *partType == "output_text") return normalizeResponseTextContentItem(value->as_object(), *partType, param, role);
    if (*partType == "input_image") return normalizeResponseImageContentItem(value->as_object(), param);
    if (*partType == "input_file") return normalizeResponseFileContentItem(value->as_object(), param);
    return invalidNormalizedField(param + ".type", param + ".type is not supported");
  }
  NormalizationResult<MessageContentItem> normalizeResponseContentItem(const json::value& value, const std::string& param, const std::string& role) { return normalizeResponseContentItem(&value, param, role); }
  NormalizationResult<ResponseMessageItem> normalizeResponseMessageItem(const json::value* value, const std::string& param) {
    if (!value || !value->is_object()) return invalidNormalizedField(param, param + " must be an object");
    if (rt::hasOwn(value->as_object(), "prompt_cache_breakpoint")) return invalidNormalizedField(param + ".prompt_cache_breakpoint", "prompt_cache_breakpoint is only valid on supported input content blocks");
    const auto* type = rt::get(*value, "type");
    if (type && !equals(type, "message")) return invalidNormalizedField(param + ".type", param + ".type must be message");
    const auto roleRaw = stringAt(*value, "role");
    const auto role = roleRaw && !roleRaw->empty() && *roleRaw != "tool" ? chatRoleToCodexRole(*roleRaw) : std::nullopt;
    if (!role) return invalidNormalizedField(param + ".role", param + ".role is invalid");
    const auto* content = rt::get(*value, "content");
    if (content && content->is_string()) {
      MessageContentItem item = *role == "assistant" ? MessageContentItem(MessageContentItemOutputText{utils::getString(content).value()}) : MessageContentItem(MessageContentItemInputText{utils::getString(content).value()});
      return success(ResponseMessageItem{*role, {std::move(item)}});
    }
    if (!content || !content->is_array()) return invalidNormalizedField(param + ".content", param + ".content must be a string or an array");
    std::vector<MessageContentItem> items;
    for (std::size_t index = 0; index < content->as_array().size(); ++index) {
      const auto normalized = normalizeResponseContentItem(content->as_array()[index], param + ".content[" + rt::numberToString(static_cast<double>(index)) + "]", *role);
      if (const auto* error = failure(normalized)) return *error;
      items.push_back(resultValue(normalized));
    }
    return success(ResponseMessageItem{*role, std::move(items)});
  }
  NormalizationResult<ResponseMessageItem> normalizeResponseMessageItem(const json::value& value, const std::string& param) { return normalizeResponseMessageItem(&value, param); }
  NormalizationResult<ResponseInputItem> normalizeFunctionCallOutputItem(const json::object& value, const std::string& param) {
    const auto* output = rt::getArray(value, "output");
    if (!equals(rt::get(value, "type"), "function_call_output") || !output) return success(ResponseInputItem(value));
    json::array normalizedOutput;
    for (std::size_t index = 0; index < output->size(); ++index) {
      const auto normalized = normalizeResponseContentItem((*output)[index], param + ".output[" + rt::numberToString(static_cast<double>(index)) + "]", "user");
      if (const auto* error = failure(normalized)) return *error;
      normalizedOutput.push_back(json::value_from(resultValue(normalized)));
    }
    auto result = value;
    rt::objectDefine(result, "type", "function_call_output"); rt::objectDefine(result, "output", normalizedOutput);
    return success(ResponseInputItem(result));
  }
  NormalizationResult<NormalizedChatMessage> normalizeChatMessage(const json::value* value, double index) {
    const auto param = "messages[" + rt::numberToString(index) + "]";
    if (!value || !value->is_object()) return invalidNormalizedField(param, param + " must be an object");
    if (rt::hasOwn(value->as_object(), "prompt_cache_breakpoint")) return invalidNormalizedField(param + ".prompt_cache_breakpoint", "prompt_cache_breakpoint is only valid on supported input content blocks");
    const auto roleRaw = stringAt(*value, "role");
    if (!roleRaw || roleRaw->empty()) return invalidNormalizedField(param + ".role", param + ".role must be a string");
    const auto role = chatRoleToCodexRole(*roleRaw);
    if (!role) return invalidNormalizedField(param + ".role", param + ".role is not supported");
    if (*roleRaw == "tool") return normalizeChatToolMessage(value->as_object(), param);
    if (rt::hasOwn(value->as_object(), "tool_call_id")) return invalidNormalizedField(param + ".tool_call_id", "tool_call_id is only valid for tool messages");
    if (*roleRaw == "assistant") return normalizeChatAssistantMessage(value->as_object(), param, *role);
    return normalizeChatPlainMessage(value->as_object(), param, *role, *roleRaw);
  }
  NormalizationResult<NormalizedChatMessage> normalizeChatMessage(const json::value& value, double index) { return normalizeChatMessage(&value, index); }
}
