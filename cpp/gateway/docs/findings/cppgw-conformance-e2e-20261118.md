# Native and Bun gateway functional conformance

## Functional findings

The Linux ARM64 run completed **170 cases per gateway**. **168 passed**, with **two shared validation failures** and **zero native-versus-Bun functional differences**. Each shared failure received HTTP 200 from both gateways against an independent HTTP 400 expectation. The local mock recorded one dispatch per runtime in each case.

1. **`chat-invalid-role`** sends `POST /v1/chat/completions` with `model: "deepseek-flash"` and `messages: [{"role":"operator","content":"hi"}]`. Both gateways forwarded that exact message to DeepSeek and returned a successful Chat completion. The published `ChatMessage` enum lists `system`, `developer`, `user`, `assistant` and `tool`. The provider projection rewrites `developer` and otherwise passes messages through. Validate the nested role on the Chat route before the provider dispatch boundary. [Published schema](../../../../static/openapi.json#L679) · [TypeScript projection](../../../../src/deepseek/index.ts#L326) · [Native projection](../../src/deepseek/index.cpp#L597)
2. **`responses-invalid-tool-type`** sends `POST /v1/responses` with `model: "deepseek-flash"`, a text input and `tools: [{"type":"bad"}]`. Both gateways removed `tools` from the upstream JSON and returned a completed text response. The published Responses request restricts its tool items to the documented tool schema. `collectTool` currently takes the same successful omission branch for an unknown type and a recognized tool unavailable on DeepSeek. Validate the wire type before provider-specific omission and retain the explicit handling for supported compatibility extensions. [Published schema](../../../../static/openapi.json#L747) · [TypeScript collection](../../../../src/deepseek/chat-projection.ts#L696) · [Native collection](../../src/deepseek/chat_projection.cpp#L468)

## Cosmetic findings

Zero cosmetic differences remained after normalization. Discovery had reported differences in live clocks, the ISO timestamp in quota-refusal text, and buffered response/item IDs. Those values matched the documented volatility rules. The comparator controls retain a failing result for changed error codes, data types, quota counters, lost headers, and changed SSE sequences.

## Measured verification

The verified application source was `b54357a92c0f2669335d83f04047c3f7de32f925`. The case matrix and runner were `3ce48d8559c131e87c9cdcf203ffb2599eac3d04`, tree `ed7b92dc129bb5d74b7cf98727cc2b1fcac2967d`. The benchmark files came from `6878261fd50c7df210308879583996bc88311724`.

The runner used Ubuntu 24.04.5 ARM64, Linux `7.0.0-1017-azure`, GCC 14.2.0 `-O2`, Boost 1.92.0, SQLite 3.54.0 with `SQLITE_DQS=3`, ICU 78.3, OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0, Zstd 1.5.5 and Bun 1.4.0. The early four-job release build compiled 369 production translation units and two vendored libraries, then linked the gateway and both offline entrypoints. It took 616.96 s with 1,301,028 KiB maximum reported child RSS. The package used the final SQLite SONAME relink, confirmed with `ldd`.

The source-gate run took 3.209 s and recorded 340 HTTP or upgrade responses, 54 upstream dispatches, eight progressive delivery proofs across four cases, and twelve correlated terminal records. Each gateway exited cleanly before and after its restart. Both outbound route probes returned `ENETUNREACH`. The 1,048,576-byte Chat fixture completed. The 33,554,433-byte Chat upload returned 400, and the oversized passkey login and Realtime uploads returned 413. The one-request key stayed at one settled dispatch across the quota rejection and restart. The cached embeddings cases returned two ordered 256-dimensional vectors, exact little-endian float32 base64, replay and conflict results, and the expected job readback and cross-principal refusal.

An independent depth-one checkout at `3ce48d8559c131e87c9cdcf203ffb2599eac3d04` fetched the pinned benchmark, installed frozen dependencies, built and verified the native artifacts, linked all three native executables and packaged 62 files. The documented command took 812.28 s end to end with 1,301,188 KiB maximum reported child RSS. The conformance phase took 3.262 s and produced a byte-identical table. Both gateways again returned all 340 case responses, with 54 mock dispatches and four clean process exits. The wrapper returned 1 for the two shared validation failures. The cold checkout remained clean.

| Source-gate evidence       | SHA-256                                                            |
| -------------------------- | ------------------------------------------------------------------ |
| Native gateway             | `ff0fcab314cf695be70ace1af284cae4341d45dbc05adcbcdf9738e7c4f5cb07` |
| Native package manifest    | `e7ccca0465824bc3ebf97f07a37acbdaef7ec62c5713cd207119ca20206ed01e` |
| Bun executable             | `086c4121c8738a8e0f5ed730e8a461bc3973b4444e372ddb77aef9a747fa2ae9` |
| `source-gate/results.json` | `493205ec4ba718bbf5638cb52139f4b0d3b5cd9fae31376b8da526dcd1f160e7` |
| `source-gate/table.md`     | `58f3b039600461ae0dcba0c6757c29f5bdbf4d4d253565c4b341513717918c85` |

| Cold-run evidence                            | SHA-256                                                            |
| -------------------------------------------- | ------------------------------------------------------------------ |
| Native gateway                               | `b716c36ebe0a42c5dfb29d1dcffe09a84152c49b68c8d3c64ee433e0a14bd3db` |
| Native package manifest                      | `b2c5eeec046897d498624dc7e678da777dc14ca73b4f1c13533012e06de281e5` |
| `cold-work/run.cdoRVjNB/output/results.json` | `515828ae6cbd242dd19e51045a1a95c19707df76062462f58945810679e4266d` |

The changed paths consisted of the Python/shell harness, declarative JSON and NDJSON fixtures, guide and findings. Verification selected the paired suite, eight comparator controls, shell lint, scoped Prettier and whitespace checks. The portable CI inventory listed 442 native tests.

### Commands

The dependency preparation commands and verified release packaging prerequisites are in [CONFORMANCE.md](../CONFORMANCE.md). The source-gate commands below ran from the repository root. Runner scratch and the process logs remained outside the checkout.

```bash
scratch=/var/tmp/cppgw-conformance.5gm9sr
bench="$scratch/bench/cpp/gateway/bench/request_path"
git archive 6878261fd50c7df210308879583996bc88311724 cpp/gateway/bench/request_path | tar -x -C "$scratch/bench"
git archive b54357a92c0f2669335d83f04047c3f7de32f925 | tar -x -C "$scratch/oracle"

LIBRARY_PATH=/usr/local/lib CPLUS_INCLUDE_PATH="$scratch/icu/source/common:$scratch/icu/source/i18n" PATH=/usr/bin:/bin \
  timeout 2400 python3 -I "$bench/build.py" --source "$scratch/oracle" --out "$scratch/build" \
    --boost "$scratch/boost_1_92_0" --cxx /usr/bin/g++-14 --jobs 4

mkdir -p "$scratch/isolation"
cp "$bench/isolated.sh" "$scratch/isolation/isolated.sh"
ln -s "$PWD/cpp/gateway/conformance/run.py" "$scratch/isolation/run.py"
timeout -k 10s 180s bash "$scratch/isolation/isolated.sh" --bench "$bench" \
  --source "$scratch/oracle" --source-revision b54357a92c0f2669335d83f04047c3f7de32f925 \
  --suite-revision 3ce48d8559c131e87c9cdcf203ffb2599eac3d04 --suite-tree ed7b92dc129bb5d74b7cf98727cc2b1fcac2967d \
  --benchmark-revision 6878261fd50c7df210308879583996bc88311724 --out "$scratch/source-gate" --bun "$(command -v bun)" \
  --binding "$scratch/oracle/.data/native-kv.binding.json" --package "$scratch/oracle/.data/conformance-package"

python3 -B cpp/gateway/conformance/compare_test.py
shellcheck cpp/gateway/conformance/run.sh
bash -n cpp/gateway/conformance/run.sh
bash cpp/gateway/tools/ci-build.sh --list
git diff --check b54357a92c0f2669335d83f04047c3f7de32f925 HEAD
```

The cold checkout used the public benchmark fallback and the documented default application snapshot.

```bash
git clone --no-tags --depth=1 "file://$PWD" "$scratch/cold-checkout"
(
  cd "$scratch/cold-checkout"
  CPP_CONFORMANCE_BOOST="$scratch/boost_1_92_0" \
    /usr/bin/time -o "$scratch/cold-run.time" -f 'wall_s=%e peak_process_kib=%M exit=%x' \
    bash cpp/gateway/conformance/run.sh "$scratch/cold-work"
)
```

The three changed JSON and Markdown paths passed the repository formatter in an archived checkout with frozen root and lint dependencies.

```sh
bun --no-env-file run format:check cpp/gateway/conformance/cases.json cpp/gateway/docs/CONFORMANCE.md \
  cpp/gateway/docs/findings/cppgw-conformance-e2e-20261118.md
```

### External findings and coverage limits

- The benchmark recipe's isolated invocation of its package wrapper raised `ModuleNotFoundError: vendor_sources` against the supplied source. The conformance wrapper passes the archived application's `cpp/gateway/tools` as the explicit preparation-only `PYTHONPATH`. The production packager also requires SQLite's `libsqlite3.so.0` SONAME. The guide gives the Ubuntu build and loader setup for SQLite 3.54.0. [Benchmark and runtime recipe](../CONFORMANCE.md#run-on-linux-arm64)
- Out-of-process Voyage inference, configured Cerebras/Lithos/OpenRouter/Claude Code/Surplus/OpenLux inference and paid fallback accumulation need additional fixture routes. WebSocket coverage includes handshake decisions and the text relay. HTTP bodies on refused WebSocket handshakes are outside the smoke client's capture.
- The runner's GitHub write credentials are unavailable. `GIT_TERMINAL_PROMPT=0 git push --dry-run origin HEAD:refs/heads/agent-jobs/cppgw-conformance-e2e-20261118` returned `could not read Username`. The local clean `agent-job` branch carries the bounded commits for the integration owner. The fetched `origin/development` is `6bd78fc5e3b0783809691c6bd45d25ac45e9696e`.

## Work contract v1

Exercise the two real gateway entrypoints over loopback using declarative requests and a shared synthetic upstream. Compare HTTP status, client-visible wire data and headers, stream event order and terminals, and durable accounting and key state. Record functional differences before cosmetic differences, with request and response evidence. Use Bun 1.4.0 and the native release build of base `b54357a92c0f2669335d83f04047c3f7de32f925` on Linux ARM64.

| Surface                   | Measurement                                                                                                                   |
| ------------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| DeepSeek                  | Existing benchmark Chat and Responses fixture, with buffered and streamed cases                                               |
| Codex                     | Existing fixture transport and smoke provider, with Models, Responses and Realtime coverage subject to mock dispatch evidence |
| Other providers           | Discover local routing hooks and cover availability or validation when the packaged fixture cannot dispatch                   |
| Access and administration | Authentication decisions, validation, CORS and health, admin reads and synthetic API-key state                                |
| Settlement                | Usage and accounting readback after delivery and restart, with rate and quota refusals                                        |

Own `cpp/gateway/conformance/`, `cpp/gateway/docs/CONFORMANCE.md`, and this report. Gateway fixes, benchmark edits, workflow changes, production state and real provider access are outside this run. The supplied base contains the production-smoke harness. The requested request-path benchmark lives on the port branch, so use its pinned files in scratch and keep the baseline source and other checkout paths intact.

The runner must prove isolated loopback and use fresh private state, home and temp directories with an allowlisted environment. Verify the selected native release build, run every case against both gateways and retain the JSON receipt and full result table, then run changed-file formatting and `git diff --check`. The launcher and provider discovery will determine which successful routes can be observed. Stop after complete local evidence and bounded branch handoff, with any external delivery blocker recorded.

## Work contract v2

The fixture map supports DeepSeek and Codex, including the Codex catalog and Realtime. Add cached Voyage response and job cases via two synthetic vector rows imported through the authenticated admin HTTP route. This exercises output, idempotency and principal ownership in the real durable store. Retain the unconfigured Voyage cases in the first phase, and enable a synthetic cache-only credential after restart. Required checks include exact wire status and media type, matching gateway request and accounting evidence, dispatch provenance, header and stream expectations, and the independently specified validation decisions. Discovery results before this extension are diagnostic.

## Case results

<!-- prettier-ignore -->
| Case | Bun | Native | Result |
| --- | ---: | ---: | --- |
| health-get | 200 | 200 | Match |
| health-head | 200 | 200 | Match |
| unknown-public-route | 404 | 404 | Match |
| unknown-v1-no-auth | 401 | 401 | Match |
| unknown-v1-authenticated | 404 | 404 | Match |
| openapi-document | 200 | 200 | Match |
| home-head | 200 | 200 | Match |
| options-untrusted | 204 | 204 | Match |
| options-trusted | 204 | 204 | Match |
| options-unknown-route | 204 | 204 | Match |
| cors-trusted-query | 200 | 200 | Match |
| cors-origin-priority | 200 | 200 | Match |
| admin-no-auth | 401 | 401 | Match |
| admin-client-token-denied | 401 | 401 | Match |
| health-providers-no-auth | 401 | 401 | Match |
| health-upstream-client-denied | 401 | 401 | Match |
| auth-missing | 401 | 401 | Match |
| auth-unknown | 401 | 401 | Match |
| passkey-session-empty | 401 | 401 | Match |
| passkey-login-malformed | 400 | 400 | Match |
| passkey-login-body-too-large | 413 | 413 | Match |
| passkey-register-admin-required | 401 | 401 | Match |
| admin-keys-before-create | 200 | 200 | Match |
| key-create-primary | 200 | 200 | Match |
| key-create-other | 200 | 200 | Match |
| key-create-quota | 200 | 200 | Match |
| key-duplicate | 409 | 409 | Match |
| key-invalid-token | 400 | 400 | Match |
| key-no-name | 400 | 400 | Match |
| auth-primary | 200 | 200 | Match |
| auth-operator | 200 | 200 | Match |
| auth-reader | 200 | 200 | Match |
| keys-list-with-usage | 200 | 200 | Match |
| keys-list-omit-usage | 200 | 200 | Match |
| paid-fallback-empty | 200 | 200 | Match |
| paid-fallback-bad-limit | 400 | 400 | Match |
| keys-audit-seed | 200 | 200 | Match |
| read-defaults | 200 | 200 | Match |
| read-passkey-users | 200 | 200 | Match |
| read-codex-catalog | 200 | 200 | Match |
| read-models-whitelist | 200 | 200 | Match |
| read-models-catalog | 200 | 200 | Match |
| read-provider-selection | 200 | 200 | Match |
| read-banked-resets | 200 | 200 | Match |
| read-codex-overage | 200 | 200 | Match |
| read-kernel-policy-queue | 200 | 200 | Match |
| read-kernel-pubkeys | 200 | 200 | Match |
| read-claude-access | 200 | 200 | Match |
| read-claude-auth-state | 200 | 200 | Match |
| read-debug-routing | 200 | 200 | Match |
| read-health-providers | 200 | 200 | Match |
| read-providers | 200 | 200 | Match |
| health-upstream-loopback | 200 | 200 | Match |
| models-no-auth | 401 | 401 | Match |
| models-unknown-token | 401 | 401 | Match |
| models-openai | 200 | 200 | Match |
| models-codex-versioned | 200 | 200 | Match |
| models-codex-repeat-version | 200 | 200 | Match |
| models-codex-new-version | 200 | 200 | Match |
| models-codex-invalid-version | 400 | 400 | Match |
| models-public-catalog | 200 | 200 | Match |
| models-capabilities-no-auth | 401 | 401 | Match |
| models-capabilities | 200 | 200 | Match |
| chat-no-auth | 401 | 401 | Match |
| chat-unknown-api-key | 401 | 401 | Match |
| chat-wrong-auth-scheme | 401 | 401 | Match |
| chat-invalid-json | 400 | 400 | Match |
| chat-empty-body | 400 | 400 | Match |
| chat-wrong-body-type | 400 | 400 | Match |
| chat-default-model | 200 | 200 | Match |
| codex-chat-stream | 200 | 200 | Match |
| chat-missing-messages | 400 | 400 | Match |
| chat-invalid-role | 200 | 200 | FUNCTIONAL (both) `/status` expected 400 and observed 200<br>FUNCTIONAL (both) `/routes` expected [] and observed ["deepseek.success"] |
| chat-invalid-stream-type | 400 | 400 | Match |
| chat-invalid-max-tokens | 400 | 400 | Match |
| chat-unknown-field | 400 | 400 | Match |
| chat-body-too-large | 400 | 400 | Match |
| chat-buffered | 200 | 200 | Match |
| chat-large-buffered | 200 | 200 | Match |
| chat-stream | 200 | 200 | Match |
| chat-stream-with-usage | 200 | 200 | Match |
| chat-upstream-unavailable | 503 | 503 | Match |
| chat-upstream-rate-limit | 429 | 429 | Match |
| responses-no-auth | 401 | 401 | Match |
| responses-invalid-json | 400 | 400 | Match |
| responses-missing-input | 400 | 400 | Match |
| responses-wrong-input-type | 400 | 400 | Match |
| responses-invalid-stream-type | 400 | 400 | Match |
| responses-invalid-max-output | 400 | 400 | Match |
| responses-unknown-field | 400 | 400 | Match |
| responses-unsupported-reasoning-type | 400 | 400 | Match |
| responses-invalid-tool-type | 200 | 200 | FUNCTIONAL (both) `/status` expected 400 and observed 200<br>FUNCTIONAL (both) `/routes` expected [] and observed ["deepseek.success"] |
| responses-buffered | 200 | 200 | Match |
| deepseek-waterfall-first-hop | 200 | 200 | Match |
| responses-stream | 200 | 200 | Match |
| responses-tool-buffered | 200 | 200 | Match |
| responses-tool-stream | 200 | 200 | Match |
| responses-error-after-delta | 200 | 200 | Match |
| responses-error-usage-readback | 200 | 200 | Match |
| codex-responses-buffered | 200 | 200 | Match |
| codex-responses-stream | 200 | 200 | Match |
| codex-responses-tool | 200 | 200 | Match |
| codex-error-after-delta | 200 | 200 | Match |
| cerebras-unconfigured | 503 | 503 | Match |
| lithos-unconfigured | 503 | 503 | Match |
| openrouter-unconfigured | 404 | 404 | Match |
| unknown-model | 404 | 404 | Match |
| images-upstream-without-image | 502 | 502 | Match |
| decisions-invalid-body | 400 | 400 | Match |
| systemone-invalid-body | 400 | 400 | Match |
| embeddings-openai-route-absent | 404 | 404 | Match |
| embeddings-auth-required | 401 | 401 | Match |
| embeddings-invalid-json | 400 | 400 | Match |
| embeddings-invalid-model | 400 | 400 | Match |
| embeddings-empty-input | 400 | 400 | Match |
| embeddings-token-array | 400 | 400 | Match |
| embeddings-invalid-dimension | 400 | 400 | Match |
| embeddings-invalid-encoding | 400 | 400 | Match |
| embeddings-extra-field | 400 | 400 | Match |
| embeddings-voyage-unconfigured | 503 | 503 | Match |
| embeddings-base64-unconfigured | 503 | 503 | Match |
| embedding-job-no-input-type | 400 | 400 | Match |
| embedding-job-no-provider | 503 | 503 | Match |
| embedding-job-unknown | 404 | 404 | Match |
| live-auth-required | 401 | 401 | Match |
| live-not-multipart | 400 | 400 | Match |
| live-body-too-large | 413 | 413 | Match |
| live-multipart-missing-sdp | 400 | 400 | Match |
| websocket-unknown-route | 404 | 404 | Match |
| websocket-no-auth | 401 | 401 | Match |
| websocket-no-call | 404 | 404 | Match |
| live-create | 201 | 201 | Match |
| live-wrong-principal | 403 | 403 | Match |
| live-requires-upgrade | 426 | 426 | Match |
| websocket-relay | 101 | 101 | Match |
| quota-readback-empty | 200 | 200 | Match |
| quota-first-request | 200 | 200 | Match |
| quota-readback-one-settlement | 200 | 200 | Match |
| quota-rejects-second-dispatch | 429 | 429 | Match |
| quota-readback-after-rejection | 200 | 200 | Match |
| quota-paid-accounting-empty | 200 | 200 | Match |
| key-rename | 200 | 200 | Match |
| key-read-renamed | 200 | 200 | Match |
| key-delete-active-denied | 400 | 400 | Match |
| key-revoke | 200 | 200 | Match |
| key-revoked-inference-denied | 401 | 401 | Match |
| key-unrevoke | 200 | 200 | Match |
| key-unrevoked-access-restored | 200 | 200 | Match |
| key-revoke-again | 200 | 200 | Match |
| keys-and-usage-before-restart | 200 | 200 | Match |
| keys-audit-before-restart | 200 | 200 | Match |
| restart-health | 200 | 200 | Match |
| restart-keys-and-usage | 200 | 200 | Match |
| restart-quota-count | 200 | 200 | Match |
| restart-quota-rejection | 429 | 429 | Match |
| restart-key-still-revoked | 401 | 401 | Match |
| restart-key-delete-revoked | 200 | 200 | Match |
| restart-deleted-key-denied | 401 | 401 | Match |
| restart-deleted-key-not-listed | 200 | 200 | Match |
| restart-paid-accounting-empty | 200 | 200 | Match |
| restart-audit | 200 | 200 | Match |
| embeddings-cache-dry-run | 200 | 200 | Match |
| embeddings-cache-seed | 200 | 200 | Match |
| embeddings-cached-float | 200 | 200 | Match |
| embeddings-cached-idempotent-replay | 200 | 200 | Match |
| embeddings-idempotent-conflict | 409 | 409 | Match |
| embeddings-cached-base64 | 200 | 200 | Match |
| embedding-job-cached | 200 | 200 | Match |
| embedding-job-cached-readback | 200 | 200 | Match |
| embedding-job-other-principal | 404 | 404 | Match |

## Continuation release check

The new Ubuntu ARM64 runner built the inherited clean commit `b12de8a8e474d21525ba2eecef71779c1ecbb579`, tree `7a4075459065b86bed28aad4afabf033a195ba98`. The documented `bash cpp/gateway/conformance/run.sh "$scratch"` completed 380 Ninja steps, linked all three native executables, and packaged 62 files. From prepared host dependencies it took 724.49 s and reported 1,300,956 KiB maximum child RSS. The paired phase took 2.849 s and reproduced the 170-row table byte for byte. It exited 1 for the two shared validation failures. The native gateway hash was `93e8f0751d025583d0acab1aa49150aee51c5b045cc81082b2adb3410746568f` and the package manifest hash was `0d209845be36b919f26b2272386668dc83110f5dcc52f4e5cbccd74e45486ca1`.

## Work contract v3

Extend the measured matrix over the same isolated production entrypoints. The pinned launcher maps DeepSeek Chat, Codex Models, Codex Responses and Realtime to the local mock. The Images routes already translate to Codex Responses, so the mock can exercise successful image delivery and a single quota charge across two upstream calls. Cover conditional catalog reads and their authentication boundary, tool history replay, Codex reasoning translation, request ID correlation, redirect refusal, and Realtime principal ownership. Exercise the agent-message route with the available API-key and anonymous credentials.

The owned paths are the suite, its guide and this report. Keep the existing cases and network boundary. Record gateway and fixture-boundary findings with their exact responses for the respective maintainers. The source snapshot above supplies the release for the expanded run. Validate with the paired `run.sh` command against that snapshot, the Python comparison and wire controls, scoped formatting and whitespace checks. The earlier results apply to the inherited matrix. Finish when the expanded run records both observations for every case, with local receipts and the Git delivery state recorded here.

## Expanded results

Bun and native agreed across all **186 cases per gateway**. **184 passed** the independent expectations. The two shared [functional findings](#functional-findings) reproduced with HTTP 200 and one DeepSeek dispatch per gateway in each case. There were zero cosmetic differences.

The 170 inherited rows above are byte-identical to their entries in the expanded table. These are the 16 additional rows.

<!-- prettier-ignore -->
| Case | Bun | Native | Result |
| --- | ---: | ---: | --- |
| agent-messages-no-auth | 401 | 401 | Match |
| agent-messages-api-key-read-refused | 403 | 403 | Match |
| agent-messages-api-key-write-refused | 403 | 403 | Match |
| models-codex-etag-hit | 304 | 304 | Match |
| models-codex-etag-miss | 200 | 200 | Match |
| models-codex-etag-requires-auth | 401 | 401 | Match |
| chat-upstream-redirect-refused | 502 | 502 | Match |
| responses-tool-output-replayed | 200 | 200 | Match |
| codex-reasoning-none-wire | 200 | 200 | Match |
| codex-reasoning-ultra-wire | 200 | 200 | Match |
| websocket-wrong-principal | 403 | 403 | Match |
| key-create-image-fanout-quota | 200 | 200 | Match |
| images-generations-fanout | 200 | 200 | Match |
| images-fanout-quota-readback | 200 | 200 | Match |
| images-fanout-quota-rejects-second-admission | 429 | 429 | Match |
| images-edits-json | 200 | 200 | Match |

Both gateways returned a bodyless 304 with the captured ETag, a 200 for an unrelated ETag, and a 401 when the matching request lacked authentication. The DeepSeek redirect reached only the original mock route. Codex received explicit `none` and translated `ultra` to `max`. The new Realtime check refused the other API key before any mock WebSocket dispatch.

Each image fan-out produced two PNG results and two Codex dispatches. The gateway wrote one delivered `response.completed` terminal with 22 input, 10 output and 32 total tokens. The API-key readback reported one request, and a second admission returned 429 without upstream dispatch. The Chat SSE case also correlated `synthetic-provider-request` between the client response header and its one delivered terminal.

## Expanded run evidence

The selected suite commit was `78b5db8d504736f5362ba40f1dda66b86d1700b6`, tree `5c565bce9d0d3ec299bc0fc4d106b6e0f0735732`. It reused the release and application snapshot from the continuation release check. Ninja reported `no work to do`. The wrapper took 3.75 s with 137,832 KiB maximum child RSS. Its paired phase took 3.087 s, with 372 HTTP or upgrade responses, 68 mock dispatches, eight progressive delivery proofs, 16 correlated terminals, and four successful process exits. The receipt recorded zero fixture or capture errors and `ENETUNREACH` on both outbound route probes. Exit 1 is the expected receipt status for the two shared failures.

| Output                             | SHA-256                                                            |
| ---------------------------------- | ------------------------------------------------------------------ |
| `run.6sgRDWQv/output/results.json` | `47e2bca287301e5bbd357d194c7b374dc57198ccccd5ec11940dd112638ca023` |
| `run.6sgRDWQv/output/table.md`     | `2bea47a8a0cf89edca4f79f7da9b007578c6d65da0bc1e3028399fec1918de8e` |

The clean release check used the dependency commands in [CONFORMANCE.md](../CONFORMANCE.md#run-on-linux-arm64). The expanded run used the paths and commands below. All nine comparison and wire controls passed, including four malformed SSE subcases. Python syntax, shell lint, shell syntax, scoped formatting and whitespace checks passed.

```bash
scratch=/var/tmp/cppgw-conformance.GsWnqbSJ
CPP_CONFORMANCE_SOURCE_REVISION=b12de8a8e474d21525ba2eecef71779c1ecbb579 \
  /usr/bin/time -o "$scratch/continuation-final.time" -f 'wall_s=%e peak_process_kib=%M exit=%x' \
  bash cpp/gateway/conformance/run.sh "$scratch"

unit=$(mktemp -d "$scratch/unit.XXXXXXXX")
mkdir -m 700 "$unit"/{home,tmp}
env -i HOME="$unit/home" TMPDIR="$unit/tmp" PATH=/usr/bin:/bin LC_ALL=C.UTF-8 \
  python3 -B cpp/gateway/conformance/compare_test.py
shellcheck cpp/gateway/conformance/run.sh
bash -n cpp/gateway/conformance/run.sh
git diff --check b54357a9..HEAD
```

The frozen formatter ran in the scratch copy with the changed files at their repository paths.

```sh
bun --no-env-file run format:check cpp/gateway/conformance/cases.json cpp/gateway/docs/CONFORMANCE.md \
  cpp/gateway/docs/findings/cppgw-conformance-e2e-20261118.md
```

The released fixture map limits live inference to DeepSeek and Codex. The cached Voyage paths remain covered through the gateway API. Configured paid routes and their settlement, direct Voyage transport, positive GitHub agent-message access, and WebSocket refusal bodies remain outside the captured evidence.

The local continuation commits are on `agent-job`. An authenticated GitHub writer is needed for push and merge. `GIT_TERMINAL_PROMPT=0 git push origin HEAD:refs/heads/agent-jobs/cppgw-conformance-e2e-20261118-44214118327-1` returned `could not read Username`. The fetched `origin/development` at `6bd78fc5e3b0783809691c6bd45d25ac45e9696e` lacks the continuation commits.
