import assert from "node:assert/strict";

import { stillOwnsFollow } from "../static/admin-supervisor.js";

Deno.test("a replaced follow request loses ownership even though its own signal is aborted", () => {
  // Switching from session A to session B aborts A and installs B's controller
  // synchronously, so A's rejection handler runs while B already owns the slot.
  // `signal.aborted` is therefore true for A and false for B, and testing the
  // signal instead of identity lets A report against and clear B.
  const a = new AbortController();
  const b = new AbortController();
  a.abort();

  assert.equal(a.signal.aborted, true);
  assert.equal(b.signal.aborted, false);
  assert.equal(stillOwnsFollow(b, a), false, "the aborted A must not act on the slot B now owns");
  assert.equal(stillOwnsFollow(b, b), true, "the live B request still owns the slot");
});

Deno.test("a follow request that was never replaced keeps ownership through its own abort", () => {
  // Stopping the view, or a hidden tab, aborts the only controller there is and
  // clears the slot. That handler must still be allowed to settle, so ownership
  // is identity alone and never excludes an aborted request.
  const only = new AbortController();
  only.abort();

  assert.equal(stillOwnsFollow(only, only), true);
  assert.equal(stillOwnsFollow(null, only), false, "a cleared slot is owned by nobody");
});

Deno.test("follow ownership is compared by identity, never by abort state", () => {
  // Two live controllers, and two aborted ones: only the exact object that
  // installed the slot may act, which is the only signal the browser gives us
  // after the fact.
  const liveA = new AbortController();
  const liveB = new AbortController();
  liveA.abort();
  liveB.abort();

  assert.equal(stillOwnsFollow(liveA, liveB), false, "matching abort states must not grant ownership");
  assert.equal(stillOwnsFollow(liveB, liveA), false, "matching abort states must not grant ownership");
});
