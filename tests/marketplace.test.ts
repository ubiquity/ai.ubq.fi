import assert from "node:assert/strict";
import handler from "../src/handler.ts";
import { setKvForTest } from "../src/kv.ts";
import {
  authAccountByOwnerKey,
  authAccountKey,
  handleMarketplaceCreateAuth,
  handleMarketplaceDisableAuth,
  handleMarketplaceListAuth,
  handleMarketplacePublicCatalog,
  handleMarketplaceUpdateAuth,
} from "../src/marketplace.ts";
import { CountingKv } from "./helpers/counting_kv.ts";

const ownerAuth = (userId: string) => (_req: Request) =>
  Promise.resolve({
    ok: true as const,
    token: `session-${userId}`,
    method: {
      kind: "passkey_session" as const,
      user_id: userId,
      handle: userId,
      is_admin: false,
      credential_count: 1,
    },
  });

const githubRepoAuth = (_req: Request) =>
  Promise.resolve({
    ok: true as const,
    token: "github-token",
    method: {
      kind: "github_token" as const,
      owner: "ubiquity",
      repo: "ai.ubq.fi",
      state_id: "state-1",
      limit_scope: "repo" as const,
    },
  });

const jsonRequest = (method: string, path: string, body: unknown): Request =>
  new Request(`https://ai.ubq.fi${path}`, {
    method,
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });

const createAccount = async (kv: CountingKv, userId = "owner-1"): Promise<string> => {
  const response = await handleMarketplaceCreateAuth(
    jsonRequest("POST", "/marketplace/auths", {
      provider: "chatgpt_codex",
      encryptedAuthJson: "ciphertext",
      pricing: { per_request: 1 },
      maxConcurrent: 2,
      labels: { region: "us" },
    }),
    {
      authenticateClient: ownerAuth(userId),
      kv: kv as unknown as Deno.Kv,
      now: () => 1_000,
      uuid: () => "account-1",
    },
  );
  assert.equal(response.status, 201);
  const body = await response.json() as { id: string; ok?: boolean };
  assert.equal(Object.hasOwn(body, "ok"), false);
  return body.id;
};

Deno.test("marketplace create atomically writes the account and owner index", async () => {
  const kv = new CountingKv();
  const id = await createAccount(kv);

  assert.equal(id, "auth_account-1");
  const atomic = kv.commands.filter((command) => command.command === "atomic.commit");
  assert.equal(atomic.length, 1);
  assert.equal(atomic[0].atomicMutations, 2);
  assert.equal(kv.commands.some((command) => command.command === "set"), false);
  const stored = await kv.get<Record<string, unknown>>(authAccountKey(id));
  assert.equal(stored.value?.ownerUserId, "passkey-user:owner-1");
  assert.equal(stored.value?.encryptedAuthJson, "ciphertext");
});

Deno.test("marketplace rejects malformed pricing and labels before persistence", async () => {
  const kv = new CountingKv();
  const invalidCreate = await handleMarketplaceCreateAuth(
    jsonRequest("POST", "/marketplace/auths", {
      provider: "chatgpt_codex",
      encryptedAuthJson: "ciphertext",
      pricing: "free",
    }),
    { authenticateClient: ownerAuth("owner-1"), kv: kv as unknown as Deno.Kv },
  );
  assert.equal(invalidCreate.status, 400);
  assert.equal(kv.commands.length, 0);

  const id = await createAccount(kv);
  kv.clearMeasurements();
  const invalidUpdate = await handleMarketplaceUpdateAuth(
    jsonRequest("PATCH", `/marketplace/auths/${id}`, { labels: ["region"] }),
    id,
    { authenticateClient: ownerAuth("owner-1"), kv: kv as unknown as Deno.Kv },
  );
  assert.equal(invalidUpdate.status, 400);
  assert.equal(kv.commands.some((command) => command.command === "atomic.commit"), false);
});

Deno.test("marketplace owner routes reject repository-scoped GitHub authentication", async () => {
  const kv = new CountingKv();
  const create = await handleMarketplaceCreateAuth(
    jsonRequest("POST", "/marketplace/auths", {
      provider: "chatgpt_codex",
      encryptedAuthJson: "ciphertext",
    }),
    { authenticateClient: githubRepoAuth, kv: kv as unknown as Deno.Kv },
  );
  assert.equal(create.status, 403);

  const list = await handleMarketplaceListAuth(
    new Request("https://ai.ubq.fi/marketplace/auths/me"),
    { authenticateClient: githubRepoAuth, kv: kv as unknown as Deno.Kv },
  );
  assert.equal(list.status, 403);
  assert.equal(kv.commands.length, 0);
});

