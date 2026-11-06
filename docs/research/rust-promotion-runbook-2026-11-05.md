# Rust Worker promotion runbook and cancellation review (2026-11-05)

This document has two parts. Part 1 reviews, at code level, whether the Rust Worker aborts the upstream provider request when the client disconnects. Part 2 is an ordered operator procedure for taking the Rust Worker from the `rust-migration` branch to production, with the claims and owner authorizations required before each step.

It is analysis and documentation only. No source, configuration, credential, deployment, DNS record or database was changed or used to produce it.

| Item                     | Value                                                                                                                                                                                                                                                                                                               |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Reviewed source          | `faeabcd9267c0135afc204a59fa72a26dbbab38b` (tip of `rust-migration`), tree `d4f9cdf55d99a2facfe371eac21634c8ef37c51c`                                                                                                                                                                                               |
| TypeScript compared      | `src/runtime/http.ts` and `src/provider/stream-relay.ts` at the same commit                                                                                                                                                                                                                                         |
| Inputs read              | `AGENTS.md`, `docs/DECISIONS.md` (all Rust/Worker/Cloudflare/migration/cutover/rollback/private-copy entries), the two 2026-11-05 research notes, `ops/`, `scripts/kv-migrate.ts`, `cloudflare.config.ts`, `wrangler.config.ts`, `rust/crates/gateway-migration`, `rust/crates/gateway-worker`, `tools/cloudflare/` |
| Local evidence it builds | `docs/research/rust-worker-local-e2e-2026-11-05.md` (row 21b: upstream cancellation BLOCKED locally)                                                                                                                                                                                                                |

Line numbers refer to the reviewed commit.

---

## Part 1. Cancellation propagation review

### Verdict

**By code, yes: on a client disconnect the Worker aborts the `AbortSignal` that every upstream inference `fetch` was created with.** It does this through two independent paths. One depends on the `enable_request_signal` compatibility flag. The other depends only on the response body being cancelled. **Neither path has been observed on real Cloudflare.** The local workerd harness could not observe outbound abort even for a control Worker (e2e row 21b). So the runbook's client-abort probe (step F5) is a hard gate before any traffic cutover.

The settlement design meets the AGENTS.md lifetime rules: one admission lifetime, at most one durable paid settlement, and delivery observed, never inferred. Four findings below (F1-F4) are inconsistencies or unproven platform assumptions, not missing cancellation wiring. F2 and F4 are fixed on branch `rust-stream-outcome-fix`; F3 is deliberately left unchanged (see its entry).

### How the Worker observes the client

1. **One request-wide cancellation is created from `request.signal`.** `runtime::route` builds `Cancellation::from_request(&request)` once per request (`rust/crates/gateway-worker/src/runtime.rs:50`) and stores it in `PlatformHost` (`runtime.rs:53-60`). `Cancellation::from_signal` (`src/transport/outbound.rs:84-106`) creates a fresh `AbortController`. It adds an `abort` listener on the incoming `request.signal` that aborts that controller and wakes the registered Rust waker (`outbound.rs:90-96`). It also handles a signal that is already aborted (`outbound.rs:97-99`). `client_aborted()` reads the incoming platform signal directly (`outbound.rs:123-125`). So the code only knows "the client went away" when Cloudflare actually aborts `request.signal`, and that requires `enable_request_signal` (`cloudflare.config.ts:10`).
2. **Every upstream inference fetch carries that controller's signal.** `send_http`/`send_constructed` build a fresh `Request` (`outbound.rs:206-220`), then call `Fetch::Request(request).send_with_signal(&cancellation.signal())` (`outbound.rs:238-240`). `Dispatch::send` passes the request-wide `host.cancellation` for both ordinary and Claude wire requests (`src/inference.rs:102-122`, wired at `inference.rs:553-560`). Child cancellations (`outbound.rs:73-75`) used by catalog, routing, compaction, image and native sidebands chain from the same controller. Billing and paid lookups deliberately use `Cancellation::background()` (`outbound.rs:77-82`; `src/paid_lookup.rs:79`, `src/claude_host/admin.rs:321`), so a client disconnect cannot stop durable accounting.
3. **`request_signal_passthrough` does not affect upstream inference.** The Worker never passes the incoming `Request` to `fetch`. It always constructs a new request at a pinned destination and attaches the signal explicitly (`outbound.rs:206-240`). As understood here, the flag governs whether a signal carries over when an existing `Request` is re-wrapped or forwarded. Its exact semantics were not re-verified against Cloudflare's documentation.

### What happens when the client disconnects

| Phase                                                                                                             | Mechanism in code                                                                                                                                                                                                                                                                                                                                                                                          | Upstream aborted?                                                                                                                                                                                                                                                             |
| ----------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Before upstream headers (dispatch, waterfall hops, buffered Cerebras body)                                        | Only the `request.signal` listener (path A). `send_constructed` maps an abort to `interrupted()` (`outbound.rs:241-249`). `Dispatch::failed` classifies it as `HopFailure::Cancelled` with `cancelled: true` (`inference.rs:149-187`). `RoutingSession::refusal` stops on cancellation (`rust/crates/gateway-routing-runtime/src/dispatch.rs:330`), so no further hop, including a paid hop, is attempted. | Yes, **if** Cloudflare fires `request.signal` (F1).                                                                                                                                                                                                                           |
| Streaming relay (`Reply::streaming`, `src/inference/stream.rs:363-415`)                                           | Path A, plus path B. In path B, `ObservedStream::into_response` pipes the Rust stream through a `TransformStream` and watches the pipe in `waitUntil`. If the client cancels the readable, the pipe rejects and the watcher calls `cancellation.abort()` (`src/transport/stream.rs:172-196`). Dropping an unfinished `ObservedStream` also aborts (`stream.rs:244-251`).                                   | Yes, through either path. Path B does not need `request.signal`. The relay also writes `: keep-alive` every 15 s (`inference/stream.rs:166-185`; `gateway-providers/src/sse.rs:12`), so a silently dead client is detected within about 15 s at most, through a failed write. |
| Quiet upstream after abort                                                                                        | `ObservedStream::poll_next` re-registers the cancellation waker on every poll and checks `signal().aborted()` before polling the source (`stream.rs:214-225`). The abort listener wakes it (`outbound.rs:90-93`).                                                                                                                                                                                          | Yes. The stream ends without waiting for another upstream byte.                                                                                                                                                                                                               |
| Buffered replay after a completed upstream (Cerebras `gpt-oss-120b`, `buffered()`, `inference/stream.rs:416-502`) | The upstream body was already fully read under the same signal (`inference/stream.rs:417-429`). Only the replay is cancelled.                                                                                                                                                                                                                                                                              | Nothing to abort. The upstream finished before the first client byte, which matches the AGENTS.md rule that this route is non-streaming.                                                                                                                                      |

### Settlement and telemetry against the AGENTS.md rules

- **One admission lifetime.** The permit is moved into the single `finish_hook` closure (`src/inference_lifecycle.rs:79-115`) and dropped only after durable finish and capture (`inference_lifecycle.rs:112`). `ObservedStream::new` registers exactly one `waitUntil` waiter per response body while the invocation is still alive (`transport/stream.rs:96-109`).
- **Exactly one terminal settlement.** `FinishSignal::complete` is guarded by `completed.replace(true)` (`transport/stream.rs:27-33`). In native-pipe mode, `finalize` records facts but defers completion to the pipe watcher (`stream.rs:121-142`, `150`, `175-195`). Early failure paths call `session.finish`/`lifecycle.finish` directly and never construct the hook's waiter (`inference.rs:562-570`, `inference/stream.rs:424-436`). Paid settlement is fenced durably: a second `settle_paid` on a settled attempt returns `Ok(false)` (`rust/crates/gateway-authority/src/paid.rs:574-588`).
- **Delivery observed, not inferred.** The Worker never writes `DeliveryOutcome::Delivered`. Its outcome is `Interrupted` only when the platform signal is aborted, and `Unobserved` otherwise (`transport/stream.rs:132-136`, `189-193`). This matches the 2026-10-18 decision that "Workers EOF supplies no native delivery receipt" (`docs/DECISIONS.md:2360`) and `docs/research/rust-wasm-port-contract.md:30`.
- **Durable writes before telemetry.** `finish_hook` awaits `lifecycle.finish` (durable observe and settle) before `capture.finish` (telemetry) (`inference_lifecycle.rs:88-111`).

### Comparison with TypeScript at HEAD

