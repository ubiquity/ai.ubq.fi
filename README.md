# Ubiquity gateway

The native C++20 gateway lives on `development` and `main`. It serves OpenAI-compatible Chat Completions, Responses, and the gateway's model, auth and operator routes. Linux ARM64 is the serving target. The tree includes C++ tests, native build and packaging scripts, and browser assets served from `static/`. The macOS ARM64 package supports native development and an offline version check. [Native gateway guide](cpp/gateway/README.md)

- [Build and CI](cpp/gateway/docs/CI.md)
- [Release packages](cpp/gateway/docs/RELEASES.md)
- [Runtime and operator guide](cpp/gateway/docs/RUNTIME.md)
- [Arch Linux ARM cutover checklist](cpp/gateway/docs/CUTOVER.md)
- [Paired conformance](cpp/gateway/docs/CONFORMANCE.md)

The final TypeScript gateway snapshots are available at [`typescript-final-development`](https://github.com/ubiquity/ai.ubq.fi/tree/typescript-final-development) and [`typescript-final-main`](https://github.com/ubiquity/ai.ubq.fi/tree/typescript-final-main).

The paired comparison with the TypeScript gateway lives at [commit `ca045a83ebaab6c50c20567d1a825638abac0891`](https://github.com/ubiquity/ai.ubq.fi/tree/ca045a83ebaab6c50c20567d1a825638abac0891/cpp/gateway/conformance).

Run native build, test and packaging commands from the checkout root.

```sh
cd "$(git rev-parse --show-toplevel)"
```

## Output-token caps

`max_completion_tokens` is the OpenAI Chat Completions cap. `max_output_tokens` is the OpenAI Responses cap. Both are positive-integer output caps. Do not swap these fields between endpoints.

| Request and provider                          | Gateway behavior                                                                                                            |
| --------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| Chat Completions to Codex                     | `max_completion_tokens` is translated to the Codex Responses field `max_output_tokens`.                                     |
| Responses to Codex                            | `max_output_tokens` is forwarded as `max_output_tokens`.                                                                    |
| Chat Completions to Cerebras (`gpt-oss-120b`) | `max_completion_tokens` is forwarded unchanged. The gateway buffers the upstream completion before replaying requested SSE. |
| Chat Completions to DeepSeek official         | `max_completion_tokens` is translated to DeepSeek's documented `max_tokens`.                                                |
| Responses to DeepSeek official                | `max_output_tokens` is translated to DeepSeek's documented `max_tokens`.                                                    |
| Paid fallback (Metered or Surplus)            | Chat `max_completion_tokens` arrives as `max_output_tokens`. Responses forwards its own cap.                                |

DeepSeek official uses `https://api.deepseek.com/chat/completions` and returns `deepseek_api_key_missing` when its key is absent. When translating Responses API tool history to Chat Completions, a function call and its output become an assistant turn with `tool_calls` followed by the tool result.
