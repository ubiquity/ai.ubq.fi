#include "uos/models/metadata.hpp"

#include "uos/recent_model_context.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"

#include <cstdint>
#include <initializer_list>
#include <utility>

namespace uos::models::metadata {
  namespace {
    using defaults::normalizeReasoningEffort;
    using defaults::ReasoningEffort;
    using openrouter_models::OpenRouterModelMetadata;
    using rt::record::Reader;
    using rt::record::Writer;

    // `positiveTokenCount(value: unknown)` over a JSON member (nullptr is undefined), a
    // `number | null` and a `number | null | undefined`.
    std::optional<double> positiveTokenCount(std::optional<double> value) {
      if (value && rt::Number::isSafeInteger(*value) && *value > 0) return value;
      return std::nullopt;
    }
    std::optional<double> positiveTokenCount(const json::value* value) { return positiveTokenCount(rt::getNumber(value)); }
    std::optional<double> positiveTokenCount(const rt::Nullable<double>& value) { return positiveTokenCount(value.toOptional()); }

    std::optional<double> firstTokenCount(std::initializer_list<rt::Nullable<double>> values) {
      for (const rt::Nullable<double>& value : values) {
        const std::optional<double> resolved = positiveTokenCount(value);
        if (resolved) return resolved;
      }
      return std::nullopt;
    }

    // `hint?.<member>`, nullptr being a null hint.
    rt::Nullable<double> member(const ModelMetadataHint* hint, rt::Nullable<double> ModelMetadataHint::*field) {
      return hint != nullptr ? hint->*field : rt::Nullable<double>();
    }

    /**
     * Advertised tiers arrive as bare strings, `null` (an explicit no-reasoning
     * entry), or `{ effort }` objects. Every non-empty advertised tier is preserved
     * in its advertised order: the gateway has no tier allowlist to enforce.
     */
    // `levels: readonly unknown[] | null | undefined`: nullptr is null or undefined.
    std::vector<ReasoningEffort> advertisedReasoningLevels(const json::array* levels) {
      if (levels == nullptr) return {};
      std::vector<ReasoningEffort> resolved;
      for (const json::value& entry : *levels) {
        const json::object* object = entry.if_object();
        const json::value* raw = object != nullptr && rt::hasOwn(*object, "effort") ? rt::get(*object, "effort") : &entry;
        const std::optional<ReasoningEffort> level = raw->is_null() ? std::optional<ReasoningEffort>("none") : normalizeReasoningEffort(raw);
        if (rt::truthy(level) && !rt::includes(resolved, *level)) resolved.push_back(*level);
      }
      return resolved;
    }

    // The catalog keeps windows as integers; std::nullopt is null.
    rt::Nullable<double> tokenCountOrNull(const std::optional<std::int64_t>& value) {
      if (!value) return nullptr;
      return static_cast<double>(*value);
    }

    ModelMetadataHint openRouterHint(const OpenRouterModelMetadata& metadata) {
      ModelMetadataHint hint;
      hint.context_window_tokens = tokenCountOrNull(metadata.context_window_tokens);
      hint.max_context_window_tokens = tokenCountOrNull(metadata.max_context_window_tokens);
      if (metadata.reasoning) {
        json::array efforts;
        for (const std::string& effort : metadata.reasoning->supported_efforts) efforts.emplace_back(json::string(effort));
        hint.supported_reasoning_levels = std::move(efforts);
      } else {
        hint.supported_reasoning_levels = nullptr;
      }
      hint.default_reasoning_effort =
        metadata.reasoning && metadata.reasoning->default_effort ? json::value(json::string(*metadata.reasoning->default_effort)) : json::value(nullptr);
      return hint;
    }

    struct DefaultLevelResolution {
      std::vector<ReasoningEffort> levels;
      std::optional<ReasoningEffort> defaultLevel;
    };