| Concern                                 | TypeScript (`node:http`)                                                                                                                                                                                                                                                            | Rust Worker                                                                                                                                                                                                                                                   |
| --------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Client-disconnect source                | Kernel socket events: `incoming` `aborted`/`error`, `outgoing` `close` before `writableFinished` abort one per-request controller (`src/runtime/http.ts:221-247`). That controller becomes `request.signal` (`http.ts:59-66`, `255`).                                               | Platform `request.signal` (path A), plus cancellation of the response stream (path B). No socket events exist.                                                                                                                                                |
| Upstream fetch signal                   | Provider handlers pass `requestSignal` to `fetch` (for example `src/provider/cerebras-handlers.ts:328`, `351`).                                                                                                                                                                     | `send_with_signal(&cancellation.signal())` (`outbound.rs:238-240`).                                                                                                                                                                                           |
| Relay cancel                            | `sendResponse` cancels the response reader on abort (`http.ts:159-192`). The relay's `cancel()` aborts its own controller, which is composed with the request signal through `AbortSignal.any`, then cancels the upstream body (`src/provider/stream-relay.ts:130-135`, `307-323`). | Pipe rejection or stream drop aborts the shared controller (`transport/stream.rs:182`, `247`).                                                                                                                                                                |
| Delivery receipt                        | Real: `outgoing` `finish` resolves `completed`, and close-before-finish rejects it (`http.ts:243-251`).                                                                                                                                                                             | None. `Unobserved` or `Interrupted` only (by design, see above).                                                                                                                                                                                              |
| Failure terminal on non-cancel teardown | `failStream` emits a provider error frame unless the terminal is `cancelled` (`stream-relay.ts:180-195`).                                                                                                                                                                           | `Relay::failed` emits a failure frame for an upstream read error when the client has not aborted (`inference/stream.rs:132-156`). A gateway **deadline** abort now writes the same frame and closes cleanly (`inference/stream.rs:57-69`, `176-178`). See F4. |

### Findings

**F1. The pre-header phase depends only on `request.signal` (unproven on Cloudflare).** Until upstream headers arrive, path B does not exist. This covers a slow first byte, every waterfall hop, and the whole buffered Cerebras read. If Cloudflare does not fire `request.signal` for a disconnected client, the upstream request runs to completion and is billed. The same holds if Cloudflare fires it only before a Response is returned, or does not fire it inside this isolate's lifetime. It is unknown whether Cloudflare also cancels outstanding subrequests on its own when the client disconnects. Nothing in this repository proves it. Required evidence: probe F5b on the candidate. No code fix is possible without platform evidence. If F5b fails, the only Worker-side mitigation is a shorter per-attempt deadline. The current deadline is `ATTEMPT_DEADLINE_MS = 1_800_000` (`inference.rs:50`). Changing it is an owner decision.

**F2. The pipe watcher overwrites the reader and body facts that `finalize` recorded (inconsistent observation; exact fix).** In native-pipe mode, `finalize` writes `reader_termination`, `body` and `delivery` into the shared observation and does not complete the signal (`transport/stream.rs:128-141`). The watcher then takes `observation.snapshot()` and unconditionally sets `reader_termination`/`body` from the pipe result alone (`stream.rs:180-188`). Consequences:

- A gateway deadline abort, which `finalize` records as `ReaderTermination::Deadline` (`stream.rs:215-221`), is reported as `Cancelled`.
- A Rust-side read error (`ReadError`/`Failed`, `stream.rs:235-237`) is reported as `Cancelled`/`Interrupted`.
- A client abort that ends the source cleanly, which `finalize` records as `Cancelled`/`Interrupted` (`stream.rs:227-230`), is reported as `Eof`/`Drained` if the pipe still resolves. Its `delivery` remains `Interrupted`, so the combination is contradictory.

These facts reach `session.finish` and capture (`inference_lifecycle.rs:64-76`, `109-111`), so cancellation telemetry and any reducer keyed on reader termination can misattribute the cause. **Fix (proposed text; implemented on branch `rust-stream-outcome-fix` as described in the status below):** in the watcher at `transport/stream.rs:180-188`, keep the Rust-side facts and let the pipe result only downgrade them:

```rust
let mut observed = observation.snapshot();
if failed {
    cancellation.abort();
    // A pipe failure proves the body did not reach the bridge; keep a more specific Rust cause.
    if matches!(observed.reader_termination, None | Some(ReaderTermination::Eof)) {
        observed.reader_termination = Some(ReaderTermination::Cancelled);
    }
    if observed.body != Some(BodyOutcome::Failed) {
        observed.body = Some(BodyOutcome::Interrupted);
    }
} else {
    observed.reader_termination.get_or_insert(ReaderTermination::Eof);
    observed.body.get_or_insert(BodyOutcome::Drained);
}
```

Add a unit test beside `failure_settles_once` (`stream.rs:268-297`) for each of the three cases above. This changes no settlement count and no delivery value.

**F2 status: fixed (branch `rust-stream-outcome-fix`).** The watcher now calls `piped(observation.snapshot(), failed, client)` (`transport/stream.rs:237`). `piped` (`transport/stream.rs:89-112`) applies the rule above exactly: a pipe failure turns only a missing or `Eof` reader into `Cancelled` and any body other than `Failed` into `Interrupted`; a resolved pipe only fills missing facts (`Eof`/`Drained`). Delivery stays `Interrupted` on a platform client abort and `Unobserved` otherwise (`observed_delivery`, `transport/stream.rs:79-85`); `Delivered` is never written. The failed-constructor path uses the same rule (`transport/stream.rs:56`). `FinishSignal::complete` still publishes exactly one terminal. Tests: `rust/crates/gateway-worker/tests/stream_outcome.rs` covers a deadline or read error after partial delivery followed by a pipe failure, a client abort followed by a clean end, the weaker-fact fills, and the moved single-settlement constructor tests.

**F3. The terminal is observed twice per streamed or buffered response (idempotent, but extra Durable Object round trips).** The relay calls `lifecycle.observe_terminal` when the provider terminal arrives (`inference/stream.rs:104-113`). `buffered()` does the same at `inference/stream.rs:459-463`. The finish hook then calls `lifecycle.finish`, which calls `observe_terminal` again (`inference_lifecycle.rs:64-69`). Each call repeats `paid_lookup::terminal_usage` and `settle_paid` (`gateway-routing-runtime/src/dispatch.rs:229-294`). Durable correctness holds: a matching semantic terminal is accepted, a conflicting one is refused (`dispatch.rs:236-245`), and the second `settle_paid` is a no-op (`paid.rs:574-588`). This is not a double settlement. It costs extra Durable Object reads and writes per paid request. A follow-up could pass a "terminal already observed" flag into `finish`. That needs owner review because it touches the settlement path.

**F3 status: left unchanged (branch `rust-stream-outcome-fix`).** The second `observe_terminal` is not a pure duplicate. It is the call that durably writes the final transport facts (reader, body, delivery) into the attempt journal (`current.observation = Some(observation)`, `gateway-routing-runtime/src/dispatch.rs:252`), with the final usage and provider request id. It is also the only `observe_terminal` for a body that ended with no semantic terminal (deadline, cancellation), and it retries a first call that failed. Skipping it would change what the journal records and when, so no bounded change removes the round trip without touching settlement ordering. The follow-up still needs owner review.

**F4. A deadline abort tears the stream down without a failure frame.** `ObservedStream::poll_next` returns `Err("Upstream stream interrupted")` for any aborted signal (`transport/stream.rs:215-224`), so a 30-minute idle deadline ends the HTTP body with no `response.failed` or Chat error frame. TypeScript `failStream` emits one for non-cancel terminals (`stream-relay.ts:187-192`). This does not affect client-disconnect cancellation and is recorded only for the parity backlog.

**F4 status: fixed (branch `rust-stream-outcome-fix`).** The streaming relay's `ObservedStream` opts in with `with_deadline_frame()` (`inference/stream.rs:464`, `transport/stream.rs:171-176`). After a gateway deadline (signal aborted, client not aborted), `poll_next` keeps polling the relay instead of tearing down (`transport/stream.rs:260`). The relay races its upstream read and heartbeat against the abort (`inference/stream.rs:180-199`). On abort it writes the source `failStream` frame through `abort_frame` and ends (`inference/stream.rs:57-69`, `inference_source.rs:257-267`). The body then closes cleanly with `ReaderTermination::Deadline`/`BodyOutcome::Interrupted` (`end_facts`, `transport/stream.rs:69-76`). A client abort still writes no frame and tears down. The frame is not a provider semantic terminal, so it records no `Failed`/`Error` terminal. Its shape follows TypeScript: Chat uses `chatStreamErrorValue` (`{"error":{"message","type":"server_error","code","param":null}}`), and Responses uses `response.failed` with `sequence_number` last. The codes are the adapters' `streamErrorCode` (`deepseek_`, `lithos_` and `openrouter_upstream_stream_error`, otherwise `upstream_stream_error`) (`inference_source.rs:232-256`). The read-error frame from `Relay::failed` now shares this builder, so its Chat shape and OpenRouter code also match TypeScript. Other `ObservedStream` bodies (buffered, refusal) keep the old teardown. Tests: `tests/stream_outcome.rs` (`deadline_writes_the_source_failure_frame_and_client_abort_writes_none`, `deadline_outranks_source_end_and_client_abort_outranks_deadline`). The relay's abort race needs a Worker runtime and is not covered by a native test.

