const [path, operation, rawIterations, rawSamples] = process.argv.slice(2);
const iterations = Number(rawIterations);
const samples = Number(rawSamples);
if (Bun.version !== "1.4.0") throw new Error("JSON oracle requires pinned Bun 1.4.0");
if (!path || !["parse", "stringify"].includes(operation)) throw new Error("Expected input, operation, iterations and samples");
if (!Number.isInteger(iterations) || iterations < 1 || iterations > 200000 || !Number.isInteger(samples) || samples < 1 || samples > 31) {
  throw new Error("Invalid benchmark bound");
}
const text = await Bun.file(path).text();
if (!text.length || Buffer.byteLength(text) > 2 * 1024 * 1024) throw new Error("Benchmark input exceeds bound");
const coldParseStarted = performance.now();
const initial: unknown = JSON.parse(text);
const coldParseUs = (performance.now() - coldParseStarted) * 1000;
const coldStringifyStarted = performance.now();
const canonical = JSON.stringify(initial);
const coldStringifyUs = (performance.now() - coldStringifyStarted) * 1000;
const checksum = (value: string): string => {
  let digest = 1469598103934665603n;
  for (const byte of new TextEncoder().encode(value)) digest = BigInt.asUintN(64, (digest ^ BigInt(byte)) * 1099511628211n);
  return String(digest);
};
let consumed = 0;
const elapsed: number[] = [];
for (let sample = 0; sample < samples; sample++) {
  const started = performance.now();
  for (let iteration = 0; iteration < iterations; iteration++) {
    if (operation === "parse") {
      const value: unknown = JSON.parse(text);
      consumed += value !== null && typeof value === "object" && !Array.isArray(value) ? Object.keys(value).length : 1;
    } else {
      consumed += JSON.stringify(initial).length;
    }
  }
  elapsed.push((performance.now() - started) * 1000);
}
console.log(
  JSON.stringify({
    runtime: "bun",
    operation,
    input_bytes: Buffer.byteLength(text),
    canonical_bytes: Buffer.byteLength(canonical),
    canonical_checksum: checksum(canonical),
    iterations,
    cold_operation_us: operation === "parse" ? coldParseUs : coldStringifyUs,
    samples_us: elapsed,
    consumed: String(consumed),
  })
);

export {};
