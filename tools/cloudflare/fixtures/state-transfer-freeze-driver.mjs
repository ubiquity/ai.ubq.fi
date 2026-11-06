// Test-only event-controlled upstream and private binding. Never deployed.
import { DurableObject } from "cloudflare:workers";

const encoder = new TextEncoder();
const json = (value) => encoder.encode(JSON.stringify(value));
const catalog = () => ({ data: [{ id: "fixture-freeze-model", object: "model", owned_by: "fixture" }] });

export class FreezeFixtureControl extends DurableObject {
  constructor(state, env) {
    super(state, env);
    this.holds = new Map();
    this.waiters = new Set();
    this.armed = new Set();
    this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS fixture_events(seq INTEGER PRIMARY KEY AUTOINCREMENT,value TEXT NOT NULL)");
  }

  events() {
    return this.ctx.storage.sql
      .exec("SELECT value FROM fixture_events ORDER BY seq")
      .toArray()
      .map((row) => JSON.parse(row.value));
  }

  emit(event) {
    this.ctx.storage.sql.exec("INSERT INTO fixture_events(value) VALUES(?)", JSON.stringify(event));
    for (const waiter of this.waiters) {
      if (waiter.kind === event.kind && waiter.name === event.name) {
        this.waiters.delete(waiter);
        waiter.resolve(event);
      }
    }
  }

  hold(name) {
    if (this.holds.has(name)) throw new Error("Fixture hold names are single use");
    let release;
    const ready = new Promise((resolve) => (release = resolve));
    this.holds.set(name, release);
    this.emit({ kind: "held", name });
    return ready;
  }

  async fetch(request) {
    const url = new URL(request.url);
    const name = url.searchParams.get("name") ?? "";
    if (url.pathname === "/events") return Response.json(this.events());
    if (url.pathname === "/wait") {
      const kind = url.searchParams.get("kind");
      const prior = this.events().find((event) => event.kind === kind && event.name === name);
      if (prior) return Response.json(prior);
      let waiter;
      const event = new Promise((resolve) => {
        waiter = { kind, name, resolve };
        this.waiters.add(waiter);
      });
      return new Response(
        new ReadableStream({
          start: async (controller) => {
            controller.enqueue(json(await event));
            controller.close();
          },
          cancel: () => this.waiters.delete(waiter),
        }),
        { headers: { "content-type": "application/json" } }
      );
    }
    if (url.pathname === "/arm") {
      this.armed.add(name);
      return new Response(null, { status: 204 });
    }
    if (url.pathname === "/release") {
      const release = this.holds.get(name);
      if (!release) return new Response("No held operation", { status: 409 });
      this.holds.delete(name);
      this.emit({ kind: "released", name });
      release();
      return new Response(null, { status: 204 });
    }
    if (url.pathname === "/catalog") {
      this.emit({ kind: "catalog", name });
      if (this.armed.delete(name)) await this.hold(name);
      return Response.json(catalog());
    }
    if (url.pathname !== "/provider") return new Response(null, { status: 404 });
    const body = await request.json();
    const ready = this.hold(body.name);
    const completion = {
      id: "chatcmpl_freeze_fixture",
      object: "chat.completion",
      created: 1_791_244_800,
      model: body.model,
      choices: [{ index: 0, message: { role: "assistant", content: "fixture answer" }, finish_reason: "stop" }],
      usage: { prompt_tokens: 7, completion_tokens: 3, total_tokens: 10 },
    };
    if (!body.stream) {
      await ready;
      this.emit({ kind: "terminal", name: body.name });
      return Response.json(completion);
    }
    const chunk = (delta, finish = null, usage) =>
      encoder.encode(
        `data: ${JSON.stringify({
          id: completion.id,
          object: "chat.completion.chunk",
          created: completion.created,
          model: body.model,
          choices: [{ index: 0, delta, finish_reason: finish }],
          ...(usage ? { usage } : {}),
        })}\n\n`
      );
    let cancelled = false;
    const stream = new ReadableStream({
      start: async (controller) => {
        controller.enqueue(chunk({ role: "assistant", content: "fixture " }));
        await ready;
        if (cancelled) return;
        if (body.mode !== "eof") {
          controller.enqueue(chunk({ content: "answer" }));
          controller.enqueue(chunk({}, "stop", completion.usage));
          controller.enqueue(encoder.encode("data: [DONE]\n\n"));
          this.emit({ kind: "terminal", name: body.name });
        } else {
          this.emit({ kind: "eof_without_terminal", name: body.name });
        }
        controller.close();
      },
      cancel: () => {
        cancelled = true;
        this.emit({ kind: "cancelled", name: body.name });
      },
    });
    return new Response(stream, { headers: { "content-type": "text/event-stream", "x-request-id": "freeze-provider-id" } });
  }
}

const control = (env, path, init) => env.CONTROL.getByName("freeze-control-v1").fetch(`https://fixture.internal${path}`, init);

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.hostname === "api.deepseek.com" && url.pathname === "/chat/completions") {
      if (request.headers.get("authorization") !== "Bearer fixture-provider-secret") return new Response(null, { status: 401 });
      const body = await request.json();
      const prompt = JSON.stringify(body.messages ?? body.input);
      const name = /fixture:freeze:([a-z-]+)/u.exec(prompt)?.[1];
      if (!name) return new Response("Unexpected fixture prompt", { status: 400 });
      return control(env, "/provider", {
        method: "POST",
        body: JSON.stringify({ name, model: body.model, stream: body.stream === true, mode: name === "eof" ? "eof" : "terminal" }),
      });
    }
    const local = ["fixture.internal", "127.0.0.1", "localhost"].includes(url.hostname);
    if (!local && url.pathname.endsWith("/models")) return control(env, "/catalog?name=background");
    if (!local) return new Response("Unconfigured fixture destination", { status: 502 });
    if (url.pathname.startsWith("/control/")) return control(env, url.pathname.slice(8) + url.search, request);
    if (url.pathname.startsWith("/operators/")) {
      const [name, ...path] = url.pathname.slice(11).split("/");
      if (!["cancel", "next", "prior", "activate", "read"].includes(name)) return new Response(null, { status: 404 });
      const target = env["OPERATOR_" + name.toUpperCase()];
      if (!target) return new Response(null, { status: 503 });
      return target.fetch(new Request(`https://gateway.internal/${path.join("/")}${url.search}`, request));
    }
    if (url.pathname.startsWith("/writers/")) {
      if (!env.WRITER_FIXTURE) return new Response("Writer fixture is not configured", { status: 503 });
      return env.WRITER_FIXTURE.fetch(new Request(`https://writer-fixture.internal/${url.pathname.slice(9)}`, request));
    }
    if (url.pathname.startsWith("/public/")) return env.GATEWAY.fetch(new Request(`https://gateway.internal/${url.pathname.slice(8)}${url.search}`, request));
    if (url.pathname === "/rpc") {
      const object = url.searchParams.get("object") ?? "gateway-authority-v1";
      return env.AUTHORITY.get(env.AUTHORITY.idFromName(object)).fetch("https://authority.internal/v1/store", {
        method: "POST",
        headers: { "content-type": "application/json", "x-uos-authority-wire-version": "2" },
        body: request.body,
      });
    }
    return env.GATEWAY.fetch(new Request(`https://gateway.internal${url.pathname}${url.search}`, request));
  },
};
