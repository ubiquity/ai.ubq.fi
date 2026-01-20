export type CodexAuthState = Readonly<{
  access_token: string;
  refresh_token: string;
  account_id: string;
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
}>;

export type ApiKeyHashRecord = Readonly<{
  id: string;
  expires_at_ms: number;
  revoked_at_ms: number | null;
  usage_limit_requests: number;
  usage_requests: number;
  usage_reset_at_ms: number;
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
  last_route: string | null;
}>;

export type ApiKeyUsageDay = Readonly<{
  day: string;
  request_count: number;
}>;

export type ApiKeyUsageDailyRecord = Readonly<{
  key_id: string;
  days: ApiKeyUsageDay[];
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
  created_at_ms: number;
  updated_at_ms: number;
}>;

export type ChatCompletionRequest = Readonly<{
  model?: unknown;
  messages?: unknown;
  reasoning_effort?: unknown;
  stream?: unknown;
}>;

export type ResponsesRequest = Readonly<{
  model?: unknown;
  input?: unknown;
  reasoning?: unknown;
  stream?: unknown;
}>;

export type MessageContentItem = Readonly<
  | { type: "input_text"; text: string }
  | { type: "output_text"; text: string }
  | { type: "input_image"; image_url: string }
>;

export type ResponseMessageItem = Readonly<{
  type: "message";
  role: "user" | "assistant" | "developer";
  content: MessageContentItem[];
}>;
