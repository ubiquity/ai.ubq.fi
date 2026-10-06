// Shared contracts for the DeepSeek V4.1 Flash provider waterfall benchmark.
//
// DESIGN.md is the authority for how these records are produced; this file
// only fixes the shapes so the corpus builder, runner, cost module and scorer
// can be written against one another.

export type CorpusClass = "small" | "medium" | "large" | "xlarge";

export type ResponsesInputItem = Record<string, unknown>;

export type CorpusEntry = Readonly<{
  id: string;
  cls: CorpusClass;
  source: Readonly<{
    session_id: string;
    thread_id: string | null;
    turn_id: string | null;
    recorded_model: string;
    recorded_at: string;
    /** Tokens the recorded turn reported for input, including cached tokens. */
    recorded_input_tokens: number;
    item_count: number;
    dropped_item_types: readonly string[];
  }>;
  /** Provider-neutral Responses-shaped request content. */
  request: Readonly<{
    instructions: string;
    input: readonly ResponsesInputItem[];
    tools: readonly ResponsesInputItem[];
    tool_choice: "auto";
    parallel_tool_calls: boolean;
    reasoning: Readonly<{ effort: string }> | null;
    max_output_tokens: number | null;
  }>;
}>;

export type TokenUsage = Readonly<{
  input_tokens: number;
  cached_input_tokens: number;
  cache_write_input_tokens: number;
  output_tokens: number;
  reasoning_tokens: number;
  total_tokens: number;
}>;

export const FAILURE_KINDS = [
  "connection_failure",
  "dns_network_failure",
  "timeout",
  "http_429",
  "http_4xx",
  "http_5xx",
  "malformed_sse",
  "interrupted_stream",
  "missing_completion",
  "malformed_json",
  "malformed_tool_call",
  "context_length_rejection",
  "capacity_failure",
  "empty_response",
  "truncated_response",
  "usage_accounting_failure",
  "client_abort",
  "gateway_rejection",
] as const;

export type FailureKind = (typeof FAILURE_KINDS)[number];

/** One attempt against one provider for one corpus entry. */
export type AttemptRecord = Readonly<{
  run_id: string;
  provider: string;
  corpus_id: string;
  cls: CorpusClass;
  period: string;
  batch: number;
  attempt: number;
  wire: "responses" | "chat";
  model: string;
  selection: readonly string[];
  gateway: string;
  started_at: string;
  finished_at: string;
  /** Milliseconds from request start; null when the stage never happened. */
  t_headers_ms: number | null;
  t_first_byte_ms: number | null;
  t_first_event_ms: number | null;
  t_first_output_ms: number | null;
  t_first_item_ms: number | null;
  t_terminal_ms: number | null;
  t_end_ms: number | null;
  http_status: number | null;
  upstream: string | null;
  provider_request_id: string | null;
  /** Upstream response id when the wire exposes one (OpenRouter generation ids). */
  response_id: string | null;
  success: boolean;
  first_attempt: boolean;
  failure_kind: FailureKind | null;
  error_code: string | null;
  error_message: string | null;
  response_terminated: boolean;
  sse_events: number;
  output_chars: number;
  tool_calls: number;
  usage: TokenUsage | null;
  usage_reported: boolean;
  /** Free-form notes; never used for scoring. */
  notes: string | null;
}>;
