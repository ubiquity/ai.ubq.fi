import assert from "node:assert/strict";

import { authenticateAdmin, authenticateClient, handleV1Auth, requireSuperAdminAuth } from "../src/auth.ts";
import {
  configureAdminAuthForListener,
  configureAdminAuthPeerForRequest,
  isAdminAuthDisabledForRequest,
  isLoopbackHostname,
  parseServeRuntimeOptions,
  type ServeRuntimeOptions,
  shouldDisableAdminAuthForListener,
} from "../src/local_admin_auth.ts";

const tcpAddress = (hostname: string): Deno.NetAddr => ({
  transport: "tcp",
  hostname,
  port: 8000,
});

const enabledOptions: ServeRuntimeOptions = Object.freeze({ disableAdminAuth: true });
const disabledOptions: ServeRuntimeOptions = Object.freeze({ disableAdminAuth: false });

Deno.test("serve runtime options keep admin auth enabled by default", () => {
  assert.deepEqual(parseServeRuntimeOptions([], { isDeploy: false }), { disableAdminAuth: false });
  assert.equal(Object.isFrozen(parseServeRuntimeOptions([], { isDeploy: false })), true);
});

Deno.test("serve runtime options accept only one exact disable-admin-auth flag", () => {
  assert.deepEqual(parseServeRuntimeOptions(["--disable-admin-auth"], { isDeploy: false }), {
    disableAdminAuth: true,
  });
  assert.throws(
    () => parseServeRuntimeOptions(["--disable-admin-auth", "--disable-admin-auth"], { isDeploy: false }),
    /may only be specified once/,
  );
  assert.throws(
    () => parseServeRuntimeOptions(["--disable-admin-auth=true"], { isDeploy: false }),
    /Unknown server argument/,
  );
  assert.throws(
    () => parseServeRuntimeOptions(["--disable-auth"], { isDeploy: false }),
    /Unknown server argument/,
  );
});

Deno.test("disable-admin-auth is rejected in Deno Deploy", () => {
  assert.throws(
    () => parseServeRuntimeOptions(["--disable-admin-auth"], { isDeploy: true }),
    /unavailable in Deno Deploy/,
  );
});

Deno.test("loopback detection accepts local TCP hostnames only", () => {
  for (const hostname of ["localhost", "LOCALHOST", "127.0.0.1", "127.42.9.3", "::1", "[::1]"]) {
    assert.equal(isLoopbackHostname(hostname), true, hostname);
  }
  for (const hostname of ["0.0.0.0", "::", "192.168.1.10", "10.0.0.1", "ai.ubq.fi", "127.0.0.1.example"]) {
    assert.equal(isLoopbackHostname(hostname), false, hostname);
  }
});

Deno.test("admin auth can only be disabled on a loopback TCP listener", () => {
  assert.equal(shouldDisableAdminAuthForListener(disabledOptions, tcpAddress("0.0.0.0")), false);
  assert.equal(shouldDisableAdminAuthForListener(enabledOptions, tcpAddress("127.0.0.1")), true);
  assert.equal(shouldDisableAdminAuthForListener(enabledOptions, tcpAddress("::1")), true);
  assert.throws(
    () => shouldDisableAdminAuthForListener(enabledOptions, tcpAddress("localhost")),
    /requires a loopback TCP listener/,
  );
  assert.throws(
    () => shouldDisableAdminAuthForListener(enabledOptions, tcpAddress("0.0.0.0")),
    /requires a loopback TCP listener/,
  );
  assert.throws(
    () =>
      shouldDisableAdminAuthForListener(enabledOptions, {
        transport: "unix",
        path: "/tmp/ai-ubq-fi.sock",
      }),
    /requires a loopback TCP listener/,
  );
});