Deno.test("marketplace owner listing is paginated and never cacheable", async () => {
  const kv = new CountingKv();
  for (let index = 0; index < 3; index += 1) {
    const id = `auth_${index}`;
    const account = {
      id,
      ownerUserId: "passkey-user:owner-1",
      provider: "chatgpt_codex",
      encryptedAuthJson: `ciphertext-${index}`,
      status: "enabled",
      pricing: null,
      maxConcurrent: null,
      health: null,
      enabled: true,
      labels: null,
      createdAt: index,
      updatedAt: index,
    };
    kv.seed(authAccountKey(id), account);
    kv.seed(authAccountByOwnerKey(account.ownerUserId, id), null);
  }

  const response = await handleMarketplaceListAuth(
    new Request("https://ai.ubq.fi/marketplace/auths/me?limit=2&cursor=opaque-cursor"),
    { authenticateClient: ownerAuth("owner-1"), kv: kv as unknown as Deno.Kv },
  );
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("cache-control"), "no-store");
  const body = await response.json() as { auths: Array<{ encryptedAuthJson?: string }>; next_cursor: string | null };
  assert.equal(body.auths.length, 2);
  assert.equal(body.auths.every((account) => account.encryptedAuthJson?.startsWith("ciphertext-")), true);
  assert.equal(body.next_cursor, null);
});

Deno.test("marketplace updates require ownership and reject protected fields", async () => {
  const kv = new CountingKv();
  const id = await createAccount(kv);

  const forbidden = await handleMarketplaceUpdateAuth(
    jsonRequest("PATCH", `/marketplace/auths/${id}`, { pricing: { per_request: 2 } }),
    id,
    { authenticateClient: ownerAuth("attacker"), kv: kv as unknown as Deno.Kv },
  );
  assert.equal(forbidden.status, 403);

  const protectedField = await handleMarketplaceUpdateAuth(
    jsonRequest("PATCH", `/marketplace/auths/${id}`, { ownerUserId: "passkey-user:attacker" }),
    id,
    { authenticateClient: ownerAuth("owner-1"), kv: kv as unknown as Deno.Kv },
  );
  assert.equal(protectedField.status, 400);

  const invalidStatus = await handleMarketplaceUpdateAuth(
    jsonRequest("PATCH", `/marketplace/auths/${id}`, { status: "healthy" }),
    id,
    { authenticateClient: ownerAuth("owner-1"), kv: kv as unknown as Deno.Kv },
  );
  assert.equal(invalidStatus.status, 400);

  const inconsistentState = await handleMarketplaceUpdateAuth(
    jsonRequest("PATCH", `/marketplace/auths/${id}`, { status: "disabled", enabled: true }),
    id,
    { authenticateClient: ownerAuth("owner-1"), kv: kv as unknown as Deno.Kv },
  );
  assert.equal(inconsistentState.status, 400);

  const disabledByStatus = await handleMarketplaceUpdateAuth(
    jsonRequest("PATCH", `/marketplace/auths/${id}`, { status: "disabled" }),
    id,
    { authenticateClient: ownerAuth("owner-1"), kv: kv as unknown as Deno.Kv },
  );
  assert.equal(disabledByStatus.status, 200);
  const disabledAccount = await kv.get<Record<string, unknown>>(authAccountKey(id));
  assert.equal(disabledAccount.value?.status, "disabled");
  assert.equal(disabledAccount.value?.enabled, false);

  const enabledByFlag = await handleMarketplaceUpdateAuth(
    jsonRequest("PATCH", `/marketplace/auths/${id}`, { enabled: true }),
    id,
    { authenticateClient: ownerAuth("owner-1"), kv: kv as unknown as Deno.Kv },
  );
  assert.equal(enabledByFlag.status, 200);
  const enabledAccount = await kv.get<Record<string, unknown>>(authAccountKey(id));
  assert.equal(enabledAccount.value?.status, "enabled");
  assert.equal(enabledAccount.value?.enabled, true);

  const updated = await handleMarketplaceUpdateAuth(
    jsonRequest("PATCH", `/marketplace/auths/${id}`, {
      pricing: { per_request: 3 },
      maxConcurrent: 4,
      labels: { region: "eu" },
    }),
    id,
    { authenticateClient: ownerAuth("owner-1"), kv: kv as unknown as Deno.Kv, now: () => 2_000 },
  );
  assert.equal(updated.status, 200);
  const stored = await kv.get<Record<string, unknown>>(authAccountKey(id));
  assert.equal(stored.value?.ownerUserId, "passkey-user:owner-1");
  assert.equal(stored.value?.encryptedAuthJson, "ciphertext");
  assert.deepEqual(stored.value?.pricing, { per_request: 3 });
  assert.equal(stored.value?.maxConcurrent, 4);
  assert.equal(stored.value?.updatedAt, 2_000);
});

