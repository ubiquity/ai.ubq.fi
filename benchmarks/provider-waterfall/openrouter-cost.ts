// Reconciles OpenRouter's authoritative per-generation cost against the
// expected rate-card cost for every successful OpenRouter attempt that
// recorded a generation id. Metadata-only GETs; writes
// results/openrouter-observed-costs.json.

import { expectedCostMicroUsd } from "./cost.ts";
import type { AttemptRecord } from "./types.ts";

type Json = Record<string, unknown>;
const isRecord = (value: unknown): value is Json => typeof value === "object" && value !== null && !Array.isArray(value);

const readEnv = (name: string): string | null => {
  try {
    const value = Deno.env.get(name);
    return value && value.trim() ? value : null;
  } catch {
    return null;
  }
};

const listJsonl = async (dir: string): Promise<string[]> => {
  const files: string[] = [];
  for await (const entry of Deno.readDir(dir)) {
    if (entry.isFile && entry.name.endsWith(".jsonl") && entry.name.startsWith("openrouter-")) files.push(`${dir}/${entry.name}`);
  }
  return files.sort();
};

const main = async (): Promise<void> => {
  const key = readEnv("OPENROUTER_API_KEY") ?? "";
  const dir = new URL("results/", import.meta.url).pathname;
  const files = await listJsonl(dir);
  const rows: Json[] = [];
  let reconciled = 0;
  let missingId = 0;
  let unavailable = 0;
  for (const file of files) {
    const text = await Deno.readTextFile(file);
    for (const line of text.split("\n")) {
      if (!line.trim()) continue;
      const record = JSON.parse(line) as AttemptRecord;
      if (!record.success || !record.usage) continue;
      if (!record.response_id) {
        missingId += 1;
        continue;
      }
      const expected = expectedCostMicroUsd(
        "openrouter",
        {
          input_tokens: record.usage.input_tokens,
          cached_input_tokens: record.usage.cached_input_tokens,
          cache_write_input_tokens: record.usage.cache_write_input_tokens,
          output_tokens: record.usage.output_tokens,
        },
        Date.parse(record.started_at)
      );
      let observedTotalUsd: number | null = null;
      let upstreamProvider: string | null = null;
      let promptTokens = 0;
      let completionTokens = 0;
      try {
        const response = await fetch(`https://openrouter.ai/api/v1/generation?id=${encodeURIComponent(record.response_id)}`, {
          headers: key ? { Authorization: `Bearer ${key}` } : {},
          signal: AbortSignal.timeout(10_000),
        });
        if (response.ok) {
          const body = (await response.json()) as unknown;
          if (isRecord(body) && isRecord(body.data)) {
            const total = body.data.total_cost;
            upstreamProvider = typeof body.data.provider_name === "string" ? body.data.provider_name : null;
            promptTokens = typeof body.data.tokens_prompt === "number" ? body.data.tokens_prompt : 0;
            completionTokens = typeof body.data.tokens_completion === "number" ? body.data.tokens_completion : 0;
            // Observed 2026-10-06: these responses-wire generations stay at
            // tokens_prompt/completion = 0 and total_cost = 0 on the public
            // generation endpoint even long after completion, so observed
            // billing is unavailable and catalogue pricing is used instead.
            if (typeof total === "number" && Number.isFinite(total) && (promptTokens > 0 || completionTokens > 0)) observedTotalUsd = total;
          }
        }
      } catch {
        // Unavailable lookup; recorded as unavailable below.
      }
      if (observedTotalUsd === null) unavailable += 1;
      if (observedTotalUsd !== null) reconciled += 1;
      rows.push({
        run_id: record.run_id,
        corpus_id: record.corpus_id,
        generation_id: record.response_id,
        upstream_provider: upstreamProvider,
        generation_tokens_prompt: promptTokens,
        generation_tokens_completion: completionTokens,
        expected_micro_usd: expected?.total_micro_usd ?? null,
        observed_micro_usd: observedTotalUsd === null ? null : Math.round(observedTotalUsd * 1_000_000),
        output_tokens: record.usage.output_tokens,
        input_tokens: record.usage.input_tokens,
      });
    }
  }
  await Deno.writeTextFile(
    `${dir}openrouter-observed-costs.json`,
    JSON.stringify({ generated_at: new Date().toISOString(), reconciled, missing_id: missingId, unavailable, rows }, null, 1)
  );
  console.log(`[openrouter-cost] reconciled=${reconciled} missing_id=${missingId} unavailable=${unavailable} rows=${rows.length}`);
};

if (import.meta.main) await main();
