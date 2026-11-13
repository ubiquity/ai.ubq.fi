# C++ Pi standby pilot: PR1 source baseline, 2026-11-13

The TypeScript fixture and actual pinned Codex client passed on file-backed disposable state. The source-only warm/load measurements and raw-client latency below are independent of production observations. Rust completed the comparable tool flow and measurements with a cancellation mismatch; no C++ gateway or standby-readiness claim is made.

## Identities and comparison scope

| Surface                              | Exact identity                                                                                                                                                                              |
| ------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Production TypeScript implementation | `d94fef52ccc76e0d5aa019154f11e47d5e10fba6`; tree `2bdb15873f1f02ea61b58d30f776a88b339409a9`; public/loopback health and identity headers agree.                                             |
| Measured fixture/client foundation   | `96cfca54d033aedff8e237a4cbcebb051f62f066`; tree `7ae301184cfe261d45b1add6e0587e77d1f26688`. Later report commits do not relabel these runtime receipts.                                    |
| Bun runtime                          | 1.4.0, Linux ARM64; SHA-256 `086c4121c8738a8e0f5ed730e8a461bc3973b4444e372ddb77aef9a747fa2ae9`.                                                                                             |
| Native KV artifact / provenance      | SHA-256 `d9e6e52f1b0fbe2e0023e63a4c0dcec4394a032801d6d2576b43d2555c314be3` / `57d7405783c9f34343f3d4fe089b3733effc079d9521cd341a3dc5bc0c87c201`; reused immutable artifact, no compilation. |
| Actual client                        | Codex 0.167.2, aarch64-unknown-linux-musl; SHA-256 `b581df3989da98e36fccd7d40cc980727a9ee87bcf0e6ef52ffb2544d6a5302f`.                                                                      |
| Historical Rust source / tree        | `b588420dc1275ac1f7548a6932a5e63330751cea` / `c9bf439743b2f03d4761639fedd17ef2731430d3`; separate from the current unmerged Rust lane.                                                      |
| Rust WASM / JS glue                  | SHA-256 `99a3e146e0193c3c0d77847167374ecf1cef5834e8e123420b7a395e6df57f53` / `1f41de113c589090c069a13bfca59c24834cd87e144f7aa26fa44413fd21bb38`.                                            |
| Rust runtime                         | workerd 1.20261012.1, AArch64; SHA-256 `fabfd6c40a3bb74a98aac54999b456bdb3b7aa7ede4cbbb33898e1f8836a2564`.                                                                                  |

The TypeScript measurement loads the complete source handler and node:http transport through the bounded conformance fixture, with one synthetic direct DeepSeek route, fixture API-key V3 authority/journal and native KV. Mock, raw/native clients and controller are separate actors. It is not the complete production enabled-feature/history profile. Rust runs an older full gateway WASM under native workerd with its own state/authority boundaries. A future one-provider C++ route has a smaller feature scope; neither process sizes nor artifact bytes establish a language-only saving.

