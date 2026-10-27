import { createServer, request as httpRequest, type IncomingMessage, type ServerResponse, type IncomingHttpHeaders } from "node:http";
import { request as httpsRequest } from "node:https";

export const LIMITS = { requests: 2, requestBytes: 65_536, outputTokens: 512, deadlineMs: 60_000, retries: 0 } as const;
export const MODEL = "ubiquity/deepseek-flash";
export const EFFORT = "low";
export const record = (value: unknown): Record<string, unknown> =>
  value !== null && typeof value === "object" && !Array.isArray(value) ? (value as Record<string, unknown>) : {};

export type RequestSample = {
  started: number;
  headers?: number;
  closed?: number;
  cancelled?: boolean;
  status?: number;
  requestId?: string;
  gitSha?: string;
  deploymentId?: string;
  bytes: number;
  markerCount: number;
  ordered: boolean;
  identity: Record<string, unknown>;
};

const forwardingHeaders = (incoming: IncomingHttpHeaders, token: string): Record<string, string> => {
  const headers: Record<string, string> = { authorization: `Bearer ${token}`, "content-type": "application/json" };
  for (const [name, value] of Object.entries(incoming)) {
    if ((name.startsWith("x-codex") || name === "originator" || name === "user-agent") && typeof value === "string") headers[name] = value;
  }
  return headers;
};

export const startProbeProxy = async (options: {
  endpoint: URL;
  token: string;
  marker: string;
  initial: string;
  signal: AbortSignal;
  expectedRelease?: { gitSha: string; deploymentId: string };
}) => {
  const samples: RequestSample[] = [];
  const active = new Set<ReturnType<typeof httpRequest>>();
  let refused = false;
  let failure: string | null = null;
  const serve = async (incoming: IncomingMessage, outgoing: ServerResponse): Promise<void> => {
    try {
      const chunks: Buffer[] = [];
      let size = 0;
      for await (const value of incoming) {
        const chunk = Buffer.isBuffer(value) ? value : Buffer.from(String(value));
        size += chunk.byteLength;
        if (size > LIMITS.requestBytes) throw new Error("Native request exceeds the 64 KiB budget");
        chunks.push(chunk);
      }
      if (refused || samples.length >= LIMITS.requests) throw new Error("Probe forwarding stopped or request budget exhausted");
      if (incoming.url !== "/v1/responses") throw new Error("Unexpected native endpoint");
      const body = record(JSON.parse(Buffer.concat(chunks).toString()) as unknown);
      if (body.model !== MODEL || record(body.reasoning).effort !== EFFORT) throw new Error("Native model or effort changed");
      body.max_output_tokens = LIMITS.outputTokens;
      const encoded = JSON.stringify(body);
      if (Buffer.byteLength(encoded) > LIMITS.requestBytes) throw new Error("Forwarded request exceeds the 64 KiB budget");
      const markerAt = encoded.indexOf(options.marker);
      const initialAt = encoded.indexOf(options.initial);
      const sample: RequestSample = {
        started: performance.now(),
        bytes: Buffer.byteLength(encoded),
        markerCount: encoded.split(options.marker).length - 1,
        ordered: initialAt >= 0 && markerAt > initialAt,
        identity: record(JSON.parse(String(incoming.headers["x-codex-turn-metadata"] ?? "null")) as unknown),
      };
      samples.push(sample);
      const headers = forwardingHeaders(incoming.headers, options.token);
      const transport = options.endpoint.protocol === "https:" ? httpsRequest : httpRequest;
      const upstream = transport(new URL("/v1/responses", options.endpoint), { method: "POST", headers, signal: options.signal }, (response) => {
        sample.headers = performance.now();
        sample.status = response.statusCode;
        sample.requestId = String(response.headers["x-uos-request-id"] ?? "");
        sample.gitSha = String(response.headers["x-uos-git-sha"] ?? "");
        sample.deploymentId = String(response.headers["x-uos-deployment-id"] ?? "");
        if (options.expectedRelease && (sample.gitSha !== options.expectedRelease.gitSha || sample.deploymentId !== options.expectedRelease.deploymentId)) {
          failure = "Inference response release differs from health preflight";
          refused = true;
          response.destroy();
          outgoing.writeHead(502, { "content-type": "application/json" });
          outgoing.end(JSON.stringify({ error: { message: failure } }));
          return;
        }
        if (sample.status === 401 || sample.status === 403) refused = true;
        outgoing.writeHead(response.statusCode ?? 502, { "content-type": response.headers["content-type"] ?? "application/json", connection: "close" });
        outgoing.flushHeaders();
        response.on("error", () => outgoing.destroy());
        response.pipe(outgoing);
      });
      active.add(upstream);
      outgoing.once("close", () => {
        sample.closed = performance.now();
        sample.cancelled = !outgoing.writableFinished;
        upstream.destroy();
      });
      upstream.once("close", () => active.delete(upstream));
      upstream.on("error", () => outgoing.destroy());
      upstream.end(encoded);
    } catch (error) {
      failure = error instanceof Error ? error.message : "Proxy refused request";
      outgoing.writeHead(400, { "content-type": "application/json" });
      outgoing.end(JSON.stringify({ error: { message: failure } }));
    }
  };
  const server = createServer((incoming, outgoing) => {
    void serve(incoming, outgoing);
  });
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  const address = server.address();
  if (!address || typeof address === "string") throw new Error("Probe proxy address unavailable");
  return {
    url: `http://127.0.0.1:${address.port}/v1`,
    samples,
    failure: () => failure,
    stop: async () => {
      for (const request of active) request.destroy();
      server.closeAllConnections();
      await new Promise<void>((resolve) =>
        server.close(() => {
          resolve();
        })
      );
    },
  };
};
