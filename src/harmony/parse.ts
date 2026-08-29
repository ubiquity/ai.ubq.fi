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

/** Normalizes raw Harmony messages into adapter turns. */
export const harmonyTurnsFromMessages = (messages: readonly HarmonyRawMessage[]): readonly HarmonyTurn[] => {
  const turns: HarmonyTurn[] = [];
  for (const message of messages) {
    if (message.role !== "assistant") continue;
    if (message.channel === "analysis") {
      if (message.content) turns.push({ kind: "reasoning", text: message.content });
      continue;
    }
    if (message.recipient) {
      // Function calls travel on the commentary channel; built-in tools
      // (browser/python) travel on the analysis channel.  Either way the
      // recipient identifies the call.
      if (message.content) {
        turns.push({
          kind: "tool_call",
          recipient: message.recipient,
          name: functionNameFromRecipient(message.recipient),
          arguments: normalizeToolArguments(message.content),
        });
      }
      continue;
    }
    if (message.channel === "final" || (message.channel === null && message.stoppedBy === "<|return|>")) {
      if (message.content) turns.push({ kind: "final", text: message.content });
    } else if (message.channel === "commentary") {
      if (message.content) turns.push({ kind: "commentary", text: message.content });
    }
  }
  return turns;
};

/**
 * Scans raw completion text into Harmony messages.  The parser is optimistic:
 * any non-empty content is retained, and only unambiguous structure is
 * interpreted.
 */
export const parseHarmonyOutput = (text: string): HarmonyParseResult => {
  const messages: HarmonyRawMessage[] = [];
  let header: Header = { ...FRESH_HEADER };
  let sections: HeaderSection[] = [];
  let currentSection: HeaderSection = { kind: "start", text: "" };
  let contentStarted = false;
  let content = "";
  let truncated = false;

  const resetHeader = (): void => {
    header = { ...FRESH_HEADER };
    sections = [];
    currentSection = { kind: "start", text: "" };
    contentStarted = false;
    content = "";
  };

  const emit = (stoppedBy: HarmonyRawMessage["stoppedBy"]): void => {
    const headerHasText = sections.some((section) => section.text.trim()) || currentSection.text.trim();
    if (content || headerHasText) {
      messages.push({
        role: header.role,
        channel: header.channel,
        recipient: header.recipient,
        constrain: header.constrain,
        content,
        stoppedBy,
      });
    }
    if (stoppedBy === "truncated") truncated = true;
    resetHeader();
  };

  let position = 0;
  while (position < text.length) {
    let markerIndex = -1;
    let marker: (typeof MARKERS)[number] | null = null;
    for (const candidate of MARKERS) {
      const index = text.indexOf(candidate, position);
      if (index !== -1 && (markerIndex === -1 || index < markerIndex)) {
        markerIndex = index;
        marker = candidate;
      }
    }

    if (marker === null || markerIndex === -1) {
      const tail = text.slice(position);
      if (tail) {
        if (contentStarted) content += tail;
        else currentSection.text += tail;
      }
      if (contentStarted && content) {
        emit("truncated");
      } else if (currentSection.text.trim()) {
        emit("truncated");
      }
      break;
    }

    const before = text.slice(position, markerIndex);
    if (before) {
      if (contentStarted) content += before;
      else currentSection.text += before;
    }

    switch (marker) {
      case START:
        if (contentStarted && content) emit("truncated");
        resetHeader();
        break;
      case CHANNEL:
        sections.push(currentSection);
        currentSection = { kind: "channel", text: "" };
        break;
      case CONSTRAIN:
        sections.push(currentSection);
        currentSection = { kind: "constrain", text: "" };
        break;
      case END: {
        header = parseHeader(sections.concat(currentSection));
        emit("<|end|>");
        break;
      }
      case CALL: {
        header = parseHeader(sections.concat(currentSection));
        emit("<|call|>");
        break;
      }
      case RETURN: {
        header = parseHeader(sections.concat(currentSection));
        emit("<|return|>");
        break;
      }
      case MESSAGE: {
        header = parseHeader(sections.concat(currentSection));
        contentStarted = true;
        break;
      }
    }
    position = markerIndex + marker.length;
  }

  if (messages.length === 0 && text.trim()) {
    messages.push({
      role: "assistant",
      channel: null,
      recipient: null,
      constrain: null,
      content: text,
      stoppedBy: "truncated",
    });
    truncated = true;
  }

  return { messages, turns: harmonyTurnsFromMessages(messages), truncated };
};