Low-risk note: `Cancellation` holds one `AtomicWaker` (`outbound.rs:65`, `119-121`). Every waiter on the request-wide cancellation shares it, including `ObservedStream` (`stream.rs:214`), admission waits (`src/admission.rs:70-71`) and extras sleeps (`src/extras_host.rs:230`). On the inference path these waits are sequential, and an aborted fetch also wakes its reader, so no lost wake-up was identified. A future concurrent waiter on the root cancellation would need its own child cancellation.

### What must be proven on real Cloudflare

Run probes F5a and F5b in Part 2 against the candidate. F5a covers path B (abort mid-stream). F5b covers path A (abort before the first byte). Both must show provider-side cancellation before any traffic cutover. If either fails, stop. Abandoned upstream generations would be billed for every disconnected client.

---

## Part 2. Promotion runbook

### 2.0 Current state, blockers and authority

Promotion cannot be completed with the tooling that exists today. The runbook below is exact where tooling exists. Where it does not, it says so and lists what must be built. **Items B1-B9 must be resolved, each under its own reviewed PR or owner decision, before step H (cutover).** Steps A-F (build, candidate deployment and probes) can run today once their own authorizations are granted.

| #   | Blocker (no supported tooling or decision today)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         | Evidence                                                                                                                                                                                                                                                                                                                                                                              |
| --- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| B1  | **No owner decision authorizes production cutover.** The decisions say production hostname cutover "remains excluded" and that this Worker is an "isolated candidate".                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   | `docs/DECISIONS.md` 2026-10-19 "Scoped native connector…" ("Production hostname traffic cutover remains excluded"); 2026-10-21 "Rust candidate authentication binds its exact browser origin"; `cloudflare.config.ts:22-23` ("Production ai.ubq.fi keeps its provider credentials on the VPS").                                                                                       |
| B2  | **Workers plan.** The 2026-10-22 decision selected Workers Free. The compressed WASM is 4.11 MB, which is over the published 3 MB Free limit and within the 10 MB paid limit (limits not re-verified).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   | `docs/DECISIONS.md` 2026-10-22; e2e doc lines 36-38 and 56.                                                                                                                                                                                                                                                                                                                           |
| B3  | **Tooling built; rehearsal on Cloudflare pending (2026-11-05).** The deployed Worker now exposes `GET`/`POST /admin/state-transfer/import`: admin allowlist token plus a one-time operator intent whose SHA-256 is the Worker setting `UOS_STATE_TRANSFER_INTENT_SHA256` (absent: `404 state_transfer_closed`). Each request carries one existing staged frame and is applied by the unchanged `execute_staged` (empty target, ordered absent keys, digest chain, one transition per chunk), after per-record typed-value, revision and absolute-expiry checks; status makes it resumable and exact retries are replayed without writes. `tools/cloudflare/state-transfer.ts import` streams `native-stage-frames` output with hash, size and inventory checks and signed receipts. The unused `local_operator_adopt`/`local_operator_stage` exports were removed. Still open: a new empty rehearsal target (the Worker name is hard-coded; a probed candidate Durable Object is `occupied`), and the per-request CPU of a ~4 MiB chunk on Workers Free is unmeasured. `InitialControlInstaller` is unchanged and not used by this path. | `docs/research/rust-state-migration-2026-11-05.md`; `rust/crates/gateway-state-transfer/src/{http,import,intent}.rs`; `gateway-worker/src/runtime.rs` (route), `src/migration_host.rs` (`state_transfer`); `tools/cloudflare/state-transfer.ts`; `tools/cloudflare/state-transfer-conformance.ts` (workerd acceptance); DECISIONS 2026-11-05 "Authenticated operator state transfer". |
| B4  | **Superseded for migration.** `kv:import-http` and `/admin/kv-migration/import` remain lossy profile tools (no expiry or revision, 5 MiB, `prod` skips `codex_auth`/`codex_models`) and must not be used for production state. The state-transfer import carries the complete selected NativeCapture as staged frames with raw keys, revisions and absolute expiry, in bounded resumable chunks.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         | `scripts/kv-migrate.ts` (`import-http` unchanged); `rust/crates/gateway-migration/src/profile_runtime.rs:44-50`; `docs/research/rust-state-migration-2026-11-05.md`.                                                                                                                                                                                                                  |
| B5  | **Tooling built; rehearsal pending (2026-11-05).** `POST /admin/state-transfer/export` (export intent) pages the Durable Object's canonical records with per-page SHA-256; the CLI repeats the export to prove quiescence. `gateway-state-reverse` (`state-transfer-reverse`) verifies the archive and runs the existing `write_private_latest` inverse with configured native custody into a NEW NativeCapture; `kv:restore-native` writes it into a NEW codec-marked Bun KV with full re-read validation. Worker-era writes (quota windows, paid accounting, keys, passkey counters) therefore survive a rollback. The rollback store holds core state, not VPS history (the pre-cutover database stays as the history archive); placing it at `.data/kv-bun.sqlite3` is an owner-authorized stopped-service step.                                                                                                                                                                                                                                                                                                                     | `rust/crates/gateway-state-transfer/src/export.rs`; `rust/crates/gateway-state-reverse/src/{archive,reverse}.rs`; `src/kv-conversion/native-restore.ts`; `docs/research/rust-state-migration-2026-11-05.md` step 6.                                                                                                                                                                   |
| B6  | **No Codex credential owner for the Worker.** Codex subscription capacity is the first waterfall tier. Refresh is native-owned through the `UOS_NATIVE_CARRIER` binding, which is not configured. A migrated access-only projection expires and cannot be refreshed. Sharing the VPS's Codex credentials would make two writers of one credential, which the Local Credential Recovery Safety rules forbid.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              | `src/native_host.rs:27-52`; DECISIONS 2026-10-21 "Native Worker HTTPS carrier…"; `CORE_STATE.md` lines 71-73; AGENTS.md "Local Credential Recovery Safety".                                                                                                                                                                                                                           |
| B7  | **Secrets for admin and Passport are not declared.** `cloudflare.config.ts` declares no `DENO_DEPLOY_TOKEN` (the admin bearer source) and no `UOS_AI_PASSPORT_*`. Whether `cf deploy` preserves secrets that the config does not declare is unconfirmed. Declaring them is a source change, which means a new SHA, CI and build.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         | `rust/crates/gateway-auth/src/bootstrap.rs:62-63`; `src/host.rs:18-32`; `gateway-auth/src/passport.rs:66-91`; e2e finding 3.                                                                                                                                                                                                                                                          |
| B8  | **No periodic scheduler on the Worker.** The VPS runs scheduled paid-billing reconciliation, capacity sampling and telemetry pruning under `DENO_TIMELINE=production`. The Worker declares no cron trigger, and the port contract says there is no periodic shared scheduler. Moving production traffic moves the work these jobs reconcile, but not the jobs.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           | `ops/README.md` "Operate the service"; `docs/research/rust-wasm-port-contract.md:46`; `cloudflare.config.ts` (no triggers).                                                                                                                                                                                                                                                           |
| B9  | **Owner parity decisions are still open, and the Darwin build-tool pins are missing.** Open decisions: early SSE headers (e9b1b90) and `text.format: text` rejection (ec4b326). Pinned hashes exist only for the Linux ARM64 Worker tools. The Mac build has no repo-pinned hashes for wasm-bindgen 0.2.129, wasm-opt 132 or esbuild 0.28.2 for darwin-arm64, and none for the `wasm32-unknown-unknown` rust-std component.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              | `docs/research/rust-parity-drift-2026-11-05.md` "Owner decisions still pending"; `scripts/rust-verify.ts:801-809`, `860-888`, `823-824` (Darwin Worker prep is refused).                                                                                                                                                                                                              |

Authorizations by step. None is granted by this document, by a merged PR, or by CI.

| Step                 | Owner authorization required                                                                                                                                                                                                                                        |
| -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| A. Plan and bindings | Paid Workers plan purchase (spend). The bindings PR (B7). Darwin tool pins (B9). Passport client registration for the candidate origin.                                                                                                                             |
| B. Integrate and CI  | None beyond normal PR review. Do not dispatch or rerun workflows for validation (AGENTS.md "GitHub Actions Budget").                                                                                                                                                |
| C-D. Build           | None (local, no credentials).                                                                                                                                                                                                                                       |
| E. Candidate deploy  | Use of the existing Cloudflare API token. Creating or uploading candidate secrets: new candidate client and admin tokens, plus provider keys. This is a **credential residence change**. Prefer new candidate-only provider keys, which means creating credentials. |
| F. Probes            | **Provider-credit spend**, bounded per provider (listed in F4-F6).                                                                                                                                                                                                  |
| G. Data rehearsal    | Reading a production database copy, which contains credentials (DECISIONS 2026-10-25/27). An authorized downtime window if a stopped source is required.                                                                                                            |
| H. Cutover           | A new DECISIONS entry superseding B1. Live data conversion. Moving production credentials to Cloudflare. A DNS or route change. Downtime.                                                                                                                           |
| I. Rollback          | Authorization for the B5 reverse (export, reverse, NEW Bun KV, stopped-service placement).                                                                                                                                                                          |

