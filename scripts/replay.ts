/**
 * Fixed local Sentinel replay consumer.
 *
 * This script is dispatched by trusted host code and executed exactly as:
 *
 *   deno run --no-prompt --no-config --no-remote --allow-read=. scripts/replay.ts
 *
 * It reads only the trusted dispatch metadata at the checkout root
 * (`.sentinel-replay-input.json`), the two exact relative fixture paths that
 * metadata names, and existing gateway source. It never scans permanent
 * fixtures, never picks the newest entry, never reads a host credential, never
 * replaces global fetch, and never performs network, KV or write operations.
 *
 * Supported coverage is deliberately narrow: exactly one complete recorded
 * chatgpt_codex, surplus or metered attempt with a compatible Responses request
 * body and an SSE 200 response. The recorded response is replayed through the
 * real gateway response code (preflightResponsesStream ->
 * prepareResponsesStreamForCommit -> createOwnedResponsesStream or the exported
 * collectBufferedResponses); no converter is copied or substituted. The
 * prepared iterator is additionally observed through a transparent wrapper
 * that records the parser's own structured failure kind; conversion itself is
 * never replaced or reimplemented. Every conversion is gated by the gateway's
 * own empty-semantic-completion guard and its completed-response semantic
 * utility. Everything else is a fixed static unavailable result.
 *
 * Never report raw request, upstream, response or error fields. The only
 * stdout this script can produce is the trusted test markers, the fixed causal
 * failure line, or the fixed unavailable line; stderr stays empty.
 */

import { config } from "../src/config.ts";
import { fetchMeteredResponses, METERED_BASE_URL } from "../src/metered.ts";
import { collectBufferedResponses } from "../src/openai.ts";
import { MAX_ACCEPTED_JSON_BODY_BYTES } from "../src/request.ts";
import {
  createOwnedResponsesStream,
  type PreparedResponsesStream,
  prepareResponsesStreamForCommit,
  responseEventFromValue,
  responseIdFromEvents,
  responsesEventSemanticKind,
} from "../src/responses_failover_stream.ts";
import {
  preflightResponsesStream,
  readResponsesStream,
  ResponsesStreamError,
  type ResponsesStreamEvent,
  type ResponsesStreamIterator,
} from "../src/responses_stream.ts";
import {
  parseSentinelUpstreamTrace,
  SENTINEL_UPSTREAM_MAX_BYTES,
  type SentinelUpstreamProvider,
  type SentinelUpstreamTrace,
} from "../src/sentinel_upstream_capture.ts";
import { fetchSurplusResponses, SURPLUS_BASE_URL } from "../src/surplus.ts";
import { getString, isRecord } from "../src/utils.ts";
import { createRecordedUpstreamReplay, type RecordedUpstreamReplay } from "../tests/helpers/sentinel-recorded-upstream.ts";

const METADATA_FILE = ".sentinel-replay-input.json";
const METADATA_MAX_BYTES = 16 * 1024;
/**
 * Base64 of the parser's decoded byte bound plus JSON framing for up to
 * SENTINEL_UPSTREAM_MAX_CHUNKS chunk strings.
 */
const UPSTREAM_FILE_MAX_BYTES = Math.ceil(SENTINEL_UPSTREAM_MAX_BYTES / 3) * 4 + 16 * 1024;
const MAX_TEST_IDS = 64;
const TEST_ID_PATTERN = /^[A-Za-z0-9._:-]{1,64}$/;
const SYNTHETIC_PAID_API_KEY = "sentinel-replay-synthetic-key";

const TEST_MARKER_PREFIX = "sentinel-replay-test:";
const CAUSAL_FAILURE_LINE = "sentinel-causal-failure:stream terminated unexpectedly\n";
const UNAVAILABLE_LINE = "sentinel-replay-unavailable: replay input is unavailable\n";

/**
 * The fixed synthetic gateway failure texts. The owned stream and the buffered
 * converter surface the same real parser failure with different exact wording.
 */
const OWNED_UPSTREAM_ENDED_MESSAGE = "The upstream stream ended unexpectedly.";
const BUFFERED_UPSTREAM_ENDED_MESSAGE = "Upstream Responses stream ended unexpectedly.";
const UPSTREAM_ENDED_CODE = "server_error";

