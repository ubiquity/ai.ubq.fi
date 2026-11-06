// Test-only entrypoint decorator. Preserve real platform lifetime ownership and
// join the exact Rust retention future before the authoritative DO readback.
import fixture from "./writer-build/index.js";

export default {
  async fetch(request, env, realContext) {
    const retained = [];
    const context = {
      waitUntil(promise) {
        retained.push(Promise.resolve(promise));
        realContext.waitUntil(promise);
      },
    };
    const response = await fixture.fetch(request, env, context);
    if (retained.length !== 1) throw new Error("Expected exactly the real WriterGroup retention task");
    await Promise.all(retained);
    const headers = new Headers(response.headers);
    headers.set("x-uos-fixture-retained-promises", String(retained.length));
    return new Response(response.body, { status: response.status, headers });
  },
};