    /**
     * Advertised tiers pass through verbatim, plus the source's own default level when
     * it did not list it. No tier is invented: the upstream rejects an effort its
     * catalog omits (`gpt-6-astra` refuses `none`), so a fabricated tier is a promise
     * the endpoint breaks with a 400.
     */
    DefaultLevelResolution withDefaultLevel(const std::vector<ReasoningEffort>& advertised, std::optional<ReasoningEffort> defaultLevel) {
      std::vector<ReasoningEffort> levels = advertised;
      if (rt::truthy(defaultLevel) && !rt::includes(levels, *defaultLevel)) levels.push_back(*defaultLevel);
      return {.levels = std::move(levels), .defaultLevel = std::move(defaultLevel)};
    }
  }

  std::optional<ModelMetadataHint> codexSnapshotMetadataHint(const json::object* record) {
    if (record == nullptr) return std::nullopt;
    const json::value* defaultLevel = rt::get(*record, "default_reasoning_level");
    const bool explicitNoneDefault = rt::hasOwn(*record, "default_reasoning_level") && defaultLevel != nullptr && defaultLevel->is_null();
    const json::value* levels = rt::get(*record, "supported_reasoning_levels");
    ModelMetadataHint hint;
    hint.context_window_tokens = rt::orNull(positiveTokenCount(rt::get(*record, "context_window")));
    hint.max_context_window_tokens = rt::orNull(positiveTokenCount(rt::get(*record, "max_context_window")));
    hint.auto_compact_token_limit_tokens = rt::orNull(positiveTokenCount(rt::get(*record, "auto_compact_token_limit")));
    hint.effective_context_window_percent = rt::orNull(positiveTokenCount(rt::get(*record, "effective_context_window_percent")));
    if (levels != nullptr && levels->is_array()) hint.supported_reasoning_levels = levels->get_array();
    else hint.supported_reasoning_levels = nullptr;
    if (explicitNoneDefault) hint.default_reasoning_effort = json::value(json::string("none"));
    else if (defaultLevel != nullptr) hint.default_reasoning_effort = *defaultLevel;
    return hint;
  }

  std::optional<ModelMetadataHint> codexSnapshotMetadataHint(const json::object& record) { return codexSnapshotMetadataHint(&record); }

  ModelMetadataHint codexSubscriptionMetadataHint() {
    ModelMetadataHint hint;
    hint.context_window_tokens = CODEX_SUBSCRIPTION_CONTEXT_WINDOW_TOKENS;
    hint.max_context_window_tokens = CODEX_SUBSCRIPTION_MAX_CONTEXT_WINDOW_TOKENS;
    return hint;
  }

  namespace {
    struct WindowResolution {
      std::optional<double> tokens;
      ModelMetadataSource source;
    };

    struct WindowCandidate {
      ModelMetadataSource source;
      std::optional<double> value;
    };

    /**
     * OpenRouter is the capability catalog for context windows: it publishes the
     * model's real maximum, while the Codex endpoint's catalog understates what it
     * accepts (916,463 tokens verified for `gpt-6-astra` against a 272,000 served
     * window and an 872,000 override cap). First-party statements only fill ids
     * OpenRouter does not know, in descending authority. A client that wants to stay
     * inside a cheaper tier sets its own window or compaction limit.
     */
    WindowResolution windowFrom(const std::vector<WindowCandidate>& candidates) {
      std::optional<double> tokens;
      ModelMetadataSource source = "unknown";
      for (const auto& [candidateSource, value] : candidates) {
        const std::optional<double> count = positiveTokenCount(value);
        if (!count) continue;
        // OpenRouter leads the list, so it also breaks ties. A narrower third-party
        // entry never shrinks a route below what the route itself declares.
        if (!tokens || *count > *tokens) {
          tokens = count;
          source = candidateSource;
        }
      }
      return {.tokens = tokens, .source = source};
    }

    std::optional<double> hintWindow(const ModelMetadataHint* hint) {
      const std::optional<double> window = positiveTokenCount(member(hint, &ModelMetadataHint::context_window_tokens));
      if (window) return window;
      return positiveTokenCount(member(hint, &ModelMetadataHint::max_context_window_tokens));
    }

    /** The wider of a declared maximum and the active window it has to contain. */
    std::optional<double> containingWindow(std::optional<double> contextWindow, std::optional<double> declaredMaxWindow) {
      if (!contextWindow) return declaredMaxWindow;
      if (!declaredMaxWindow) return contextWindow;
      return rt::Math::max(*contextWindow, *declaredMaxWindow);
    }

