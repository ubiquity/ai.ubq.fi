import assert from "node:assert/strict";
import deploySource from "../ops/deploy.ts" with { type: "text" };

Deno.test("VPS deployment reloads systemd after unit updates and before restarting the gateway", () => {
  // `ops/ai-ubq-fi.service` is linked from `/etc/systemd/system`, so a checkout
  // update changes the linked unit, but systemd serves its cached definition
  // until `daemon-reload`. The reload must precede the restart so a changed
  // `ExecStart`, environment, or sandbox setting is active for the run the
  // health check validates.
  const compact = deploySource.replace(/\s+/gu, "");
  const reloadIndex = compact.indexOf('"systemctl","daemon-reload"');
  const restartIndex = compact.indexOf('"systemctl","restart","ai-ubq-fi.service"');
  const healthIndex = compact.indexOf('"http://127.0.0.1:7999/health"');
  assert.notStrictEqual(reloadIndex, -1, "deploy.ts must run `systemctl daemon-reload`");
  assert.notStrictEqual(restartIndex, -1, "deploy.ts must restart ai-ubq-fi.service");
  assert.notStrictEqual(healthIndex, -1, "deploy.ts must check the local health endpoint");
  assert.ok(reloadIndex < restartIndex, "`systemctl daemon-reload` must run before the ai-ubq-fi.service restart");
  assert.ok(restartIndex < healthIndex, "the restart must precede the health check");
  assert.match(compact, /systemd_daemon_reload/);
});
