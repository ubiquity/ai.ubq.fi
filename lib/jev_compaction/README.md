# Gateway Jev compaction

Bounded, header-marked compaction for `POST /v1/responses`. An explicitly marked Codex compaction request
(`x-codex-turn-metadata` with `request_kind: "compaction"` and `compaction.implementation: "responses"`) is answered
locally: TypeSafe Jev decides which tool calls and tool results still matter, local code renders the retained transcript
verbatim, and the standard Responses assistant-memory response is returned. Ordinary requests keep the normal provider
route and are never parsed or rewritten by this module.

## Provenance

Ported from [`0x4007/fast-jev-compaction`](https://github.com/0x4007/fast-jev-compaction), commit
`c1eab5fdbd6bde7d67f2da070116496558836884` (MIT, see `LICENSE` in this directory):

| File here        | Upstream source                                                                                  |
| ---------------- | ------------------------------------------------------------------------------------------------ |
| `types.ts`       | `src/types.ts`                                                                                   |
| `request.ts`     | `src/request.ts`                                                                                 |
| `client.ts`      | `src/client.ts`                                                                                  |
| `state.ts`       | `src/state.ts`                                                                                   |
| `compact.ts`     | `src/compact.ts`                                                                                 |
| `codex_items.ts` | `codex/codex-items.ts`                                                                           |
| `compaction.ts`  | `codex/jev-compaction-proxy.ts` (Jev call, timeout fetch, response payloads and validation only); gateway adapter remains in `src/jev_compaction/compaction.ts` |

Mechanical adaptations only: Deno `.ts` import specifiers, repository formatting, lazy `Deno.env` credential access
instead of `process.env`, caller abort propagation into the Jev call, parsed-body input instead of a raw string, and a
trimmed export set. The selection algorithm, renderer, validation rules, batching limits (`maxStateTokens` 25,000,
`maxRequestTokens` 30,000), per-request 30-second Jev bound and the 400,000-character summary cap are unchanged.

The upstream HTTP server, CLI proxy, standalone service and root `package.json` were deliberately not copied; the
gateway route in `src/handler.ts` calls the adapter in `src/jev_compaction/`. The two prototype acceptance harnesses in
this directory (`jev-compaction-smoke.ts` and `jev-codex-smoke.ts`) use the real gateway handler and either an injected
or the real Jev asker; they are vendored with this prototype rather than product scripts.
