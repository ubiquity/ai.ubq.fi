import assert from "node:assert/strict";
import { createHash, createHmac, randomBytes, timingSafeEqual } from "node:crypto";
const key = Buffer.alloc(32, 31);
const keyId = "fixture-matrix-key";
const publication = "/uos/native/publication";
const authority = "/uos/native/renewal-authority";
const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
const mac = (text) => createHmac("sha256", key).update(text).digest("base64url");
async function send(site, kind, path, body, badMac = false) {
  const bytes = Buffer.isBuffer(body) ? body : Buffer.from(JSON.stringify(body));
  const nonce = randomBytes(32).toString("base64url");
  const time = Date.now();
  const signature = mac(`uos-native-http-v1\0POST\0${path}\0${keyId}\0${time}\0${nonce}\0${hash(bytes)}`);
  const url = new URL(path, site);
  url.searchParams.set("source_context", kind);
  const response = await fetch(url, { method: "POST", headers: { "content-type": "application/json", "x-uos-native-version": "1", "x-uos-native-key-id": keyId, "x-uos-native-time-ms": String(time), "x-uos-native-nonce": nonce, "x-uos-native-mac": badMac ? "invalid" : signature }, body: bytes });
  const output = Buffer.from(await response.arrayBuffer());
  if (!badMac && response.status === 200) {
    const prefix = path === publication ? "uos-native-publication-response-v1" : "uos-native-renewal-authority-response-v1";
    const expected = Buffer.from(mac(`${prefix}\0POST\0${path}\0${keyId}\0${nonce}\0${hash(bytes)}\0${response.status}\0${hash(output)}`), "base64url");
    const actual = Buffer.from(response.headers.get("x-uos-native-response-mac") ?? "", "base64url");
    assert.equal(actual.length, 32);
    assert.ok(timingSafeEqual(expected, actual), "actual request/status/body response MAC");
  }
  return { response, output, value: output.length ? JSON.parse(output.toString("utf8")) : null };
}
// The controller provides eight isolated stores bound to the same entrypoint-only artifact.
// URLs are ordinary controlled client endpoints, never actor/proof/store callbacks.
export async function runServedSourceCases(sites) {
  const observations = [];
  for (const kind of ["live_mapped", "live_create", "cache", "inference"]) {
    for (const mode of ["changed", "unchanged"]) {
      const site = sites[kind][mode];
      const health = new URL("/health", site);
      health.searchParams.set("source_context", kind);
      const prepared = await fetch(health);
      assert.equal(prepared.status, 200);
      const metadata = JSON.parse(Buffer.from(prepared.headers.get("x-uos-fixture-source-external-metadata"), "base64url"));
      assert.equal(metadata.context, kind);
      const bytes = Buffer.from(metadata[mode === "changed" ? "request_base64" : "unchanged_request_base64"], "base64url");
      const request = JSON.parse(bytes);
      assert.equal(request.operation, "source_external_access_publish");
      assert.equal(request.v, 2);
      assert.equal(request.claim_ref, metadata.claim_ref);
      assert.equal(request.document.native_refreshed_at, null);
      assert.ok(!("refresh_token" in request.document));
      assert.notEqual((await send(site, kind, publication, bytes, true)).response.status, 200);
      const unknown = { ...request, principal: "caller-cannot-supply-authority" };
      assert.notEqual((await send(site, kind, publication, unknown)).response.status, 200);
      const receipt = await send(site, kind, publication, bytes);
      assert.equal(receipt.response.status, 200);
      assert.equal(receipt.value.claim_ref, metadata.claim_ref);
      assert.equal(receipt.value.publication_revision, request.publication_revision);
      if (mode === "changed") {
        assert.equal(receipt.value.current_source_revision, request.publication_revision);
        assert.equal(receipt.value.current_normalized_revision, request.publication_revision);
      } else {
        assert.equal(receipt.value.disposition, "already_current");
        assert.notEqual(receipt.value.current_source_revision, request.publication_revision);
        assert.notEqual(receipt.value.current_normalized_revision, request.publication_revision);
      }
      assert.equal((await send(site, kind, publication, bytes)).value.disposition, "already_current");
      const published = await send(site, kind, authority, { operation: "published", v: 1, operation_id: metadata.operation_id, claim_ref: metadata.claim_ref, publication_revision: request.publication_revision });
      assert.equal(published.response.status, 200);
      assert.equal(published.value.v, 2);
      assert.equal(published.value.publication_revision, request.publication_revision);
      if (kind === "inference") {
        const settlement = published.response.headers.get("x-uos-fixture-inference-settlement");
        assert.ok(settlement, "actual owned work is released and awaited after terminal callback");
        const observed = JSON.parse(Buffer.from(settlement, "base64url"));
        assert.ok(["settled", "error"].includes(observed.state));
        // Preserve the real ordinary request result; an observed error must remain explicit.
        if (observed.state === "error") assert.equal(typeof observed.status, "number");
      }
      observations.push({ kind, mode, publication_revision: request.publication_revision, current_source_revision: receipt.value.current_source_revision });
    }
  }
  return observations;
}