/**
 * Distinct fixed HTTPS route map. The recorded transport never performs
 * network I/O: it only matches the exact dispatch URL of the next recorded
 * attempt. Paid routes must equal the endpoints the exported paid fetch
 * functions request; the codex route mirrors the real gateway endpoint and the
 * unused cerebras route stays synthetic.
 */
const PROVIDER_ROUTES: Readonly<Record<SentinelUpstreamProvider, string>> = Object.freeze({
  chatgpt_codex: `${config.codexBaseUrl}/responses`,
  surplus: `${SURPLUS_BASE_URL}/v1/responses`,
  metered: `${METERED_BASE_URL}/v1/responses`,
  cerebras: "https://sentinel-replay.invalid/cerebras/v1/chat/completions",
});

type SupportedProvider = "chatgpt_codex" | "surplus" | "metered";
type SupportedTerminal = "eof" | "cancelled";
type ReplayOutcome = "completed" | "causal" | "unavailable";
/**
 * What a real converter surfaced. Matching the exact fixed failure output is
 * never a classification by itself: only observed parser evidence can turn it
 * into the causal result.
 */
type ConverterOutcome = "completed" | "premature_eof_output" | "unavailable";

type DispatchInput = Readonly<{
  requestPath: string;
  upstreamPath: string;
  testIds: readonly string[];
}>;

type RequestEnvelope = Readonly<{
  bodyText: string;
  body: Record<string, unknown>;
}>;

class UnavailableInput extends Error {
  constructor() {
    super("Sentinel replay input is unavailable");
    this.name = "UnavailableInput";
  }
}

const unavailable: () => never = () => {
  throw new UnavailableInput();
};

const encoder = new TextEncoder();

const isPlainRecord = (value: unknown): value is Record<string, unknown> => isRecord(value) && !Array.isArray(value);

const decodeUtf8 = (bytes: Uint8Array): string => new TextDecoder("utf-8", { fatal: true }).decode(bytes);

const parseJson = (text: string): unknown => {
  try {
    return JSON.parse(text);
  } catch {
    return unavailable();
  }
};

const relativeSegments = (value: string): readonly string[] => {
  if (!value || value.startsWith("/") || value.includes("\\") || value.includes("\0")) unavailable();
  const segments = value.split("/");
  if (segments.some((segment) => segment === "" || segment === "." || segment === "..")) unavailable();
  return segments;
};

const absoluteUnder = (cwd: string, segments: readonly string[]): string => {
  const base = cwd.endsWith("/") ? cwd.slice(0, -1) : cwd;
  return `${base}/${segments.join("/")}`;
};

/** Every ancestor and the final entry must be a regular non-symlink entry inside cwd. */
const requireRegularFile = (cwd: string, segments: readonly string[]): string => {
  for (let index = 0; index < segments.length - 1; index += 1) {
    const ancestor = absoluteUnder(cwd, segments.slice(0, index + 1));
    let info: Deno.FileInfo;
    try {
      info = Deno.lstatSync(ancestor);
    } catch {
      return unavailable();
    }
    if (!info.isDirectory || info.isSymlink) unavailable();
  }
  const absolute = absoluteUnder(cwd, segments);
  let info: Deno.FileInfo;
  try {
    info = Deno.lstatSync(absolute);
  } catch {
    return unavailable();
  }
  if (!info.isFile || info.isSymlink) unavailable();
  return absolute;
};

const readBoundedFile = (absolute: string, maxBytes: number): Uint8Array => {
  let info: Deno.FileInfo;
  try {
    info = Deno.lstatSync(absolute);
  } catch {
    return unavailable();
  }
  if (!info.isFile || info.isSymlink || info.size > maxBytes) unavailable();
  let bytes: Uint8Array;
  try {
    bytes = Deno.readFileSync(absolute);
  } catch {
    return unavailable();
  }
  if (bytes.byteLength > maxBytes) unavailable();
  return bytes;
};

