import { test } from "bun:test";
import assert from "node:assert/strict";
import { request as httpRequest, type IncomingMessage } from "node:http";
import { serveHttp } from "#http";

const deferred = <T>() => {
  let resolve: (value: T) => void = () => {};
  const promise = new Promise<T>((accept) => {
    resolve = accept;
  });
  return { promise, resolve };
};

test("HTTP sends SSE headers before the client releases the first body chunk", async () => {
  const release = deferred<undefined>();
  const pulling = deferred<undefined>();
  const receipt = deferred<Promise<void>>();
  let produced = false;
  let delivered = false;
  const server = serveHttp({ hostname: "127.0.0.1", port: 0 }, (_request, info) => {
    receipt.resolve(info.completed);
    void info.completed.then(
      () => {
        delivered = true;
      },
      () => {}
    );
    return new Response(
      new ReadableStream<Uint8Array>({
        async pull(controller) {
          pulling.resolve(undefined);
          await release.promise;
          produced = true;
          controller.enqueue(new TextEncoder().encode("data: released\n\n"));
          controller.close();
        },
      }),
      { status: 202, statusText: "Accepted", headers: { "content-type": "text/event-stream; charset=utf-8", "x-fixture": "delayed-body" } }
    );
  });
  const address = await server.ready;
  let client: ReturnType<typeof httpRequest> | undefined;
  // This only bounds a deadlock: the producer is released by observed headers,
  // never by a timer or a race against elapsed time.
  const headers = new Promise<IncomingMessage>((resolve, reject) => {
    client = httpRequest({ hostname: "127.0.0.1", port: address.port, path: "/delayed", agent: false }, resolve);
    client.once("error", reject);
    client.end();
  });
  const watchdog = setTimeout(() => client?.destroy(new Error("SSE headers were withheld until the first body chunk")), 1000);
  try {
    const response = await headers;
    clearTimeout(watchdog);
    await pulling.promise;
    assert.equal(response.statusCode, 202);
    assert.equal(response.statusMessage, "Accepted");
    assert.equal(response.headers["content-type"], "text/event-stream; charset=utf-8");
    assert.equal(response.headers["x-fixture"], "delayed-body");
    assert.equal(response.readableLength, 0);
    assert.equal(produced, false);
    assert.equal(delivered, false);
    const body = new Promise<string>((resolve, reject) => {
      let text = "";
      response.setEncoding("utf8");
      response.on("data", (chunk: string) => {
        text += chunk;
      });
      response.once("end", () => {
        resolve(text);
      });
      response.once("error", reject);
    });
    release.resolve(undefined);
    assert.equal(await body, "data: released\n\n");
    await receipt.promise;
    assert.equal(delivered, true);
  } finally {
    clearTimeout(watchdog);
    client?.destroy();
    release.resolve(undefined);
    await server.shutdown();
  }
});

test("HTTP preserves a non-SSE failure status before any body is sent", async () => {
  const server = serveHttp(
    { hostname: "127.0.0.1", port: 0 },
    () =>
      new Response(
        new ReadableStream<Uint8Array>({
          pull(controller) {
            controller.error(new Error("fixture body failed"));
          },
        }),
        { status: 202, headers: { "content-type": "application/json" } }
      )
  );
  try {
    const address = await server.ready;
    const response = await fetch(`http://127.0.0.1:${address.port}`);
    assert.equal(response.status, 500);
    assert.equal(await response.text(), "Internal Server Error");
  } finally {
    await server.shutdown();
  }
});
