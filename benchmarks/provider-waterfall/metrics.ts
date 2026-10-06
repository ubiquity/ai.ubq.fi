// Metric derivation for the DeepSeek V4.1 Flash provider waterfall benchmark.
//
// Aggregation unit: one (provider, corpus entry) item. Every attempt is a row;
// first-attempt success, retry recovery, speed, usage and cost come from the
// item's attempts, and the failure taxonomy counts classified failures.

import type { AttemptRecord, CorpusClass } from "./types.ts";

export type FailureCounts = Readonly<Record<string, number>>;

export type ProviderMetrics = Readonly<{
  provider: string;
  wire: string;
  model: string;
  samples: number;
  successful_items: number;
  first_attempt_successes: number;
  first_attempt_success_rate: number;
  retried_items: number;
  retry_recoveries: number;
  retry_recovery_rate: number | null;
  attempts_total: number;
  failures_total: number;
  failures: FailureCounts;
  median_ttft_ms: number | null;
  p90_ttft_ms: number | null;
  p95_ttft_ms: number | null;
  p99_ttft_ms: number | null;
  median_tps: number | null;
  p90_tps: number | null;
  p95_tps: number | null;
  median_e2e_ms: number | null;
  p90_e2e_ms: number | null;
  p95_e2e_ms: number | null;
  p99_e2e_ms: number | null;
  input_tokens: number;
  cached_input_tokens: number;
  cache_write_input_tokens: number;
  output_tokens: number;
  reasoning_tokens: number;
  cache_hit_ratio: number | null;
  usage_coverage: number;
  per_class: Readonly<Record<CorpusClass, { samples: number; successes: number }>>;
  by_wire: Readonly<Record<string, number>>;
}>;

export const percentile = (values: readonly number[], fraction: number): number | null => {
  if (!values.length) return null;
  const sorted = [...values].sort((left, right) => left - right);
  if (sorted.length === 1) return sorted[0];
  const index = (sorted.length - 1) * fraction;
  const lower = Math.floor(index);
  const upper = Math.ceil(index);
  if (lower === upper) return sorted[lower];
  return sorted[lower] + (sorted[upper] - sorted[lower]) * (index - lower);
};

const isSuccess = (record: AttemptRecord): boolean => record.success;

const itemKey = (record: AttemptRecord): string => `${record.provider}::${record.corpus_id}`;

export const computeProviderMetrics = (records: readonly AttemptRecord[]): readonly ProviderMetrics[] => {
  const byProvider = new Map<string, AttemptRecord[]>();
  for (const record of records) {
    const list = byProvider.get(record.provider) ?? [];
    list.push(record);
    byProvider.set(record.provider, list);
  }
  const rows: ProviderMetrics[] = [];
  for (const [provider, providerRecords] of byProvider) {
    const items = new Map<string, AttemptRecord[]>();
    for (const record of providerRecords) {
      const list = items.get(itemKey(record)) ?? [];
      list.push(record);
      items.set(itemKey(record), list);
    }
    const classes: Record<CorpusClass, { samples: number; successes: number }> = {
      small: { samples: 0, successes: 0 },
      medium: { samples: 0, successes: 0 },
      large: { samples: 0, successes: 0 },
      xlarge: { samples: 0, successes: 0 },
    };
    const failures: Record<string, number> = {};
    const ttfts: number[] = [];
    const tpsValues: number[] = [];
    const e2eValues: number[] = [];
    const usage = { input_tokens: 0, cached_input_tokens: 0, cache_write_input_tokens: 0, output_tokens: 0, reasoning_tokens: 0, total_tokens: 0 };
    let successfulItems = 0;
    let firstAttemptSuccesses = 0;
    let retriedItems = 0;
    let retryRecoveries = 0;
    let usageRows = 0;
    const byWire: Record<string, number> = {};
    for (const [key, attempts] of items) {
      void key;
      const ordered = [...attempts].sort((left, right) => left.attempt - right.attempt);
      const first = ordered[0];
      const successful = ordered.find(isSuccess) ?? null;
      for (const attempt of ordered) {
        if (!attempt.success && attempt.failure_kind) failures[attempt.failure_kind] = (failures[attempt.failure_kind] ?? 0) + 1;
        byWire[attempt.wire] = (byWire[attempt.wire] ?? 0) + 1;
      }
      classes[first.cls].samples += 1;
      if (successful) {
        successfulItems += 1;
        classes[first.cls].successes += 1;
        if (successful.first_attempt) firstAttemptSuccesses += 1;
        const firstOutput = successful.t_first_output_ms;
        const end = successful.t_end_ms;
        if (firstOutput !== null) ttfts.push(firstOutput);
        if (firstOutput !== null && end !== null && successful.usage && end > firstOutput) {
          tpsValues.push((successful.usage.output_tokens / (end - firstOutput)) * 1000);
        }
        if (end !== null) e2eValues.push(end);
        if (successful.usage) {
          usageRows += 1;
          usage.input_tokens += successful.usage.input_tokens;
          usage.cached_input_tokens += successful.usage.cached_input_tokens;
          usage.cache_write_input_tokens += successful.usage.cache_write_input_tokens;
          usage.output_tokens += successful.usage.output_tokens;
          usage.reasoning_tokens += successful.usage.reasoning_tokens;
          usage.total_tokens += successful.usage.total_tokens;
        }
      }
      if (ordered.length > 1) {
        retriedItems += 1;
        if (!first.success && successful) retryRecoveries += 1;
      }
    }
    const retryOffered = retriedItems;
    rows.push({
      provider,
      wire: providerRecords[0]?.wire ?? "unknown",
      model: providerRecords[0]?.model ?? "unknown",
      samples: items.size,
      successful_items: successfulItems,
      first_attempt_successes: firstAttemptSuccesses,
      first_attempt_success_rate: items.size ? firstAttemptSuccesses / items.size : 0,
      retried_items: retryOffered,
      retry_recoveries: retryRecoveries,
      retry_recovery_rate: retryOffered ? retryRecoveries / retryOffered : null,
      attempts_total: providerRecords.length,
      failures_total: providerRecords.filter((record) => !record.success).length,
      failures,
      median_ttft_ms: percentile(ttfts, 0.5),
      p90_ttft_ms: percentile(ttfts, 0.9),
      p95_ttft_ms: percentile(ttfts, 0.95),
      p99_ttft_ms: percentile(ttfts, 0.99),
      median_tps: percentile(tpsValues, 0.5),
      p90_tps: percentile(tpsValues, 0.9),
      p95_tps: percentile(tpsValues, 0.95),
      median_e2e_ms: percentile(e2eValues, 0.5),
      p90_e2e_ms: percentile(e2eValues, 0.9),
      p95_e2e_ms: percentile(e2eValues, 0.95),
      p99_e2e_ms: percentile(e2eValues, 0.99),
      input_tokens: usage.input_tokens,
      cached_input_tokens: usage.cached_input_tokens,
      cache_write_input_tokens: usage.cache_write_input_tokens,
      output_tokens: usage.output_tokens,
      reasoning_tokens: usage.reasoning_tokens,
      cache_hit_ratio: usage.input_tokens > 0 ? usage.cached_input_tokens / usage.input_tokens : null,
      usage_coverage: successfulItems ? usageRows / successfulItems : 0,
      per_class: classes,
      by_wire: byWire,
    });
  }
  return rows.sort((left, right) => left.provider.localeCompare(right.provider));
};

