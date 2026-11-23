# Shared upstream auth follower mode

Base `0c545781` on `claude/cpp-gateway-port`.

## Restart checklist

- [x] Read the runtime, cutover, CI and porting guides and trace the shared credential paths.
- [x] Guard request-time renewal, pool writers and CLI access. Expose follower status and expiry.
- [x] Register scripted runtime coverage for expired and rejected credentials, admin writes, health and Gemini.
- [x] Add stopped-state import with the read-only Codex usage probe and document the operator steps.
- [x] Run the registration check and focused Release and ASan gates. Bind the results to the tested commit and tree.
- [x] Request committed-diff review against `claude/cpp-gateway-port`, address the findings and leave a clean checkout.

## Credential paths at the base

| Path | Credential effect |
| --- | --- |
| `codex/auth_refresh.cpp` and `codex/auth.cpp` | `getValidAuth` shares proactive refresh work by account. `refreshAuthCoordinated` checks native ownership, rereads the pool, and competes for a durable refresh lease. A waiter can adopt the winner's generation. The lease owner sends `POST https://auth.openai.com/oauth/token` and persists by compare-and-set against the account's old generation. The no-KV and unclaimed paths can also call OAuth. `refreshAuthStateless` can call the same endpoint without that lease. A reuse response can require human repair. |
| `codex/responses_runners.cpp`, `codex/responses_operations.cpp`, `models/codex_models_fetch.cpp`, `live/calls.cpp`, and `live/sideband.cpp` | Responses proactively renew and can renew after a 401 or expired 403, including short retries. Chat Completions and other Codex-backed inference use the same Responses transport. Exact-version model discovery and the active upstream health probe can proactively renew and retry a 401. Live calls and sideband use the same proactive validator. |
| `admin/codex.cpp` and `codex/experiment.cpp` | `POST /admin/codex/auth` may refresh the uploaded credential while fetching models, then atomically replaces or adds an account and may replace catalog and runtime records. The POST cache-scope experiment explicitly rotates one slot with stateless refresh and then compares and sets the pool. Its GET reads the experiment baseline. |
| `admin/defaults.cpp` and `cache/kv_migration_base.cpp` | The writable form of `POST /admin/kv-migration/import` accepts the `codex_auth` singleton. A raw pool import can replace the credential without OAuth validation. |
| `codex/auth.cpp`, `codex/native_auth.cpp`, and `codex/native_auth_transport.cpp` | A missing pool can be seeded from `CODEX_AUTH_JSON_B64` or local `~/.codex/auth.json`. Binding a persisted credential to native ownership writes metadata after matching the disk credential. The owned refresh can adopt a newer disk generation or verify the account at the app-server control socket and send `account/read` with `refreshToken: true`. It rereads and validates the generation before persisting. The external-native adoption planner checks identity and generation. |
| `codex/auth_repair.cpp` and `provider/capacity_parse.cpp` | The native repair planner ranks assessed candidates by expiry and freshness, checks upstream authentication and expiry, and applies selected replacements to matching account slots. The account-bound usage check is `GET /backend-api/wham/usage`. The C++ planner has no transport or writer at the base. Capacity reads already use the usage URL. |
| `codex/supervisor_transport.cpp`, `codex/supervisor_inventory.cpp`, and `codex/supervisor_brief.cpp` | Supervisor inventory, output and brief use the configured app-server socket or read local thread data. The socket allowlist includes `account/rateLimits/read`. The separate native-auth transport owns the explicit refresh RPC. A Codex app-server or an independently run CLI owns its own authentication behavior. |
| `provider/gemini.cpp`, `provider/gemini_quota.cpp`, and `runtime/permissions_linux.cpp` | Inference and the quota refresh can run Antigravity `agy`. With its opt-in and a validated install, the runtime grants process execution and access to the private CLI state. The child can renew OAuth and write its state without a gateway refresh call. |
| `provider/claude_code_credentials.cpp` and `admin/claude_code.cpp` | Claude's configured credentials are static API keys or bearer tokens. The gateway can manage its local roster and read usage, but holds no refresh material and implements no rotation for it. |

The separate native-authority engine seeds its own SQL namespace from a supplied routing roster. The sealed gateway startup opens the gateway KV and does not instantiate that engine. Routing, catalog promotion and banked reset also read or fence on the auth pool. Their durable writes concern routing, catalog or quota rather than credentials.

## Serving in follower mode