/** Trusted dispatch metadata: exact shape, 16 KiB bound, fatal UTF-8, ordered valid IDs. */
const readDispatchInput = (cwd: string): DispatchInput => {
  const bytes = readBoundedFile(requireRegularFile(cwd, [METADATA_FILE]), METADATA_MAX_BYTES);
  const parsed = parseJson(decodeUtf8(bytes));
  if (!isPlainRecord(parsed)) unavailable();
  const expectedKeys = ["version", "requestPath", "upstreamPath", "testIds"];
  const keys = Object.keys(parsed);
  if (keys.length !== expectedKeys.length || keys.some((key) => !expectedKeys.includes(key))) unavailable();
  if (parsed.version !== "v1") unavailable();
  const requestPath = parsed.requestPath;
  const upstreamPath = parsed.upstreamPath;
  if (typeof requestPath !== "string" || typeof upstreamPath !== "string") unavailable();
  if (requestPath === upstreamPath) unavailable();
  if (!Array.isArray(parsed.testIds)) unavailable();
  if (parsed.testIds.length < 1 || parsed.testIds.length > MAX_TEST_IDS) unavailable();
  const testIds: string[] = [];
  const seen = new Set<string>();
  for (const candidate of parsed.testIds) {
    if (typeof candidate !== "string" || !TEST_ID_PATTERN.test(candidate) || seen.has(candidate)) unavailable();
    seen.add(candidate);
    testIds.push(candidate);
  }
  return { requestPath, upstreamPath, testIds };
};

/** Existing request envelope: `{ body: string, ...sanitized envelope }` with a Responses-shaped body. */
const readRequestEnvelope = (cwd: string, requestPath: string): RequestEnvelope => {
  const bytes = readBoundedFile(requireRegularFile(cwd, relativeSegments(requestPath)), MAX_ACCEPTED_JSON_BODY_BYTES);
  const parsed = parseJson(decodeUtf8(bytes));
  if (!isPlainRecord(parsed) || typeof parsed.body !== "string") unavailable();
  const bodyText = parsed.body;
  if (encoder.encode(bodyText).byteLength > MAX_ACCEPTED_JSON_BODY_BYTES) unavailable();
  const body = parseJson(bodyText);
  if (!isPlainRecord(body)) unavailable();
  // A chat-completions body is never replayed; a Responses body requires `input`.
  if (Array.isArray(body.messages)) unavailable();
  const input = body.input;
  if (typeof input !== "string" && !Array.isArray(input)) unavailable();
  const stream = body.stream;
  if (stream !== undefined && typeof stream !== "boolean") unavailable();
  return { bodyText, body };
};

/** Existing recorded upstream trace with truncation flags and the parser's own bounds enforced. */
const readUpstreamTrace = (cwd: string, upstreamPath: string): SentinelUpstreamTrace => {
  const bytes = readBoundedFile(requireRegularFile(cwd, relativeSegments(upstreamPath)), UPSTREAM_FILE_MAX_BYTES);
  const parsed = parseJson(decodeUtf8(bytes));
  try {
    return parseSentinelUpstreamTrace(parsed);
  } catch {
    return unavailable();
  }
};

/** Exactly one complete untruncated supported attempt; everything else is unavailable. */
const selectSupportedAttempt = (trace: SentinelUpstreamTrace): Readonly<{ provider: SupportedProvider; terminal: SupportedTerminal }> => {
  if (trace.attempts_truncated || trace.bytes_truncated || trace.chunks_truncated) unavailable();
  if (trace.attempts.length !== 1) unavailable();
  const attempt = trace.attempts[0]!;
  // Cerebras is a chat-completions transport and is never replayed here.
  if (attempt.provider !== "chatgpt_codex" && attempt.provider !== "surplus" && attempt.provider !== "metered") {
    unavailable();
  }
  if (attempt.terminal !== "eof" && attempt.terminal !== "cancelled") unavailable();
  if (attempt.status !== 200) unavailable();
  if (attempt.content_type !== "text/event-stream") unavailable();
  return { provider: attempt.provider, terminal: attempt.terminal };
};

