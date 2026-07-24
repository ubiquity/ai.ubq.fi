import assert from "node:assert/strict";
import { proxyResponsesStream } from "../src/responses_stream.ts";

const loopbackPermission = await Deno.permissions.query({ name: "net", host: "127.0.0.1" });

Deno.test({
  name: "Responses proxy terminates a fragmented real HTTP stream without waiting for trailing bytes",
  ignore: loopbackPermission.state !== "granted",
  async fn() {
    const encoder = new TextEncoder();
    const delta = encoder.encode(
      'data: {"type":"response.output_text.delta","delta":"real HTTP 🌍"}\n\n',
    );
    const terminal = encoder.encode(
      'data: {"type":"response.completed","response":{"status":"completed"}}\r\n\r\n',
    );
    const chunks = [
      delta.slice(0, delta.length - 3),
      delta.slice(delta.length - 3),
      terminal.slice(0, 7),
      terminal.slice(7, terminal.length - 1),
      terminal.slice(terminal.length - 1),
    ];
    const timers = new Set<ReturnType<typeof setTimeout>>();
    let upstreamCancelled = false;
    let resolveCancelled = () => {};
    const cancelled = new Promise<void>((resolve) => {
      resolveCancelled = resolve;
    });

    const server = Deno.serve(
      { hostname: "127.0.0.1", port: 0, onListen: () => {} },
      () =>
        new Response(
          new ReadableStream<Uint8Array>({
            start(controller) {
              chunks.forEach((chunk, index) => {
                const timer = setTimeout(() => {
                  timers.delete(timer);
                  controller.enqueue(chunk);
                }, 5 * (index + 1));
                timers.add(timer);
              });
              const trailing = setTimeout(() => {
                timers.delete(trailing);
                controller.enqueue(
                  encoder.encode(
                    'data: {"type":"response.output_text.delta","delta":"post-terminal"}\n\n',
                  ),
                );
              }, 250);
              timers.add(trailing);
              const close = setTimeout(() => {
                timers.delete(close);
                controller.close();
              }, 1_000);
              timers.add(close);
            },
            cancel() {
              upstreamCancelled = true;
              for (const timer of timers) clearTimeout(timer);
              timers.clear();
              resolveCancelled();
            },
          }),
          { headers: { "Content-Type": "text/event-stream" } },
        ),
    );

    try {
      const address = server.addr as Deno.NetAddr;
      const upstream = await fetch(`http://127.0.0.1:${address.port}/responses`);
      assert.ok(upstream.body);
      const startedAt = performance.now();
      const output = await new Response(proxyResponsesStream(upstream.body)).text();
      const elapsedMs = performance.now() - startedAt;

      assert.match(output, /real HTTP 🌍/);
      assert.match(output, /response.completed/);
      assert.doesNotMatch(output, /post-terminal/);
      assert.ok(elapsedMs < 200, `terminal delivery took ${elapsedMs.toFixed(1)}ms`);
      await Promise.race([
        cancelled,
        new Promise<never>((_, reject) =>
          setTimeout(() => reject(new Error("real HTTP upstream was not cancelled")), 200)
        ),
      ]);
      assert.equal(upstreamCancelled, true);
    } finally {
      for (const timer of timers) clearTimeout(timer);
      await server.shutdown();
    }
  },
});