- Proactive, coordinated and stateless refresh refuse before joining refresh work, acquiring a lease or reaching OAuth. Native delegation also refuses at the transport. The follower reads its persisted pool without seeding it or binding a local credential file.
- A refused renewal is a 401 credential failure inside Codex routing. This allows the normal active-account transition and preserves the full-pool evidence gate for paid fallback. The Codex attempt log and inference request terminal log identify `upstream_auth_follower_stale`. The public error remains the existing auth failure.
- The auth upload, cache-scope experiment and a writable local KV import containing the auth pool return 409. Read-only checks, unrelated migration rows, and quota and account routing operations use their existing paths.
- Follower mode blocks supervisor app-server connections and the Antigravity CLI. The gateway keeps process-execution denial even if the Gemini integration was opted in. `GET /health` adds the mode and the milliseconds to the earliest known copied Codex access-token expiry.

## Stopped-state credential adoption

The launcher verifies the same immutable package and confines the one-shot import. It requires the exact follower switch and two separate owner-private, checkpointed, marked databases. The process gets a read grant for the source database, keeps the usual write grant for the destination `.data`, and starts no listener. The source is opened with SQLite's immutable read-only URI.

The import matches existing account IDs and retains slot order and follower-only accounts. It checks expiry and freshness, uses the repair module's candidate validation with the account-bound usage GET, and uses the existing pool upsert checks for native ownership. The usage request sends the access bearer and account binding and refuses redirects. Any failed validation aborts the batch. Once validated, the import checks the destination pool revision and the serving renewal window again before updating the existing auth singleton and its KV revision in a transaction. It bypasses serving-time queue and expiry maintenance, so unrelated stored values and revisions stay intact. The operator steps are in [CUTOVER.md](../CUTOVER.md#running-side-by-side-on-one-host).

## Committed-diff review

- Base `0c54578151b88da9afe6d014007dbffe1176214b`, head `1820b39a0039c32716e6f9607d611d39b67dfe60`, model `ubiquity/deepseek-flash` from the local Codex configuration. Both refs were fetched. The native Codex 0.171.0 executable ran `codex review --base origin/claude/cpp-gateway-port -c 'sandbox_mode="read-only"' -c 'shell_environment_policy.inherit="none"' -c 'shell_environment_policy.set={PATH="/usr/bin:/bin"}'`.
- An earlier attempt at head `a43b78c05436945414c6ee47c68834eb04cf6394` reached its 540-second deadline without a final review. The following findings are from the completed review at `1820b39a`.

> [P2] Reject imported tokens inside the follower refresh window
>
> The import accepts a replacement whenever its JWT expires after the current instant, but follower inference calls `auth::needsRefresh` before every attempt and `getValidAuth` then throws `CodexAuthFollowerStaleError` for any token within two minutes of expiry (`src/codex/auth.cpp:518-522`, `src/codex/auth_refresh.cpp:604-606`). An import can therefore report success and write the pool while that account is immediately unusable. Require the replacement to outlive the refresh window, or let follower mode use still-valid tokens until actual expiry. This only affects imports of tokens nearing expiry.

The import now checks `auth::needsRefresh` before probing and again inside the write transaction. A regression case supplies a valid-looking token with one minute left and verifies refusal before the usage request and an unchanged destination database.

> [P3] Correct the auth import test path in PORT-MANIFEST
>
> This is the only `tests` entry in `PORT-MANIFEST.json` using a repository-root path; the manifest's direct-test paths are relative to `cpp/gateway` (`tests/...`), as `cpp/gateway/PORTING.md:9` describes. A consumer looking up the direct test will not find it, so use `tests/codex_auth_follower_import_test.cpp`.

The registry now points to `tests/codex_auth_follower_import_test.cpp`.

The same model completed `codex review --commit 3a36e11f338c74d10dd52db872591ee0e594c7a8` against parent `1820b39a0039c32716e6f9607d611d39b67dfe60` with the same configuration flags.

> Delay renewal-window rejection until after token replacement is selected
>
> `auth::needsRefresh(incoming)` runs before the `previousExpiry` comparison, so a leader token already inside the two-minute renewal window aborts the entire import even when that token is older or shorter-lived than the follower's current credential and would be skipped on the next line. This only affects sources containing a near-expiry token that is not selected for adoption, but it contradicts the documented behavior that a rejected replacement aborts the update (`cpp/gateway/docs/CUTOVER.md:22`) and can block otherwise valid imports. Move the freshness check after the newer-token comparison, or perform it only for credentials added to `adopted`; the new regression test uses an expired destination token and does not cover this case.

