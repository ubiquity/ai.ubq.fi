import assert from "node:assert/strict";

import { hasStaticAsset } from "../src/static.ts";

Deno.test("static assets register frontend module dependencies", () => {
  for (
    const path of [
      "/admin.js",
      "/auth.js",
      "/auth-relay.js",
      "/foreground-refresh.js",
      "/network.js",
      "/reasoning-select.js",
    ]
  ) {
    assert.equal(hasStaticAsset(path), true, `${path} should be registered`);
  }
});