### 2.1 Fixed identities used below

```sh
export CF_ACCOUNT_ID=5c0228bee348f4cf48829ffe21b61fe9      # cloudflare.config.ts:6, ops/rust-worker-deploy.sh:36
export WORKER=uos-ai-rust-candidate                          # cloudflare.config.ts:3 (also hard-coded in both ops scripts)
export VPS=codex@vps.pavlovcik.com                           # AGENTS.md, ops/README.md
export VPS_ROOT=/home/codex/repos/ubiquity/ai.ubq.fi
export VPS_IP=129.158.58.222                                 # ops/README.md "DNS and TLS"
export BUN=/path/to/bun-1.4.0/bin                            # checksum-pinned Bun 1.4.0 on the Mac (exact path not recorded in the repo)
```

Pinned tool versions (sources: `rust/rust-toolchain.toml`, `scripts/rust-verify.ts:13-15`, `ops/rust-worker-build.sh:43-51`, root `package.json:81-85`, `tools/cloudflare/verify-build-identity.ts:19`): Rust `1.98.1` (revision `48a229ce`), `worker-build 0.8.7`, `wasm-bindgen 0.2.129`, `wasm-opt` (binaryen) `132`, `esbuild 0.28.2`, Bun `1.4.0`, `cf 1.0.0-beta.12`, `wrangler 4.151.1`, `workerd 1.20261012.1`, `miniflare 5.20261012.1-beta`, compatibility date `2026-10-18`.

Recorded tool hashes. None of them is a Darwin pin.

| Tool                      | Platform                       | SHA-256                                                            | Source                                  |
| ------------------------- | ------------------------------ | ------------------------------------------------------------------ | --------------------------------------- |
| wasm-bindgen 0.2.129      | Linux aarch64 archive (CI pin) | `1797c349a0f45d30946e8986b184135e12839ab576c9a04203e85febfc5dcb35` | `scripts/rust-verify.ts:863-866`        |
| binaryen version_132      | Linux aarch64 archive (CI pin) | `c58562417836c5d0493d89bdefc434933bdc097db641b483df86bcfa557a107f` | `scripts/rust-verify.ts:868-871`        |
| esbuild 0.28.2            | Linux arm64 npm tgz (CI pin)   | `a96dbfa41d3ef5dbd1ef22b1c10d5187be9267e86093a870f06402a7ec931596` | `scripts/rust-verify.ts:873-876`        |
| wasm-bindgen 0.2.129 musl | Linux x86_64 binary (observed) | `fb59da714982a04273e9e4b09a85e523e5dd04de35b1a4a8b1641785ee7a71cf` | e2e doc line 50 (worker-build download) |
| wasm-opt 132              | Linux x86_64 binary (observed) | `1014958e6f20d412f1542320b43970214b0fb1ed780595e8f7c0d8761ed53725` | e2e doc line 51                         |
| esbuild 0.28.2            | Linux x86_64 binary (observed) | `e1698a3d5c6c0798fee4fd3b5cc816651f460c63d390a7a26ea4beb0b1884100` | e2e doc line 52                         |

### Step A. Owner decisions and source prerequisites

Claims required before step A: none.

1. **Workers plan.** The owner upgrades account `5c0228bee348f4cf48829ffe21b61fe9` to a paid Workers plan and records a DECISIONS entry that supersedes the Free-plan premise of 2026-10-22. Keep the bounded CORE_STATE profile unless the owner separately re-decides history retention.
2. **Bindings PR (B7).** Add `DENO_DEPLOY_TOKEN`, `UOS_AI_PASSPORT_CLIENT_ID`, `UOS_AI_PASSPORT_ORIGIN` and `UOS_AI_PASSPORT_REDIRECT_URI` (and `UOS_AI_PASSPORT_AUDIENCE` only if it is not the default) as `bindings.secret()` in `cloudflare.config.ts` `worker.env`. Make this a bounded PR into `rust-migration`. This step is skipped only if a dry run proves that `cf deploy --prebuilt` preserves secrets that the config does not declare. That is unconfirmed.
3. **Darwin tool pins (B9).** Record darwin-arm64 SHA-256 pins for `wasm-bindgen 0.2.129`, `wasm-opt 132`, `esbuild 0.28.2` and the `rust-std-1.98.1-wasm32-unknown-unknown` component. Get the owner's acceptance in a DECISIONS entry, because the repository pins only Linux artifacts.
4. **Passport client.** Register a Passport client whose redirect URI is `https://<candidate-origin>/auth/callback`. The redirect origin must equal `UOS_AI_PASSPORT_ORIGIN` exactly (`gateway-auth/src/passport.rs:78-86`). This is an owner action at `passport.ubq.fi`. It is not in this repository.
5. Resolve or explicitly defer the open parity decisions (B9).

### Step B. Integrate the exact source and obtain green CI

Claims required: the A2 PR is merged into `rust-migration`. `rust-migration` is integrated into `development` through a normal PR with an ancestry-preserving merge commit (AGENTS.md "Repository Completion and Checkout Handoff").

```sh
git fetch origin development
SHA=$(git rev-parse origin/development)   # the merge commit produced by the integration PR
echo "$SHA"
gh run list --workflow "Gateway CI" --commit "$SHA" --json databaseId,status,conclusion,event
gh run view <run-id> --json jobs --jq '.jobs[] | {name, conclusion}'
```

- Use only the automatic post-merge run (`.github/workflows/deno-deploy.yml:4-7`). Do not dispatch, rerun or add triggers (AGENTS.md "GitHub Actions Budget").
- Require `conclusion=success` for `scope`, `portable`, `rust-native`, `rust-wasm` and `validate` (`deno-deploy.yml:174-290`). `rust-wasm` uploads `ai-ubq-fi-rust-worker-<sha>` (`deno-deploy.yml:264-270`). Keep it as an independent Linux ARM64 build reference. The deploy script cannot consume it, because it requires the local receipt.
- Every later claim is bound to this exact `$SHA`. A failed CI run stops the pipeline.

### Step C. Prepare the Mac (Darwin arm64) build host

The Mac is the Rust build host. "Do not use the VPS as a build or execution fallback" (DECISIONS 2026-10-19). `ops/rust-worker-build.sh:7-11` accepts only `Darwin:arm64` or Linux ARM64.

Claims required: step B is green for `$SHA`. The step A3 pins exist. Network access is available for preparation only.

```sh
export SHA=<full 40-hex SHA from step B>
export TASK=/private/tmp/uos-rust-worker-$SHA      # must be under /tmp, /private/tmp, /var/tmp or the checkout (ops/rust-worker-build.sh:23-26)
export SRC=$TASK/src
export TOOLS=$TASK/tools                           # rustToolPath requires tools inside dirname(RUSTUP_HOME) (scripts/rust-verify.ts:187-201)
mkdir -p "$TASK" "$TOOLS" "$TASK/cache" "$TASK/tmp" && chmod 700 "$TASK"

# Isolated exact-source checkout. It must have no .env/.env.local/.env.production/.env.production.local/.dev.vars
# in it or in any ancestor (ops/rust-worker-build.sh:13-20), so never build inside the Mac service checkout,
# whose root .env is the shared credential symlink.
git clone --no-checkout https://github.com/ubiquity/ai.ubq.fi.git "$SRC"
git -C "$SRC" checkout --detach "$SHA"
test "$(git -C "$SRC" rev-parse HEAD)" = "$SHA"
test -f "$SRC/bun.lock" && test -f "$SRC/tools/lint/bun.lock" && test -f "$SRC/rust/Cargo.lock"
(cd "$SRC" && PATH="$BUN:/usr/bin:/bin" bun install --frozen-lockfile)   # cf, wrangler, workerd into node_modules; never regenerate the lock

export RUSTUP_HOME=$TOOLS/rustup CARGO_HOME=$TOOLS/cargo CARGO_TARGET_DIR=$TASK/target XDG_CACHE_HOME=$TASK/cache TMPDIR=$TASK/tmp
rustup toolchain install 1.98.1-aarch64-apple-darwin --profile minimal --target wasm32-unknown-unknown
cargo +1.98.1 install worker-build --version 0.8.7 --locked --root "$TOOLS"
cargo +1.98.1 install wasm-bindgen-cli --version 0.2.129 --locked --root "$TOOLS"
# wasm-opt 132 and esbuild 0.28.2 darwin-arm64: download, verify against the step A3 pins, extract under $TOOLS.
#   esbuild: https://registry.npmjs.org/@esbuild/darwin-arm64/-/darwin-arm64-0.28.2.tgz  (package/bin/esbuild)
#   binaryen: the version_132 macOS arm64 release archive (exact asset name not recorded in this repository)
shasum -a 256 <each downloaded archive>      # must equal the A3 pin before extraction
export WASM_BINDGEN_BIN=$TOOLS/bin/wasm-bindgen WASM_OPT_BIN=$TOOLS/binaryen/bin/wasm-opt ESBUILD_BIN=$TOOLS/esbuild/bin/esbuild
"$WASM_BINDGEN_BIN" --version   # exactly "wasm-bindgen 0.2.129" (ops/rust-worker-build.sh:49)
"$WASM_OPT_BIN" --version       # contains 132 (scripts/rust-verify.ts:903-905)
"$ESBUILD_BIN" --version        # 0.28.2
(cd "$SRC" && cargo +1.98.1 fetch --manifest-path rust/Cargo.toml --locked)   # the Darwin build runs with CARGO_NET_OFFLINE=true (scripts/rust-verify.ts:213)
```