The import selects newer candidates before checking their renewal window. The mixed-pool regression checks that a newer account is imported while an older source credential is skipped and the follower's fresher credential is retained.

The same model completed `codex review --commit 2c2eea095d6c4b60cf209fde4141059ae3e1c432` against parent `3a36e11f338c74d10dd52db872591ee0e594c7a8` with the same configuration flags. It reported no further findings.

> The change correctly defers the renewal-window rejection until after the superseded-offer comparison, so an older or shorter-lived near-expiry source credential no longer aborts an otherwise valid import. The added regression test exercises that mixed-pool case, and the existing rejection and write-time revalidation paths remain intact.

## Verification

The final code commit is `2c2eea095d6c4b60cf209fde4141059ae3e1c432` and its tree is `250ebe4a6a419fd668dbb3d79baf0c0bd004c4cd`. All 20 CI driver receipts recorded that identity and the same input digest `420682afdb247dbacdd212ec1a0b48e37ace0b65bb7e00c30cdbfe7d83682f6b`. The host was Ubuntu 26.04.1 on Linux ARM64 with Clang 21.1.8, Boost 1.90 and ICU 78.2. SQLite 3.54.0 was built with the workflow's checked source tarball and `-O2 -DSQLITE_DQS=3`. The driver verified that version from inside the fixture boundary.

The following driver commands ran for each filter in the table. The named compiler wrappers invoked the installed Clang through the restored ccache. Tests ran under the driver's Linux network, mount and PID namespace helper via sudo with a fresh home and temp directory, a credential-free environment and loopback mocks.

```sh
export CC=/var/tmp/cppgw-follower-20261120/clang
export CXX=/var/tmp/cppgw-follower-20261120/clang++
export UOS_CI_BUILD_DIR=/var/tmp/cppgw-follower-20261120/build
UOS_CI_JOBS=2 bash cpp/gateway/tools/ci-build.sh --config release "$filter"
UOS_CI_JOBS=4 bash cpp/gateway/tools/ci-build.sh --config asan "$filter"
```

| Filter | Binaries passed per configuration | Cases passed per configuration |
| --- | ---: | ---: |
| `codex_auth` | 9 | 135 |
| `codex_native_auth` | 1 | 31 |
| `codex_external_native_auth` | 1 | 7 |
| `routing` | 10 | 121 |
| `admin` | 19 | 258 |
| `gemini` | 2 | 31 |
| `health` | 3 | 35 |
| `codex_models_fetch` | 1 | 5 |
| `codex_supervisor` | 9 | 159 |
| `openai_telemetry` | 1 | 10 |
| **Total** | **56** | **792** |

The first Release import fixture on `a43b78c0` failed because `readFileSync` supports UTF-8 strings and raw bytes and the fixture requested base64. Commit `1820b39a` compares raw bytes. The final Release and ASan runs passed all four import cases and all six follower HTTP cases. `python3 -B cpp/gateway/tools/links.py --check` resolved all 452 tests. `python3 -B cpp/gateway/tools/cmake-sources.py /var/tmp/cppgw-follower-20261120/registered-sources.cmake` registered 367 sources. `sh -n cpp/gateway/ops/launch-native.sh` and `git diff --check origin/claude/cpp-gateway-port...HEAD` passed.

The bound launcher fixture passed on the same code commit. `env -i PATH=/usr/bin:/bin LANG=C.UTF-8 python3 -B /var/tmp/cppgw-follower-20261120/build-launch-fixture.py` linked the Release objects, native startup, entrypoint and pinned SQLite. The gateway SHA-256 was `7464de26d905102d946ba2a104072afc5f2e258fb92f9fbbbb747cbc994a4508` and the package manifest SHA-256 was `b50fbff51c8aec66bb36aee1c445eef9911ddaab5bc6ca56e9bb106481849d59`. This was a local ARM64 launch fixture. The proof ran `check-import` through `sudo -n unshare --net --mount --pid --fork --mount-proc --propagation private --kill-child=SIGKILL` and `cpp/gateway/cmake/ci/isolate.py`. The fixture's outside-route probe returned `ENETUNREACH` and the gateway reported Landlock ABI 8 and seccomp TSYNC. The sealed launcher refused an import without the switch, rejected one bearer with unchanged database bytes, and adopted the accepted bearer on the next run. It issued two usage GETs and zero OAuth calls, updated one destination KV row plus the global revision, preserved the source snapshot and exposed the new expiry after restart. The running follower returned 409 for the credential upload.
