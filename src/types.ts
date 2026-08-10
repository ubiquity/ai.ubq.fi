export type CodexAuthState = Readonly<{
  access_token: string;
  refresh_token: string;
  account_id: string;
  updated_at_ms: number;
}>;

export type CodexAuthPoolState = Readonly<{
  accounts: readonly CodexAuthState[];
  updated_at_ms: number;
}>;

export type ApiKeyRecord = Readonly<{
  id: string;
  name: string;
  prefix: string;
  hash: string;
  created_at_ms: number;
  expires_at_ms: number;
  revoked_at_ms: number | null;
  usage_limit_requests: number;
  usage_requests: number;
  usage_reset_at_ms: number;
  window_ms: number;
  /**
   * Runtime request-quota ledger version. Version 3 reserves capacity before
   * route handling and commits it immediately before the first provider
   * transport; V2 counters are migration input only.
   */
  usage_quota_version: 3;
  paid_fallback_enabled: boolean;
  paid_fallback_limit_microcredits: number;
  paid_fallback_spent_microcredits: number;
  paid_fallback_reserved_microcredits: number;
  paid_fallback_reservation_request_id: string | null;
  paid_fallback_model_ids: string[];
  paid_fallback_quota_per_credit: number;
  paid_fallback_max_exposure_microcredits?: Record<string, number>;
  paid_fallback_pricing_checked_at_ms: number | null;
}>;

export type ApiKeyHashRecord = Readonly<{
  id: string;
  expires_at_ms: number;
  revoked_at_ms: number | null;
  usage_limit_requests: number;
  usage_requests: number;
  usage_reset_at_ms: number;
  window_ms: number;
  usage_quota_version: 3;
  paid_fallback_enabled: boolean;
  paid_fallback_limit_microcredits: number;
  paid_fallback_spent_microcredits: number;
  paid_fallback_reserved_microcredits: number;
  paid_fallback_reservation_request_id: string | null;
}>;

export type ApiKeyUsageWindowV3 = Readonly<{
  v: 3;
  key_id: string;
  policy_version: string;
  window_start_ms: number;
  window_reset_at_ms: number;
  committed_requests: number;
  reserved_requests: number;
  updated_at_ms: number;
}>;

export type ApiKeyUsageRequestV3 = Readonly<{
  v: 3;
  key_id: string;
  request_id: string;
  route: string;
  state: "reserved" | "dispatched" | "released";
  reserved_at_ms: number;
  lease_expires_at_ms: number;
  provider: "chatgpt_codex" | "yunwu" | "voyage" | null;
  dispatched_at_ms: number | null;
  released_at_ms: number | null;
  release_reason: string | null;
}>;

export type ApiKeyUsageRecord = Readonly<{
  key_id: string;
  total_requests: number;
  stream_requests: number;
  non_stream_requests: number;
  completed_requests: number;
  error_requests: number;
  input_tokens: number;
  output_tokens: number;
  total_tokens: number;
  first_seen_at_ms: number;
  last_seen_at_ms: number;
  last_model: string | null;
  last_reasoning: string | null;
  last_route: string | null;
  yunwu_fallback_requests: number;
  yunwu_input_tokens: number;
  yunwu_output_tokens: number;
  yunwu_total_tokens: number;
  yunwu_spend_microcredits: number;
}>;

export type ApiKeyUsageDay = Readonly<{
  day: string;
  request_count: number;
  yunwu_fallback_requests: number;
  yunwu_spend_microcredits: number;
}>;

export type ApiKeyUsageDailyRecord = Readonly<{
  key_id: string;
  days: ApiKeyUsageDay[];
  updated_at_ms: number;
}>;

export type ApiKeyRequestLogRecord = Readonly<{
  id: string;
  key_id: string;
  route: string;
  path: string;
  method: string;
  status_code: number;
  stream: boolean;
  model: string | null;
  reasoning: string | null;
  created_at_ms: number;
  provider: "chatgpt_codex" | "voyage" | "yunwu";
  fallback_reason: string | null;
  provider_request_id: string | null;
  completed_at_ms: number | null;
  latency_ms: number | null;
  input_tokens: number | null;
  output_tokens: number | null;
  provider_quota: number | null;
  quota_per_credit: number | null;
  spend_microcredits: number | null;
  paid_fallback_window_reset_at_ms: number | null;
  billing_status: "not_applicable" | "pending" | "reconciled" | "not_billed" | "unresolved";
}>;

export type PaidFallbackWindowV3 = Readonly<{
  v: 3;
  key_id: string;
  policy_version: string;
  window_reset_at_ms: number;
  limit_microcredits: number;
  settled_microcredits: number;
  reserved_microcredits: number;
  pending_count: number;
  updated_at_ms: number;
}>;