`ops/rust-worker-build.sh:27-29` requires all three binary overrides. A pre-seeded `~/.cache/worker-build` alone does not satisfy the script. **Not confirmed:** whether `worker-build 0.8.7` reads `WASM_BINDGEN_BIN`/`WASM_OPT_BIN`/`ESBUILD_BIN` instead of downloading into its cache. On Linux x86_64 it downloaded its own copies into `~/.cache/worker-build/` (e2e doc lines 46-54). Step D checks for that.

### Step D. Exact-source Worker build

Claims required: the step C outputs exist. `git -C "$SRC" status --porcelain --untracked-files=all` is empty. No Cloudflare token is in the environment.

```sh
ls -la "$XDG_CACHE_HOME" ~/Library/Caches ~/.cache 2>/dev/null | grep -i worker-build > "$TASK/worker-build-cache.before" || true
cd "$SRC"
env -i HOME="$HOME" PATH="$TOOLS/bin:$BUN:/usr/bin:/bin" \
  RUSTUP_HOME="$RUSTUP_HOME" CARGO_HOME="$CARGO_HOME" CARGO_TARGET_DIR="$CARGO_TARGET_DIR" \
  XDG_CACHE_HOME="$XDG_CACHE_HOME" TMPDIR="$TMPDIR" \
  WASM_BINDGEN_BIN="$WASM_BINDGEN_BIN" WASM_OPT_BIN="$WASM_OPT_BIN" ESBUILD_BIN="$ESBUILD_BIN" \
  /bin/bash ops/rust-worker-build.sh 2>&1 | tee "$TASK/build.log"
ls -la "$XDG_CACHE_HOME" ~/Library/Caches ~/.cache 2>/dev/null | grep -i worker-build > "$TASK/worker-build-cache.after" || true
diff "$TASK/worker-build-cache.before" "$TASK/worker-build-cache.after" && echo "no worker-build download"
```

What the script does (`ops/rust-worker-build.sh`):

- Refuses non-ARM64 hosts (lines 7-11), dotenv files in the checkout or any ancestor (13-21), non-task-local tool directories (23-26), missing tool overrides (27-29), missing lockfiles (30) and a dirty tree (31-32).
- Moves `CARGO_TARGET_DIR` to `<dir>/exact-worker/<sha>-<tree>` (37-38).
- Verifies Bun 1.4.0, worker-build 0.8.7, wasm-bindgen 0.2.129, cf and wrangler (40-48).
- On Darwin, runs `worker-build --release --locked` through `rustVerificationEnvironment` (`CARGO_NET_OFFLINE`, rustc 1.98.1 check; lines 53-61).
- Runs `tools/cloudflare/verify-build-identity.ts <sha>`, which boots the artifact in the pinned workerd behind the fixture-network boundary and requires the served Git SHA to equal `$SHA` (line 66; `verify-build-identity.ts:21-47`).
- Runs `cf build` and `cf deploy --prebuilt --dry-run` (lines 67-68).
- Writes the receipt.

Receipt: `$SRC/.data/rust-worker-build/$SHA/receipt.json`, created with flag `wx`, so a second build of the same SHA in the same checkout fails rather than overwrite (line 89). Schema `{version:1, gitSha, gitTree, worker:"uos-ai-rust-candidate", accountId, compatibilityDate:"2026-10-18", tools:{bun,workerBuild,wasmBindgen,cf,wrangler}, files:[{path,sha256}]}`. `files` covers every tracked file under `rust`, `static`, `package.json`, `bun.lock`, `cloudflare.config.ts`, `wrangler.config.ts`, both ops scripts, every file under `.cloudflare/output/v0`, and `build/index.js` and `build/index_bg.wasm` (lines 76-89).

Record:

```sh
shasum -a 256 ".data/rust-worker-build/$SHA/receipt.json"
PATH="$BUN:/usr/bin:/bin" bun --no-env-file --no-install -e 'const r=await Bun.file(`.data/rust-worker-build/${process.argv[1]}/receipt.json`).json(); if(r.gitSha!==process.argv[1]) throw 0; for (const f of r.files) if (/gateway-worker\/(build\/|entrypoint)|^\.cloudflare\/output\/v0\/config\.json$/.test(f.path)) console.log(f.sha256, f.path)' "$SHA"
gzip -9 -c rust/crates/gateway-worker/build/index_bg.wasm | wc -c    # e2e: 4,111,972 at 66c04d3; must be < the paid limit
```

Acceptance for step D: script exit 0, receipt present, no worker-build cache growth. Optionally, compare `index_bg.wasm` with the CI artifact for the same SHA. Cross-host reproducibility is not established; a mismatch is a finding, not automatically a failure.

### Step E. Non-production candidate deployment

Claims required: the step D receipt for `$SHA` is in `$SRC`, and the step B CI is green for `$SHA`. The owner has authorized use of the Cloudflare token and the candidate secrets. The Worker is `uos-ai-rust-candidate` on `workers.dev` (`cloudflare.config.ts:12`). It has no routes, so production is untouched.

```sh
cd "$SRC"
read -rs CLOUDFLARE_API_TOKEN; export CLOUDFLARE_API_TOKEN     # explicit; never from any .env (ops/rust-worker-deploy.sh:9)
env -i HOME="$HOME" PATH="$BUN:/usr/bin:/bin" CLOUDFLARE_API_TOKEN="$CLOUDFLARE_API_TOKEN" \
  /bin/bash ops/rust-worker-deploy.sh 2>&1 | tee "$TASK/deploy.log"
```

`ops/rust-worker-deploy.sh`:

- Re-checks the token, lockfiles, clean inputs, dotenv absence and Bun 1.4.0 (lines 9-23).
- Re-hashes every receipt file against disk and requires the receipt's SHA, tree, worker, account and date to match `HEAD` (25-35).
- Runs `cf deploy --prebuilt --message "Rust gateway <sha>"` with `CLOUDFLARE_ACCOUNT_ID` fixed and wrangler config, cache and logs under `.data/rust-worker-tools/` (37-43).

It invokes no Rust tool. Record the `workers.dev` URL from `deploy.log` as `CAND`.

**Secrets.** The repository contains no secret-provisioning command. Check the pinned CLI's own help first (`env -i HOME="$HOME" PATH="$BUN:/usr/bin:/bin" CLOUDFLARE_API_TOKEN="$CLOUDFLARE_API_TOKEN" bun --no-env-file --no-install run cf --help`; whether it has a `secret` subcommand is unconfirmed). Otherwise use the pinned wrangler, whose `secret put` prompts without echo:

```sh
w() { env -i HOME="$HOME" PATH="$BUN:/usr/bin:/bin" CLOUDFLARE_API_TOKEN="$CLOUDFLARE_API_TOKEN" CLOUDFLARE_ACCOUNT_ID="$CF_ACCOUNT_ID" \
  XDG_CONFIG_HOME="$SRC/.data/rust-worker-tools/config" "$SRC/node_modules/.bin/wrangler" "$@"; }
openssl rand -base64 32 | tr -d '\n' > "$TASK/candidate-client-token" && chmod 600 "$TASK/candidate-client-token"
openssl rand -base64 32 | tr -d '\n' > "$TASK/candidate-admin-token"  && chmod 600 "$TASK/candidate-admin-token"
w secret put UOS_AI_TOKEN      --name "$WORKER" < "$TASK/candidate-client-token"   # NEW candidate-only client token
w secret put DENO_DEPLOY_TOKEN --name "$WORKER" < "$TASK/candidate-admin-token"    # NEW candidate-only admin bearer (gateway-auth/src/bootstrap.rs:63)
w secret put UOS_AI_PASSPORT_CLIENT_ID    --name "$WORKER"   # from step A4
w secret put UOS_AI_PASSPORT_ORIGIN       --name "$WORKER"   # exactly $CAND (https, no path)
w secret put UOS_AI_PASSPORT_REDIRECT_URI --name "$WORKER"   # exactly $CAND/auth/callback
for k in DEEPSEEK_API_KEY CEREBRAS_API_KEY LITHOSAI_API_KEY OPENROUTER_API_KEY SURPLUS_API_KEY METERED_API_KEY CLAUDE_API_KEY; do
  w secret put "$k" --name "$WORKER"     # paste a candidate-only key; owner-authorized (credential residence change)
done
w secret list --name "$WORKER"           # names only
```