    struct ReasoningResolution {
      std::vector<ReasoningEffort> levels;
      std::optional<ReasoningEffort> defaultLevel;
      ModelMetadataSource source;
    };

    /** First source, in precedence order, that advertises at least one tier. */
    ReasoningResolution reasoningFrom(const ModelMetadataHint* codex, const ModelMetadataHint* provider, const ModelMetadataHint* enrichment) {
      const std::pair<const char*, const ModelMetadataHint*> candidates[] = {
        {"codex_upload", codex},
        {"provider_discovery", provider},
        {"openrouter", enrichment},
      };
      for (const auto& [source, hint] : candidates) {
        const json::array* levels = hint != nullptr && hint->supported_reasoning_levels.hasValue() ? &*hint->supported_reasoning_levels : nullptr;
        const json::value* defaultEffort = hint != nullptr && hint->default_reasoning_effort ? &*hint->default_reasoning_effort : nullptr;
        DefaultLevelResolution resolved = withDefaultLevel(advertisedReasoningLevels(levels), normalizeReasoningEffort(defaultEffort));
        if (!resolved.levels.empty()) return {.levels = std::move(resolved.levels), .defaultLevel = std::move(resolved.defaultLevel), .source = source};
      }
      return {.levels = {}, .defaultLevel = std::nullopt, .source = "unknown"};
    }
  }

  ResolvedModelMetadata resolveModelMetadata(std::string_view modelId) { return resolveModelMetadata(modelId, ModelMetadataSources{}); }

