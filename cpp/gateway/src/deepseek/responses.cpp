// Port of src/deepseek/responses.ts.
//
// Responses <-> DeepSeek Chat Completions adapter.
//
// The Codex CLI speaks only the Responses API (`wire_api = "chat"` was removed
// from the client), so this module translates a Responses request into a Chat
// Completions body, a Chat completion back into a Responses object, and a Chat
// SSE stream into the Responses event sequence the client consumes.
//
// The translation is not required by a provider gap: DeepSeek now serves a
// native Responses endpoint (`POST /responses` and `/v1/responses`, probed
// 2026-09-21). It stays the right seam because the provider's native endpoint is
// documented as stateless with several control parameters ignored, and because
// the `reasoning_content` fill this adapter applies to tool-bearing tails is a
// measured provider requirement a native response would have to reproduce. A
// migration is a separate evidence-driven evaluation, not an assumed cure.
//
// Only the gateway-known subset is translated. Anything else fails closed with
// an `invalid_request_error` rather than being forwarded as an approximation.
//
// Provider-specific behavior is not hard-coded here: one `ChatOnlyResponsesProfile`
// per upstream carries the model table, the reasoning projection, the finish
// vocabulary, the usage counters and the capability checks, and the exported
// translations take the profile they serve. DeepSeek and LithosAI therefore
// share this one translation instead of a second copy drifting from it. Every
// entry point defaults to the DeepSeek profile, so the route that shipped
// against this module is unchanged.
#include "uos/deepseek/responses.hpp"