- Never reuse production `UOS_AI_TOKEN` or `DENO_DEPLOY_TOKEN` values for the candidate. Provider keys should be **new candidate-only keys**, so that revoking them later cannot affect the VPS. Reusing a VPS key requires explicit owner authorization, and the key must be read without printing it.
- **Unconfirmed:** whether a later `cf deploy --prebuilt` keeps secrets that the config does not declare (B7). After every redeploy, re-run `secret list` and probe F2.
- `METERED_API_KEY` serves OpenLux (`src/routing_host.rs:400`, `src/catalog.rs:20`, with fallback `OPENLUX_API_KEY`).
- Codex subscription is **not** available on the candidate: the authority is empty and there is no native carrier (B6). Requests for Codex-family models therefore fall through to paid tiers.

### Step F. Candidate acceptance probes

Claims required: step E completed for `$SHA`. The owner has authorized a bounded provider spend; the suggested ceiling is listed per probe. `read -rs TOKEN` holds the candidate client token. Record every response with its timestamp and `$SHA`.

```sh
export CAND=https://uos-ai-rust-candidate.<account-subdomain>.workers.dev
read -rs TOKEN
H=(-H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json")
```

**F1. Health identity.**

```sh
curl -sS -D "$TASK/health.h" "$CAND/health" -o "$TASK/health.json"
PATH="$BUN:/usr/bin:/bin" bun --no-env-file -e 'const [sha,b,h]=process.argv.slice(1);const j=await Bun.file(b).json();const t=await Bun.file(h).text();
if(j.release.git_sha!==sha||j.release.deployment_id!==`cloudflare-${sha}`) throw new Error("body identity");
for (const [n,v] of [["x-uos-git-sha",sha],["x-uos-deployment-id",`cloudflare-${sha}`]]) if(!new RegExp(`^${n}: ${v}\\r?$`,"mi").test(t)) throw new Error(n); console.log("identity ok")' "$SHA" "$TASK/health.json" "$TASK/health.h"
```

The e2e doc row 1 shows the body and header shape. This is the Worker equivalent of the VPS `vps-<sha>` rule.

**F2. Models.** `curl -sS -o /dev/null -w '%{http_code}\n' "$CAND/v1/models"` must print `401`. `curl -sS "${H[@]}" "$CAND/v1/models" > "$TASK/models.json"` must return `{"object":"list","data":[…]}` with no extra fields. Group the `id` values by `owned_by` and pick the cheapest model per configured provider. The Codex-native shape (`?client_version=X.Y.Z`) is a separate contract (AGENTS.md).

**F3. Inference, one non-stream and one stream request per configured provider.** Ceiling: about 2 × 7 requests of at most 256 output tokens each.

```sh
probe() { # $1 model, $2 stream(true|false), $3 max tokens
  curl -sS -D - "${H[@]}" "$CAND/v1/chat/completions" \
    -d "{\"model\":\"$1\",\"stream\":$2,\"max_completion_tokens\":$3,\"messages\":[{\"role\":\"user\",\"content\":\"Reply with the single word OK.\"}]}"
}
probe "<deepseek model>" false 16; probe "<deepseek model>" true 16
# repeat for Cerebras, Lithos, OpenRouter, Surplus, OpenLux and Claude models from F2
```

- Check `x-uos-upstream` and the provider correlation header to confirm which provider answered.
- Use 256 output tokens for reasoning models. Cerebras `gpt-oss-120b` with a too-small budget returns 502 `cerebras_upstream_invalid_response`, by design (AGENTS.md). Its `stream:true` reply is a buffered replay, not incremental output.
- Repeat one model through `/v1/responses` (`{"model":…,"input":"Reply with OK.","max_output_tokens":16,"stream":…}`).
- Codex subscription cannot be probed here (B6).

**F4. Decisions route.** One request. The body shape comes from the validator (`rust/crates/gateway-decisions/src/request.rs:128-320`) and is not live-proven:

```sh
curl -sS "${H[@]}" "$CAND/v1/decisions" -d '{"model":"<OpenRouter decision model id>","state":"The sky is blue.","questions":{"q":{"type":"noul","instructions":"Is the state about the sky?","criteria":{"true":"yes","false":"no"}}}}'
```

A cold OpenRouter catalog can answer 400 `param:model` until the background warm completes (parity drift doc, residual differences). Retry once after about 30 s; do not loop.

**F5. Client-abort cancellation (hard gate, Part 1).** Run `w tail "$WORKER" --format json > "$TASK/tail.jsonl"` in a second terminal. The Cloudflare tail event fields (`outcome`, `logs`, `exceptions`) are taken from Cloudflare's tail format and were not exercised here.

- **F5a (path B, mid-stream).** Use OpenRouter, which reports per-generation usage:

  ```sh
  curl -sS -N --max-time 4 "${H[@]}" "$CAND/v1/chat/completions" \
    -d '{"model":"<openrouter model>","stream":true,"max_completion_tokens":4000,"messages":[{"role":"user","content":"Count from 1 to 3000, one number per line."}]}' \
    | tee "$TASK/abort-a.sse"; echo "curl exit=$?"     # expect 28 (timeout)
  grep -o '"id":"[^"]*"' "$TASK/abort-a.sse" | head -1   # the generation id
  ```

  On the provider side, look the generation up in the OpenRouter dashboard Activity, or with `curl -sS -H "Authorization: Bearer <candidate OpenRouter key>" "https://openrouter.ai/api/v1/generation?id=<gen-id>"` (field names not confirmed here). **Pass:** completion tokens are far below the roughly 6,000 a full count needs, and/or the generation is marked cancelled. The tail event ends within seconds of the abort, and `curl "$CAND/health"` returns 200 afterwards. **Fail:** the provider reports the full completion. Stop the promotion.

- **F5b (path A, before the first byte).** Same request, `--max-time 1`, against a model with a slow first token, such as a reasoning model at high effort. Check the provider side again. If only F5a passes, finding F1 is confirmed and step H stays blocked.

- Optionally repeat F5a on DeepSeek (`ubiquity/deepseek-flash`). DeepSeek's dashboard aggregates usage per day, so the evidence is weaker.

**F6. Passport sign-in to `/admin`.** Open `$CAND/auth/login?return_to=%2Fadmin` in a browser. Expect a 302 to `passport.ubq.fi/authorize` with `origin` and `redirect_uri` equal to the bound values, and `__Host-` state and verifier cookies (e2e row 11). Complete sign-in. `/admin` must load signed-in, and `GET /admin/api-keys` must return 200 in the session. Passkeys are not expected to work on `workers.dev`. Passkey ceremonies trust RP `ai.ubq.fi` and only the additional origin `https://rust-candidate.ai.ubq.fi` (`src/host.rs:52-61`). That hostname is not routed to this Worker, and serving it needs a DNS or custom-domain change, which is not authorized.

**F7. Admin bearer.** `curl -sS -H "Authorization: Bearer $(cat "$TASK/candidate-admin-token")" "$CAND/admin/providers/selection"` must return 200 (e2e row 30). Do not run admin mutations against the candidate beyond what F6 or F7 need.

Acceptance for step F: F1-F7 pass, F5a and F5b show provider-side cancellation, and every receipt names `$SHA`. A failed probe means no step H.

### Step G. Data migration rehearsal

**The deployed path now exists (B3, B5): `docs/research/rust-state-migration-2026-11-05.md`.** Steps 1-5 below are its capture, selection and staging; that runbook adds the authenticated import, the export verification and the reverse. Step 6 remains the isolated local workerd rehearsal.

Claims required: owner authorization to read a production database copy, which contains credentials and refresh material. For a true stopped-source capture, also an authorized downtime window. Private-copy rules (DECISIONS 2026-10-25/27) and the Bun/Deno codec rules (AGENTS.md) apply. Never touch `.data/kv.sqlite3` (legacy Deno), and never convert a live database.

