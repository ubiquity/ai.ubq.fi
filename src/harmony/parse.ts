/**
 * Harmony assistant-output parser (plan m01).
 *
 * Tolerant scanner for the Harmony special-token grammar emitted by gpt-oss
 * models.  It accepts both documented header orders ("recipient in the role
 * section" and "recipient in the channel section"), missing
 * `<|start|>assistant` prefixes (a completion continues the prompt's open
 * assistant message), omitted `<|end|>` before `<|call|>`/`<|return|>`, and
 * truncated tail text.
 *
 * Grammar reference:
 * https://github.com/openai/harmony/blob/main/docs/format.md
 */

import type { HarmonyChannel, HarmonyParseResult, HarmonyRawMessage, HarmonyTurn } from "./types.ts";

const START = "<|start|>";
const CHANNEL = "<|channel|>";
const MESSAGE = "<|message|>";
const END = "<|end|>";
const CALL = "<|call|>";
const RETURN = "<|return|>";
const CONSTRAIN = "<|constrain|>";

const MARKERS = [START, CHANNEL, CONSTRAIN, MESSAGE, END, CALL, RETURN] as const;

const ASSISTANT_CHANNELS: ReadonlySet<string> = new Set(["analysis", "commentary", "final"]);

const isAssistantChannel = (value: string): value is HarmonyChannel => ASSISTANT_CHANNELS.has(value);

const firstToken = (value: string): string | null => {
  const match = /^\s*([^\s<]+)/.exec(value);
  return match ? match[1] : null;
};

const findRecipient = (value: string): string | null => {
  const match = /(?:^|\s)to=([^\s<]+)/.exec(value);
  return match ? match[1] : null;
};

/**
 * Strips the `functions.` namespace prefix from a recipient to recover the
 * model-facing tool name (mirrors vLLM's `extract_function_from_recipient`).
 * Recipients outside the `functions.` namespace (e.g. built-in browser
 * tools) are returned unchanged.
 */
export const functionNameFromRecipient = (recipient: string): string => {
  const prefix = "functions.";
  return recipient.startsWith(prefix) ? recipient.slice(prefix.length) : recipient;
};

/**
 * Normalizes tool arguments: valid JSON is re-serialized compactly so replay
 * and instrumentation are deterministic; invalid JSON stays verbatim (the
 * shared application pipeline rejects it later).
 */
export const normalizeToolArguments = (argumentsText: string): string => {
  if (!argumentsText.trim()) return argumentsText;
  try {
    return JSON.stringify(JSON.parse(argumentsText));
  } catch {
    return argumentsText;
  }
};

/** One header section, keyed by the marker that introduced it. */
type HeaderSection = { kind: "start" | "channel" | "constrain"; text: string };

type Header = {
  role: string;
  channel: HarmonyChannel | null;
  recipient: string | null;
  constrain: string | null;
};

const FRESH_HEADER: Header = { role: "assistant", channel: null, recipient: null, constrain: null };

/** Extracts role/channel/recipient/constrain from the ordered header sections. */
const parseHeader = (sections: readonly HeaderSection[]): Header => {
  const startText = sections.find((section) => section.kind === "start")?.text ?? "";
  const channelText = sections.find((section) => section.kind === "channel")?.text ?? "";
  const constrainText = sections.find((section) => section.kind === "constrain")?.text ?? "";

  const roleToken = firstToken(startText);
  const role = roleToken && roleToken !== "assistant" ? roleToken : "assistant";
  const channelToken = firstToken(channelText);
  return {
    role,
    channel: channelToken && isAssistantChannel(channelToken) ? channelToken : null,
    recipient: findRecipient(startText) ?? findRecipient(channelText),
    constrain: firstToken(constrainText),
  };
};

/**
 * Maps one assistant message to its adapter turn, or null when the message
 * carries no turn at all (no content, or a channel that never becomes one).
 */
const turnFromAssistantMessage = (message: HarmonyRawMessage): HarmonyTurn | null => {
  if (message.recipient) {
    // Function calls travel on the commentary channel; built-in tools
    // (browser/python) travel on the analysis channel.  Either way the
    // recipient identifies the call.
    if (!message.content) return null;
    return {
      kind: "tool_call",
      recipient: message.recipient,
      name: functionNameFromRecipient(message.recipient),
      arguments: normalizeToolArguments(message.content),
    };
  }
  if (!message.content) return null;
  if (message.channel === "analysis") return { kind: "reasoning", text: message.content };
  if (message.channel === "final" || (message.channel === null && message.stoppedBy === "<|return|>")) {
    return { kind: "final", text: message.content };
  }
  if (message.channel === "commentary") return { kind: "commentary", text: message.content };
  return null;
};

