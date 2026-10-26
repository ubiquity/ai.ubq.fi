import { test } from "bun:test";
import assert from "node:assert/strict";
import { serveHttp } from "#http";
import { earlySseHeadersCommitted, tryCommitEarlySseHeaders } from "../src/runtime/http-early-sse.ts";

const deferred = () => {
  let resolve = () => {};
  const promise = new Promise<void>((accept) => {
    resolve = accept;
  });
  return { promise, resolve };
};
const headers = () => new Headers({ "content-type": "text/event-stream", "cache-control": "no-store", "x-uos-request-id": "readiness-fixture" });

test("admitted SSE headers arrive while the original handler remains held without body bytes", async () => {
  const release = deferred();
  let selected = false;
  let completed = false;
  const server = serveHttp({ hostname: "127.0.0.1", port: 0 }, async (request, info) => {
    await request.text();
    info.completed.then(
      () => {
        completed = true;
      },
      () => {}
    );
    tryCommitEarlySseHeaders(request, { headers: headers() });
    await release.promise;
    selected = true;
    return new Response("data: selected\n\n", { headers: headers() });
  });
  const address = await server.ready;
  const fetching = fetch(`http://127.0.0.1:${address.port}/v1/responses`, { method: "POST", body: "{}" });
  try {
    const response = await Promise.race([
      fetching,
      Bun.sleep(400).then(() => {
        throw new Error("headers waited for the original handler");
      }),
    ]);
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("x-uos-request-id"), "readiness-fixture");
    assert.equal(selected, false);
    assert.equal(completed, false);
    const body = response.body;
    assert.ok(body);
    const reader = body.getReader();
    let bodyArrived = false;
    const first = reader.read().then((value) => {
      bodyArrived = true;
      return value;
    });
    await Bun.sleep(20);
    assert.equal(bodyArrived, false);
    release.resolve();
    assert.equal(new TextDecoder().decode((await first).value), "data: selected\n\n");
    assert.equal((await reader.read()).done, true);
  } finally {
    release.resolve();
    await (await fetching).body?.cancel().catch(() => {});
    await server.shutdown();
  }
}, 5000);

test("early SSE refuses unread bodies and framing conflicts, commits once and removes its physical registration", async () => {
  let observed: Request | undefined;
  const server = serveHttp({ hostname: "127.0.0.1", port: 0 }, async (request) => {
    observed = request;
    assert.equal(tryCommitEarlySseHeaders(request, { headers: headers() }), false);
    await request.text();
    for (const invalid of [
      new Headers({ "content-type": "application/json" }),
      ...["content-length", "content-encoding", "transfer-encoding"].map((name) => new Headers({ "content-type": "text/event-stream", [name]: "0" })),
    ]) {
      assert.equal(tryCommitEarlySseHeaders(request, { headers: invalid }), false);
    }
    assert.equal(tryCommitEarlySseHeaders(request, { headers: headers() }), true);
    assert.equal(tryCommitEarlySseHeaders(request, { headers: headers() }), true);
    assert.equal(earlySseHeadersCommitted(request), true);
    return new Response("data: done\n\n", { headers: headers() });
  });
  try {
    const address = await server.ready;
    const response = await fetch(`http://127.0.0.1:${address.port}`, { method: "POST", body: "{}" });
    assert.equal(await response.text(), "data: done\n\n");
  } finally {
    await server.shutdown();
  }
  assert.ok(observed);
  assert.equal(earlySseHeadersCommitted(observed), false);
  assert.equal(tryCommitEarlySseHeaders(observed, { headers: headers() }), false);
});

test("pending selection sends comments only and stops before a fragmented selected API event", async () => {
  const select = deferred();
  const finish = deferred();
  const server = serveHttp({ hostname: "127.0.0.1", port: 0 }, async (request) => {
    await request.text();
    tryCommitEarlySseHeaders(request, { headers: headers() });
    await select.promise;
    return new Response(
      new ReadableStream<Uint8Array>({
        async start(controller) {
          controller.enqueue(new TextEncoder().encode('data: {"text":"first'));
          await finish.promise;
          controller.enqueue(new TextEncoder().encode('last"}\n\n'));
          controller.close();
        },
      }),
      { headers: headers() }
    );
  });
  try {
    const address = await server.ready;
    const response = await fetch(`http://127.0.0.1:${address.port}`, { method: "POST", body: "{}" });
    const body = response.body;
    assert.ok(body);
    const reader = body.getReader();
    const comment = await reader.read();
    assert.equal(new TextDecoder().decode(comment.value), ":\n\n");
    select.resolve();
    assert.equal(new TextDecoder().decode((await reader.read()).value), 'data: {"text":"first');
    let inserted = false;
    const next = reader.read().then((value) => {
      inserted = true;
      return value;
    });
    await Bun.sleep(450);
    assert.equal(inserted, false, "a pending selected body must not receive header-phase heartbeats");
    finish.resolve();
    assert.equal(new TextDecoder().decode((await next).value), 'last"}\n\n');
    assert.equal((await reader.read()).done, true);
  } finally {
    select.resolve();
    finish.resolve();
    await server.shutdown();
  }
}, 5000);