1. **Consistent backup on the VPS** (`ops/README.md` "Data migration and recovery": use a consistent SQLite backup or stop the gateway; a plain copy can omit the WAL):

   ```sh
   ssh "$VPS" "cd $VPS_ROOT && umask 077 && mkdir -p .data/rust-rehearsal && \
     sqlite3 .data/kv-bun.sqlite3 \".backup '.data/rust-rehearsal/kv-bun-$(date -u +%Y%m%dT%H%M%SZ).sqlite3'\" && \
     sha256sum .data/rust-rehearsal/*.sqlite3"
   ```

   The `sqlite3` CLI's online backup API produces a consistent copy with no WAL. Its availability on the VPS is unconfirmed. `tools/cloudflare/private-bun-copy.ts` is the reviewed private-copy operator, but it is bound to the Mac's signed Python role (DECISIONS 2026-10-27). A copy taken from a live service proves neither stopped-source currentness nor custody (DECISIONS 2026-10-25). It is acceptable for a rehearsal and **not** for adoption. For adoption, `systemctl stop ai-ubq-fi.service` inside the authorized window, then copy, so the WAL is absent or empty.

2. **Transfer privately** to the Mac task directory, mode 600, and verify the hash on both ends.

3. **Complete native capture** (`rust/crates/gateway-migration/examples/native-operator-capture.ts:220-240`). It refuses a source with a non-empty WAL or a rollback journal (`src/kv-conversion/convert.ts:51-63`):

   ```sh
   bun --no-env-file --no-install rust/crates/gateway-migration/examples/native-operator-capture.ts \
     --source "$TASK/kv-bun-copy.sqlite3" --staging-parent "$TASK/private" --source-git-sha <serving VPS sha from its /health> \
     --captured-at-ms <ms> --destination "$TASK/private/capture.json" --receipt "$TASK/private/capture.receipt.json"
   ```

4. **Verify, then select the bounded core state.** Both commands read private JSON on stdin only (`examples/native-capture-verify.rs`, `examples/native-core-state-manifest.rs`; input `{v,capture,directory,expected_entries,expected_sha256}`):

   ```sh
   cd rust && cargo run --locked -p gateway-migration --example native-capture-verify < "$TASK/private/verify.input.json"
   cd rust && cargo run --locked -p gateway-migration --example native-core-state-manifest < "$TASK/private/manifest.input.json"
   ```

   Compare the counts with `CORE_STATE.md`: the 2026-10-22 snapshot selected 2,317 of 140,220 rows and needed 4,640 Durable Object writes. The new snapshot will differ. Selection refuses at 100,000 or more writes.

5. **Stage frames.** This requires current native custody registrations, `{home, fence{owner, account_id, generation_hash, authority_revision}}`, for every Codex account (`examples/native-stage-frames.rs:40-52`, `118-140`). Managed refresh material is rejected without custody (`gateway-migration/README.md` paragraph 10). No credential refresh or rotation is permitted.

   ```sh
   cd rust && cargo run --locked -p gateway-migration --example native-stage-frames < "$TASK/private/frames.input.json"
   ```

6. **Apply to an isolated local workerd Durable Object**, with `tools/cloudflare/state-migration-stage-driver.ts` (input `{v,source_sha,source_tree,run_directory,worker_js,worker_wasm,workerd_binary,stage_apply,frames,receipt}`, lines 24-36). It hard-codes the task root `/private/tmp/ai-rust-wasm-20261018` (line 12). Use the step D `build/index.js` and `build/index_bg.wasm` hashes.

7. **Validate locally.** The receipts must show `activation_authorized:false`. Start the same artifact against that state directory (the e2e harness pattern) and check: API-key authentication with a migrated key, `/admin/api-keys` count, current API-window counters, paid-fallback pending totals, and a latest inverse export (`README.md` paragraph 4).

**Verification on Cloudflare** uses the state-transfer import and export (state-migration runbook steps 3-4), not `kv:import-http` (B4). The target must be a new empty Durable Object that has never served traffic; importing production keys into the current candidate would also make its Durable Object the future production target, because `cloudflare.config.ts` has only one Worker name.

**Still open for a real rehearsal and adoption:**

1. A separate rehearsal Worker name, or a parameterized name. Today the name `uos-ai-rust-candidate` is hard-coded in `cloudflare.config.ts:3`, `ops/rust-worker-build.sh:89` and `ops/rust-worker-deploy.sh:30`.
2. Built: the authenticated, intent-gated import route and operator CLI (DECISIONS 2026-11-05 "Authenticated operator state transfer").
3. Built: the export route, `state-transfer-reverse` and `kv:restore-native`, so that step I preserves writes.
4. Measure one ~4 MiB append chunk against the Workers Free per-request CPU limit on the rehearsal Worker.
5. A way to reset or retire the rehearsal Durable Object. For example, a Durable Object `deleted_classes` migration on the rehearsal Worker only. No such migration tooling exists.

### Step H. Cutover

**Blocked until B1-B9 are closed and steps F and G are accepted.** The sequence below is the target procedure. Where it depends on missing tooling, it says so.

Claims required: a DECISIONS entry authorizing production cutover. A green CI and build receipt for the exact `$SHA` already accepted in step F. F5a and F5b pass. The B3 tooling exists and was rehearsed. Codex credential ownership has moved to exactly one owner (B6). Scheduled-maintenance parity is decided (B8). The rollback path (step I) is built and rehearsed. Owner authorization is recorded for live data conversion, production secrets on Cloudflare, the route change and the downtime window.

1. **Hold the VPS deployment lock** for the whole window, so no VPS release changes the rollback target. `.data/deploy.lock` is a POSIX `flock` (`ops/deploy-lock-bun.ts:5-21`):

   ```sh
   ssh -t "$VPS" "cd $VPS_ROOT && exec flock -n .data/deploy.lock -c 'echo LOCK HELD; exec sleep 7200'"
   ```

2. **Production secrets on the Worker.** Set `UOS_AI_TOKEN` to the production client token set and `DENO_DEPLOY_TOKEN` to the production admin set, using the step E `secret put`. Swap provider keys only as authorized. Then redeploy the identical receipt (`ops/rust-worker-deploy.sh`) and re-run F1.
3. **Stop VPS writers** (downtime starts): `ssh "$VPS" "sudo systemctl stop ai-ubq-fi.service"`. Confirm that `.data/kv-bun.sqlite3-wal` is absent or empty.
4. **Capture, select and stage** exactly as in step G 1-5, from the stopped database.
5. **Adopt into the deployed Durable Object** with the state-transfer import, then verify through the export (state-migration runbook steps 3-4). Do not substitute `kv:import-http`.
6. **Validate on `$CAND`** with production credentials: F1, F2, one F3 request, and a migrated API key that authenticates.
7. **Flip the route.** Today Cloudflare has the proxied `ai.ubq.fi` A record pointing to `129.158.58.222`, plus a script-less Worker route `ai.ubq.fi/*` that excludes the hostname from the `*.ubq.fi/*` wildcard Worker (`ops/README.md` "DNS and TLS"; AGENTS.md). Keep the A record proxied and unchanged, and assign the existing `ai.ubq.fi/*` route to `uos-ai-rust-candidate`. Cloudflare API v4; the route endpoints are standard but were not exercised in this repository, and the token needs Workers Routes edit permission on the zone:

   ```sh
   ZONE=<ubq.fi zone id>
   curl -sS -H "Authorization: Bearer $CLOUDFLARE_API_TOKEN" "https://api.cloudflare.com/client/v4/zones/$ZONE/workers/routes" > "$TASK/routes.before.json"
   ROUTE=<id of the pattern "ai.ubq.fi/*" route from routes.before.json>
   curl -sS -X PUT -H "Authorization: Bearer $CLOUDFLARE_API_TOKEN" -H "Content-Type: application/json" \
     "https://api.cloudflare.com/client/v4/zones/$ZONE/workers/routes/$ROUTE" -d "{\"pattern\":\"ai.ubq.fi/*\",\"script\":\"$WORKER\"}"
   ```

   Leave the wildcard Worker and every other hostname unchanged. Never point `ai.ubq.fi` at Deno Deploy.

8. **Public verification** (the Worker equivalent of the VPS acceptance rule): `https://ai.ubq.fi/health` must return the body and both headers with `$SHA` and `cloudflare-$SHA`, using the F1 checker against `https://ai.ubq.fi`. Then send one **direct authenticated inference request from the Mac** to `https://ai.ubq.fi/v1/responses` and one to `$CAND`. Both must succeed with `x-uos-upstream` present. Confirm that existing clients (the Codex `uos` profile) still work. A health response alone is not acceptance.
9. **Keep the VPS stopped, not deleted.** Its release, `.env`, `.data/kv-bun.sqlite3` and legacy `.data/kv.sqlite3` stay untouched as the rollback source. Release the lock from H1 only after acceptance, or after rollback.

### Step I. Rollback

