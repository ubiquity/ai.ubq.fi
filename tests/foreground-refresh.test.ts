import assert from "node:assert/strict";

import { bindForegroundRefresh } from "../static/foreground-refresh.js";

class TestDocument extends EventTarget {
  visibilityState: "hidden" | "visible" = "visible";
}

const flushTimer = () => new Promise((resolve) => setTimeout(resolve, 5));

Deno.test("foreground refresh coalesces focus events and ignores hidden pages", async () => {
  const windowTarget = new EventTarget();
  const documentTarget = new TestDocument();
  let refreshes = 0;
  const unbind = bindForegroundRefresh(
    () => {
      refreshes += 1;
    },
    { windowTarget, documentTarget, delayMs: 0 },
  );

  windowTarget.dispatchEvent(new Event("focus"));
  documentTarget.dispatchEvent(new Event("visibilitychange"));
  await flushTimer();
  assert.equal(refreshes, 1);

  documentTarget.visibilityState = "hidden";
  windowTarget.dispatchEvent(new Event("focus"));
  documentTarget.dispatchEvent(new Event("visibilitychange"));
  await flushTimer();
  assert.equal(refreshes, 1);

  documentTarget.visibilityState = "visible";
  documentTarget.dispatchEvent(new Event("visibilitychange"));
  await flushTimer();
  assert.equal(refreshes, 2);

  unbind();
  windowTarget.dispatchEvent(new Event("focus"));
  await flushTimer();
  assert.equal(refreshes, 2);
});
