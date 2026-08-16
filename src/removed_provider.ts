export type RemovedProviderResponseTiming = Readonly<{
  onDispatch?: () => void;
  onHeaders?: () => void;
}>;

let apiKeyForTest: string | null | undefined;
export const readRemovedProviderApiKey = (): string | null => apiKeyForTest ?? null;
export const setRemovedProviderApiKeyForTest = (value: string | null | undefined): void => {
  apiKeyForTest = value;
};

export const deriveRemovedProviderSessionId = (..._args: unknown[]): Promise<null> => Promise.resolve(null);

export const fetchRemovedProviderResponses = (
  _body: Record<string, unknown>,
  _options: Readonly<
    {
      apiKey: string;
      sessionId: string | null;
      signal: AbortSignal;
      timing?: RemovedProviderResponseTiming;
      beforeDispatch?: () => Promise<unknown>;
    }
  >,
): Promise<{ response: Response }> => Promise.reject(new Error("The removed provider is disabled."));

export const isEligibleRemovedProviderModel = (_model: string): boolean => false;
export const removedProviderModelFromEvent = (_value: Record<string, unknown>): string | null => null;
export const removedProviderTaskTypeFromResponse = (_value: Record<string, unknown>): string | null => null;
export const stripRemovedProviderMetadata = (value: Record<string, unknown>): Record<string, unknown> => value;