export const loadRunRecords = async (paths: readonly string[]): Promise<readonly AttemptRecord[]> => {
  const records: AttemptRecord[] = [];
  for (const path of paths) {
    const text = await Deno.readTextFile(path);
    for (const line of text.split("\n")) {
      if (!line.trim()) continue;
      records.push(JSON.parse(line) as AttemptRecord);
    }
  }
  return records;
};

const parseArgs = (args: readonly string[]): Map<string, string> => {
  const map = new Map<string, string>();
  for (const arg of args) {
    const match = /^--([a-z0-9-]+)(?:=(.*))?$/i.exec(arg);
    if (match) map.set(match[1].toLowerCase(), match[2] ?? "true");
  }
  return map;
};

const listJsonl = async (dir: string): Promise<string[]> => {
  const files: string[] = [];
  for await (const entry of Deno.readDir(dir)) {
    if (entry.isFile && entry.name.endsWith(".jsonl")) files.push(`${dir}/${entry.name}`);
  }
  return files.sort();
};

if (import.meta.main) {
  const args = parseArgs(Deno.args);
  const dir = args.get("dir") ?? new URL("results/", import.meta.url).pathname;
  const paths = args.get("files") ? (args.get("files")?.split(",") ?? []) : await listJsonl(dir);
  const all = await loadRunRecords(paths);
  const period = args.get("period") ?? "w1-eve";
  const records = args.get("all") === "true" ? all : all.filter((record) => record.period === period);
  const metrics = computeProviderMetrics(records);
  const out = args.get("out") ?? `${dir.replace(/\/$/, "")}/metrics.json`;
  await Deno.writeTextFile(out, JSON.stringify({ generated_at: new Date().toISOString(), sources: paths, providers: metrics }, null, 1));
  for (const row of metrics) {
    console.log(
      `${row.provider.padEnd(11)} n=${row.samples} ok=${row.successful_items} first=${row.first_attempt_success_rate.toFixed(3)} ` +
        `e2e_p50=${row.median_e2e_ms?.toFixed(0) ?? "-"} e2e_p95=${row.p95_e2e_ms?.toFixed(0) ?? "-"} ` +
        `ttft_p50=${row.median_ttft_ms?.toFixed(0) ?? "-"} tps_p50=${row.median_tps?.toFixed(1) ?? "-"} cache=${row.cache_hit_ratio?.toFixed(3) ?? "-"}`
    );
  }
  console.log(`[metrics] wrote ${out}`);
}
