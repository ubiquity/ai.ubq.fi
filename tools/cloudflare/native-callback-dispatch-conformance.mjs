// Finite synthetic fixture around the actual Rust Worker/DO callback path. No public/provider/native-process transport.
import assert from "node:assert/strict";
import { createHash, createHmac, randomBytes } from "node:crypto";
import { mkdir, mkdtemp, readFile, writeFile } from "node:fs/promises";
import { join, resolve } from "node:path";

const [sha, build, binary, schema, evidence] = process.argv.slice(2);
assert.match(sha ?? "", /^[a-f0-9]{40}$/u);
assert.equal(Bun.version, "1.4.0");
assert(build && binary && schema && evidence);
const protocol = await readFile("rust/crates/gateway-auth-native-wire/src/native_read.rs", "utf8");
const fixed = (name) => {
  const match = protocol.match(new RegExp(`${name}:\\s*&str\\s*=\\s*"([^"]+)"`));
  assert(match, `Missing defining protocol constant ${name}`);
  return match[1];
};
const authorityPath = fixed("NATIVE_RENEWAL_AUTHORITY_PATH");
const publicationPath = fixed("NATIVE_PUBLICATION_PATH");
const key = Buffer.alloc(32, 31);
const keyId = "fixture-matrix-key";
const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
const mac = (bytes) => createHmac("sha256", key).update(bytes).digest("base64url");
const q = JSON.stringify;
const cases = [];
const passportSource = `let status=200,date="10/27/2026",admin=true;export default{async fetch(r){if(new URL(r.url).pathname==="/_control"){const v=await r.json();status=v.status??status;date=v.date??date;admin=v.admin??admin;return new Response("ok")}return new Response(JSON.stringify({authenticated:true,session:{subject:"\\ud800\\u0000",clientId:"ai",audience:"ai",origin:"https://ai.ubq.fi",redirectUri:"https://ai.ubq.fi/auth/callback",createdAt:date},user:{id:"\\ud800\\u0000",handle:"",isAdmin:admin,scopes:[]}}),{status,headers:{"content-type":"application/json"}})}}`;