Deno.test("guarded runtime bypass grants local super-admin access only to loopback peers", async () => {
  const localRequest = new Request("http://127.0.0.1/admin/api-keys");
  const remoteRequest = new Request("https://ai.ubq.fi/admin/api-keys");

  configureAdminAuthForListener(enabledOptions, tcpAddress("127.0.0.1"));
  // Fail closed until a loopback peer is observed.
  assert.equal(isAdminAuthDisabledForRequest(localRequest), false);
  const localAuthNoPeer = await authenticateAdmin(localRequest);
  assert.equal(localAuthNoPeer.ok, false);

  // A forwarded or port-forwarded request has a non-loopback peer and must
  // never receive the bypass, regardless of its (client-controlled) URL host.
  configureAdminAuthPeerForRequest(tcpAddress("192.168.1.10"));
  assert.equal(isAdminAuthDisabledForRequest(localRequest), false);
  const localAuthForwarded = await authenticateAdmin(localRequest);
  assert.equal(localAuthForwarded.ok, false);

  // A unix-socket peer never qualifies.
  configureAdminAuthPeerForRequest({ transport: "unix", path: "/tmp/ai-ubq-fi.sock" });
  assert.equal(isAdminAuthDisabledForRequest(localRequest), false);

  configureAdminAuthPeerForRequest(tcpAddress("127.0.0.1"));
  try {
    assert.equal(isAdminAuthDisabledForRequest(localRequest), true);
    assert.equal(isAdminAuthDisabledForRequest(remoteRequest), false);

    const localAuth = await authenticateAdmin(localRequest);
    assert.equal(localAuth.ok, true);
    if (localAuth.ok) {
      assert.equal(localAuth.method.kind, "disabled");
      assert.equal(localAuth.is_super_admin, true);
    }
    assert.equal(await requireSuperAdminAuth(localRequest), null);

    const remoteAuth = await authenticateAdmin(remoteRequest);
    assert.equal(remoteAuth.ok, false);
    if (!remoteAuth.ok) assert.equal(remoteAuth.response.status, 401);

    const whoami = await handleV1Auth(new Request("http://127.0.0.1/uos/auth"));
    assert.equal(whoami.status, 200);
    const body = await whoami.json();
    assert.equal(body.auth.is_admin, true);
    assert.equal(body.auth.is_super_admin, true);
    assert.equal(body.auth.method.kind, "disabled");
  } finally {
    configureAdminAuthForListener(disabledOptions, tcpAddress("127.0.0.1"));
  }

  assert.equal(isAdminAuthDisabledForRequest(localRequest), false);
  const defaultAuth = await authenticateAdmin(localRequest);
  assert.equal(defaultAuth.ok, false);
  if (!defaultAuth.ok) assert.equal(defaultAuth.response.status, 401);

  const defaultWhoami = await handleV1Auth(new Request("http://127.0.0.1/uos/auth"));
  assert.equal(defaultWhoami.status, 200);
  const defaultWhoamiBody = await defaultWhoami.json();
  assert.equal(defaultWhoamiBody.auth.is_admin, false);
  assert.equal(defaultWhoamiBody.auth.is_super_admin, false);

  const otherLoopbackClient = await authenticateClient(new Request("http://127.42.9.3/v1/models"));
  assert.equal(otherLoopbackClient.ok, false);
  if (!otherLoopbackClient.ok) assert.equal(otherLoopbackClient.response.status, 401);
});
Deno.test("production VPS timeline denies unauthenticated loopback clients", async () => {
  const req = new Request("http://127.0.0.1:8001/v1/responses");
  const origTimeline = Deno.env.get("DENO_TIMELINE");
  try {
    Deno.env.set("DENO_TIMELINE", "production");
    const authProd = await authenticateClient(req);
    assert.equal(authProd.ok, false);
    if (!authProd.ok) assert.equal(authProd.response.status, 401);

    Deno.env.delete("DENO_TIMELINE");
    const authLocal = await authenticateClient(req);
    assert.equal(authLocal.ok, true);
    if (authLocal.ok) assert.equal(authLocal.method.kind, "disabled");
  } finally {
    if (origTimeline !== undefined) {
      Deno.env.set("DENO_TIMELINE", origTimeline);
    } else {
      Deno.env.delete("DENO_TIMELINE");
    }
  }
});