#include "uos/provider/cerebras.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/request_policy.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace uos::deepseek::responses {
  DeepSeekResponsesFailure failure(const std::string& param, const std::string& message, const std::optional<std::string>& code) {
    return DeepSeekResponsesFailure{.message = message, .param = param, .code = code};
  }

  /** A trusted internal distinction; it is never accepted from request metadata. */
  DeepSeekResponsesFailure aggregateContextOverflow(const std::string& message) {
    DeepSeekResponsesFailure overflow = failure("input", message, std::string("context_length_exceeded"));
    overflow.kind = "aggregate_context_overflow";
    return overflow;
  }

  namespace {
    /** The provider's own key check, mirroring `requireDeepSeekApiKey` in `./deepseek.ts`. */
    void requireDeepSeekResponsesApiKey() {
      if (!rt::truthy(deepseek::index::readDeepSeekApiKey())) {
        throw deepseek::index::DeepSeekError("The requested model is not configured.", "deepseek_api_key_missing", 503);
      }
    }

    // `(effort: string) => projectDeepSeekReasoningEffort(effort)`: the inherited `Object` function
    // (std::nullopt there) is undefined here; every other projection is its JSON value.
    rt::Nullable<json::value> projectDeepSeekResponsesReasoningEffort(const std::string& effort) {
      std::optional<json::value> projected = deepseek::index::projectDeepSeekReasoningEffort(effort);
      if (!projected) return rt::undefined;
      return rt::Nullable<json::value>(std::move(*projected));
    }
  }

  constinit const ChatOnlyResponsesProfile DEEPSEEK_RESPONSES_PROFILE{
    .id = "deepseek",
    .label = "DeepSeek",
    .upstreamModelFor = deepseek::index::deepSeekUpstreamModelFor,
    .projectReasoningEffort = projectDeepSeekResponsesReasoningEffort,
    .finishDisposition = static_cast<deepseek::index::DeepSeekFinishDisposition (*)(const json::value*)>(deepseek::index::deepSeekFinishDisposition),
    .thinkingToolChoiceConflict = static_cast<std::optional<std::string> (*)(const json::value*, const json::value*, const json::value*)>(
      deepseek::index::deepSeekThinkingToolChoiceConflict),
    .toolChoiceThinkingConflictMessage = deepseek::index::deepSeekToolChoiceThinkingConflictMessage,
    .cachedPromptTokens = deepseek::index::deepSeekCachedPromptTokens,
    .reasoningTokens = deepseek::index::deepSeekReasoningTokens,
    .requireApiKey = requireDeepSeekResponsesApiKey,
    // DeepSeek reports usage on its final content chunk only when the request
    // asks for it, so the streaming body carries `stream_options.include_usage`.
    .requiresStreamUsageOption = true,
  };

  namespace {
    /** The seven tiers this provider accepted on 2026-09-23, as a membership set. */
    constexpr const auto& LITHOS_REASONING_LEVEL_SET = provider::lithos::LITHOS_REASONING_LEVELS;

    /**
     * Projects a requested reasoning tier onto the LithosAI wire value, or null
     * when the provider does not accept it.
     *
     * The tier is sent verbatim: unlike DeepSeek there is no advanced Codex preset
     * to translate, and `ultra` is NOT mapped to `max` because this provider
     * refuses `ultra`. The provider accepted exactly these seven lowercase names,
     * so a case-insensitive match forwards the canonical spelling and everything
     * else fails closed instead of being sent.
     */
    rt::Nullable<json::value> projectLithosReasoningEffort(const std::string& effort) {
      const std::string level = rt::toLowerCase(rt::trim(effort));
      if (std::find(LITHOS_REASONING_LEVEL_SET.begin(), LITHOS_REASONING_LEVEL_SET.end(), std::string_view(level)) == LITHOS_REASONING_LEVEL_SET.end()) return nullptr;
      return rt::Nullable<json::value>(json::value(json::string(level)));
    }

    /**
     * No thinking-mode `tool_choice` restriction was observed or documented on this
     * provider, so this profile reports no conflict rather than importing DeepSeek's
     * measured restriction. The message builder completes the profile contract; it
     * is unreachable while this function returns null.
     */
    std::optional<std::string> lithosThinkingToolChoiceConflict([[maybe_unused]] const json::value* _reasoningEffort, [[maybe_unused]] const json::value* _thinking,
                                                                [[maybe_unused]] const json::value* _toolChoice) {
      return std::nullopt;
    }

    std::string lithosToolChoiceThinkingConflictMessage(const std::string& conflict, const std::string& field) {
      return "tool_choice '" + conflict + "' is not supported while " + field + " keeps LithosAI thinking mode active; set " + field +
             " to 'none' or use tool_choice 'auto'";
    }

    // `requireApiKey: requireLithosApiKey`, called without an argument (the key it returns is unused).
    void requireLithosResponsesApiKey() { (void)provider::lithos::requireLithosApiKey(); }
  }

  constinit const ChatOnlyResponsesProfile LITHOS_RESPONSES_PROFILE{
    .id = "lithos",
    .label = "LithosAI",
    .upstreamModelFor = provider::lithos::lithosUpstreamModelFor,
    .projectReasoningEffort = projectLithosReasoningEffort,
    // `length` means the same output-budget truncation DeepSeek reports, so the
    // one shared mapping is reused rather than a second vocabulary invented.
    .finishDisposition = static_cast<deepseek::index::DeepSeekFinishDisposition (*)(const json::value*)>(deepseek::index::deepSeekFinishDisposition),
    .thinkingToolChoiceConflict = lithosThinkingToolChoiceConflict,
    .toolChoiceThinkingConflictMessage = lithosToolChoiceThinkingConflictMessage,
    .cachedPromptTokens = provider::lithos::lithosCachedPromptTokens,
    .reasoningTokens = provider::lithos::lithosReasoningTokens,
    .requireApiKey = requireLithosResponsesApiKey,
    // Usage arrives unconditionally on every streaming call and is never gated
    // on `stream_options.include_usage`, which this provider's wire does not use.
    .requiresStreamUsageOption = false,
  };

  namespace {
    /** The provider's own key check, mirroring `requireCerebrasApiKey` in `../provider/cerebras.ts`. */
    void requireCerebrasResponsesApiKey() {
      if (!rt::truthy(provider::cerebras::readCerebrasApiKey())) {
        throw provider::cerebras::CerebrasError("The requested model is not configured.", "cerebras_api_key_missing", 503);
      }
    }

    /**
     * Every tier the Cerebras route advertises for any of its ids, as a membership
     * set. The per-id lists live with the route's hints and are enforced by the
     * Cerebras transport, which refuses a tier the requested model does not accept;
     * this adapter only has to stop a tier the route never advertised at all.
     */
    // Derived at module load from request-policy's hints: constructed on first use.
    const std::vector<std::string>& CEREBRAS_REASONING_LEVEL_SET() {
      static const std::vector<std::string> levels = [] {
        std::vector<std::string> set;
        for (const std::string_view id : provider::cerebras::CEREBRAS_MODELS) {
          const models::metadata::ModelMetadataHint hint = request_policy::cerebrasProviderHint(std::string(id));
          if (!hint.supported_reasoning_levels.hasValue()) continue;
          for (const json::value& level : *hint.supported_reasoning_levels) {
            if (!level.is_string()) continue;
            std::string normalized = rt::toLowerCase(rt::trim(level.get_string()));
            if (std::find(set.begin(), set.end(), normalized) == set.end()) set.push_back(std::move(normalized));
          }
        }
        return set;
      }();
      return levels;
    }

    /**
     * Projects a requested reasoning tier onto the Cerebras wire value, or null
     * when no id on this route advertises it.
     *
     * The tier is sent verbatim, exactly like LithosAI: there is no advanced Codex
     * preset to translate, and `ultra` is NOT mapped to `max` because this route
     * refuses it. Whether the requested model itself accepts the (advertised) tier
     * is the transport's per-id decision, so `none` reaches `gpt-oss-120b` and is
     * refused there rather than being pre-filtered here against one model's list.
     */
    rt::Nullable<json::value> projectCerebrasReasoningEffort(const std::string& effort) {
      const std::string level = rt::toLowerCase(rt::trim(effort));
      const std::vector<std::string>& set = CEREBRAS_REASONING_LEVEL_SET();
      if (std::find(set.begin(), set.end(), level) == set.end()) return nullptr;
      return rt::Nullable<json::value>(json::value(json::string(level)));
    }

    /**
     * No thinking-mode `tool_choice` restriction was observed or documented on this
     * provider, so this profile reports no conflict rather than importing DeepSeek's
     * measured restriction. The message builder completes the profile contract; it
     * is unreachable while this function returns null.
     */
    std::optional<std::string> cerebrasThinkingToolChoiceConflict([[maybe_unused]] const json::value* _reasoningEffort, [[maybe_unused]] const json::value* _thinking,
                                                                  [[maybe_unused]] const json::value* _toolChoice) {
      return std::nullopt;
    }

    std::string cerebrasToolChoiceThinkingConflictMessage(const std::string& conflict, const std::string& field) {
      return "tool_choice '" + conflict + "' is not supported while " + field + " keeps Cerebras reasoning active; set " + field +
             " to 'none' or use tool_choice 'auto'";
    }
  }

  constinit const ChatOnlyResponsesProfile CEREBRAS_RESPONSES_PROFILE{
    .id = "cerebras",
    .label = "Cerebras",
    .upstreamModelFor = provider::cerebras::cerebrasUpstreamModelFor,
    .projectReasoningEffort = projectCerebrasReasoningEffort,
    // `length` means the same output-budget truncation DeepSeek reports, so the
    // one shared mapping is reused rather than a second vocabulary invented.
    .finishDisposition = static_cast<deepseek::index::DeepSeekFinishDisposition (*)(const json::value*)>(deepseek::index::deepSeekFinishDisposition),
    .thinkingToolChoiceConflict = cerebrasThinkingToolChoiceConflict,
    .toolChoiceThinkingConflictMessage = cerebrasToolChoiceThinkingConflictMessage,
    .cachedPromptTokens = provider::cerebras::cerebrasCachedPromptTokens,
    .reasoningTokens = provider::cerebras::cerebrasReasoningTokens,
    .requireApiKey = requireCerebrasResponsesApiKey,
    // This route buffers the provider's non-streaming answer and replays it, so
    // there is no upstream stream that could carry `stream_options`.
    .requiresStreamUsageOption = false,
  };

  OriginalToolName originalTool(const std::string& name, const rt::OrderedMap<std::string, OriginalToolName>& toolNames) {
    if (const OriginalToolName* found = toolNames.get(name)) return *found;
    return OriginalToolName{.name = name, .namespace_ = std::nullopt};
  }

  std::string originalToolName(const std::string& name, const rt::OrderedMap<std::string, OriginalToolName>& toolNames) {
    return originalTool(name, toolNames).name;
  }
}
