import type { RecordProvider } from "./provider_health.ts";
import type { SelectableProviderId } from "./provider_selection.ts";

// ── Provider presentation ────────────────────────────────────────────────────

/**
 * The waterfall tiers, in the order the gateway tries them. The operator panel
 * renders its tier filter from this list, so a new tier is one entry here.
 */
export const PROVIDER_TIERS = [
  { id: "subscription", label: "Subscription" },
  { id: "paid", label: "Paid fallback" },
  { id: "direct", label: "Direct" },
] as const;

export type ProviderTierId = (typeof PROVIDER_TIERS)[number]["id"];

export type ProviderPresentation = Readonly<{
  label: string;
  tier: ProviderTierId;
  detail: string;
  endpoints: readonly string[];
  /** The key the provider-health view reports this route under. */
  health_key: RecordProvider;
}>;

/**
 * Display copy for every selectable provider, in the order the roster lists
 * them. The panel holds no provider list of its own: it renders these rows, so
 * adding a provider here is what makes it appear in the operator UI.
 */
export const PROVIDER_PRESENTATION: Readonly<Record<SelectableProviderId, ProviderPresentation>> = {
  codex: {
    label: "Codex",
    tier: "subscription",
    detail: "ChatGPT subscription capacity. The waterfall always tries it first.",
    endpoints: ["/v1/responses", "/v1/chat/completions"],
    health_key: "codex",
  },
  surplus: {
    label: "Metered 1",
    tier: "paid",
    detail: "Surplus Intelligence. Second tier of the paid waterfall.",
    endpoints: ["/v1/responses", "/v1/chat/completions"],
    health_key: "surplus",
  },
  openlux: {
    label: "Metered 2",
    tier: "paid",
    detail: "OpenLux. Last tier of the paid waterfall.",
    endpoints: ["/v1/responses", "/v1/chat/completions"],
    health_key: "metered",
  },
  deepseek: {
    label: "DeepSeek",
    tier: "direct",
    detail: "Official DeepSeek key, served on Chat Completions only.",
    endpoints: ["/v1/chat/completions"],
    health_key: "deepseek",
  },
  cerebras: {
    label: "Cerebras",
    tier: "direct",
    detail: "GPT-OSS 120B, served on Chat Completions only.",
    endpoints: ["/v1/chat/completions"],
    health_key: "cerebras",
  },
  lithos: {
    label: "LithosAI",
    tier: "direct",
    detail: "LithosAI key, served on Chat Completions upstream and Responses through the gateway's translation.",
    endpoints: ["/v1/chat/completions", "/v1/responses"],
    health_key: "lithos",
  },
};

/**
 * Presentation for any roster id: the table entry, or a complete derived one so
 * a provider added to `SELECTABLE_PROVIDER_IDS` renders correctly before anyone
 * writes its copy.
 */
export const providerPresentation = (id: string): ProviderPresentation => {
  const known = Object.entries(PROVIDER_PRESENTATION).find(([knownId]) => knownId === id)?.[1];
  if (known) return known;
  return {
    label: id,
    tier: "direct",
    detail: `${id} has no presentation entry yet.`,
    endpoints: [],
    // The fallback reports under its own id, the convention every direct route
    // already follows, so a new provider can be rostered before its health key.
    health_key: id as RecordProvider,
  };
};
