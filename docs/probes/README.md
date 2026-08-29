# Cerebras Harmony protocol probes (plan m01)

Command:
`deno run --allow-env=CEREBRAS_API_KEY --allow-net=api.cerebras.ai --allow-write=docs/probes scripts/probes/cerebras-harmony-probes.ts`

Without `CEREBRAS_API_KEY` the command prints a skip notice and exits 0 without making any request.

## What it does

Runs the bounded m01 scenario manifest (`src/harmony/probes.ts`) against the exact model `gpt-oss-120b` through the
existing Cerebras transport (`src/cerebras.ts`) and writes one JSONL result file per run into this directory:

`cerebras-harmony-protocol-<utc-timestamp>.jsonl`

Scenarios cover: reasoning return at low/medium/high, reasoning replay after a completed final answer, a deliberate
`reasoning_content` echo (boundary evidence), generic and native Harmony tool call/result shape, native tool-result
replay via tool role and user role, consecutive tool turns, mixed strictness rejection, strictness normalization
(all-false / all-true), structured output alone (json_object, json_schema, native response formats), tools combined with
`response_format`, parallel calls, and the zero-tool bounded bootstrap classifier at low and medium effort.

## Sanitization

Results are sanitized: prompts, private reasoning (`analysis`/`reasoning_content`), tool argument values and API keys
are never recorded. Request summaries carry roles, tool names, strictness values and flags; response summaries carry
status, content/reasoning presence and lengths, tool-call shape, and a 120-character final-content preview.

## Deterministic tests

The same manifest runs offline through scripted fake transports in `tests/harmony-probes.test.ts`; adapter, renderer,
parser, conversation-state and classifier contracts have focused tests in `tests/harmony-*.test.ts`.
