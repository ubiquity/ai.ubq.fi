# Gateway decisions

## 2026-11-25 — Concurrent inference streams

Each gateway process admits up to 512 active inference requests so a burst of 500 coding agents can each hold a stream. A further 128 requests can wait up to five seconds for a permit. The gateway returns HTTP 503 before contacting a provider when that queue fills or a wait expires.

## 2026-11-26 — Gemini subscription models and tools

The Antigravity subscription adapter advertises Gemini 4 Neon low, medium, high and xhigh tiers through the normal, public and exact-version Codex catalogs when the operator opt-in, CLI installation, sign-in state and provider selection are valid. Declared Codex tools use a bounded JSON tool-call prompt protocol, validate the returned tool name against the request, emit typed Responses and Chat Completions calls, and replay `function_call_output` in the follow-up prompt.