  ResolvedModelMetadata resolveModelMetadata(std::string_view modelId, const ModelMetadataSources& sources) {
    const std::optional<OpenRouterModelMetadata> openRouter =
      sources.openRouter.isUndefined() ? openrouter_models::openRouterMetadataFor(modelId) : sources.openRouter.toOptional();
    const ModelMetadataHint* codex = sources.codex.hasValue() ? &*sources.codex : nullptr;
    const ModelMetadataHint* subscription = sources.codexSubscription.hasValue() ? &*sources.codexSubscription : nullptr;
    const ModelMetadataHint* provider = sources.provider.hasValue() ? &*sources.provider : nullptr;
    const std::optional<ModelMetadataHint> enrichmentHint = openRouter ? std::optional<ModelMetadataHint>(openRouterHint(*openRouter)) : std::nullopt;
    const ModelMetadataHint* enrichment = enrichmentHint ? &*enrichmentHint : nullptr;
    // Once the uploaded endpoint declares a window, its subscription bound is
    // only a fallback and must not participate as a wider peer candidate.
    const ModelMetadataHint* subscriptionFallback = !hintWindow(codex) ? subscription : nullptr;

    const WindowResolution resolvedWindow = windowFrom({
      {"openrouter", hintWindow(enrichment)},
      {"codex_upload", hintWindow(codex)},
      {"provider_discovery", hintWindow(provider)},
      {"codex_subscription", hintWindow(subscriptionFallback)},
    });
    const std::optional<double> contextWindow = resolvedWindow.tokens;
    const std::optional<double> declaredMaxWindow =
      windowFrom({
                   {"openrouter", positiveTokenCount(member(enrichment, &ModelMetadataHint::max_context_window_tokens))},
                   {"codex_upload", positiveTokenCount(member(codex, &ModelMetadataHint::max_context_window_tokens))},
                   {"provider_discovery", positiveTokenCount(member(provider, &ModelMetadataHint::max_context_window_tokens))},
                   {"codex_subscription", positiveTokenCount(member(subscriptionFallback, &ModelMetadataHint::max_context_window_tokens))},
                 })
        .tokens;
    const std::optional<double> declaredAutoCompact =
      firstTokenCount({member(codex, &ModelMetadataHint::auto_compact_token_limit_tokens), member(provider, &ModelMetadataHint::auto_compact_token_limit_tokens)});
    const rt::Nullable<double> codexPercent = member(codex, &ModelMetadataHint::effective_context_window_percent);
    const rt::Nullable<double> declaredPercent = codexPercent.hasValue() ? codexPercent : member(provider, &ModelMetadataHint::effective_context_window_percent);
    const ReasoningResolution reasoning = reasoningFrom(codex, provider, enrichment);

    const std::optional<double> percent = positiveTokenCount(declaredPercent);
    std::optional<double> autoCompact = declaredAutoCompact;
    if (contextWindow) autoCompact = recent_model_context::resolvedAutoCompactTokenLimit(*contextWindow, declaredAutoCompact);
    std::optional<double> effectivePercent = percent;
    if (!effectivePercent && contextWindow) effectivePercent = recent_model_context::CODEX_EFFECTIVE_CONTEXT_WINDOW_PERCENT;
    return ResolvedModelMetadata{
      .context_window_tokens = contextWindow,
      .max_context_window_tokens = containingWindow(contextWindow, declaredMaxWindow),
      .auto_compact_token_limit_tokens = autoCompact,
      .effective_context_window_percent = effectivePercent,
      .supported_reasoning_levels = !reasoning.levels.empty() ? std::optional<std::vector<ReasoningEffort>>(reasoning.levels) : std::nullopt,
      .default_reasoning_effort = reasoning.defaultLevel,
      .context_source = resolvedWindow.source,
      .reasoning_source = reasoning.source,
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ModelMetadataHint& value) {
    out = Writer()
            .nullable("context_window_tokens", value.context_window_tokens)
            .nullable("max_context_window_tokens", value.max_context_window_tokens)
            .nullable("auto_compact_token_limit_tokens", value.auto_compact_token_limit_tokens)
            .nullable("effective_context_window_percent", value.effective_context_window_percent)
            .nullable("supported_reasoning_levels", value.supported_reasoning_levels)
            .orUndefined("default_reasoning_effort", value.default_reasoning_effort)
            .done();
  }

  ModelMetadataHint tag_invoke(const json::value_to_tag<ModelMetadataHint>&, const json::value& value) {
    const Reader in(value, "ModelMetadataHint");
    return {
      .context_window_tokens = in.nullable<double>("context_window_tokens"),
      .max_context_window_tokens = in.nullable<double>("max_context_window_tokens"),
      .auto_compact_token_limit_tokens = in.nullable<double>("auto_compact_token_limit_tokens"),
      .effective_context_window_percent = in.nullable<double>("effective_context_window_percent"),
      .supported_reasoning_levels = in.nullable<json::array>("supported_reasoning_levels"),
      .default_reasoning_effort = in.orUndefined<json::value>("default_reasoning_effort"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ResolvedModelMetadata& value) {
    out = Writer()
            .orNull("context_window_tokens", value.context_window_tokens)
            .orNull("max_context_window_tokens", value.max_context_window_tokens)
            .orNull("auto_compact_token_limit_tokens", value.auto_compact_token_limit_tokens)
            .orNull("effective_context_window_percent", value.effective_context_window_percent)
            .orNull("supported_reasoning_levels", value.supported_reasoning_levels)
            .orNull("default_reasoning_effort", value.default_reasoning_effort)
            .required("context_source", value.context_source)
            .required("reasoning_source", value.reasoning_source)
            .done();
  }

  ResolvedModelMetadata tag_invoke(const json::value_to_tag<ResolvedModelMetadata>&, const json::value& value) {
    const Reader in(value, "ResolvedModelMetadata");
    return {
      .context_window_tokens = in.orNull<double>("context_window_tokens"),
      .max_context_window_tokens = in.orNull<double>("max_context_window_tokens"),
      .auto_compact_token_limit_tokens = in.orNull<double>("auto_compact_token_limit_tokens"),
      .effective_context_window_percent = in.orNull<double>("effective_context_window_percent"),
      .supported_reasoning_levels = in.orNull<std::vector<ReasoningEffort>>("supported_reasoning_levels"),
      .default_reasoning_effort = in.orNull<std::string>("default_reasoning_effort"),
      .context_source = in.required<std::string>("context_source"),
      .reasoning_source = in.required<std::string>("reasoning_source"),
    };
  }
}
