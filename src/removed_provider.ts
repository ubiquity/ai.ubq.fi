export type RemovedProviderResponseTiming = Readonly<{
  onDispatch?: () => void;
  onHeaders?: () => void;
}>;

export type RemovedProviderResponsesOptions = Readonly<{
  apiKey: string;
  sessionId: string | null;
  signal: AbortSignal;
  timing?: RemovedProviderResponseTiming;
  beforeDispatch?: () => Promise<unknown>;
}>;

/** Test-only adapter for exercising the disabled fallback boundary. */
export type RemovedProviderTestAdapter = Readonly<{
  fetchResponses: (
    body: Record<string, unknown>,
    options: RemovedProviderResponsesOptions,
  ) => Promise<{ response: Response }>;
  modelFromEvent: (value: Record<string, unknown>) => string | null;
  isEligibleModel: (model: string) => boolean;
}>;

let apiKeyForTest: string | null | undefined;
let testAdapter: RemovedProviderTestAdapter | null = null;
export const readRemovedProviderApiKey = (): string | null => apiKeyForTest ?? null;
export const setRemovedProviderApiKeyForTest = (value: string | null | undefined): void => {
  apiKeyForTest = value;
};
export const setRemovedProviderTestAdapterForTest = (value: RemovedProviderTestAdapter | null): void => {
  testAdapter = value;
};

export const deriveRemovedProviderSessionId = (..._args: unknown[]): Promise<null> => Promise.resolve(null);

export const fetchRemovedProviderResponses = (
  body: Record<string, unknown>,
  options: RemovedProviderResponsesOptions,
): Promise<{ response: Response }> =>
  testAdapter?.fetchResponses(body, options) ?? Promise.reject(new Error("The removed provider is disabled."));

export const isEligibleRemovedProviderModel = (model: string): boolean => testAdapter?.isEligibleModel(model) ?? false;
export const removedProviderModelFromEvent = (value: Record<string, unknown>): string | null =>
  testAdapter?.modelFromEvent(value) ?? null;
export const removedProviderTaskTypeFromResponse = (_value: Record<string, unknown>): string | null => null;
export const stripRemovedProviderMetadata = (value: Record<string, unknown>): Record<string, unknown> => value;