Retained Rust release profile is LTO=true, codegen-units=1, opt-level=s, strip=debuginfo; its 12,597,999-byte WASM and 139,427,808-byte workerd executable are disk sizes, not RSS. Current clean `rust-migration` is `178af4bde50acdaaec3b47f17b76854f32187990`, tree `a5c2fcf8b807134923c810beb2d5a1d5c9e90ea2`, 91 unmerged commits and open PR [#1361](https://github.com/ubiquity/ai.ubq.fi/pull/1361). It is preserved, not silently substituted for the measured artifact.

The selected pilot path is client `deepseek-flash` → official DeepSeek `/chat/completions` → source Chat-to-Responses projection. Native DeepSeek Responses is not equivalent on current evidence: developer authority, custom/namespaced tools, reasoning events, parallel-tool control and usage/cancellation differ or remain unproved. No paid/provider generation was used in these synthetic baselines; the documented legacy alias is a separate compatibility choice.

## Production observation stays separate

The eight-day TypeScript service snapshot had RSS 594,328 KiB/PSS 592,415 KiB, process swap 35,408 KiB, cgroup current 628,809,728 B and lifetime peak 989,462,528 B. These are history-bearing production observations, not idle or controlled traffic. The owner's approximate 700 MB TypeScript/350 MB Rust comparison is not reproduced by mixing that snapshot with the isolated measurements below.

## TypeScript accepted source and client evidence

The committed raw socket suite covers split argument receipt before upstream continuation, same-call-ID result replay and final text, real disconnect/upstream cancellation with one dispatch charge, a 240 KiB accepted history, authentication/validation/revocation/exhaustion refusal before dispatch, separate gateway/mock owners and the observed namespace replay quirk. The fixture limits are 262,144 bytes, 256 events, five-second I/O/receipt deadlines, two active requests and one waiter; they do not establish full production size/capacity parity.

The actual pinned Codex phase made two requests through the real source handler and Chat projection. It received `fixture_echo`, call `call_fixture_echo_1`, arguments `{"text":"fixture-argument-ok"}`, sent the exact plain-string result `fixture-tool-ok` with the same ID, and completed with `fixture-final-ok`. Native final-text callback preceded the matching mock ACK and producer resumption. Codex ignores argument-delta fragments, so native qualification uses holdToolDelta=false; the independent raw client proves argument-fragment incrementality. Synthetic preflight alone was not used as gateway acceptance.

Every final TS case used a new marked file-backed Bun SQLite database, saved before stop, drained request work, closed it, freshly reopened it and compared exact values/versionstamps and unchanged dispatch/quota-commit counts. Native acceptance committed2/reserved0; the latency cases committed22/reserved0 each. Each dispatched request has one reserve and one dispatch-charge transition, not a second generation or extra reopened charge. Direct DeepSeek paid settlement is explicitly not_applicable.

**Filesystem boundary:** VPS `/tmp` is tmpfs. File-backed close/reopen proves process-lifetime persistence and ledger semantics; it does not prove power-loss, host-reboot or production-failover durability. Rust task-temp SQLite has the same caveat when stored under this VPS task root. Physical Pi `/tmp` is disk-backed, so later Pi fsync and failure behavior must be measured on that target. No production database was opened, converted or copied.

| TS gateway-only snapshot                      | RSS MiB, c1 / c2 | PSS MiB, c1 / c2 | Cgroup charge MiB, c1 / c2 |
| --------------------------------------------- | ---------------- | ---------------- | -------------------------- |
| Cold ready                                    | 79.223 / 81.691  | 51.115 / 52.990  | 41.273 / 42.852            |
| Warm idle, 2 s after one excluded tool warmup | 85.621 / 89.586  | 54.576 / 58.175  | 41.586 / 45.313            |
| One/two text streams held 1 s after receipt   | 86.055 / 90.734  | 54.843 / 59.291  | 41.867 / 46.246            |
| Postload idle, 2 s                            | 86.121 / 91.391  | 54.886 / 59.906  | 41.820 / 47.082            |

A Python gate moves the gateway into its measured subgroup before Bun exec/import/native allocation. Binary hashing, mock, native/raw clients and controller remain outside. Actual limits were gateway 512 MiB, enclosing task 768 MiB, CPU1, swap0; all kernel readbacks passed. Maximum recorded gateway cgroup peak was47.242 MiB; new swap/OOM/max events were zero. RSS/PSS and cgroup charges measure different shared-page/accounting boundaries and cannot be subtracted to infer helper memory or substituted for the Pi target.

| Raw HTTP/SSE latency | N   | Excluded warmups    | p95 nearest-rank ms | Range ms      |
| -------------------- | --- | ------------------- | ------------------- | ------------- |
| Concurrency 1        | 20  | One tool + one text | 34.827              | 8.613–44.890  |
| Concurrency 2        | 20  | One tool + one text | 62.067              | 10.590–64.573 |

Latency starts immediately before the committed raw client opens the request and stops at the first parsed meaningful text delta, before ACK. All 40 samples had no earlier terminal and exact provider-first-delta < matching client receipt < producer resumption. The estimator is ceil(0.95*N), one-indexed. These are raw-client controlled-local protocol latencies, **not native Codex p95**, provider generation or production-network latency. Native acceptance is the separate two-request result above.

Accepted TS capture references under repository key `c18ba89e18e8c4c810e8dfab831978ea7ed5aa33d6f343bf3282dea4e12db13c`: actual client `231317f6-a119-421a-b2a9-ffea18eaafa0` (child0,3.452 s), memory `1b4b35f5-a065-4556-bbbb-61f9d816495a` (child0,14.168 s), latency `e44da73e-b62e-4896-bedc-9ef5ef15c637` (child0,9.408 s). All final children exited0 and exact units/cgroups were collected. [TypeScript receipt](cpp-pi-standby-baseline-2026-11-13/typescript.json) retains numeric precision, memory phases, source/binary hashes and raw-artifact digests.

## Rust final comparison

The shared two-turn raw-client case passed with the same exact call ID/arguments/result/final text, first-fragment receipt barriers, completed-item accumulation and explicit cached-zero versus absent-cache usage. Source fixture is the same measured96cf foundation; Rust remains historical b588. No native Codex-through-Rust result is claimed.

| Rust gateway-only snapshot           | RSS MiB, c1 / c2  | PSS MiB, c1 / c2  | Cgroup current B, c1 / c2 |
| ------------------------------------ | ----------------- | ----------------- | ------------------------- |
| Cold ready                           | 106.055 / 105.906 | 104.338 / 104.208 | 72802304 / 72359936       |
| Warm idle, 2 s after excluded warmup | 287.496 / 275.133 | 285.779 / 273.435 | 255238144 / 242200576     |
| Held load, 1 s                       | 291.613 / 280.191 | 289.896 / 278.493 | 256274432 / 244350976     |
| Postload idle, 2 s                   | 287.160 / 275.801 | 285.443 / 274.103 | 251654144 / 239845376     |

Measured gateway cgroup peaks were258,482,176 B and253,239,296 B. Workerd/WASM/authority ran inside the pre-exec512 MiB gateway subgroup; mock/client/controller/namespace actors were outside under the768 MiB enclosing bound, CPU1/swap0. All six cases had zero new swap/OOM/max events and no remaining cgroups/processes. Shared mapped pages make RSS/PSS and cgroup accounting distinct.

Raw-client p95 used the same N20, two excluded warmups, two-second warm idle, first parsed meaningful delta before ACK and nearest-rank estimator: concurrency1 **231.545 ms**, concurrency2 **373.316 ms**; p50 was123.277/220.105 ms. These are controlled local raw-client metrics, not Codex or real-provider latency. No diagnostic release was used.

**Cancellation upstream-release assertion FAILED.** After a real downstream destroy following a partial tool delta, no receipt/resume occurred and one generation was dispatched, but upstream stayed open until its5,000 ms mock deadline: close5,001.09 ms after provider-first-delta,4,878.29 ms after downstream disconnect. The persisted terminal was response.failed/eof/drained/delivery-unobserved, with one finished routing row/no pending marker. This is accepted negative evidence, not an abandoned guard attempt, a source fix, or a claim about production Cloudflare cancellation.

Rust's ordinary synthetic token has no API/kernel/paid reservations. Its verified stopped/copied SQLite routing journals do not establish equivalence to TypeScript's synthetic API-quota ledger, and Worker delivery-unobserved must not become Delivered. The listener wrapper records/refuses an auxiliary OpenRouter models GET with503 before generation counting; the original mock factory/frame/barrier block is unchanged. These feature/accounting/runtime differences and the older artifact prevent language-level attribution or complete parity claims.

[Sealed Rust receipt](cpp-pi-standby-baseline-2026-11-13/rust.json) records all six exact references, source/runtime/build/corpus/plan hashes, RSS/PSS phases and accounting limits. Receipt UUIDs: roundtrip2163280e-47e4-436b-97ce-b53e9b130559; cancellation225b2739-8b0c-4c49-95b9-de978995698d (child1); primary one4abd1110-eeee-41e2-8a23-4dce548a9435; two343c4e9a-7d7f-4b1c-90f0-ef1ae0a62099; latency-oneb953fc45-6817-4389-9e51-b0779b6fef20; latency-two9eedbac8-a538-43ff-bdb8-d551d3925271. All other final captures exited0. Earlier pre-dispatch guard failures are excluded from this accepted packet.

Earlier one-request SETUP measurements are not comparison results: embedded mock/control workerd RSS107.53→298.26 MiB; external-loopback bridge RSS105.97→276.77 MiB. Their helpers/placement differ, they use three-second single-request snapshots, and the full setup cgroup includes clients/helpers. Their successful bridge proof and unobserved Worker delivery do not prove shared two-turn, API-quota/paid settlement, cancellation or sustained-load parity.

## Source quirks are evidence, not fixes

The file-backed namespace vector advertises Chat tools `fixture_echo` and `clock_fixture_echo`, returns `{name:"fixture_echo",namespace:"clock"}`, then observes bare `fixture_echo` in replayed upstream history. The test records the behavior without repairing it. Its v2 source receipt predates the final native-adapter-only correction; the application oracle remains d94.

The directional half-close probe used older fixture `f587f3a36927437fca722472c9c4ba1a25c37b1e`, tree `f20c068224f7aecdb871b0532457df2bd79473db`, against the same d94 handler. Sending the complete414-byte Content-Length body then immediate write-FIN produced clean EOF, zero response bytes, no admission/quota/upstream dispatch, internal499/pre_admission/interrupted, and no wire HTTP499. Full-duplex control received HTTP200, progressive arguments, one completed terminal and one charge. This does not qualify later FIN, RST, full-close or terminal-write-failure behavior. [Quirk receipt](cpp-pi-standby-baseline-2026-11-13/source-quirks.json) binds exact revisions and observation scope.

## Pi stack and build preparation only

The off-Pi packaged stack uses 142 signed-index public archives, frozen manifest SHA-256 `9dde9684e06dcbbdc5433fd26c8517cb0ff0b63c2a80e2501f05f6ce27eb52f5`. Native ASan+UBSan/leak and focused clang-tidy passed only the small HTTP/JSON/coroutine/TLS-context probe; they are not gateway sanitizer results. The Pi-target probe is 559,824 B, SHA-256 `cc56a55e8aa0869ba64e412146dd2a3ec049b9d4eebb1fa26cf1991282efe13e`, with target symbol maxima GLIBC2.34/GLIBCXX3.4.21/CXXABI1.3.13. No `-march=native` artifact is used.

That exact probe ran once on physical Debian12 arm64 Pi, glibc2.36/OpenSSL3.0.22. Actual kernel limits were 96 MiB/swap0 before GO; all seven loaded-library hashes matched the frozen sysroot. Exit0 in0.128664 s, wait4 maxRSS14,400 KiB, held cgroup peak10,194,944 B (9.72 MiB), zero swap/OOM/max events; all descendants and the transient cgroup settled. These different RSS/charge measures do not establish a sustained footprint. No network/TLS handshake, listener, gateway, client, provider or ledger ran. Evidence suffix `32697524-92c4-46b2-8edb-f0388b3cbdc5`; [Pi receipt](cpp-pi-standby-baseline-2026-11-13/physical-pi-stack.json).

Earlier ordinary-service Pi sampling observed 193.51 MiB low MemAvailable over 60 s. Keep the provisional 96 MiB candidate target plus 64 MiB host reserve; the nominal extra margin is33.51 MiB before added helpers. Existing host swap was active. Recheck headroom and effective fresh-task controls before later gateway qualification; the stack run's172.02 MiB pre-exec availability is a separate observation. Do not stop ordinary services or infer a gateway budget from this probe.

## Explicitly unmeasured or limited acceptance

| Surface                                                          | TypeScript                                         | Historical Rust/workerd                         | C++ / Pi gateway |
| ---------------------------------------------------------------- | -------------------------------------------------- | ----------------------------------------------- | ---------------- |
| Shared raw two-turn / accumulated output                         | Passed within fixture bounds                       | Passed within fixture bounds                    | UNMEASURED       |
| Actual pinned Codex                                              | Passed, two requests                               | UNMEASURED                                      | UNMEASURED       |
| Prompt upstream release after disconnect                         | Passed source case                                 | FAILED at5 s safety deadline                    | UNMEASURED       |
| API quota / paid accounting                                      | API charge/reopen proved; paid not_applicable      | Routing journals only; API/paid parity unproved | UNMEASURED       |
| Four-stream capacity / overload                                  | UNMEASURED                                         | UNMEASURED                                      | UNMEASURED       |
| Large histories / slow readers / repeated cancel / one-hour soak | Limited240 KiB source vector; remaining UNMEASURED | UNMEASURED                                      | UNMEASURED       |
| Clean/incremental complete gateway build time / peak extra disk  | UNMEASURED                                         | Historical build log only, not fresh benchmark  | UNMEASURED       |
| Actual Pi gateway / restart / power-loss / standby authority     | UNMEASURED                                         | UNMEASURED                                      | UNMEASURED       |

Complete sanitized [TS two-turn](cpp-pi-standby-baseline-2026-11-13/typescript-two-turn.raw.json), [TS cancellation](cpp-pi-standby-baseline-2026-11-13/typescript-cancel.raw.json), [Rust two-turn](cpp-pi-standby-baseline-2026-11-13/rust-two-turn.raw.json) and [Rust cancellation](cpp-pi-standby-baseline-2026-11-13/rust-cancel.raw.json) receipts preserve original payloads, response chunks, ordered events, usage, IDs/arguments, transport outcomes and ledger/readback data. Only credential headers and machine-local SQLite paths are redacted/aliased; source/published hashes and exact redactions are recorded separately. Private state files and native prompts are omitted.

## Reproduction, delivery and remaining work

[Accepted scripts and commands](cpp-pi-standby-baseline-2026-11-13/reproduce/README.md) bind exact frozen fixture inputs, binaries, native artifact and host-specific resource requirements. Script source is recoverable JSON data so absolute-import VPS recipes cannot enter the repository's recursive TypeScript include. The Python materializer only validates hashes and renders fresh task paths/unit names; it never launches Git, services, tests or providers. Its render-only selfcheck is not a new runtime receipt. No private DB/native CODEX_HOME, raw native prompts, credentials, failed-run logs or customer data is published.

The historical base hosted run [42056439542](https://github.com/ubiquity/ai.ubq.fi/actions/runs/42056439542) remains failed, with portable/validate failure and later native/Rust/Darwin/artifact jobs skipped. Do not relabel it green or dispatch a convenience run. Publication requires scoped local gates, including strict typechecking, bound to the publishing commit and tree; their results belong in its delivery receipt or PR. Capture-phase runtime receipts retain their recorded `96cfca54d033aedff8e237a4cbcebb051f62f066` identity. A later automatic post-merge run is a separate hosted receipt. Local review is asynchronous and was not run in this report phase.

PR1 supplies the recorded source oracle and bounded comparisons. C++ gateway source/release, gateway sanitizers, four-stream overload, large histories/slow readers, repeated cancellation, restart recovery, one-hour soak, actual Pi gateway latency/memory and production authority/ingress failover remain unproved and outside this PR1 readiness claim. Close/reopen on tmpfs cannot fill those gaps. Retain all unrelated dirty state and the separate Rust PR; no production service, credentials, public ingress or persistent automation was changed by these baseline captures.
