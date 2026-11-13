import { initializeTestNativeKv } from "../../scripts/test-native-kv.ts";
import { setKvForTest } from "../../src/kv.ts";
import { openKv } from "../../src/storage/kv.ts";
initializeTestNativeKv({
  artifactDirectory: "/home/codex/repos/ubiquity/ai.ubq.fi/.data/releases/d94fef52ccc76e0d5aa019154f11e47d5e10fba6/.native/kv",
  expectedProvenanceSha256: "57d7405783c9f34343f3d4fe089b3733effc079d9521cd341a3dc5bc0c87c201",
  inputsManifest: new URL("../../tools/native-kv/inputs.json", import.meta.url).pathname,
});
const database = await openKv(":memory:");
setKvForTest(database);
export const { startConformanceMock } = await import("../../tests/helpers/responses-conformance-fixture.ts");
const { shutdownOptionalTelemetry } = await import("../../serve.ts");
export const closeMockEnvironment = async () => {
  await shutdownOptionalTelemetry();
  database.close();
  setKvForTest(null);
};
