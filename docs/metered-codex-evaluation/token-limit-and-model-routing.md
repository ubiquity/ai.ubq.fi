# Token-limit and model-routing diagnostics

Date measured: 2026-07-19

## Question

Metered frequently returned more output tokens than direct Codex and `ai.ubq.fi`. Two explanations were considered:

1. metered ignored the requested output limit; or
2. metered served a different model behind the `gpt-5.6-sol` name.

## Documentation findings

The exported metered documentation states that `/v1/chat/completions` accepts `max_tokens` and describes it as the
maximum generated token count. However, that section is written for Metered's GPTs-compatible route and does not
document model-specific behavior for `gpt-5.6-sol`.

The same export contains:

- `max_tokens` for Chat Completions;
- `max_completion_tokens` only in the separate MiniMax `/v1/messages` section;
- a Responses example whose returned object includes `max_output_tokens: null`, without documenting that field in the
  request parameter table.

Source: [Metered API documentation](https://metered.apifox.cn/)

## Controlled 32-token tests

All three metered requests used the same deliberately long prompt:

```text
Write a detailed explanation of stable deduplication algorithms in at least
250 words. Include implementation considerations, complexity, edge cases, and
examples. Do not stop early.
```

The prompt was reported as 51 input tokens in every response.

| Endpoint and field              | Requested cap | HTTP | Output tokens | Reasoning tokens | Visible chars | Completion state |  Latency |
| ------------------------------- | ------------: | ---: | ------------: | ---------------: | ------------: | ---------------- | -------: |
| Chat + `max_tokens`             |            32 |  200 |         1,142 |               76 |         5,236 | `stop`           | 24.101 s |
| Chat + `max_completion_tokens`  |            32 |  200 |           983 |               51 |         4,035 | `stop`           | 19.274 s |
| Responses + `max_output_tokens` |            32 |  200 |           708 |               57 |         3,106 | `completed`      | 14.864 s |

Every tested limit was ignored:

- output exceeded 32 tokens by a large margin;
- Chat Completions returned `finish_reason: "stop"`, not `"length"`;
- Responses returned `completed` with no `incomplete_details`.

## Direct OpenAI control

The same `gpt-5.6-sol` Responses request was sent directly to the ChatGPT Codex subscription backend.

With `max_output_tokens: 32`, direct OpenAI returned:

```text
HTTP 400
Unsupported parameter: max_output_tokens
```

The rejection arrived in 357 ms.

The valid request was then repeated without that field:

| Measure            | Direct Codex result |
| ------------------ | ------------------: |
| HTTP/status        |   200 / `completed` |
| Input tokens       |                  51 |
| Output tokens      |               1,276 |
| Reasoning tokens   |                  45 |
| Visible characters |               5,729 |
| Latency            |            32.551 s |
| Model              |       `gpt-5.6-sol` |

## Conclusion about the token limit

Metered's proxy almost certainly strips or fails to forward output-limit fields on its Codex route:

1. the public-facing compatibility layer accepts each field;
2. direct Codex rejects `max_output_tokens` as unsupported;
3. metered returns HTTP 200 instead of propagating that rejection; and
4. metered generates an ordinary uncapped completion.

This also explains the earlier `max_tokens: 300` benchmark responses that reported more than 300 completion tokens.

`ai.ubq.fi` behaves similarly by design: its Chat Completions translation currently treats `max_tokens` as ignored and
does not forward an output cap to the Codex Responses backend.

## Conclusion about model identity

The longer metered outputs are not evidence of a substituted model. In the control, direct OpenAI generated even more
output than Metered.

Signals consistent with the real Codex backend include:

- exactly matching input-token counts;
- the same reported model identifier;
- comparable reasoning-token accounting;
- comparable visible-character-to-output-token ratios;
- a similar uncapped output-length distribution; and
- behavior consistent with sanitizing a parameter unsupported by the direct Codex subscription endpoint.

These observations strongly support the hypothesis that metered forwards to real Codex capacity, probably through pooled
accounts or another intermediary. They do not cryptographically prove the upstream model. A black-box proxy can always
rewrite identifiers, metadata, and usage fields.

## Implication for clients

Do not rely on `max_tokens`, `max_completion_tokens`, or `max_output_tokens` to bound `gpt-5.6-sol` output through the
tested metered route.

Use prompt-level constraints where possible, monitor actual usage, and apply a client-side streaming cutoff only if the
route supports streaming cancellation without charging for the remainder. That cancellation behavior was not tested.

## Fast-mode and service-tier diagnostic

Date measured: 2026-07-21

OpenAI's user-facing Codex `/fast` mode uses more ChatGPT credits in exchange for higher inference speed. It is distinct
from Metered's “price priority” router setting, which chooses among metered groups by cost.

Three small, deterministic metered requests were used to test the wire-level tier field:

| Requested tier | HTTP | Returned tier | Input | Output | Reasoning | Latency |
| -------------- | ---: | ------------- | ----: | -----: | --------: | ------: |
| omitted        |  200 | `default`     |    33 |     13 |         0 | 2.124 s |
| `fast`         |  200 | `default`     |    33 |     13 |         0 | 2.706 s |
| `priority`     |  200 | `default`     |    34 |     15 |         0 | 1.734 s |

The omitted-tier and `fast` requests used exactly the same prompt and produced exactly the requested literal output. A
single latency sample is not a speed benchmark, but the response metadata establishes that metered did not preserve
either requested non-default tier.

Controls against the direct ChatGPT Codex backend showed:

- raw `service_tier: "fast"` was rejected with HTTP 400 as unsupported; and
- raw `service_tier: "priority"` completed, but the returned response still reported `service_tier: "default"`.

The current Codex client treats Fast as a product feature and maps its configuration to supported upstream behavior;
passing the literal word `fast` to the backend is not equivalent to activating `/fast` in the client.

Metered's live `gpt-5.6-sol` catalog advertised `-low`, `-medium`, `-high`, `-xhigh`, `-max`, and `-ultra` reasoning
suffixes. It did not advertise a `-fast` suffix or a Fast-specific price. No Fast behavior is documented in the exported
Metered documentation either.

Conclusion: the tested metered route silently downgrades or strips Fast/Priority tier requests. There is no observed
2.5x Fast surcharge because there is no observed Fast service. “Price priority” should not be conflated with Codex
`/fast`.