1. **Freeze Worker writes and restore the origin route.** Use the previously rehearsed write-freeze mechanism to reject new state-changing requests across the production hostname, `workers.dev` and every other ingress, stop native-owner publications, and drain admitted requests and their durable settlement/background writes. Keep the authenticated export route available to the operator. A route change alone does not quiesce the Worker. If that complete freeze cannot be established, stop the rollback here. Keep the VPS service stopped, then restore the saved script-less exclusion route; public requests may be unavailable until the latest-state restore completes.

   ```sh
   curl -sS -X PUT -H "Authorization: Bearer $CLOUDFLARE_API_TOKEN" -H "Content-Type: application/json" \
     "https://api.cloudflare.com/client/v4/zones/$ZONE/workers/routes/$ROUTE" -d '{"pattern":"ai.ubq.fi/*"}'
   ```

2. **Recover the latest state before restarting the VPS.** Restarting on the original stopped database would discard every write the Worker accepted after H5, including API-key quota windows, paid-fallback accounting, new keys and passkey counters. While the Worker remains frozen and the VPS remains stopped, export the latest Worker state with two matching full passes, reverse with the current step G custody registrations, and run `kv:restore-native` into a NEW codec-marked Bun file. Validate its raw keys, revisions, exact absolute expiries and typed values, preserve the original stopped database and private receipts, and place only the validated NEW store at `.data/kv-bun.sqlite3` under the held deployment lock (state-migration runbook step 6). Close the transfer intent after recovery. If any export, reverse, validation or placement fails, keep the VPS stopped and retain all evidence; never start the stale original store. Rehearse this complete path before H.
3. **Start and verify the recovered VPS.** Only after I2 succeeds, start `ai-ubq-fi.service` and check its localhost health. Verify `https://ai.ubq.fi/health` shows the retained VPS release's full SHA and `vps-<sha>` in the body and both identity headers, then send one authenticated Mac-to-VPS request through `curl --resolve ai.ubq.fi:443:$VPS_IP` with the Cloudflare Origin CA root trusted (`ops/README.md` "DNS and TLS"). Retain the frozen Worker and recovery artifacts until acceptance.

   ```sh
   ssh "$VPS" "sudo systemctl start ai-ubq-fi.service && curl --fail -sS http://127.0.0.1:7999/health"
   ```

4. The Bun/Deno codec rules are unaffected. The VPS keeps `.data/kv-bun.sqlite3` (Bun/JSC marker). Returning the VPS itself to Deno still needs the stopped-state `bun-to-deno` conversion with `.data/bun-rollback-receipt.json` (`ops/README.md` "Bun runtime and offline cutover"). That procedure is independent of this Worker.

### 2.2 Pre-step checklist

| Before | Every one of these must be true, with evidence bound to `$SHA`                                                                                                                                                                                                                         |
| ------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| B      | A2 bindings PR merged. `rust-migration` → `development` merged with a merge commit. No workflow dispatched or rerun for validation.                                                                                                                                                    |
| C      | `Gateway CI` automatic post-merge run green for `$SHA`, including `rust-native`, `rust-wasm` and `validate`. Darwin tool pins accepted (A3).                                                                                                                                           |
| D      | Fresh clone at `$SHA`, clean, no dotenv in the checkout or any ancestor. `bun install --frozen-lockfile` with both lockfiles present and unmodified. Tool versions verified. No `CLOUDFLARE_API_TOKEN` in the build environment.                                                       |
| E      | Receipt `.data/rust-worker-build/$SHA/receipt.json` exists with recorded hash. `verify-build-identity` passed. No worker-build cache download. Paid plan active (A1). Cloudflare token supplied interactively. Candidate secrets authorized; new candidate client and admin tokens.    |
| F      | `/health` shows `cloudflare-$SHA` on the candidate. Provider-spend authorization recorded with its ceiling.                                                                                                                                                                            |
| G      | Owner authorization to read a credential-bearing database copy. For an adoption-grade capture: authorized downtime and a stopped source with no WAL. Native custody registrations current. No refresh or rotation planned.                                                             |
| H      | DECISIONS cutover entry. B1-B9 closed. F1-F7 passed on the same `$SHA`. G rehearsal accepted. B3 state-transfer import and B5 reverse rehearsed on a new empty Worker target, CI-green. Single Codex credential owner. VPS deploy lock held. Route ID and pre-change route JSON saved. |
| I      | Pre-change route JSON available. VPS release and stopped database untouched since H3. Rollback authorization for the B5 reverse; a quiesced Worker for the export.                                                                                                                     |

### 2.3 Not confirmed from code

- Cloudflare behavior: whether `request.signal` fires after a streamed Response is returned (F1); whether a client disconnect cancels the response `ReadableStream` promptly; `request_signal_passthrough` semantics; the tail event field names; current Workers size and plan limits.
- Whether `worker-build 0.8.7` honors `WASM_BINDGEN_BIN`/`WASM_OPT_BIN`/`ESBUILD_BIN`, and where it caches on macOS.
- Whether `cf 1.0.0-beta.12` has a `secret` subcommand, and whether `cf deploy --prebuilt` preserves secrets the config does not declare.
- The binaryen version_132 macOS asset name; whether `sqlite3` is installed on the VPS; the `ubq.fi` zone ID and route ID; the candidate `workers.dev` subdomain; OpenRouter generation-API field names.
- Whether the Mac and Linux ARM64 Worker builds of one SHA are byte-identical.

## Addendum 2026-11-05: VPS exact-source build procedure (verified)

The VPS (Linux ARM64) built `47e8973fc05a921381f0de37a34bc6b447ab19b9` with `ops/rust-worker-build.sh` in 471 s, peak target 735 MiB, all three tool overrides honoured and nothing downloaded; `cf build` and `cf deploy --prebuilt --dry-run` succeeded without a token. The task-local toolchain lives under `/home/codex/rust-worker-prep/` and is reached as `/var/tmp/rust-worker-prep` (a symlink; re-create it with `ln -sfn /home/codex/rust-worker-prep /var/tmp/rust-worker-prep` if systemd-tmpfiles ages it out). Environment is stored in `/home/codex/rust-worker-prep/worker-build-env.sh` (no secrets).

```sh
T=/var/tmp/rust-worker-prep
export RUSTUP_HOME=$T/rustup CARGO_HOME=$T/cargo CARGO_TARGET_DIR=$T/target XDG_CACHE_HOME=$T/cache TMPDIR=$T/tmp
export RUSTUP_TOOLCHAIN=1.98.1-aarch64-unknown-linux-gnu CARGO_INCREMENTAL=0 CARGO_BUILD_JOBS=2
export WASM_BINDGEN_BIN=$T/cargo/bin/wasm-bindgen WASM_OPT_BIN=$T/tools/binaryen/bin/wasm-opt ESBUILD_BIN=$T/tools/esbuild/bin/esbuild
export PATH=$RUSTUP_HOME/toolchains/1.98.1-aarch64-unknown-linux-gnu/bin:$CARGO_HOME/bin:/usr/local/bin:/usr/bin:/bin
unset NODE_OPTIONS
SHA=<full 40-hex SHA>
df -h /                                   # keep >= 4.5G free for the 3G floor
SRC=/home/codex/rust-worker-prep/checkout-$SHA
GIT_CONFIG_GLOBAL=/dev/null git clone --no-checkout https://github.com/ubiquity/ai.ubq.fi.git "$SRC"
git -C "$SRC" checkout --detach "$SHA"
cd "$SRC" && bun install --no-env-file --frozen-lockfile && cargo fetch --manifest-path rust/Cargo.toml --locked
test -z "$(git status --porcelain --untracked-files=all)"
env -i HOME="$HOME" bash -c '. /home/codex/rust-worker-prep/worker-build-env.sh; cd '"$SRC"'; exec timeout 3600 nice -n 19 ionice -c3 /bin/bash ops/rust-worker-build.sh' 2>&1 | tee /home/codex/rust-worker-prep/build-$SHA.log
grep -E 'Using custom (Wasm Bindgen|Wasm Opt|Esbuild)' /home/codex/rust-worker-prep/build-$SHA.log
sha256sum ".data/rust-worker-build/$SHA/receipt.json" rust/crates/gateway-worker/build/index_bg.wasm
rm -rf /var/tmp/rust-worker-prep/target/*
# Deploy only with authorization, from the same $SRC: CLOUDFLARE_API_TOKEN=... bash ops/rust-worker-deploy.sh
```

Findings: the receipt file is created with `wx`, so a second build of the same SHA in the same checkout fails (use a fresh checkout). ARM64 and x86_64 builds of one SHA differ in `index_bg.wasm` (embedded registry paths, host-dependent crate hashes) and in `index.js` identifier naming, so byte-identical cross-host builds are not established; the deployed artifact is bound to the receipt of the host that built it. Tool pins and archive hashes: binaryen `version_132` aarch64 `c58562417836c5d0493d89bdefc434933bdc097db641b483df86bcfa557a107f`; `@esbuild/linux-arm64` 0.28.2 `a96dbfa41d3ef5dbd1ef22b1c10d5187be9267e86093a870f06402a7ec931596`.