async function reserve() {
  const server = Bun.serve({ hostname: "127.0.0.1", port: 0, fetch: () => new Response(null, { status: 503 }) });
  const port = server.port;
  server.stop(true);
  return port;
}
await mkdir(resolve(evidence), { recursive: true });
for (const scenario of ["native-callback-legacy", "native-callback-source", "native-callback-published", "native-callback-unknown"]) {
  const workspace = await mkdtemp(join(resolve(evidence), `${scenario}-`));
  await mkdir(join(workspace, "state"));
  const port = await reserve();
  const controlPort = await reserve();
  const config = `using Workerd=import ${q(resolve(schema))};const config:Workerd.Config=(services=[(name="denied",network=(allow=[])),(name="assets",disk=(path=${q(resolve("static"))})),(name="passport",worker=(modules=[(name="passport.mjs",esModule=${q(passportSource)})],compatibilityDate="2026-10-18",globalOutbound="denied")),(name="gateway",worker=(modules=[(name="index.js",esModule=embed ${q(join(resolve(build), "index.js"))}),(name="index_bg.wasm",wasm=embed ${q(join(resolve(build), "index_bg.wasm"))})],compatibilityDate="2026-10-18",compatibilityFlags=["enable_request_signal","request_signal_passthrough"],globalOutbound="passport",bindings=[(name="AUTHORITY",durableObjectNamespace=(className="GatewayAuthority")),(name="ASSETS",service="assets"),(name="PASSPORT_FIXTURE",service="passport"),(name="UOS_MATRIX_SCENARIO",text=${q(scenario)}),(name="UOS_AI_TOKEN",text="fixture-access"),(name="DENO_DEPLOY_TOKEN",text="fixture-admin"),(name="DENO_TIMELINE",text="production"),(name="DENO_DEPLOY",text="fixture-workerd"),(name="UOS_AI_PASSPORT_CLIENT_ID",text="ai"),(name="UOS_AI_PASSPORT_AUDIENCE",text="ai"),(name="UOS_AI_PASSPORT_ORIGIN",text="https://ai.ubq.fi"),(name="UOS_AI_PASSPORT_REDIRECT_URI",text="https://ai.ubq.fi/auth/callback")],durableObjectNamespaces=[(className="GatewayAuthority",uniqueKey=${q(`callback-${scenario}`)},enableSql=true)],durableObjectStorage=(localDisk="state")))],sockets=[(name="main",http=(),service="gateway"),(name="control",http=(),service="passport")]);`;
  const configPath = join(workspace, "runtime.capnp");
  await writeFile(configPath, config);
  const child = Bun.spawn(
    [
      resolve(binary),
      "serve",
      configPath,
      "--experimental",
      "--import-path",
      "/",
      `--socket-addr=main=127.0.0.1:${port}`,
      `--socket-addr=control=127.0.0.1:${controlPort}`,
    ],
    {
      stdin: "ignore",
      stdout: "pipe",
      stderr: "pipe",
      env: { PATH: "/usr/bin:/bin", TMPDIR: workspace },
    }
  );
  const stdout = new Response(child.stdout).text();
  const stderr = new Response(child.stderr).text();
  const origin = `http://127.0.0.1:${port}`;
  async function send(path, data, options = {}) {
    const body = typeof data === "string" ? data : JSON.stringify(data);
    const nonce = options.nonce ?? randomBytes(32).toString("base64url");
    const now = Date.now();
    const signature = mac(`uos-native-http-v1\0POST\0${path}\0${keyId}\0${now}\0${nonce}\0${hash(body)}`);
    const response = await fetch(origin + path, {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "x-uos-native-version": "1",
        "x-uos-native-key-id": keyId,
        "x-uos-native-time-ms": String(now),
        "x-uos-native-nonce": nonce,
        "x-uos-native-mac": options.badMac ? "A".repeat(43) : signature,
      },
      body,
    });
    const text = await response.text();
    const responseMac = response.headers.get("x-uos-native-response-mac");
    if (options.signed !== false) {
      const domain = path === publicationPath ? "uos-native-publication-response-v1" : "uos-native-renewal-authority-response-v1";
      const expected = mac(`${domain}\0POST\0${path}\0${keyId}\0${nonce}\0${hash(body)}\0${response.status}\0${hash(text)}`);
      assert.equal(responseMac, expected, `${scenario}: raw status/body MAC`);
      assert.notEqual(responseMac, mac(`${domain}\0POST\0${path}\0${keyId}\0${nonce}\0${hash(body)}\0${response.status}\0${hash(text + " ")}`));
    } else assert.equal(responseMac, null);
    if (options.error) assert.notEqual(response.status, 200, text);
    else assert.equal(response.status, 200, text);
    return { response, text, value: JSON.parse(text), nonce };
  }
  try {
    let health;
    for (let attempt = 0; attempt < 100; attempt++) {
      if (child.exitCode !== null) throw new Error("workerd exited before readiness");
      try {
        health = await fetch(origin + "/health");
        break;
      } catch {
        await Bun.sleep(25);
      }
    }
    assert(health);
    assert.equal(health.status, 200, await health.clone().text());
    assert((await health.text()).includes(sha));
    const metadata = JSON.parse(Buffer.from(health.headers.get("x-uos-fixture-callback-metadata") ?? "", "base64url").toString());
    const request = (operation, extra = {}) => ({ operation, v: 1, operation_id: metadata.operation_id, claim_ref: metadata.claim_ref, ...extra });
    if (scenario === "native-callback-published") {
      const published = await send(authorityPath, request("published", { publication_revision: metadata.publication_revision }));
      assert.equal(published.value.v, 2);
      assert.equal(published.value.operation, "published");
      await send(authorityPath, request("unknown"));
      cases.push("source2-genuine-managed-terminal-published-and-owned-cleanup");
      continue;
    }
    if (scenario === "native-callback-unknown") {
      const unknown = await send(authorityPath, request("unknown"));
      assert.deepEqual(unknown.value, { operation: "unknown", v: 2 });
      cases.push("source2-owned-unknown-after-real-session-and-pool-deletion");
      continue;
    }
    const claimed = await send(authorityPath, {
      operation: "claim",
      v: 1,
      original_control_body_base64url: metadata.original_control_body_base64url,
      original_auth_headers: metadata.original_auth_headers,
    });
    assert.equal(claimed.value.v, metadata.v);
    assert.equal(claimed.value.operation, "claim");
    assert(claimed.value.start_permit);
    const base = { operation_id: claimed.value.operation_id, claim_ref: claimed.value.claim_ref };
    const started = await send(authorityPath, request("start", { ...base, start_permit: claimed.value.start_permit }));
    assert.equal(started.value.v, metadata.v);
    assert.equal(started.value.operation, "start");
    const rechecked = await send(authorityPath, request("recheck", base));
    assert.equal(rechecked.value.v, metadata.v);
    assert.equal(rechecked.value.operation, "recheck");
    await send(authorityPath, request("start", { ...base, start_permit: "wrong-permit" }), { error: true });
    await send(authorityPath, request("recheck", { ...base, extra: true }), { error: true, signed: false });
    await send(authorityPath, request("recheck", base), { error: true, signed: false, badMac: true });
    if (metadata.v === 1) {
      const fence = claimed.value.fence;
      await send(publicationPath, {
        operation: "managed_access_publish",
        v: 2,
        action_ref: metadata.action_ref,
        operation_id: metadata.operation_id,
        native_owner_id: fence.native_owner_id,
        owner_revision: fence.owner_revision,
        source_revision: fence.source_revision,
        normalized_revision: fence.normalized_revision,
        publication_revision: metadata.publication_revision,
        account_id_hash: fence.account_id_hash,
        previous_native_source_generation: fence.source_native_generation,
        native_source_generation: "b".repeat(64),
        access_generation: metadata.publication_access_generation,
        document: { account_id: metadata.account_id, access_token: metadata.publication_access, native_refreshed_at: null },
      });
      const published = await send(authorityPath, request("published", { ...base, publication_revision: metadata.publication_revision }));
      assert.equal(published.value.operation, "published");
      assert.equal(published.value.v, 1);
    } else {
      assert.deepEqual([...rechecked.value.fence.principal].map((c) => c.charCodeAt(0)).slice(-2), [0xd800, 0]);
      await fetch(`http://127.0.0.1:${controlPort}/_control`, { method: "POST", body: JSON.stringify({ date: "not a date" }) });
      await send(authorityPath, request("recheck", base), { error: true });
    }
    const unknown = await send(authorityPath, request("unknown", base));
    assert.equal(unknown.value.operation, "unknown");
    assert.equal(unknown.value.v, metadata.v);
    cases.push(`${scenario}-paired-claim-start-recheck-errors-unknown-raw-mac`);
  } finally {
    child.kill("SIGTERM");
    await child.exited;
    await writeFile(join(workspace, "stdout.log"), await stdout);
    await writeFile(join(workspace, "stderr.log"), await stderr);
  }
}
await writeFile(
  join(resolve(evidence), "callback-conformance.json"),
  JSON.stringify(
    {
      source_sha: sha,
      cases,
      fixture_composition: true,
      shipping_entrypoint_acceptance: false,
      network: "fixed synthetic Passport service only; denied public/provider outbound",
      all_source_external_published: "E3 held",
    },
    null,
    2
  )
);
