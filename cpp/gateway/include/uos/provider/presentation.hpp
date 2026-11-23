#pragma once
// Port of src/provider/presentation.ts.
//
// The presentation tables are `inline const` values built from literals; PROVIDER_PRESENTATION
// keeps the TS object's property order (rt::OrderedMap), which `Object.entries` exposes.
#include "uos/provider/health.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace uos::provider::presentation {
  // ── Provider presentation ────────────────────────────────────────────────────

  // One `{ id, label }` entry of PROVIDER_TIERS.
  struct ProviderTier {
    std::string_view id;
    std::string_view label;
  };

  /**
   * The waterfall tiers, in the order the gateway tries them. The operator panel
   * renders its tier filter from this list, so a new tier is one entry here.
   */
  inline constexpr std::array<ProviderTier, 4> PROVIDER_TIERS = {{
    {"subscription", "Subscription"},
    {"paid", "Paid fallback"},
    {"direct", "Direct"},
    {"gateway", "Gateway"},
  }};

  // `(typeof PROVIDER_TIERS)[number]["id"]`
  using ProviderTierId = std::string;

  struct ProviderPresentation {
    std::string label;
    ProviderTierId tier;
    std::string detail;
    std::vector<std::string> endpoints;
    /** The key the provider-health view reports this route under. */
    health::RecordProvider health_key;
    friend bool operator==(const ProviderPresentation&, const ProviderPresentation&) = default;
  };

  /**
   * The gateway-owned provider identity behind the synthetic
   * `ubiquity/deepseek-flash` model. It is an ordinary selectable provider:
   * checking it allows the route, and an active selection without it switches the
   * route off.
   */
  inline constexpr std::string_view GATEWAY_PROVIDER_ID = "ubiquity";

  inline const ProviderPresentation GATEWAY_PROVIDER_PRESENTATION{
    .label = "Ubiquity",
    .tier = "gateway",
    .detail =
      "Gateway-owned, always-current DeepSeek Flash waterfall: official DeepSeek, OpenRouter, LithosAI, Surplus, and OpenLux when they advertise the current release.",
    .endpoints = {"/v1/responses"},
    .health_key = "ubiquity",
  };

  /**
   * Display copy for every selectable provider, in the order the roster lists
   * them. The panel holds no provider list of its own: it renders these rows, so
   * adding a provider here is what makes it appear in the operator UI.
   */
  // `Readonly<Record<SelectableProviderId, ProviderPresentation>>`, in the TS object's property order.
  inline const rt::OrderedMap<selection::SelectableProviderId, ProviderPresentation> PROVIDER_PRESENTATION{
    {"codex",
     {
       .label = "Codex",
       .tier = "subscription",
       .detail = "ChatGPT subscription capacity. The waterfall always tries it first.",
       .endpoints = {"/v1/responses", "/v1/chat/completions"},
       .health_key = "codex",
     }},
    {"claude_code",
     {
       .label = "Claude Code",
       .tier = "subscription",
       .detail =
         "Claude subscription capacity. Tried before the paid waterfall for Claude models when a credential, the principal allowlist, and a workspace are configured.",
       .endpoints = {"/v1/responses", "/v1/chat/completions"},
       .health_key = "claude_code",
     }},
    {"gemini",
     {
       .label = "Gemini",
       .tier = "subscription",
       .detail =
         "Antigravity Starter subscription through the official CLI headless command. Gemini 3.8 Flash is text-only; credentials, quota resets, and credit settings remain owned by the signed-in CLI.",
       .endpoints = {"/v1/responses", "/v1/chat/completions"},
       .health_key = "gemini",
     }},
    {"surplus",
     {
       .label = "Metered 1",
       .tier = "paid",
       .detail = "Surplus Intelligence. Second tier of the paid waterfall.",
       .endpoints = {"/v1/responses", "/v1/chat/completions"},
       .health_key = "surplus",
     }},
    {"openlux",
     {
       .label = "Metered 2",
       .tier = "paid",
       .detail = "OpenLux. Last tier of the paid waterfall.",
       .endpoints = {"/v1/responses", "/v1/chat/completions"},
       .health_key = "metered",
     }},
    {"deepseek",
     {
       .label = "DeepSeek",
       .tier = "direct",
       .detail = "Official DeepSeek key, served on Chat Completions upstream and Responses through the gateway's translation.",
       .endpoints = {"/v1/chat/completions", "/v1/responses"},
       .health_key = "deepseek",
     }},
    {"cerebras",
     {
       .label = "Cerebras",
       .tier = "direct",
       .detail = "GPT-OSS 120B, served on Chat Completions only.",
       .endpoints = {"/v1/chat/completions"},
       .health_key = "cerebras",
     }},
    {"openrouter",
     {
       .label = "OpenRouter",
       .tier = "direct",
       .detail =
         "OpenRouter's served text catalogue on both OpenAI wires, plus typed decisions (noul, choice, score, and box) on the Decisions API.",
       .endpoints = {"/v1/chat/completions", "/v1/responses", "/v1/decisions", "/v1/systemone"},
       .health_key = "openrouter",
     }},
    {"lithos",
     {
       .label = "LithosAI",
       .tier = "direct",
       .detail = "LithosAI key, served on Chat Completions upstream and Responses through the gateway's translation.",
       .endpoints = {"/v1/chat/completions", "/v1/responses"},
       .health_key = "lithos",
     }},
    {"ubiquity", GATEWAY_PROVIDER_PRESENTATION},
  };

  /**
   * Presentation for any roster id: the table entry, or a complete derived one so
   * a provider added to `SELECTABLE_PROVIDER_IDS` renders correctly before anyone
   * writes its copy.
   */
  ProviderPresentation providerPresentation(const std::string& id);

  // ---- JSON conversion (members in declaration order) --------------------------------------
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderPresentation& value);
  ProviderPresentation tag_invoke(const json::value_to_tag<ProviderPresentation>&, const json::value& value);
}
