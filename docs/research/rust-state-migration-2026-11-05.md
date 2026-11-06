# Rust Worker state migration runbook (2026-11-05)

This runbook moves the bounded core state (DECISIONS 2026-10-22 "Workers Free core-state migration excludes historical telemetry") from a stopped VPS Bun store into the deployed Rust Worker's Durable Object, and moves the Worker's current state back into a NEW Bun store for rollback. It closes the tooling gaps B3, B4 and B5 of `docs/research/rust-promotion-runbook-2026-11-05.md`. It grants no authority: every live step below still needs the owner authorizations listed in that runbook (section 2.0) and the DECISIONS entry "Authenticated operator state transfer - 2026-11-05".

## What was built

| Piece                    | Where                                                                                                                                                   | Contract                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Worker import route      | `GET`/`POST /admin/state-transfer/import` (`rust/crates/gateway-state-transfer/src/{http,import,intent}.rs`, routed in `gateway-worker/src/runtime.rs`) | Admin allowlist token (`DENO_DEPLOY_TOKEN`) only, plus header `x-uos-state-transfer-intent` whose SHA-256 must equal the Worker setting `UOS_STATE_TRANSFER_INTENT_SHA256`. With the setting absent the route answers `404 state_transfer_closed`. An import intent names exactly one content-addressed capture id. Each `POST` body is one existing staged frame (`begin`/`append`/`seal`/`bind_owner`, at most 32 MiB) and is applied by the unchanged `gateway_migration::staged::execute_staged`: empty target, ordered absent keys, digest chain, one atomic transition per chunk. Before that call every record's typed value, revision and absolute expiry is validated. `GET` returns the resume status (`empty`/`staging`/`sealed`/`occupied`, `next_sequence`, chain `digest`). An exact retry of the last committed chunk, of `begin`, or of any data frame after `seal` is answered `replayed` with no write; an out-of-order chunk, another capture, a different header or a non-empty target without a staged capture is refused (409). |
| Worker export route      | `POST /admin/state-transfer/export` (`gateway-state-transfer/src/export.rs`)                                                                            | Export intent only. Body `{"v":1,"cursor":null,"limit":1..256}`. Answers one page of raw canonical records `[{key, record}]` in tuple order, at most 8 MiB, with `x-uos-state-export-sha256`, `-count` and `-next-cursor` headers. Refuses a partially staged target and a target without a sealed manifest.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| Operator CLI             | `tools/cloudflare/state-transfer.ts` (`bun --no-env-file run state:transfer`)                                                                           | `intent`, `import`, `export`, `verify`. The admin token and intent are read only from the one command's environment (`UOS_STATE_TRANSFER_ADMIN_TOKEN`, `UOS_STATE_TRANSFER_INTENT`). Import checks the frames file hash, byte size, frame inventory and single capture against the `native-stage-frames` receipt before sending, resumes from the Worker's own status after an unknown delivery outcome, and requires a sealed complete target at the end. Export checks every page hash and count and repeats the full export (default 2 passes) to prove the target was quiescent. Receipts are new owner-only files, HMAC-SHA256-signed with the intent.                                                                                                                                                                                                                                                                                                                                                                                           |
| Reverse step             | `rust/crates/gateway-state-reverse` (example `state-transfer-reverse`)                                                                                  | Verifies the export archive (whole-file hash, record count, strict key order, typed values, absolute expiry), serves it as a read-only query store, and runs the existing `write_private_latest` latest inverse with configured native custody. Writes a NEW private `source.private.json` (NativeCapture v2) and `custody.private.json` (Rust-only control rows).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| Bun restore              | `src/kv-conversion/native-restore.ts` (`bun --no-env-file run kv:restore-native`)                                                                       | Writes a NativeCapture v2 into a NEW codec-marked Bun KV built beside the destination, re-reads it and checks every raw key, revision, exact i64 absolute expiry and typed value, then publishes it with an exclusive hard link. It refuses an existing destination or WAL/SHM/journal sidecar and never opens a live database.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| Worker-level conformance | `tools/cloudflare/state-transfer-conformance.ts` with example `state-transfer-fixture-frames`                                                           | Runs a built gateway Worker on the locked workerd with its SQLite Durable Object and the synthetic fixture: closed route 404, missing admin 401, missing intent 403, import, sealed replay, restart, export with a separate intent and `verify`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |

