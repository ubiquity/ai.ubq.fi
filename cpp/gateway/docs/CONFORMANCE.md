# Native gateway functional conformance

Run the packaged ARM64 release and the TypeScript application from the pinned production `development` commit on Bun 1.4.0 against the same loopback mock. The suite shares the request-path benchmark's launch arguments, isolation and DeepSeek fixtures. It uses the native checkout's production-smoke Bun launcher and Python helpers. The launcher serves through the pinned application's `#http` and `ws@8.18.3`. Both runtimes open their own new, codec-marked SQLite state and use ordinary authenticated routes. They restart once for durable readback. [Production pin](../conformance/oracle-revision.txt)

## Run on Linux ARM64

Use a Linux ARM64 host with Landlock ABI 8 or newer, passwordless `sudo`, `ip`, Python, `jq` and Bun 1.4.0. Build the portable archive from a clean candidate checkout. Verify the archive SHA-256 against the build receipt before unpacking and sealing it on this host. [Portable package](RELEASES.md#build-locally)

```sh
scratch=$(mktemp -d /var/tmp/cppgw-conformance.XXXXXXXX)
mkdir "$scratch/bundle"
tar --no-same-owner --no-same-permissions -xzf /absolute/verified/uos-gateway-linux-arm64.tar.gz -C "$scratch/bundle"
python3 -B "$scratch/bundle/seal.py" "$scratch/package" > "$scratch/seal-receipt.json"
bash cpp/gateway/conformance/run.sh "$scratch" "$scratch/seal-receipt.json"
```

The launcher validates the seal and the candidate commit and tree in the package build profile. It uses `oracle.sh --production` to fetch the exact production revision and create a detached worktree in scratch. Bun installs from the application's frozen root lockfile with install scripts disabled. Both sides use the native KV artifact with the seal's independently bound provenance hash, and the production application's KV inputs must match the packaged inputs byte for byte. [Pin update procedure](../conformance/README.md#move-the-production-pin)

Dependency preparation can use the network. The test namespace has only loopback, proves IPv4 and IPv6 route denial and drops back to the invoking uid. Each gateway process gets an empty home, fresh temp and synthetic credentials for the mock routes. Bun launches with `--no-env-file --no-install`. Set `BUN` to the absolute path of the pinned executable when it is outside `PATH`.

## Expected result

The pinned baseline is 186 cases, with 184 passing cases, zero wire differences and two shared expectation
failures. In `chat-invalid-role` and `responses-invalid-tool-type`, both gateways dispatch to the DeepSeek
mock and return HTTP 200. The assertions still require HTTP 400 with zero dispatch, so the known baseline
exits 1. Verify the specific IDs, both outcomes and the absence of any other differences in the receipt.
[Final qualification](findings/cppgw-final-20261119.md) · [Suite gate](../conformance/README.md)

## Cases and evidence

| Surface                        | Exercises                                                                                                                                                                                      |
| ------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Chat Completions and Responses | Buffered and progressive SSE, tool calls and history replay, Codex `none` and `ultra` on the upstream wire, provider 429/503, and error after a delivered delta                                |
| Providers                      | DeepSeek and Codex inference, exact-version Codex catalog revalidation and authentication, DeepSeek waterfall first hop and redirect refusal, and unavailable-provider decisions               |
| Embeddings                     | UOS validation and unconfigured provider responses, then cached float and base64, idempotency and job ownership. The suite imports two synthetic vector rows through the admin HTTP API.       |
| Images                         | JSON generation and editing through Codex Responses, with two returned images and two upstream usages charged to one gateway request                                                           |
| Transport and access           | Authentication and CORS, oversized and malformed bodies, public and unknown routes, health, admin and agent-message access, and Codex Realtime SDP, ownership and WebSocket relay              |
| State                          | API-key creation, rename, revoke, delete and audit, a one-request quota that persists across restart, empty paid-fallback history, and provider request IDs correlated with delivery terminals |

Successful out-of-process Voyage inference and configured Cerebras, Lithos, OpenRouter, Claude Code, Surplus and OpenLux inference require provider routing hooks beyond the packaged mock map. Paid fallback accumulation requires those hooks too. The WebSocket probe observes the upgrade status, headers and the authenticated text round trip.

Cases are in [cases.json](../conformance/cases.json). A case names a method, path, credential alias and optional body or benchmark fixture. The mock mode selects a fixed upstream script. `expect.status` is required. `expect.routes` asserts the dispatch sequence, `expect.values` checks selected JSON Pointers in the observation, and `expect.absent` asserts that fields are omitted. `expect.media_type` selects a media type such as `text/event-stream` or `application/sdp`. Ordinary HTTP responses default to JSON. `capture` saves named response values for later requests. For example, each gateway supplies its own generated key ID to the same revoke request template using `$other_id`. Use `$$` for a literal dollar sign in a string value. A case tagged `log` can require one terminal record and inspect its semantic type, delivered outcome and token usage.

Status, public response bodies and relevant headers are compared for every case. SSE comparisons include named events, JSON payloads in order, sequence numbers and the final event. The progressive probes hold the mock's subsequent frames until the client receives the first delta. The runner records malformed SSE metadata as a wire failure and continues the matrix. The comparator preserves error codes, presence, types and consequential ordering. It maps generated IDs consistently, normalizes explicitly named clocks, and allows two seconds of clock drift for retry/reset headers. Error wording, equivalent header formatting and explicitly unordered list permutations are cosmetic. An expectation can fail on both runtimes even when their wire responses match.

The suite independently checks the Git SHA and deployment ID in HTTP headers and in successful health GET bodies. It also checks the SHA on each synthetic API-key audit row. Native expects the full package SHA and `vps-<full-sha>`. Bun expects the pinned source's unstamped `unknown` values. The comparator accepts a difference on those paths only when both values match their respective expectations. The original observations remain in the receipt.

The command prints the new output directory. `results.json` holds the commit and tree, fixture and binary hashes, both observations, dispatch evidence, expectation failures and field-level differences. `table.md` summarizes the cases. Synthetic credential values are redacted in the receipt. Exit `0` means all functional checks passed, `1` means the suite completed and found a functional failure, and `2` means the comparison runner could not complete. Setup errors and the watchdog can also exit nonzero. [Measured baseline and complete table](findings/cppgw-conformance-e2e-20261118.md)

## Historical Bun fixtures

The historical source stays pinned separately. Prepare it in a new detached worktree outside the
candidate checkout with `bash cpp/gateway/conformance/oracle.sh /absolute/scratch/legacy`, then install
its frozen dependencies with Bun 1.4.0 and the harness's explicit preparation environment. Preserve
the original transport receipt, `#http` and `ws@8.18.3`, and run any comparison inside its real
loopback-only namespace. [Historical pin](../conformance/legacy-oracle-revision.txt) ·
[Standalone smoke procedure](../tests/production_smoke/README.md)