/** Normalizes raw Harmony messages into adapter turns. */
export const harmonyTurnsFromMessages = (messages: readonly HarmonyRawMessage[]): readonly HarmonyTurn[] => {
  const turns: HarmonyTurn[] = [];
  for (const message of messages) {
    if (message.role !== "assistant") continue;
    const turn = turnFromAssistantMessage(message);
    if (turn) turns.push(turn);
  }
  return turns;
};

/** One recognized special token. */
type Marker = (typeof MARKERS)[number];

/** Scanner state for one completion: the parser's former local variables. */
type ScannerState = {
  header: Header;
  sections: HeaderSection[];
  currentSection: HeaderSection;
  contentStarted: boolean;
  content: string;
  messages: HarmonyRawMessage[];
  truncated: boolean;
};

const freshScannerState = (): ScannerState => ({
  header: { ...FRESH_HEADER },
  sections: [],
  currentSection: { kind: "start", text: "" },
  contentStarted: false,
  content: "",
  messages: [],
  truncated: false,
});

/** Drops the pending header sections and whatever they collected. */
const resetScannerHeader = (state: ScannerState): void => {
  state.header = { ...FRESH_HEADER };
  state.sections = [];
  state.currentSection = { kind: "start", text: "" };
  state.contentStarted = false;
  state.content = "";
};

/** Appends text to the open content or, before `<|message|>`, to the header section. */
const appendScannerText = (state: ScannerState, text: string): void => {
  if (!text) return;
  if (state.contentStarted) state.content += text;
  else state.currentSection.text += text;
};

/** Emits the pending message when it carries anything, then starts a fresh header. */
const emitScannerMessage = (state: ScannerState, stoppedBy: HarmonyRawMessage["stoppedBy"]): void => {
  const headerHasText = state.sections.some((section) => section.text.trim()) || state.currentSection.text.trim();
  if (state.content || headerHasText) {
    state.messages.push({
      role: state.header.role,
      channel: state.header.channel,
      recipient: state.header.recipient,
      constrain: state.header.constrain,
      content: state.content,
      stoppedBy,
    });
  }
  if (stoppedBy === "truncated") state.truncated = true;
  resetScannerHeader(state);
};

/** Applies one special token to the scanner state. */
const applyScannerMarker = (state: ScannerState, marker: Marker): void => {
  switch (marker) {
    case START:
      if (state.contentStarted && state.content) emitScannerMessage(state, "truncated");
      resetScannerHeader(state);
      return;
    case CHANNEL:
      state.sections.push(state.currentSection);
      state.currentSection = { kind: "channel", text: "" };
      return;
    case CONSTRAIN:
      state.sections.push(state.currentSection);
      state.currentSection = { kind: "constrain", text: "" };
      return;
    case END:
      state.header = parseHeader(state.sections.concat(state.currentSection));
      emitScannerMessage(state, "<|end|>");
      return;
    case CALL:
      state.header = parseHeader(state.sections.concat(state.currentSection));
      emitScannerMessage(state, "<|call|>");
      return;
    case RETURN:
      state.header = parseHeader(state.sections.concat(state.currentSection));
      emitScannerMessage(state, "<|return|>");
      return;
    case MESSAGE:
      state.header = parseHeader(state.sections.concat(state.currentSection));
      state.contentStarted = true;
      return;
    default:
      return;
  }
};

/** The earliest special token at or after `position`, or null when none remains. */
const nextMarkerAt = (text: string, position: number): Readonly<{ index: number; marker: Marker }> | null => {
  let markerIndex = -1;
  let marker: Marker | null = null;
  for (const candidate of MARKERS) {
    const index = text.indexOf(candidate, position);
    if (index === -1) continue;
    if (markerIndex !== -1 && index >= markerIndex) continue;
    markerIndex = index;
    marker = candidate;
  }
  return marker === null || markerIndex === -1 ? null : { index: markerIndex, marker };
};

/**
 * Scans raw completion text into Harmony messages.  The parser is optimistic:
 * any non-empty content is retained, and only unambiguous structure is
 * interpreted.
 */
export const parseHarmonyOutput = (text: string): HarmonyParseResult => {
  const state = freshScannerState();

  let position = 0;
  while (position < text.length) {
    const found = nextMarkerAt(text, position);
    if (found === null) {
      appendScannerText(state, text.slice(position));
      if ((state.contentStarted && state.content) || state.currentSection.text.trim()) emitScannerMessage(state, "truncated");
      break;
    }

    appendScannerText(state, text.slice(position, found.index));
    applyScannerMarker(state, found.marker);
    position = found.index + found.marker.length;
  }

  if (state.messages.length === 0 && text.trim()) {
    state.messages.push({
      role: "assistant",
      channel: null,
      recipient: null,
      constrain: null,
      content: text,
      stoppedBy: "truncated",
    });
    state.truncated = true;
  }

  return { messages: state.messages, turns: harmonyTurnsFromMessages(state.messages), truncated: state.truncated };
};