/**
 * Dispatch the recorded attempt. Codex raw recorded bytes feed the recorded
 * transport directly (no provider dispatch, no auth or routing simulation).
 * Paid attempts go through the exported provider fetch functions with the
 * recorded fetcher and an explicit synthetic key, preserving their real
 * normalization. No host key is ever read.
 */
const openRecordedResponse = async (provider: SupportedProvider, request: RequestEnvelope, replay: RecordedUpstreamReplay): Promise<Response> => {
  let response: Response;
  try {
    if (provider === "chatgpt_codex") {
      response = await replay.fetch(PROVIDER_ROUTES.chatgpt_codex, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: request.bodyText,
      });
    } else if (provider === "surplus") {
      response = (
        await fetchSurplusResponses(request.body, {
          apiKey: SYNTHETIC_PAID_API_KEY,
          fetcher: replay.fetch,
        })
      ).response;
    } else {
      response = (
        await fetchMeteredResponses(request.body, {
          apiKey: SYNTHETIC_PAID_API_KEY,
          fetcher: replay.fetch,
        })
      ).response;
    }
  } catch {
    return unavailable();
  }
  if (!response.ok || !response.body) unavailable();
  return response;
};

/**
 * Structured evidence recorded from the real parser while the real converter
 * consumes the prepared iterator. The buffered converter maps every iterator
 * failure, including malformed events and changed response identities, to the
 * same fixed 502 body, so that failure text alone never proves causality.
 * Recording this observation never replaces or reimplements conversion.
 */
type ParserEvidence = {
  /** The parser itself threw its recognized premature_eof kind. */
  prematureEof: boolean;
  /** The parser threw a different failure (malformed event, read error, ...). */
  otherFailure: boolean;
  /** The iterator was fully consumed and returned done without a terminal. */
  exhaustedWithoutTerminal: boolean;
  /** Upstream-declared terminal event type the parser observed, if any. */
  upstreamTerminalType: string | null;
};

const emptyParserEvidence = (): ParserEvidence => ({
  prematureEof: false,
  otherFailure: false,
  exhaustedWithoutTerminal: false,
  upstreamTerminalType: null,
});

/**
 * Transparent observation of the real prepared iterator: identical events in
 * the identical order, the identical terminal `done` result, the identical
 * error object rethrown to the converter, and the identical iterator cleanup.
 * The only added effect is the structured parser evidence above.
 */
const observeParserIterator = (iterator: ResponsesStreamIterator, evidence: ParserEvidence): ResponsesStreamIterator =>
  (async function* (): AsyncGenerator<ResponsesStreamEvent, unknown, unknown> {
    try {
      while (true) {
        let next: IteratorResult<ResponsesStreamEvent, unknown>;
        try {
          next = await iterator.next();
        } catch (error) {
          if (error instanceof ResponsesStreamError && error.kind === "premature_eof") evidence.prematureEof = true;
          else evidence.otherFailure = true;
          throw error;
        }
        if (next.done || !next.value) {
          if (evidence.upstreamTerminalType === null) evidence.exhaustedWithoutTerminal = true;
          return next.value;
        }
        if (next.value.terminal) evidence.upstreamTerminalType ??= next.value.type;
        yield next.value;
      }
    } finally {
      await iterator.return("Sentinel replay parser observation closed").catch(() => {});
    }
  })();

/** The owned stream's fixed failure event for a parser failure, matched exactly. */
const isSyntheticPrematureEofEvent = (event: ResponsesStreamEvent): boolean => {
  if (event.type === "response.failed") {
    const response = event.value.response;
    if (!isPlainRecord(response) || response.status !== "failed") return false;
    const error = response.error;
    if (!isPlainRecord(error)) return false;
    return error.code === UPSTREAM_ENDED_CODE && error.message === OWNED_UPSTREAM_ENDED_MESSAGE;
  }
  if (event.type === "error") {
    return event.value.code === UPSTREAM_ENDED_CODE && event.value.message === OWNED_UPSTREAM_ENDED_MESSAGE;
  }
  return false;
};

/** The buffered converter's fixed 502 body for a parser failure, matched exactly. */
const isBufferedPrematureEofResponse = (status: number, parsed: unknown): boolean => {
  if (status !== 502 || !isPlainRecord(parsed)) return false;
  const error = parsed.error;
  if (!isPlainRecord(error)) return false;
  return error.code === UPSTREAM_ENDED_CODE && error.message === BUFFERED_UPSTREAM_ENDED_MESSAGE;
};

