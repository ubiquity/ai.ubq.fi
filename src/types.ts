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
<<<<<<< Updated upstream
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
=======
>>>>>>> Stashed changes
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
