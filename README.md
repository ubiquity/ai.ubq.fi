# Ubiquity gateway

This branch contains the native C++20 gateway for OpenAI-compatible Chat Completions, Responses, and the gateway's model, auth and operator routes. The Linux ARM64 package provides the server and offline KV tools. The macOS ARM64 package supports native development and version checks. [Native gateway guide](cpp/gateway/README.md)

- [Build and CI](cpp/gateway/docs/CI.md)
- [Release packages](cpp/gateway/docs/RELEASES.md)
- [Runtime and operator guide](cpp/gateway/docs/RUNTIME.md)
- [Paired conformance](cpp/gateway/docs/CONFORMANCE.md)

The live service and its deployment configuration use the separately maintained TypeScript production branch. The historical TypeScript and Rust gateways are available at commit `2b23fb6837e2b5c0dc9ffa50771f52d322dd413a`. The paired test fetches that revision into a detached worktree and runs the Bun gateway there. [Legacy source](https://github.com/ubiquity/ai.ubq.fi/tree/2b23fb6837e2b5c0dc9ffa50771f52d322dd413a)

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

## Codex auth upload for the existing service

Run the upload helper only for an approved account change. Check the account identity and credential custody first, because upload can refresh the upstream token and update the service's catalog and account pool. Use a separate legacy checkout and keep the auth file private. [Legacy upload and account guidance](https://github.com/ubiquity/ai.ubq.fi/blob/2b23fb6837e2b5c0dc9ffa50771f52d322dd413a/README.md)

```sh
scratch=$(mktemp -d)
bash cpp/gateway/conformance/oracle.sh "$scratch/legacy"
cd "$scratch/legacy"
cd "$(git rev-parse --show-toplevel)"
test -f bun.lock && test -f tools/lint/bun.lock
bun --no-env-file install --frozen-lockfile --ignore-scripts
bun --no-env-file --no-install run upload:auth --url https://ai.ubq.fi --auth-json ~/.codex/auth.json
```