/** Semantic content the gateway's own utility accepts on a completed response. */
const hasSemanticCompletion = (event: ResponsesStreamEvent): boolean => responsesEventSemanticKind(event) !== null;

/**
 * The gateway's own empty-semantic-completion guard
 * (`prepareResponsesAttempt`): a committed `response.completed` terminal with
 * no semantic event is never a successful completion, however nonempty its
 * output array looks.
 */
const isEmptySemanticCompletion = (prepared: PreparedResponsesStream): boolean =>
  prepared.terminal?.type === "response.completed" && prepared.semantic === null;

/**
 * The actual converted Response object must carry completed status, the
 * Responses object shape and the observed response identity. Malformed output
 * the real converter did not repair must not become a claimed success.
 */
const isCompletedResponseObject = (value: Record<string, unknown>, observedResponseId: string | null): boolean => {
  if (value.status !== "completed" && value.status !== "succeeded") return false;
  if (value.object !== "response") return false;
  const id = getString(value.id)?.trim();
  if (!id) return false;
  return observedResponseId === null || id === observedResponseId;
};

/**
 * stream=true: the owned stream must produce a validated terminal. Its fixed
 * failure event is only recorded as an observed converter output, never as a
 * classification.
 */
const consumeOwnedResponses = async (prepared: PreparedResponsesStream): Promise<ConverterOutcome> => {
  const observedResponseId = responseIdFromEvents(prepared.buffered);
  const owned = createOwnedResponsesStream({
    initial: prepared.buffered,
    iterator: prepared.iterator,
    responseId: observedResponseId,
  });
  let terminalEvent: ResponsesStreamEvent | null = null;
  for await (const event of readResponsesStream(owned)) {
    if (event.terminal) terminalEvent = event;
  }
  if (terminalEvent === null) unavailable();
  if (terminalEvent.type === "response.completed") {
    // The terminal is the real owned stream's own output: validate the actual
    // completed Response it carries, not just its event type.
    const response = terminalEvent.value.response;
    if (!isPlainRecord(response)) unavailable();
    if (!isCompletedResponseObject(response, observedResponseId)) unavailable();
    if (!hasSemanticCompletion(terminalEvent)) unavailable();
    return "completed";
  }
  if (isSyntheticPrematureEofEvent(terminalEvent)) return "premature_eof_output";
  return unavailable();
};

/**
 * stream=false: the exported buffered converter must return a completed
 * response or its exact fixed failure body. That body is only recorded as an
 * observed converter output, never as a classification.
 */
const consumeBufferedResponses = async (prepared: PreparedResponsesStream, provider: SupportedProvider): Promise<ConverterOutcome> => {
  const observedResponseId = responseIdFromEvents(prepared.buffered);
  const response = await collectBufferedResponses({ provider, responseId: observedResponseId, prepared });
  const parsed = parseJson(await response.text());
  if (response.ok) {
    if (!isPlainRecord(parsed) || !isCompletedResponseObject(parsed, observedResponseId)) unavailable();
    // The actual buffered Response object is what the converter returned: the
    // gateway's own utility decides whether it carries useful semantic content
    // instead of trusting a nonempty output array.
    const completed = responseEventFromValue({ type: "response.completed", response: parsed });
    if (!hasSemanticCompletion(completed)) unavailable();
    return "completed";
  }
  if (isBufferedPrematureEofResponse(response.status, parsed)) return "premature_eof_output";
  return unavailable();
};

/**
 * Causal only when the recorded body ended (eof), the real parser recorded an
 * actual end-of-stream signal, no upstream terminal or other failure was
 * declared, and the converter produced its exact fixed failure output.
 */
const classifyConverterFailure = (terminal: SupportedTerminal, evidence: ParserEvidence): ReplayOutcome => {
  if (terminal !== "eof") return "unavailable";
  if (evidence.upstreamTerminalType !== null) return "unavailable";
  if (evidence.otherFailure) return "unavailable";
  if (evidence.prematureEof || evidence.exhaustedWithoutTerminal) return "causal";
  return "unavailable";
};