The removed `local_operator_adopt` and `local_operator_stage` wasm exports had no caller (`state-migration-stage-driver.ts` drives `native-stage-apply` over the Store RPC). The local workerd rehearsal path is unchanged.

## Preconditions and limits

- **A NEW, empty target.** `begin` refuses any row (`occupied`). A candidate Durable Object that has served any request may already hold rows (catalog caches, rate state). Use a Worker name or Durable Object namespace that has never served traffic, and keep it unrouted and unprobed from `begin` until the export verification. The Worker name is still hard-coded (promotion runbook step G "What must be built" item 1).
- **Quiescence.** Neither direction is one snapshot. Import requires the target to be quiescent from `begin` to `seal`. Export requires no Worker writes during both passes; the CLI refuses differing passes.
- **Custody.** Stage frames require current native custody registrations for every managed Codex account (`native-stage-frames`, unchanged). The reverse step reconstructs managed refresh material only from the same configured custody; without registrations it refuses any managed account. No credential is refreshed or rotated by either direction.
- **Workers Free CPU (unmeasured).** `native-stage-frames` packs about 4 MiB per append chunk (`gateway_migration::staged`, `PREFERRED_BATCH_BYTES`). Parsing, validating and hashing a chunk in WASM may exceed the Free plan's per-request CPU limit. Measure on the rehearsal Worker first. A smaller chunk size is a `gateway-migration` change (that crate is at its line ceiling).
- **History.** The forward profile excludes historical telemetry. The rollback store therefore holds core state plus every Worker-era write, not the VPS history. The pre-cutover stopped VPS database stays untouched as the history archive; there is no merge tool.

## Commands

Identities as in the promotion runbook section 2.1 (`$WORKER`, `$VPS`, `$VPS_ROOT`, `$BUN`, the `w` wrangler helper from step E). `$TASK` is an owner-only (0700) task directory on the Mac, `$SRC` the exact-source checkout of the deployed `$SHA`, and `$ORIGIN` the target Worker's exact `https` origin.

### 1. Capture: stop the source, copy, hash

```sh
ssh -t "$VPS" "cd $VPS_ROOT && exec flock -n .data/deploy.lock -c 'echo LOCK HELD; exec sleep 7200'"   # keep open
ssh "$VPS" "sudo systemctl stop ai-ubq-fi.service"
ssh "$VPS" "cd $VPS_ROOT && test ! -s .data/kv-bun.sqlite3-wal && test ! -e .data/kv-bun.sqlite3-journal && umask 077 && \
  mkdir -p .data/state-transfer && cp .data/kv-bun.sqlite3 .data/state-transfer/kv-bun-stopped.sqlite3 && \
  sha256sum .data/kv-bun.sqlite3 .data/state-transfer/kv-bun-stopped.sqlite3"
scp "$VPS:$VPS_ROOT/.data/state-transfer/kv-bun-stopped.sqlite3" "$TASK/kv-bun-stopped.sqlite3" && chmod 600 "$TASK/kv-bun-stopped.sqlite3"
sha256sum "$TASK/kv-bun-stopped.sqlite3"     # must equal both VPS hashes
```

For a rehearsal only, restart the service after the copy. For adoption, the VPS stays stopped until the Worker is accepted or rolled back.

### 2. Stage: complete capture, core-state selection, frames

