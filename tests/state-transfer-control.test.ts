import assert from "node:assert/strict";
import { test } from "bun:test";
import { authorizationDigest, mintControlIntent, type Authorization } from "../tools/cloudflare/state-transfer-control.ts";
import { sha256 } from "../tools/cloudflare/state-transfer.ts";
const authorization: Authorization = {
  purpose: "drain",
  data: { binding: { identity: { object_id: "a".repeat(64), source_sha: "b".repeat(40), source_tree: "c".repeat(40) }, serving_epoch: 0 }, drain_revision: 0 },
};
test("private controller authorization matches the Rust golden and binds epoch/object/purpose", () => {
  assert.equal(authorizationDigest(authorization), "947df3937807c80ca75bed9d3b59cf50d4043b01da6149cfb7bf81c71501945c");
  const minted = mintControlIntent(authorization);
  assert.equal(minted.sha256, sha256(minted.token));
  assert.match(minted.token, /^uos-state-transfer-v2\.drain\.[a-f0-9]{64}\.[a-f0-9]{64}$/);
  assert.notEqual(
    authorizationDigest({ ...authorization, data: { ...authorization.data, binding: { ...authorization.data.binding, serving_epoch: 1 } } }),
    authorizationDigest(authorization)
  );
  assert.notEqual(
    authorizationDigest({
      ...authorization,
      data: {
        ...authorization.data,
        binding: { ...authorization.data.binding, identity: { ...authorization.data.binding.identity, object_id: "d".repeat(64) } },
      },
    }),
    authorizationDigest(authorization)
  );
});
