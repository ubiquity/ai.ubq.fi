// Provider-level candidate roster for the DeepSeek V4.1 Flash waterfall benchmark.
//
// Each candidate is addressed through the local gateway by a distinct client
// model id. Surplus and OpenLux share the paid catalogue id
// `deepseek-v4.1-flash`, so the runner pins the paid tier by narrowing the
// gateway's provider selection to exactly the candidate while its batch runs.

export type ProviderId = "surplus" | "openlux" | "lithos" | "deepseek" | "openrouter";

export type ClientWire = "responses" | "chat";

export type ProviderSpec = Readonly<{
  id: ProviderId;
  label: string;
  /** Client-facing model id used for this provider's requests. */
  model: string;
  /** Client wire the provider can serve for this model today. */
  wire: ClientWire;
  /**
   * Provider selection pinned while this candidate's batch runs. The gateway
   * narrows routing to exactly these ids; an empty list means no narrowing.
   */
  selection: readonly string[];
  /** Expected `x-uos-upstream` value on the gateway response. */
  expected_upstream: string;
}>;

export const BENCHMARK_PROVIDERS: readonly ProviderSpec[] = [
  {
    id: "surplus",
    label: "Surplus Intelligence",
    model: "deepseek-v4.1-flash",
    wire: "responses",
    selection: ["surplus"],
    expected_upstream: "surplus",
  },
  {
    id: "openlux",
    label: "OpenLux",
    model: "deepseek-v4.1-flash",
    wire: "chat",
    selection: ["openlux"],
    expected_upstream: "metered",
  },
  {
    id: "lithos",
    label: "LithosAI",
    model: "deepseek-ai/DeepSeek-V4.1-Flash",
    wire: "responses",
    selection: ["lithos"],
    expected_upstream: "lithos",
  },
  {
    id: "deepseek",
    label: "DeepSeek direct",
    model: "deepseek-flash",
    wire: "responses",
    selection: ["deepseek"],
    expected_upstream: "deepseek",
  },
  {
    id: "openrouter",
    label: "OpenRouter",
    model: "deepseek/deepseek-v4.1-flash",
    wire: "responses",
    selection: ["openrouter"],
    expected_upstream: "openrouter",
  },
] as const;

export const providerById = (id: string): ProviderSpec => {
  const found = BENCHMARK_PROVIDERS.find((provider) => provider.id === id);
  if (!found) throw new Error(`Unknown benchmark provider: ${id}`);
  return found;
};