```sh
cd "$SRC"
bun --no-env-file --no-install rust/crates/gateway-migration/examples/native-operator-capture.ts \
  --source "$TASK/kv-bun-stopped.sqlite3" --staging-parent "$TASK/private" --source-git-sha <VPS /health sha> \
  --captured-at-ms <ms> --destination "$TASK/private/capture.json" --receipt "$TASK/private/capture.receipt.json"
cd rust
cargo run --locked --offline -p gateway-migration --example native-capture-verify < "$TASK/private/verify.input.json"
cargo run --locked --offline -p gateway-migration --example native-core-state-manifest < "$TASK/private/manifest.input.json"
# frames.input.json: {v:1, capture:<selected_capture_path>, directory, frames:"$TASK/private/frames.ndjson",
#   receipt:"$TASK/private/frames.receipt.json", source_git_sha, expected:{entries, sha256:<selected_capture_sha256>},
#   registrations:[{home, fence{owner, account_id, generation_hash, authority_revision}}]}
cargo run --locked --offline -p gateway-migration --example native-stage-frames < "$TASK/private/frames.input.json"
sha256sum "$TASK/private/frames.ndjson"      # FRAMES_SHA256
CAPTURE_ID=$(bun --no-env-file -e 'console.log((await Bun.file(process.argv.at(-1)).json()).summary.capture_id)' "$TASK/private/frames.receipt.json")
```

Check the core-state manifest counts against `CORE_STATE.md` (writes must stay below 100,000).

### 3. Import into the target Worker

```sh
cd "$SRC"
bun --no-env-file run state:transfer intent --scope import --capture-id "$CAPTURE_ID" --out "$TASK/private/import.intent"
# prints {"sha256": ...}; store only the digest on the Worker:
printf %s '<sha256 from above>' | w secret put UOS_STATE_TRANSFER_INTENT_SHA256 --name "$WORKER"
UOS_STATE_TRANSFER_ADMIN_TOKEN="$(cat "$TASK/candidate-admin-token")" UOS_STATE_TRANSFER_INTENT="$(cat "$TASK/private/import.intent")" \
  bun --no-env-file run state:transfer import --origin "$ORIGIN" --frames "$TASK/private/frames.ndjson" --frames-sha256 "$FRAMES_SHA256" \
  --frames-receipt "$TASK/private/frames.receipt.json" --receipt "$TASK/private/import.receipt.json"
```

A failed or interrupted run is rerun with the same command and a new `--receipt` path; it resumes from the Worker's `next_sequence` after checking the chain digest. The receipt records `applied`, `replayed`, `resumed_at`, the final `sealed` status and `activation_authorized: false`.

### 4. Verify counts and hashes through the export

```sh
bun --no-env-file run state:transfer intent --scope export --out "$TASK/private/export.intent"
printf %s '<sha256 from above>' | w secret put UOS_STATE_TRANSFER_INTENT_SHA256 --name "$WORKER"
UOS_STATE_TRANSFER_ADMIN_TOKEN="$(cat "$TASK/candidate-admin-token")" UOS_STATE_TRANSFER_INTENT="$(cat "$TASK/private/export.intent")" \
  bun --no-env-file run state:transfer export --origin "$ORIGIN" --archive "$TASK/private/after-import.ndjson" --receipt "$TASK/private/after-import.receipt.json"
bun --no-env-file --no-install -e 'const [a,b]=Bun.argv.slice(-2); console.log(JSON.stringify({v:1,import_intent:await Bun.file(a).text(),export_intent:await Bun.file(b).text()}))' \
  "$TASK/private/import.intent" "$TASK/private/export.intent" | \
  bun --no-env-file --no-install run state:transfer verify --origin "$ORIGIN" --frames "$TASK/private/frames.ndjson" --frames-sha256 "$FRAMES_SHA256" \
  --archive "$TASK/private/after-import.ndjson" --frames-receipt "$TASK/private/frames.receipt.json" \
  --import-receipt "$TASK/private/import.receipt.json" --export-receipt "$TASK/private/after-import.receipt.json"
w secret delete UOS_STATE_TRANSFER_INTENT_SHA256 --name "$WORKER"     # close the routes
```

`verify` requires bounded private stdin JSON `{v:1,import_intent,export_intent}`, both scoped receipt signatures, the explicitly expected origin, actual frame/archive hashes and page inventory, a sealed complete import, at least two stable export passes and the matching capture manifest. Exported records must equal staged records + 1 sealed manifest (+1 when an owner frame was bound) immediately after import. The old counts-only invocation without private stdin and actual evidence now fails closed; environment credentials cannot substitute for the two intent keys. The pipeline above passes synthetic or real intent values only through private stdin, never command arguments or printed output. For full typed-state equality, also run step 6 on this archive and compare the reverse capture with the step 2 selected capture.

