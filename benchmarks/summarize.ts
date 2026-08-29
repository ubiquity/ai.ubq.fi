/**
 * Summarize existing benchmark results without any adapter execution or
 * external inference.
 *
 * Usage (from the repository root):
 *   deno task benchmark:summary                       # aggregate benchmark-runs
 *   deno task benchmark:summary -- --runs=other-root  # another runs root
 *   deno task benchmark:summary -- --json             # JSON to stdout only
 *   deno task benchmark:summary -- --out=out/summary.json
 */

import { aggregateResults, formatSummary } from "./metrics.ts";
import { BenchmarkResult, DEFAULT_RUNS_ROOT, validateBenchmarkResult } from "./schemas.ts";

export interface SummarizeOptions {
  runsRoot: string;
  jsonOnly: boolean;
  outPath: string | null;
}

export function parseSummarizeArgs(argv: string[]): SummarizeOptions | { help: true } {
  // `deno task benchmark:summary -- args` forwards a literal "--"; accept both forms.
  if (argv[0] === "--") argv = argv.slice(1);
  const opts: SummarizeOptions = { runsRoot: DEFAULT_RUNS_ROOT, jsonOnly: false, outPath: null };
  for (const arg of argv) {
    if (arg === "--help" || arg === "-h") return { help: true };
    if (arg.startsWith("--runs=")) opts.runsRoot = arg.slice("--runs=".length);
    else if (arg === "--json") opts.jsonOnly = true;
    else if (arg.startsWith("--out=")) opts.outPath = arg.slice("--out=".length);
    else throw new Error(`unknown argument ${arg} (see --help)`);
  }
  return opts;
}

export const SUMMARIZE_HELP = `usage: deno task benchmark:summary -- [--runs=benchmark-runs] [--json] [--out=path]

  --runs   runs root to scan for result.jsonl files (default: benchmark-runs)
  --json   print the aggregated summary as JSON instead of a table
  --out    write the aggregated summary JSON to this path (default: <runs>/summary.json)`;

/** Recursively collect and validate every result.jsonl file under a runs root. */
export function loadResults(runsRoot: string): BenchmarkResult[] {
  const results: BenchmarkResult[] = [];
  const walk = (dir: string) => {
    for (const entry of Deno.readDirSync(dir)) {
      const p = `${dir}/${entry.name}`;
      if (entry.isDirectory) walk(p);
      else if (entry.isFile && entry.name === "result.jsonl") {
        const line = Deno.readTextFileSync(p).split("\n").find((l) => l.trim() !== "");
        if (line === undefined) throw new Error(`${p}: empty result.jsonl`);
        results.push(validateBenchmarkResult(JSON.parse(line)));
      }
    }
  };
  if (!existsDir(runsRoot)) return results;
  walk(runsRoot);
  return results.sort((a, b) => a.created_at.localeCompare(b.created_at));
}

function existsDir(path: string): boolean {
  try {
    return Deno.statSync(path).isDirectory;
  } catch {
    return false;
  }
}

if (import.meta.main) {
  try {
    const parsed = parseSummarizeArgs(Deno.args);
    if ("help" in parsed) {
      console.log(SUMMARIZE_HELP);
      Deno.exit(0);
    }
    const results = loadResults(parsed.runsRoot);
    if (results.length === 0) console.error(`no result.jsonl files found under ${parsed.runsRoot}`);
    const summary = aggregateResults(results, parsed.runsRoot);
    if (parsed.jsonOnly) {
      console.log(JSON.stringify(summary, null, 2));
    } else {
      const outPath = parsed.outPath ?? `${parsed.runsRoot}/summary.json`;
      Deno.writeTextFileSync(outPath, JSON.stringify(summary, null, 2) + "\n");
      console.log(formatSummary(summary, true));
      console.log(`\nsummary written to ${outPath}`);
    }
  } catch (err) {
    console.error(`error: ${(err as Error).message}`);
    console.error(SUMMARIZE_HELP);
    Deno.exit(2);
  }
}