/** Frozen real response sequence: replayed body -> preflight -> first event plus iterator -> prepare -> observed converter. */
const runReplaySequence = async (
  provider: SupportedProvider,
  terminal: SupportedTerminal,
  response: Response,
  body: Record<string, unknown>
): Promise<ReplayOutcome> => {
  let prepared: PreparedResponsesStream;
  try {
    const preflight = await preflightResponsesStream(response.body!);
    const replayed = (async function* (): AsyncGenerator<ResponsesStreamEvent> {
      try {
        yield preflight.first;
        for await (const event of preflight.iterator) yield event;
        return undefined;
      } finally {
        await preflight.iterator.return("Sentinel replay preflight closed").catch(() => {});
      }
    })();
    prepared = await prepareResponsesStreamForCommit(replayed);
  } catch (error) {
    // Precommit evidence only: the real parser (or the precommit stage
    // consuming it) ended the recorded body before a terminal and before any
    // semantic output. A recorded eof plus its recognized premature_eof kind
    // is the causal failure; every other precommit failure is unavailable.
    return terminal === "eof" && error instanceof ResponsesStreamError && error.kind === "premature_eof" ? "causal" : "unavailable";
  }
  const evidence = emptyParserEvidence();
  // Mirror the gateway's own empty-semantic-completion guard
  // (prepareResponsesAttempt): a `response.completed` terminal committed
  // without a semantic event is rejected before any conversion.
  if (isEmptySemanticCompletion(prepared)) unavailable();
  // A terminal buffered before semantic output is upstream-declared evidence,
  // never the parser's premature EOF.
  if (prepared.terminal) evidence.upstreamTerminalType ??= prepared.terminal.type;
  const observed: PreparedResponsesStream = {
    ...prepared,
    iterator: observeParserIterator(prepared.iterator, evidence),
  };
  const outcome = body.stream === true ? await consumeOwnedResponses(observed) : await consumeBufferedResponses(observed, provider);
  return outcome === "premature_eof_output" ? classifyConverterFailure(terminal, evidence) : outcome;
};

const runAttempt = async (
  provider: SupportedProvider,
  terminal: SupportedTerminal,
  response: Response,
  body: Record<string, unknown>
): Promise<ReplayOutcome> => {
  try {
    return await runReplaySequence(provider, terminal, response, body);
  } catch {
    // Any failure outside the classified replay sequence is unavailable.
    return unavailable();
  }
};

const runReplay = async (input: DispatchInput): Promise<ReplayOutcome> => {
  const cwd = Deno.cwd();
  const request = readRequestEnvelope(cwd, input.requestPath);
  const trace = readUpstreamTrace(cwd, input.upstreamPath);
  const attempt = selectSupportedAttempt(trace);
  let replay: RecordedUpstreamReplay;
  try {
    replay = createRecordedUpstreamReplay(trace, PROVIDER_ROUTES);
  } catch {
    return unavailable();
  }
  const response = await openRecordedResponse(attempt.provider, request, replay);
  const outcome = await runAttempt(attempt.provider, attempt.terminal, response, request.body);
  if (outcome === "unavailable") return outcome;
  // No unconsumed captured bytes and no invented EOF may back a claim.
  try {
    replay.assertComplete();
  } catch {
    return unavailable();
  }
  return outcome;
};

const writeStdout = (text: string): void => {
  Deno.stdout.writeSync(encoder.encode(text));
};

const emitUnavailable = (): never => {
  writeStdout(UNAVAILABLE_LINE);
  Deno.exit(2);
};

try {
  const input = readDispatchInput(Deno.cwd());
  const outcome = await runReplay(input);
  if (outcome === "unavailable") emitUnavailable();
  writeStdout(input.testIds.map((id) => `${TEST_MARKER_PREFIX}${id}\n`).join(""));
  if (outcome === "causal") {
    writeStdout(CAUSAL_FAILURE_LINE);
    Deno.exit(1);
  }
  Deno.exit(0);
} catch {
  emitUnavailable();
}
