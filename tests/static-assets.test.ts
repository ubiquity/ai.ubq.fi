import assert from "node:assert/strict";

import { hasStaticAsset } from "../src/static.ts";

Deno.test("static assets register admin module dependencies", () => {
  for (const path of ["/admin.js", "/auth.js", "/network.js"]) {
    assert.equal(hasStaticAsset(path), true, `${path} should be registered`);
  }
});
