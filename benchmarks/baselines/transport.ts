/**
 * Injected chat transports for the m03 baseline adapters.
 *
 * The benchmark contract is hermetic by default: adapters A/B/D are refused
 * by the runner because they flag `requiresExternalInference`, and every one
 * of them accepts an injected transport so focused tests stay deterministic
 * and offline. A real transport is only attached when a consumer explicitly
 * configures one (the orchestrator owns the approved live gate).
 *
 * Interface: a {@link ChatTransport} takes a Chat Completions request body
 * and returns a minimal response object. The native `Response` type
 * satisfies it structurally, so the gateway transport can be used directly;
 * tests substitute plain objects.
 */

import { fetchCerebrasChatCompletions } from "../../src/cerebras.ts";

export interface ChatTransportOptions {
  signal?: AbortSignal;
}

/** Minimal response shape; the native `Response` satisfies it. */
export interface ChatTransportResponse {
  readonly status: number;
  readonly ok: boolean;
  json(): Promise<unknown>;
}

export type ChatTransport = (
  body: Record<string, unknown>,
  options?: ChatTransportOptions,
) => Promise<ChatTransportResponse>;

/**
 * The live gateway transport for the current GPT-OSS Chat Completions
 * behavior: exactly the transport `src/openai.ts` uses for
 * `gpt-oss-120b` (`fetchCerebrasChatCompletions` with the existing
 * `CEREBRAS_API_KEY`, the gateway deadline and Cerebras schema projection).
 * Constructing the transport reads nothing; the key is resolved per request.
 */
export const gatewayChatTransport = (): ChatTransport => {
  return (body, options) =>
    fetchCerebrasChatCompletions(body, {
      apiKey: undefined,
      ...(options?.signal ? { signal: options.signal } : {}),
    });
};

/**
 * Generic OpenAI-compatible transport used by the strong control adapter
 * (D). The API key is supplied by the configuration object; this helper
 * never reads process environment variables, so no new secret interface is
 * introduced without owner approval.
 */
export const openAICompatibleTransport = (baseUrl: string, apiKey: string | null): ChatTransport => {
  const url = `${baseUrl.replace(/\/+$/, "")}/chat/completions`;
  return async (body, options) => {
    const headers = new Headers({
      Accept: "application/json",
      "Content-Type": "application/json",
    });
    if (apiKey) headers.set("Authorization", `Bearer ${apiKey}`);
    return await fetch(url, {
      method: "POST",
      headers,
      body: JSON.stringify(body),
      signal: options?.signal,
    });
  };
};
