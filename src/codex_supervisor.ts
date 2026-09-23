// Supervisor admin handlers, split out of the former monolithic module.

import { json, openaiError } from "./http.ts";

import { ensureSupervisorSnapshot } from "./codex_supervisor_inventory.ts";
import { followEntryPayload, readFollowUpdate } from "./codex_supervisor_follow.ts";
import {
  FOLLOW_HEARTBEAT_EVERY,
  FOLLOW_MAX_DURATION_MS,
  FOLLOW_POLL_MS,
  SOURCE_ID_PATTERN,
  THREAD_ID_PATTERN,
  boundedText,
  nowMs,
  resolveSupervisorConfig,
  waitForPoll,
} from "./codex_supervisor_config.ts";

const supervisorError = (status: number, message: string): Response => openaiError(status, message, "invalid_request_error");

export const handleAdminCodexSupervisorSessions = async (): Promise<Response> => {
  const snapshot = await ensureSupervisorSnapshot();
  return json(200, snapshot, { "Cache-Control": "no-store" });
};

const ENCODER = new TextEncoder();

/**
 * SSE follow for one selected session. It polls the newest persisted turn every
 * two seconds and appends newly recorded assistant messages and command output.
 * It is explicitly not a token-level stream, and reports `unavailable` instead
 * of inventing output when the source cannot be read.
 */
export const handleAdminCodexSupervisorOutput = async (req: Request): Promise<Response> => {
  const url = new URL(req.url);
  const sourceId = (url.searchParams.get("source") ?? "").trim();
  const threadId = (url.searchParams.get("id") ?? "").trim();
  const initialCursor = (url.searchParams.get("cursor") ?? "").trim().slice(0, 240);
  if (!SOURCE_ID_PATTERN.test(sourceId)) return supervisorError(400, "Invalid supervisor source id");
  if (!THREAD_ID_PATTERN.test(threadId)) return supervisorError(400, "Invalid thread id");
  const config = await resolveSupervisorConfig();
  const source = config.sources.find((entry) => entry.id === sourceId);
  if (!source) return supervisorError(400, "Unknown supervisor source");
  const snapshot = await ensureSupervisorSnapshot();
  const known = snapshot.sessions.some((session) => session.sourceId === sourceId && session.id === threadId);
  if (!known) return supervisorError(409, "Session is not in the sampled inventory for this source; refresh the panel and select a listed session");
  const sourceView = snapshot.sources.find((entry) => entry.id === sourceId);
  if (sourceView?.state === "unavailable") return supervisorError(409, `Source ${sourceId} is unavailable: ${sourceView.reason ?? "unknown reason"}`);

  let closed = false;
  let polls = 0;
  let lastUnavailable = "";
  let unavailablePolls = 0;
  let cursor = initialCursor;
  const revisions = new Map<string, string>();
  const startedAt = nowMs();
  const stream = new ReadableStream<Uint8Array>({
    start(controller) {
      const send = (event: string, data: unknown): void => {
        if (closed) return;
        try {
          controller.enqueue(ENCODER.encode(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`));
        } catch {
          closed = true;
        }
      };
      const stop = (): void => {
        if (closed) return;
        closed = true;
        try {
          controller.close();
        } catch {
          // The client may already have gone away.
        }
      };
      req.signal.addEventListener("abort", stop, { once: true });
      void (async () => {
        send("ready", {
          sourceId,
          threadId,
          machine: source.name,
          mode: "recorded",
          note: "Updates as Codex records output (about every 2s), not a token-level stream. Only assistant messages and command output are shown.",
        });
        while (!closed && !req.signal.aborted && nowMs() - startedAt < FOLLOW_MAX_DURATION_MS) {
          try {
            const update = await readFollowUpdate(source, threadId, cursor, req.signal, revisions);
            cursor = update.cursor;
            polls += 1;
            if (update.entries.length > 0) {
              send("entries", {
                entries: update.entries.map(followEntryPayload),
                cursor: update.cursor,
                turnId: update.turnId,
                turnStatus: update.turnStatus,
                enriched: update.enriched,
                sampledAtMs: nowMs(),
              });
            } else if (polls % FOLLOW_HEARTBEAT_EVERY === 0) {
              send("heartbeat", { turnId: update.turnId, turnStatus: update.turnStatus, sampledAtMs: nowMs() });
            }
          } catch (error) {
            unavailablePolls += 1;
            const message = error instanceof Error ? boundedText(error.message, 200) : "output read failed";
            if (message !== lastUnavailable || unavailablePolls % FOLLOW_HEARTBEAT_EVERY === 0) {
              lastUnavailable = message;
              send("unavailable", { message, sampledAtMs: nowMs() });
            }
          }
          await waitForPoll(FOLLOW_POLL_MS, req.signal);
        }
        send("end", { reason: req.signal.aborted ? "client closed" : "follow duration limit reached" });
        stop();
      })();
    },
    cancel() {
      closed = true;
    },
  });
  return new Response(stream, {
    headers: {
      "Content-Type": "text/event-stream; charset=utf-8",
      "Cache-Control": "no-store",
      "X-Accel-Buffering": "no",
    },
  });
};