### 5. Optional local acceptance of the same artifact

```sh
cd "$SRC/rust" && cargo run --locked --offline -q -p gateway-state-reverse --example state-transfer-fixture-frames > "$TASK/fixture-frames.ndjson"
cd "$SRC" && bun --no-env-file --no-install tools/cloudflare/state-transfer-conformance.ts \
  --worker-js "$SRC/rust/crates/gateway-worker/build/index.js" --worker-wasm "$SRC/rust/crates/gateway-worker/build/index_bg.wasm" \
  --frames "$TASK/fixture-frames.ndjson" --run-directory "$TASK/conformance-run"
```

### 6. Reverse and rollback without losing Worker writes

Requires the owner's rollback authorization (promotion runbook step I). Route traffic away from the Worker first (step I1), so the export is quiescent; the VPS stays stopped.

```sh
# 6a. Export the Worker's latest state (steps 4's intent, export, receipt; 2 equal passes).
# 6b. Reverse with the same custody registrations used in step 2:
cat > "$TASK/private/reverse.input.json" <<EOF
{"v":1,"now_ms":<ms>,"archive":{"path":"$TASK/private/latest.ndjson","sha256":"<archive_sha256>","records":<records>},
 "registrations":[{"home":"...","fence":{"owner":"...","account_id":"...","generation_hash":"...","authority_revision":"..."}}],
 "artifacts":{"rust_git_sha":"$SHA","rust_git_tree":"<tree>","wasm_sha256":"<index_bg.wasm sha256>","native_sha256":"<reverse binary sha256>","native_nonce_capture_sha256":null},
 "output_directory":"$TASK/private/reverse"}
EOF
cd "$SRC/rust" && cargo run --locked --offline -q -p gateway-state-reverse --example state-transfer-reverse < "$TASK/private/reverse.input.json"
# 6c. NEW Bun KV from the reverse capture; never over an existing file:
cd "$SRC" && bun --no-env-file run kv:restore-native --capture "$TASK/private/reverse/source.private.json" --capture-sha256 <source.sha256 from 6b> \
  --destination "$TASK/private/kv-bun-rollback.sqlite3" --receipt "$TASK/private/restore.receipt.json"
# 6d. Place it on the stopped VPS (owner-authorized; service stopped; deploy lock held):
scp "$TASK/private/kv-bun-rollback.sqlite3" "$VPS:$VPS_ROOT/.data/state-transfer/kv-bun-rollback.sqlite3"
ssh "$VPS" "cd $VPS_ROOT && sha256sum .data/state-transfer/kv-bun-rollback.sqlite3 && \
  mv .data/kv-bun.sqlite3 .data/state-transfer/kv-bun-pre-rollback.sqlite3 && \
  mv .data/state-transfer/kv-bun-rollback.sqlite3 .data/kv-bun.sqlite3 && sudo systemctl start ai-ubq-fi.service"
```

`custody.private.json` holds the Worker's Rust-only control and replay rows; keep it with the receipts as private evidence. It is never written into the Bun store. Keep the legacy `.data/kv.sqlite3` untouched throughout.

## Evidence (this branch, synthetic data only)

- `cargo test -p gateway-state-reverse`: several-chunk import with an exact replay of every committed chunk; import, a post-adoption Worker write, export through the route handler, archive and latest inverse equal the target's own inverse; the reverse capture equals the atomic-adoption inverse with every raw key, original revision and absolute expiry; refusals before any write.
- `cargo test -p gateway-state-transfer`: intent scopes and refusals; export page trimming and resume.
- `bun test tests/state-transfer-cli.test.ts tests/native-capture-restore.test.ts`: frame binding, resume after a lost reply, chain-divergence refusal, export hash checks, signed receipts; NativeCapture to NEW Bun KV and recapture byte-identical, including an `i64::MAX` expiry.
- Worker-level: a debug `worker-build` of this branch on workerd 1.20261012.1 passed `state-transfer-conformance.ts`, and the reverse of its export restored into a NEW Bun KV; its reverse capture was byte-identical to the in-memory atomic-adoption path.

No Cloudflare account, VPS, production database or credential was touched to produce this evidence.