export type PaidFallbackRequestV3 = Readonly<{
  v: 3;
  key_id: string;
  request_id: string;
  policy_version: string;
  route: string;
  path: string;
  model: string;
  stream: boolean;
  reasoning: string | null;
  window_reset_at_ms: number;
  reserved_microcredits: number;
  quota_per_credit: number;
  provider_request_id: string | null;
  provider_quota: number | null;
  input_tokens: number | null;
  output_tokens: number | null;
  dispatch_state: "reserved" | "dispatched" | "not_dispatched";
  terminal_state: "pending" | "completed" | "failed" | "incomplete" | "cancelled" | "ambiguous";
  spend_microcredits: number | null;
  billing_state: "pending" | "settled" | "not_billed" | "unresolved";
  reconciliation_attempts: number;
  last_reconciliation_at_ms: number | null;
  dispatched_at_ms: number | null;
  terminal_at_ms: number | null;
  settled_at_ms: number | null;
  created_at_ms: number;
  updated_at_ms: number;
}>;

export type KernelAuthUsageRecord = Readonly<{
  owner: string;
  repo: string;
  total_requests: number;
  stream_requests: number;
  non_stream_requests: number;
  completed_requests: number;
  error_requests: number;
  input_tokens: number;
  output_tokens: number;
  total_tokens: number;
  first_seen_at_ms: number;
  last_seen_at_ms: number;
  last_model: string | null;
  last_reasoning: string | null;
  last_route: string | null;
}>;

export type KernelAuthUsageDay = Readonly<{
  day: string;
  request_count: number;
}>;

export type KernelAuthUsageDailyRecord = Readonly<{
  owner: string;
  repo: string;
  days: KernelAuthUsageDay[];
  updated_at_ms: number;
}>;

export type KernelAuthLimitRecord = Readonly<{
  owner: string;
  repo: string;
  usage_limit_requests: number;
  usage_requests: number;
  usage_reset_at_ms: number;
  window_ms: number;
  expires_at_ms: number;
  created_at_ms: number;
  updated_at_ms: number;
}>;

export type KernelOrgUsageRecord = Readonly<{
  owner: string;
  total_requests: number;
  stream_requests: number;
  non_stream_requests: number;
  completed_requests: number;
  error_requests: number;
  input_tokens: number;
  output_tokens: number;
  total_tokens: number;
  first_seen_at_ms: number;
  last_seen_at_ms: number;
  last_model: string | null;
  last_reasoning: string | null;
  last_route: string | null;
}>;

export type KernelOrgUsageDay = Readonly<{
  day: string;
  request_count: number;
}>;

export type KernelOrgUsageDailyRecord = Readonly<{
  owner: string;
  days: KernelOrgUsageDay[];
  updated_at_ms: number;
}>;

export type KernelOrgLimitRecord = Readonly<{
  owner: string;
  usage_limit_requests: number;
  usage_requests: number;
  usage_reset_at_ms: number;
  window_ms: number;
  expires_at_ms: number;
  created_at_ms: number;
  updated_at_ms: number;
}>;

export type KernelPolicyQueueItem = Readonly<{
  owner: string;
  repo: string;
  request_count: number;
  first_seen_at_ms: number;
  last_seen_at_ms: number;
  last_route: string | null;
}>;

export type ChatCompletionRequest = Readonly<{
  model?: unknown;
  messages?: unknown;
  reasoning_effort?: unknown;
  stream?: unknown;
}>;

export type ResponsesRequest = Readonly<{
  client_metadata?: unknown;
  model?: unknown;
  input?: unknown;
  instructions?: unknown;
  reasoning?: unknown;
  stream?: unknown;
}>;

export type PromptCacheBreakpoint = Readonly<{ mode: "explicit" }>;

export type MessageContentItem = Readonly<
  | { type: "input_text"; text: string; prompt_cache_breakpoint?: PromptCacheBreakpoint }
  | { type: "output_text"; text: string }
  | {
    type: "input_image";
    image_url?: string;
    file_id?: string;
    detail?: "auto" | "low" | "high" | "original" | null;
    prompt_cache_breakpoint?: PromptCacheBreakpoint;
  }
  | {
    type: "input_file";
    file_id?: string;
    file_data?: string;
    file_url?: string;
    filename?: string | null;
    detail?: "auto" | "low" | "high";
    prompt_cache_breakpoint?: PromptCacheBreakpoint;
  }
>;

export type ResponseMessageItem = Readonly<{
  type: "message";
  role: "user" | "assistant" | "developer";
  content: MessageContentItem[];
}>;

// The Responses API can accept additional input item types beyond "message"
// (e.g. tool-calling items like reasoning/function_call/function_call_output).
export type ResponseInputItem = ResponseMessageItem | Readonly<Record<string, unknown> & { type: string }>;