Deno.test("marketplace disable requires ownership", async () => {
  const kv = new CountingKv();
  const id = await createAccount(kv);
  const forbidden = await handleMarketplaceDisableAuth(
    new Request(`https://ai.ubq.fi/marketplace/auths/${id}/disable`),
    id,
    {
      authenticateClient: ownerAuth("attacker"),
      kv: kv as unknown as Deno.Kv,
    },
  );
  assert.equal(forbidden.status, 403);

  const disabled = await handleMarketplaceDisableAuth(
    new Request(`https://ai.ubq.fi/marketplace/auths/${id}/disable`),
    id,
    {
      authenticateClient: ownerAuth("owner-1"),
      kv: kv as unknown as Deno.Kv,
      now: () => 3_000,
    },
  );
  assert.equal(disabled.status, 200);
  const stored = await kv.get<Record<string, unknown>>(authAccountKey(id));
  assert.equal(stored.value?.enabled, false);
  assert.equal(stored.value?.status, "disabled");
});

Deno.test("marketplace public catalog is routed and omits owner credentials", async () => {
  const kv = new CountingKv();
  await createAccount(kv);

  const direct = await handleMarketplacePublicCatalog(new Request("https://ai.ubq.fi/marketplace/auths"), {
    kv: kv as unknown as Deno.Kv,
  });
  assert.equal(direct.status, 200);
  const directBody = await direct.json() as { auths: Array<Record<string, unknown>> };
  assert.equal(directBody.auths.length, 1);
  assert.equal(Object.hasOwn(directBody.auths[0], "ownerUserId"), false);
  assert.equal(Object.hasOwn(directBody.auths[0], "encryptedAuthJson"), false);

  setKvForTest(kv as unknown as Deno.Kv);
  try {
    const routed = await handler(new Request("https://ai.ubq.fi/marketplace/auths"));
    assert.equal(routed.status, 200);
    const routedBody = await routed.json() as { auths: unknown[] };
    assert.equal(routedBody.auths.length, 1);
  } finally {
    setKvForTest(null);
  }
});

Deno.test("marketplace public catalog validates and bounds pagination", async () => {
  const kv = new CountingKv();
  for (let index = 0; index < 3; index += 1) {
    kv.seed(authAccountKey(`auth_${index}`), {
      id: `auth_${index}`,
      ownerUserId: `passkey-user:${index}`,
      provider: "chatgpt_codex",
      encryptedAuthJson: "ciphertext",
      status: "enabled",
      pricing: null,
      maxConcurrent: null,
      health: null,
      enabled: true,
      labels: null,
      createdAt: index,
      updatedAt: index,
    });
  }

  const page = await handleMarketplacePublicCatalog(
    new Request("https://ai.ubq.fi/marketplace/auths?limit=2&cursor=opaque-cursor"),
    { kv: kv as unknown as Deno.Kv },
  );
  assert.equal(page.status, 200);
  const pageBody = await page.json() as { auths: unknown[]; next_cursor: string | null };
  assert.equal(pageBody.auths.length, 2);
  assert.equal(pageBody.next_cursor, null);

  for (const value of ["0", "101", "1.5", "invalid"]) {
    const invalid = await handleMarketplacePublicCatalog(
      new Request(`https://ai.ubq.fi/marketplace/auths?limit=${value}`),
      { kv: kv as unknown as Deno.Kv },
    );
    assert.equal(invalid.status, 400, value);
    assert.equal((await invalid.json() as { error?: { param?: string } }).error?.param, "limit");
  }
});

Deno.test("marketplace routes reject malformed ids without throwing", async () => {
  const response = await handler(new Request("https://ai.ubq.fi/marketplace/auths/%E0%A4%A", { method: "PATCH" }));
  assert.equal(response.status, 400);
});
