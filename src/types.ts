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
}>;

export type ApiKeyHashRecord = Readonly<{
  id: string;
  expires_at_ms: number;
  revoked_at_ms: number | null;
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
