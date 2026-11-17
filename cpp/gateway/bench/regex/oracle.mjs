import { readFileSync } from "node:fs";
import { redactSupervisorSecrets } from "../../../../src/codex/supervisor-secret.ts";

const [manifestPath, caseName, samplesText, warmupsText, iterationsText] = process.argv.slice(2);
const fixture = JSON.parse(readFileSync(manifestPath, "utf8")).find((row) => row.name === caseName);
if (!fixture) throw new Error("Unknown benchmark case");
if (Bun.version !== "1.4.0") throw new Error(`Unqualified Bun ${Bun.version}`);
const samples = Number(samplesText),
  warmups = Number(warmupsText),
  iterations = Number(iterationsText);
const regex = new RegExp(fixture.pattern ?? "", fixture.flags ?? "");
function describeMatch(match) {
  return match && { index: match.index, end: match.index + match[0].length, captures: [...match].map((part) => part ?? null) };
}
function run() {
  regex.lastIndex = fixture.lastIndex ?? 0;
  if (fixture.operation === "redact") return redactSupervisorSecrets(fixture.input);
  if (fixture.operation === "test") return { test: regex.test(fixture.input), lastIndex: regex.lastIndex };
  if (fixture.operation === "exec") return { match: regex.exec(fixture.input), lastIndex: regex.lastIndex };
  if (fixture.operation === "replace") return { text: fixture.input.replace(regex, "[redacted]"), lastIndex: regex.lastIndex };
  if (fixture.operation === "split") return { parts: fixture.input.split(regex), lastIndex: regex.lastIndex };
  if (fixture.operation === "matchAll") return { matches: [...fixture.input.matchAll(regex)], lastIndex: regex.lastIndex };
  throw new Error("Unknown benchmark operation");
}
let result;
for (let warmup = 0; warmup < warmups; ++warmup) for (let iteration = 0; iteration < iterations; ++iteration) result = run();
const times = [];
for (let sample = 0; sample < samples; ++sample) {
  const started = performance.now();
  for (let iteration = 0; iteration < iterations; ++iteration) result = run();
  times.push((performance.now() - started) / iterations);
}
if (fixture.operation === "exec") result.match = describeMatch(result.match);
if (fixture.operation === "matchAll") result.matches = result.matches.map(describeMatch);
console.log(JSON.stringify({ case: caseName, samples_ms: times, result }));
