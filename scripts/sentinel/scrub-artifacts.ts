import { scrubGeneratedEvidence } from "./encrypt-artifacts.ts";

if (import.meta.main) {
  if (Deno.env.get("GITHUB_ACTIONS") !== "true") {
    throw new Error("Sentinel artifact scrubbing may run only in GitHub Actions");
  }
  await scrubGeneratedEvidence(".sentinel");
  console.log("Scrubbed generated Sentinel plaintext after durable evidence upload.");
}
