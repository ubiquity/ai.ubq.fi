# Gateway coroutine and callback lifetime audit, area D

## Contract v1

Audit the C++20 sources directly under `src/`, the twelve assigned trees and the corresponding `include/uos/` headers. The baseline is commit `6d28e858e7ab8b8b1036db4a4adfdade0465ed40`, tree `6abb54014cf79ba7c64b24be4d28dde250c40ccf`, from `agent-jobs/cppgw-integrate-x1-20261118-44302040764-1`. Paths and line numbers below are relative to `cpp/gateway/`.

The owned deliverables are this report, `analysis/lifetime/d/sites.json`, and audit scripts in that directory. Source fixes, production and test edits, TypeScript, workflow changes, deployment, credential handling and live provider probes are outside this audit. Rejected and selected promises retain the existing no-sibling-cancellation semantics. Use only local or in-memory fixtures with a clean environment, empty home and fresh temp directory. Put toolchains, dependencies and outputs outside the checkout.

| Surface                              | Ownership traced                                                                        |
| ------------------------------------ | --------------------------------------------------------------------------------------- |
| HTTP and inference handlers          | Request and response frames, detached jobs, stream and delivery callbacks, and shutdown |
| Admin, auth, catalog and Sentinel    | Awaited versus escaping KV or fetch work, shared caches, abort and replay callbacks     |
| Authority and KV conversion          | Local and RPC modes, queued work, SQLite and file handles, conversion streams           |
| Models, compaction and other helpers | Owning arguments, views, iterators, generator and callback holders                      |

Read the coroutine rules in `PORTING.md`, the portable driver in `docs/CI.md`, and the prior kernel-failure report in `docs/findings/cppgw-clang-tidy-audit-20261117.md`. Use the five requested clang-tidy checks, lexical finders, and direct reading. A grep or tidy hit is a candidate. A site is a distinct lifetime operation or a repeated sequence with identical ownership in one function. Header declarations record the interface owners where relevant. Fixes and out-of-area findings are reported for dispatch.

Acceptance uses a file-by-file commit of the checklist, Markdown verdicts and JSON. Reconcile both ledgers, verify all source lines against the baseline, and check `git diff --check`. The changed surface is audit Markdown, JSON and scripts. Use changed-file formatting and script checks for those deliverables. Select a small sanitizer control if a confirmed defect has a cheap local witness. The CI driver and its ASan shards are the native build reference. A full application build has no bearing on an audit-only source tree identical to the baseline.

The initial unknowns are current ownership after the integration refactors, availability of the pinned dependencies on this runner, and the cost of the Clang pass. Bound setup at 20 minutes. Pilot tooling and retain per-unit logs, with at most four workers and per-unit cutoffs. Continue reading if a translation unit fails or exceeds its cutoff. Each committed file is a restart checkpoint. Stop when the inventory and ranked findings reconcile, or leave the precise pending files and measured limits in the committed report.

## Runner and commands

Ubuntu 24.04.5 LTS, Linux AArch64, four CPUs, 15 GiB RAM. The audit used Clang and clang-tidy 21.1.8 from apt.llvm.org. Dependency preparation completed under the 20-minute cap. The command flags below match the scratch runs. The root paths are temporary except the apt packages and selected SQLite prefix.

```sh
sudo -n apt-get -o Acquire::Retries=1 update
sudo -n apt-get install -y libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev ninja-build pkg-config python3-yaml
curl -fsSLo llvm-key.asc --max-time 40 https://apt.llvm.org/llvm-snapshot.gpg.key
gpg --batch --show-keys --with-colons llvm-key.asc
sudo -n install -m 0644 llvm-key.asc /usr/share/keyrings/cppgw-lifetime-llvm.asc
printf '%s\n' 'deb [signed-by=/usr/share/keyrings/cppgw-lifetime-llvm.asc] https://apt.llvm.org/noble/ llvm-toolchain-noble-21 main' |
  sudo -n tee /etc/apt/sources.list.d/cppgw-lifetime-llvm.list >/dev/null
sudo -n apt-get -o Acquire::Retries=1 -o Acquire::https::Timeout=30 update
sudo -n apt-get -o Acquire::Retries=1 -o Acquire::https::Timeout=30 install -y clang-21 clang-tidy-21
```

The signing-key fingerprint was `6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`. OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5 came from Ubuntu. All three upstream archives passed SHA-256 checks before extraction.

| Archive                                                                                              | SHA-256                                                            | Command run in an external scratch directory                                                                                                                                                                                   |
| ---------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| [Boost 1.92.0](https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2)                 | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` | `./bootstrap.sh --with-libraries=json,container` then `./b2 -j2 --with-json --with-container variant=release link=static runtime-link=shared cxxstd=20 stage`. Produced 869,382-byte JSON and 163,406-byte Container archives. |
| [SQLite 3.54.0](https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz)                          | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` | `./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'`, `make -j1`, `sudo -n make install`, `sudo -n ldconfig`. `pkg-config --modversion sqlite3` returned 3.54.0.                  |
| [ICU 78.3](https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz) | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` | `../icu/source/configure --prefix="$icu/prefix" --disable-tests --disable-samples --disable-extras --disable-static`, then `make -j1`, `make install`. The staged pkg-config returned 78.3.                                    |

The repository's `tools/tidy.sh` verifies the entire routine configuration before running a selected check. LLVM 21 reported 11 unrelated check names from the earlier LLVM 24 snapshot as unknown during `--verify-config`. The area runner uses the original `.clang-tidy` options and header filter with an exact check override. Its `--list-checks` inventory showed all five requested checks. The first run used Ubuntu ICU 74.2 headers and completed 143/143 translation units in 209.645 seconds with zero failures or timeouts. It emitted 100 deduplicated diagnostics including transitive headers, 9 capturing-lambda and 91 reference-parameter. The ICU 78.3 rerun completed 143/143 translation units in 208.652 seconds with the same 100 diagnostics and zero failures or timeouts. The audit scripts and exact bound are reproducible with the following commands.

```sh
python3 -B cpp/gateway/analysis/lifetime/d/audit.py pending
python3 -B cpp/gateway/analysis/lifetime/d/audit.py scan src/admin
rg -n -U 'co_await|co_return|rt::(spawn|all|allSettled|race|any)|addEventListener|setTimeout|setInterval' cpp/gateway/src
clang-tidy-21 --config-file=cpp/gateway/.clang-tidy --verify-config
clang-tidy-21 --config-file=cpp/gateway/.clang-tidy \
  --checks='-*,cppcoreguidelines-avoid-capturing-lambda-coroutines,cppcoreguidelines-avoid-reference-coroutine-parameters,misc-coroutine-hostile-raii,bugprone-dangling-handle,bugprone-use-after-move' --list-checks
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$empty_home" TMPDIR="$fresh_tmp" \
  python3 -B cpp/gateway/analysis/lifetime/d/tidy.py --boost "$boost/boost_1_92_0" --out "$scratch/tidy" --jobs 4 --timeout 90
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$empty_home" TMPDIR="$fresh_tmp" \
  python3 -B cpp/gateway/analysis/lifetime/d/tidy.py --boost "$boost/boost_1_92_0" --icu "$icu/prefix/include" \
  --out "$scratch/tidy-pinned" --jobs 4 --timeout 90
python3 -B cpp/gateway/analysis/lifetime/d/audit.py verify
python3 -B cpp/gateway/analysis/lifetime/d/audit.py verify --complete --tidy-summary "$scratch/tidy-pinned/summary.json"
git diff --check 6d28e858e7ab8b8b1036db4a4adfdade0465ed40
```

Each audit entry binds its source coordinates to the baseline Git blobs. `audit.py verify` reconciles the checklist, Markdown and JSON and checks coverage of 1,719 lexical `co_await` lines, 84 join or escape lines, and 22 callback-registration lines. The pinned Clang receipt anchors 99 in-scope diagnostics directly to verdict lines, 8 capturing-lambda and 91 reference-parameter. The remaining diagnostic is in `include/uos/codex/supervisor_config.hpp:256`, where `rt::async` holds the closure and the closure owns shared state. `audit.py pending` prints the restart order. `audit.py record` writes both ledgers and makes one Git commit per reviewed file.

## Focused sanitizer controls

A scratch harness used the production `rt::race`, `Loop` and Promise identities with 128 synthetic 64 KiB payloads per condition. With a winner and a pending private loser, 128/128 loser/payload pairs remained live on the same running loop. Rejecting the private loser after observing the winner left 0/128 pairs live. Loop teardown released all 128 original pairs. This measures retained owners before teardown, when a process-long event loop still serves requests. The two-condition run exited 0 with no ASan, UBSan or LeakSanitizer diagnostic. Compilation took 20.03 s and execution 0.02 s.

The test source was created outside the checkout as `$source_dir/race_owners.cpp`. Its `Task<Observation>` used `auto winner = std::make_shared<std::array<char, 64 * 1024>>();`, a private `withResolvers<std::shared_ptr<Payload>>()`, and a ready `Promise::resolve(std::move(winner))`. It returned a weak Promise identity and a weak winning payload captured before starting the race. Each trial awaited the race with `co_await racing.wait()`. The control then called `abandoned.reject()`. The driver alternated 128 trials of each condition with `rt::blockOn(loop, sample(settleLoser))`, called `loop.run()`, and counted owners before and after the loop scope. All code used local data.

```sh
scratch=/tmp/cppgw-lifetime-d.gktp0iv5
source_dir="$scratch/race-asan.ngbqeWmT"
trial=$(mktemp -d "$scratch/race-recheck.XXXXXXXX")
mkdir "$trial/home" "$trial/tmp"
gateway="$PWD/cpp/gateway"
boost_dir="$scratch/boost_1_92_0"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$trial/home" TMPDIR="$trial/tmp" \
  /usr/bin/time -f 'compile elapsed %e s' /usr/bin/clang++-21 -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING -DBOOST_JSON_NO_LIB -pthread -I"$gateway/include" -isystem "$boost_dir" \
  "$source_dir/race_owners.cpp" "$gateway/src/rt/async.cpp" "$gateway/src/rt/clock.cpp" "$gateway/src/rt/number.cpp" \
  "$boost_dir/stage/lib/libboost_json.a" -o "$trial/race_owners"
sudo -n unshare --net -- setpriv --reuid="$(id -u)" --regid="$(id -g)" --clear-groups --no-new-privs \
  env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$trial/home" TMPDIR="$trial/tmp" \
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  /usr/bin/time -f 'run elapsed %e s' "$trial/race_owners"
```

The queue ownership harness uses the real admission controller with one active and one queued slot. It takes a weak observer of the queued caller's abort signal. One trial aborts the caller before dropping the loop. The other drops the loop and all strong public owners with the 60,000 ms unreferenced timer still pending. The settled case released its signal, and the abandoned case retained it. LeakSanitizer reported 1,504 bytes in 17 allocations, with exit 1. Clang compilation took 31.36 s and the run took 0.06 s.

```sh
trial=/tmp/cppgw-lifetime-d.gktp0iv5/admission-asan.hVQ2zQOg
gateway="$PWD/cpp/gateway"
boost_dir=/tmp/cppgw-lifetime-d.gktp0iv5/boost_1_92_0
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$trial/home" TMPDIR="$trial/tmp" \
  /usr/bin/time -f 'compile elapsed %e s' /usr/bin/clang++-21 -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING -DBOOST_JSON_NO_LIB -pthread -I"$gateway/include" -isystem "$boost_dir" \
  "$trial/admission_owners.cpp" "$gateway/src/inference_admission.cpp" "$gateway/src/rt/async.cpp" "$gateway/src/rt/clock.cpp" \
  "$gateway/src/rt/number.cpp" "$gateway/src/rt/abort.cpp" "$boost_dir/stage/lib/libboost_json.a" -o "$trial/admission_owners"
sudo -n unshare --net -- setpriv --reuid="$(id -u)" --regid="$(id -g)" --clear-groups --no-new-privs \
  env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$trial/home" TMPDIR="$trial/tmp" \
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=halt_on_error=1 \
  /usr/bin/time -f 'run elapsed %e s' "$trial/admission_owners"
```

The telemetry queue harness invokes the production `detail::waitForProgress` on one loop with an `inFlight` vector holding a fulfilled write. Each trial checks that the write wins and the timer is cleared, empties `inFlight`, and drains ready jobs. After 16 warmups it measures two batches of 256 waits with `__sanitizer_get_current_allocated_bytes()`. The original retained 71,680 bytes per batch while the loop was live, or **280 bytes per wait**. The proposed change in a scratch copy retained 0 bytes in both batches. Both returned to the pre-loop allocation count after teardown and exited 0 with no ASan, UBSan or LeakSanitizer diagnostic. Build times were 21.17 s and 21.09 s. Both runs took 0.02 s. The scratch control header contains only the queue timeout edit shown in the ranked list.

```sh
trial=/tmp/cppgw-lifetime-d-queue.urBQfL2z
gateway="$PWD/cpp/gateway"
boost_dir=/tmp/cppgw-lifetime-d.gktp0iv5/boost_1_92_0
flags=(-std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
  -DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING -DBOOST_JSON_NO_LIB -pthread
  -I"$gateway/include" -isystem "$boost_dir")
for fixture in queue_owners queue_owners_control; do
  include=()
  if [[ "$fixture" == queue_owners_control ]]; then include=(-I"$trial/control"); fi
  env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$trial/home" TMPDIR="$trial/tmp" \
    /usr/bin/time -f 'compile %e s' timeout 120 /usr/bin/clang++-21 "${include[@]}" "${flags[@]}" \
    "$trial/queue_owners.cpp" "$gateway/src/rt/async.cpp" "$gateway/src/rt/clock.cpp" \
    "$gateway/src/rt/number.cpp" "$boost_dir/stage/lib/libboost_json.a" -o "$trial/$fixture"
  trial_run=$(mktemp -d "$trial/run.XXXXXXXX")
  mkdir "$trial_run/home" "$trial_run/tmp"
  sudo -n unshare --net -- setpriv --reuid="$(id -u)" --regid="$(id -g)" --clear-groups --no-new-privs \
    env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$trial_run/home" TMPDIR="$trial_run/tmp" \
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    /usr/bin/time -f "$fixture run %e s" timeout 15 "$trial/$fixture"
done
```

## File checklist

286 first-party files.

- [x] `src/admin/api_key_audit.cpp`
- [x] `src/admin/api_key_mutations.cpp`
- [x] `src/admin/api_keys.cpp`
- [x] `src/admin/capacity_observation_sync.cpp`
- [x] `src/admin/chart_source_proxy.cpp`
- [x] `src/admin/claude_code.cpp`
- [x] `src/admin/codex.cpp`
- [x] `src/admin/codex_reset.cpp`
- [x] `src/admin/defaults.cpp`
- [x] `src/admin/error_log.cpp`
- [x] `src/admin/index.cpp`
- [x] `src/admin/kernel.cpp`
- [x] `src/admin/sentinel_incident.cpp`
- [x] `src/agent_messages.cpp`
- [x] `src/analytics.cpp`
- [x] `src/api_key_policy.cpp`
- [x] `src/api_keys.cpp`
- [x] `src/auth/index.cpp`
- [x] `src/auth/local_admin.cpp`
- [x] `src/auth/local_development_key.cpp`
- [x] `src/auth/passkeys.cpp`
- [x] `src/auth/passport.cpp`
- [x] `src/auth/passport_sessions.cpp`
- [x] `src/auth/relay.cpp`
- [x] `src/authority/admission_state.cpp`
- [x] `src/authority/authority_rpc.cpp`
- [x] `src/authority/contracts.cpp`
- [x] `src/authority/credential_publication.cpp`
- [x] `src/authority/delivery_outbox.cpp`
- [x] `src/authority/durable_store.cpp`
- [x] `src/authority/native_api_key.cpp`
- [x] `src/authority/native_api_key_audit.cpp`
- [x] `src/authority/native_api_key_policy.cpp`
- [x] `src/authority/native_authority.cpp`
- [x] `src/authority/native_authority_api.cpp`
- [x] `src/authority/native_authority_codex.cpp`
- [x] `src/authority/native_authority_contracts.cpp`
- [x] `src/authority/native_authority_kernel.cpp`
- [x] `src/authority/native_authority_paid.cpp`
- [x] `src/authority/native_authority_principals.cpp`
- [x] `src/authority/native_authority_rpc.cpp`
- [x] `src/authority/native_authority_runtime.cpp`
- [x] `src/authority/native_authority_sql.cpp`
- [x] `src/authority/native_codex.cpp`
- [x] `src/authority/native_codex_429.cpp`
- [x] `src/authority/native_codex_capacity.cpp`
- [x] `src/authority/native_codex_contracts.cpp`
- [x] `src/authority/native_codex_evaluation.cpp`
- [x] `src/authority/native_codex_mutations.cpp`
- [x] `src/authority/native_contracts.cpp`
- [x] `src/authority/native_kernel.cpp`
- [x] `src/authority/native_kernel_contracts.cpp`
- [x] `src/authority/native_paid.cpp`
- [x] `src/authority/native_paid_contracts.cpp`
- [x] `src/authority/native_paid_policy.cpp`
- [x] `src/authority/native_principal_contracts.cpp`
- [x] `src/authority/native_runtime_contracts.cpp`
- [x] `src/bounded_response_body.cpp`
- [x] `src/catalog/index.cpp`
- [x] `src/catalog/models.cpp`
- [x] `src/catalog/promotion.cpp`
- [x] `src/catalog/store.cpp`
- [x] `src/catalog/types.cpp`
- [x] `src/config.cpp`
- [x] `src/debug_routing.cpp`
- [x] `src/decisions/handlers.cpp`
- [x] `src/defaults.cpp`
- [x] `src/handler/admission.cpp`
- [x] `src/handler/http.cpp`
- [x] `src/handler/index.cpp`
- [x] `src/handler/serve_handler.cpp`
- [x] `src/handler/terminal_log.cpp`
- [x] `src/handler/terminal_route.cpp`
- [x] `src/health.cpp`
- [x] `src/http.cpp`
- [x] `src/images.cpp`
- [x] `src/inference_admission.cpp`
- [x] `src/inference_deadline.cpp`
- [x] `src/input_normalization.cpp`
- [x] `src/jev_compaction/compaction.cpp`
- [x] `src/jev_compaction/hybrid.cpp`
- [x] `src/kv.cpp`
- [x] `src/kv_conversion/cli.cpp`
- [x] `src/kv_conversion/convert.cpp`
- [x] `src/kv_conversion/cursors.cpp`
- [x] `src/kv_conversion/legacy_codec.cpp`
- [x] `src/kv_conversion/native_schema.cpp`
- [x] `src/kv_conversion/profile_json.cpp`
- [x] `src/kv_conversion/proof.cpp`
- [x] `src/kv_conversion/typed_value.cpp`
- [x] `src/lib/jev_compaction/client.cpp`
- [x] `src/lib/jev_compaction/codex_items.cpp`
- [x] `src/lib/jev_compaction/compact.cpp`
- [x] `src/lib/jev_compaction/jev_codex_smoke.cpp`
- [x] `src/lib/jev_compaction/jev_compaction_smoke.cpp`
- [x] `src/lib/jev_compaction/request.cpp`
- [x] `src/lib/jev_compaction/state.cpp`
- [x] `src/lib/jev_compaction/types.cpp`
- [x] `src/metered_quota.cpp`
- [x] `src/models/catalog.cpp`
- [x] `src/models/codex_models.cpp`
- [x] `src/models/codex_models_availability.cpp`
- [x] `src/models/codex_models_fetch.cpp`
- [x] `src/models/codex_models_whitelist.cpp`
- [x] `src/models/metadata.cpp`
- [x] `src/models/openrouter_models.cpp`
- [x] `src/openai.cpp`
- [x] `src/openai_schema.cpp`
- [x] `src/openai_telemetry.cpp`
- [x] `src/optional_telemetry_queue.cpp`
- [x] `src/quota_projection.cpp`
- [x] `src/recent_model_context.cpp`
- [x] `src/release.cpp`
- [x] `src/request.cpp`
- [x] `src/request_policy.cpp`
- [x] `src/responses_attempts.cpp`
- [x] `src/responses_buffered.cpp`
- [x] `src/responses_failover_stream.cpp`
- [x] `src/responses_handler.cpp`
- [x] `src/responses_native_headers.cpp`
- [x] `src/responses_request.cpp`
- [x] `src/responses_stream.cpp`
- [x] `src/runtime_config.cpp`
- [x] `src/sentinel/incident_outbox.cpp`
- [x] `src/sentinel/replay_admin.cpp`
- [x] `src/sentinel/replay_capture.cpp`
- [x] `src/sentinel/replay_envelope.cpp`
- [x] `src/sentinel/replay_limits.cpp`
- [x] `src/sentinel/replay_model.cpp`
- [x] `src/sentinel/replay_observation.cpp`
- [x] `src/sentinel/replay_read.cpp`
- [x] `src/sentinel/replay_retention.cpp`
- [x] `src/sentinel/replay_retention_bootstrap.cpp`
- [x] `src/sentinel/replay_retention_metadata.cpp`
- [x] `src/sentinel/replay_retention_schema.cpp`
- [x] `src/sentinel/replay_store.cpp`
- [x] `src/sentinel/upstream_capture.cpp`
- [x] `src/serve.cpp`
- [x] `src/static.cpp`
- [x] `src/systemone/handlers.cpp`
- [x] `src/types.cpp`
- [x] `src/upstream_wire.cpp`
- [x] `src/utils.cpp`
- [x] `include/uos/admin/api_key_audit.hpp`
- [x] `include/uos/admin/api_key_mutations.hpp`
- [x] `include/uos/admin/api_keys.hpp`
- [x] `include/uos/admin/capacity_observation_sync.hpp`
- [x] `include/uos/admin/chart_source_proxy.hpp`
- [x] `include/uos/admin/claude_code.hpp`
- [x] `include/uos/admin/codex.hpp`
- [x] `include/uos/admin/codex_reset.hpp`
- [x] `include/uos/admin/defaults.hpp`
- [x] `include/uos/admin/error_log.hpp`
- [x] `include/uos/admin/index.hpp`
- [x] `include/uos/admin/kernel.hpp`
- [x] `include/uos/admin/sentinel_incident.hpp`
- [x] `include/uos/agent_messages.hpp`
- [x] `include/uos/analytics.hpp`
- [x] `include/uos/api_key_policy.hpp`
- [x] `include/uos/api_keys.hpp`
- [x] `include/uos/auth/index.hpp`
- [x] `include/uos/auth/local_admin.hpp`
- [x] `include/uos/auth/local_development_key.hpp`
- [x] `include/uos/auth/passkeys.hpp`
- [x] `include/uos/auth/passport.hpp`
- [x] `include/uos/auth/passport_sessions.hpp`
- [x] `include/uos/auth/relay.hpp`
- [x] `include/uos/authority/admission_state.hpp`
- [x] `include/uos/authority/authority_rpc.hpp`
- [x] `include/uos/authority/contracts.hpp`
- [x] `include/uos/authority/credential_publication.hpp`
- [x] `include/uos/authority/delivery_outbox.hpp`
- [x] `include/uos/authority/durable_store.hpp`
- [x] `include/uos/authority/native_api_key.hpp`
- [x] `include/uos/authority/native_api_key_audit.hpp`
- [x] `include/uos/authority/native_api_key_policy.hpp`
- [x] `include/uos/authority/native_authority.hpp`
- [x] `include/uos/authority/native_authority_api.hpp`
- [x] `include/uos/authority/native_authority_codex.hpp`
- [x] `include/uos/authority/native_authority_contracts.hpp`
- [x] `include/uos/authority/native_authority_kernel.hpp`
- [x] `include/uos/authority/native_authority_paid.hpp`
- [x] `include/uos/authority/native_authority_principals.hpp`
- [x] `include/uos/authority/native_authority_rpc.hpp`
- [x] `include/uos/authority/native_authority_runtime.hpp`
- [x] `include/uos/authority/native_authority_sql.hpp`
- [x] `include/uos/authority/native_codex.hpp`
- [x] `include/uos/authority/native_codex_429.hpp`
- [x] `include/uos/authority/native_codex_capacity.hpp`
- [x] `include/uos/authority/native_codex_contracts.hpp`
- [x] `include/uos/authority/native_codex_evaluation.hpp`
- [x] `include/uos/authority/native_codex_mutations.hpp`
- [x] `include/uos/authority/native_contracts.hpp`
- [x] `include/uos/authority/native_kernel.hpp`
- [x] `include/uos/authority/native_kernel_contracts.hpp`
- [x] `include/uos/authority/native_paid.hpp`
- [x] `include/uos/authority/native_paid_contracts.hpp`
- [x] `include/uos/authority/native_paid_policy.hpp`
- [x] `include/uos/authority/native_principal_contracts.hpp`
- [x] `include/uos/authority/native_runtime_contracts.hpp`
- [x] `include/uos/bounded_response_body.hpp`
- [x] `include/uos/catalog/index.hpp`
- [x] `include/uos/catalog/models.hpp`
- [x] `include/uos/catalog/promotion.hpp`
- [x] `include/uos/catalog/store.hpp`
- [x] `include/uos/catalog/types.hpp`
- [x] `include/uos/config.hpp`
- [x] `include/uos/debug_routing.hpp`
- [x] `include/uos/decisions/handlers.hpp`
- [x] `include/uos/defaults.hpp`
- [x] `include/uos/handler/admission.hpp`
- [x] `include/uos/handler/http.hpp`
- [x] `include/uos/handler/index.hpp`
- [x] `include/uos/handler/serve_handler.hpp`
- [x] `include/uos/handler/terminal_log.hpp`
- [x] `include/uos/handler/terminal_route.hpp`
- [x] `include/uos/health.hpp`
- [x] `include/uos/http.hpp`
- [x] `include/uos/images.hpp`
- [x] `include/uos/inference_admission.hpp`
- [x] `include/uos/inference_deadline.hpp`
- [x] `include/uos/input_normalization.hpp`
- [x] `include/uos/jev_compaction/compaction.hpp`
- [x] `include/uos/jev_compaction/hybrid.hpp`
- [x] `include/uos/kv.hpp`
- [x] `include/uos/kv_conversion/cli.hpp`
- [x] `include/uos/kv_conversion/convert.hpp`
- [x] `include/uos/kv_conversion/cursors.hpp`
- [x] `include/uos/kv_conversion/legacy_codec.hpp`
- [x] `include/uos/kv_conversion/native_schema.hpp`
- [x] `include/uos/kv_conversion/profile_json.hpp`
- [x] `include/uos/kv_conversion/proof.hpp`
- [x] `include/uos/kv_conversion/typed_value.hpp`
- [x] `include/uos/lib/jev_compaction/client.hpp`
- [x] `include/uos/lib/jev_compaction/codex_items.hpp`
- [x] `include/uos/lib/jev_compaction/compact.hpp`
- [x] `include/uos/lib/jev_compaction/jev_codex_smoke.hpp`
- [x] `include/uos/lib/jev_compaction/jev_compaction_smoke.hpp`
- [x] `include/uos/lib/jev_compaction/request.hpp`
- [x] `include/uos/lib/jev_compaction/state.hpp`
- [x] `include/uos/lib/jev_compaction/types.hpp`
- [x] `include/uos/metered_quota.hpp`
- [x] `include/uos/models/catalog.hpp`
- [x] `include/uos/models/codex_models.hpp`
- [x] `include/uos/models/codex_models_availability.hpp`
- [x] `include/uos/models/codex_models_fetch.hpp`
- [x] `include/uos/models/codex_models_whitelist.hpp`
- [x] `include/uos/models/metadata.hpp`
- [x] `include/uos/models/openrouter_models.hpp`
- [x] `include/uos/openai.hpp`
- [x] `include/uos/openai_schema.hpp`
- [x] `include/uos/openai_telemetry.hpp`
- [x] `include/uos/optional_telemetry_queue.hpp`
- [x] `include/uos/quota_projection.hpp`
- [x] `include/uos/recent_model_context.hpp`
- [x] `include/uos/release.hpp`
- [x] `include/uos/request.hpp`
- [x] `include/uos/request_policy.hpp`
- [x] `include/uos/responses_attempts.hpp`
- [x] `include/uos/responses_buffered.hpp`
- [x] `include/uos/responses_failover_stream.hpp`
- [x] `include/uos/responses_handler.hpp`
- [x] `include/uos/responses_native_headers.hpp`
- [x] `include/uos/responses_request.hpp`
- [x] `include/uos/responses_stream.hpp`
- [x] `include/uos/runtime_config.hpp`
- [x] `include/uos/sentinel/incident_outbox.hpp`
- [x] `include/uos/sentinel/replay_admin.hpp`
- [x] `include/uos/sentinel/replay_capture.hpp`
- [x] `include/uos/sentinel/replay_envelope.hpp`
- [x] `include/uos/sentinel/replay_limits.hpp`
- [x] `include/uos/sentinel/replay_model.hpp`
- [x] `include/uos/sentinel/replay_observation.hpp`
- [x] `include/uos/sentinel/replay_read.hpp`
- [x] `include/uos/sentinel/replay_retention.hpp`
- [x] `include/uos/sentinel/replay_retention_bootstrap.hpp`
- [x] `include/uos/sentinel/replay_retention_metadata.hpp`
- [x] `include/uos/sentinel/replay_retention_schema.hpp`
- [x] `include/uos/sentinel/replay_store.hpp`
- [x] `include/uos/sentinel/upstream_capture.hpp`
- [x] `include/uos/serve.hpp`
- [x] `include/uos/static.hpp`
- [x] `include/uos/systemone/handlers.hpp`
- [x] `include/uos/types.hpp`
- [x] `include/uos/upstream_wire.hpp`
- [x] `include/uos/utils.hpp`

## Site ledger

A checked file has a corresponding entry here and in the JSON ledger.

### `src/admin/api_key_audit.cpp`

Reviewed list traversal, atomic staging and request forwarding.

- L27, 33 **SAFE** · The list coroutine owns the store, iterator and page vector, and awaits each iterator operation while those owners are in its frame.
- L41, 53, 75, 76, 77 **SAFE** · Request and override handles enter the owning handler by value, and the synchronous list wrapper copies the input before the child runs.
- L66 **SAFE** · The audit sources are captureless synchronous calls consumed during event construction.

### `src/admin/api_key_mutations.cpp`

Reviewed update authorization, versioned writes, revocation and guarded deletion. The wrappers at the end are synchronous.

- L42, 43 **SAFE** · The record reader invokes both reference-capturing helpers synchronously before its input can go away.
- L104, 118, 133 **SAFE** · The resolver frames own KV, copied JSON and records, and the paid initializer receives an owned signal.
- L153, 159, 167, 169, 170, 182, 183 **SAFE** · Each projection or reservation step owns its fields and KV and completes before that frame leaves scope.
- L196, 205, 214, 225, 228 **SAFE** · The write frame retains original entries, optional snapshots and its atomic operation through commit and conflict handling.
- L234, 236, 240, 247, 252, 269, 274, 434, 435 **SAFE** · The wrapper eagerly copies context. The update coroutine keeps the variant containing the borrowed target alive while each Promise completes, and its children receive copies.
- L277, 279, 284, 290, 303, 315, 436, 437, 438, 439 **SAFE** · The revocation frame retains the request, context, KV, fetched records and atomic operation through commit.
- L328, 334, 341, 350, 356, 358, 365, 369 **SAFE** · The local-deletion claim and release tasks take entry, key and claim values and keep each atomic operation in its awaiting frame.
- L374, 376, 381, 385, 387, 392, 396, 400, 404, 413, 415, 440, 441 **SAFE** · The deletion frame owns both the result variant and the operation, including the blocked alternative accessed after the release wait.
- L423, 425, 428, 429 **SAFE** · The prefix vector is frame-local and immutable, and each iterator and yielded entry remains owned through its awaited deletion.

### `src/admin/api_keys.cpp`

Reviewed paid-fallback fan-out, request-owned mutation records, detached reconciliation, and immutable list snapshots.

- L61, 63, 65, 66, 213, 217 **SAFE** · Both paid ledger children take their key ID and KV owner by value. Fail-fast all can release the parent without invalidating either child.
- L91, 103, 106, 121, 124, 128, 145, 150, 293, 294 **SAFE** · The synchronous create wrapper copies the context, and the create and policy frames own their request, signal, records and atomic operation until the respective waits finish.
- L157, 163, 177, 354, 355 **SAFE** · History owns its request, KV and normalized ID through the key read and record list. Both overloads transfer ownership to the history frame.
- L173, 174 **SAFE** · The detached reconciler owns the copied optional shared KV and timestamp in its frame. The rejection handler borrows nothing.
- L297, 303, 304, 322 **SAFE** · The listing frame owns the store, iterator and record vector. The vector is sorted before usage awaits and stays immutable while element references are in use.
- L327, 330, 333, 334 **SAFE** · Each paid-field child copies its row and shared KV before it starts. The parent keeps independent output and ordering state.

### `src/admin/capacity_observation_sync.cpp`

Reviewed the capped body reader and ingest options handoff.

- L35, 40, 41 **SAFE** · The request, stream handle, reader and chunk vector are frame-local owners. Cancellation waits with the same reader and the rejection callback is captureless.
- L56, 61, 81, 87, 100, 103 **SAFE** · The plain wrapper copies KV and clock callback into the ingest frame. Storage receives a named task with copied host and moved points; the borrowed JSON field is converted to an owning string first.

### `src/admin/chart_source_proxy.cpp`

Reviewed the body cancellation, local and remote response reads, and retained capacity push.

- L55, 56, 71, 74, 78, 131, 134, 138 **SAFE** · Each fetch or cancellation child has a named response or body owner in its awaiting frame, and Fetch retains its callable through the delegated task.
- L152, 168, 169, 170 **SAFE** · The push task owns copied samples, fetcher and shared state. The normal transport's timeout aborts the exchange and the owned finally reaction clears inFlight on settlement.
- L179, 186, 191, 196, 205, 207, 212, 215, 218, 221 **SAFE** · The proxy frame owns request, pathname string, dependency callbacks and local response. Work escaping through the push is copied into its own frame.

### `src/admin/claude_code.cpp`

Reviewed credential validation and persistence, account targeting, guard and access handlers, and the compatibility quota read.

- L67, 88 **SAFE** · The validation frame owns both parsed credentials and workspace, and the passive status projection uses its snapshot only after the wait.
- L132, 186, 193 **SAFE** · JSON member pointers refer to unchanged frame-local body storage while account resolution is pending.
- L171, 174 **SAFE** · The persistence frame owns its submission, account patch and store while both writes are awaited.
- L202, 209, 212, 225, 234, 238, 242 **SAFE** · The upload frame retains request, input, options and the settled variant. The account reference after the final status wait still points into that frame-local variant.
- L250, 253, 257, 259, 267, 272, 276, 279, 287, 292, 299, 306, 310 **SAFE** · The quota, deletion and guard operations await each child with a frame-local store and account or body snapshot. The raw member used after resolution is in the unchanged local JSON.
- L316, 321, 324, 328, 335, 338, 342, 345 **SAFE** · Both access and active-account handlers retain request, store and parsed IDs until completion, including the store borrowed by the access write.

### `src/admin/codex.cpp`

Reviewed all admin Codex actions, catalog construction and refresh, provider selection, and prompt purge.

- L46, 52, 53, 54 **SAFE** · The roster owns its accounts and every hash child owns its account ID. The fail-fast combination cannot invalidate the remaining hash inputs.
- L70, 71, 72, 78, 83, 88, 93, 95 **SAFE** · The request, scenario and baseline are owned by the handler. The experiment task owns its baseline before the handler awaits it.
- L103, 105, 107, 111, 124 **SAFE** · The telemetry overload owns the adapter closure in the by-value reader parameter throughout the awaited task. The analytics handler likewise keeps its callback and options during direct delegation.
- L128, 129, 135, 153, 166, 174 **SAFE** · Native and memory KV readers acquire a store owner synchronously. The upload frame owns validation and atomic state, and keeps the database alive through the directly awaited catalog write.
- L181, 183, 188, 193, 194, 195, 197, 201, 202, 227, 228, 233, 257, 258, 263 **SAFE** · The individual paths await children with stable frame-local receiver, request or iterator owners. The entries and result snapshots are values.
- L207, 208, 209 **DEFECT** · Catalog override lifetime. A whitelist read rejects while an injected capturing catalog coroutine is pending. The join unwinds the dependency object; the pending coroutine can resume through its destroyed closure. Hold the catalog callable in rt::async before starting its Promise.
- L216, 217, 218, 219, 220, 221 **DEFECT** · Refresh override lifetime. A refresh child rejects while a sibling created by an injected capturing refresh coroutine is suspended. Handler exit destroys the callback holding its captures; the sibling can still resume. Wrap each injected callback in rt::async with copied options.
- L238, 239, 240 **DEFECT** · Provider selection override lifetime. The selection read rejects while an injected capturing catalog coroutine is pending. Handler exit destroys the callable; the pending build can resume through it. Hold the catalog callable in rt::async before starting its Promise.
- L71 **DEFECT** · The debug-routing reset constructs an owning scenario string directly in the await operand. GCC 12.2 requires a named scenario or a separately constructed Task.

### `src/admin/codex_reset.cpp`

Reviewed the manual-reset dispatch and error translation.

- L6, 9, 14 **SAFE** · The handler owns request and dependencies. The hash view is converted to an owning string before the separately constructed consuming Promise, which the handler awaits.

### `src/admin/defaults.cpp`

Reviewed defaults reads and cutover writes, and the migration admin handlers.

- L43, 118, 121, 125, 129, 130, 132, 133, 134 **SAFE** · The request and database handle remain in the awaiting frame. The migration line vector and options are local to the caller until the import task completes.
- L51, 52, 54 **SAFE** · The normal GET children own their KV store state or use an independent diagnostics task. The result tuple is named before its entries are borrowed.
- L53, 54 **DEFECT** · Defaults diagnostics override lifetime. A KV read rejects while an injected capturing diagnostics coroutine waits. Fail-fast unwinding drops the callback object and the pending child can resume through the freed closure. Wrap the injected callable in rt::async.
- L61, 70, 85, 100, 107, 113 **SAFE** · The update attempt owns the fetched tuple, cutover guard and options across awaits. The public POST retries with copied arguments and keeps each child within its own await.
- L99 **DEFECT** · GCC 12.2 coroutine-rule violation in the defaults write. Atomic commit suspends and returns a temporary, and the enclosing expression immediately accesses its member. Bind const auto result = co_await atomic->commit(), then assign committed = result.ok.

### `src/admin/error_log.cpp`

Reviewed error persistence, host log sizes, history iteration and replay retention reporting.

- L143, 172, 176, 178 **SAFE** · The public wrappers copy all input into the owning recorder before returning. The record, key, options and store remain frame-local until the write ends.
- L269, 277 **SAFE** · The immutable path vector is owned by the frame throughout the microtask turn and the current path reference remains valid.
- L299, 314, 328, 333, 335, 337 **SAFE** · The history frame retains the iterator, KV and its copied result entries; the sorting callable runs synchronously.
- L432, 435, 443, 448, 462 **SAFE** · The request and history are values and the named store owner outlives the directly awaited maintenance borrowing it.

### `src/admin/index.cpp`

Include-only translation unit. The admin public interfaces are audited in the header.

### `src/admin/kernel.cpp`

Rechecked the previously reported queue-list defect and both usage fan-outs against the integrated source, then traced the admin writes and diagnostics.

- L50, 56, 90, 92 **SAFE** · The database Promise yields an owning handle. The nullable adapter callbacks are captureless and transform outcomes by value.
- L96, 98, 105, 112, 114, 118, 123, 129, 131 **SAFE** · Public-key writes await an atomic operation held by the same coroutine and retain the request, KV and copied records.
- L79, 85, 135, 138, 143, 144, 146, 147 **SAFE** · The fixed queue workers receive copied KV, shared output and owner or record parameters. Both fail-fast joins may outlive the parent without losing child owners.
- L159, 166, 181, 187, 188, 195, 201, 202 **SAFE** · Usage workers likewise hold their output maps and input records in frame parameters. Their safety survives exceptional sibling failure.
- L172, 176, 209, 212, 215, 220, 223, 229, 235, 236, 239, 242 **SAFE** · Inventory and usage snapshots are named values. Direct delegation keeps the query strings and options in the caller or copies them into the child.
- L270, 277, 279, 284, 286, 293, 300, 308, 310, 314, 315 **SAFE** · Policy mutations use named options and await each child while the request and resolved input variants remain live.
- L324, 332, 335, 340, 342, 345, 346, 351, 368, 372, 373 **SAFE** · The projection children own their KV and configuration values, and the rejection adapters borrow nothing. References into the named result tuple remain valid across stats recording.
- L379, 380, 384, 386, 389, 390, 397, 398, 399, 403, 406, 407, 408 **SAFE** · Diagnostics use by-value account and signal state, stable local body and request owners, and runtime combined signals whose dependents are weak.
- L413, 414, 415, 416, 418, 421, 422, 426, 427, 430, 431, 432, 433, 435, 438, 439, 443, 444, 445 **SAFE** · Each row or identity Promise is built from owned copies before the join. The lookups capture by reference only during synchronous none_of calls, and PATCH views refer to the immutable local JSON.
- L380, 384, 406, 439 **DEFECT** · Account diagnostics and the overage update construct owning cohort or account strings in direct await calls. The GCC 12.2 profile requires those strings or each Task to be constructed in an earlier statement.

### `src/admin/sentinel_incident.cpp`

Reviewed incident ID validation and the paged incident response.

- L121, 137, 138, 145, 146, 156, 158 **SAFE** · Query strings and the URL are frame-local, the query callbacks are inline algorithms, and the handler keeps both the KV owner and its injected reader closure through the directly awaited list.

### `src/agent_messages.cpp`

Reviewed both GitHub-authenticated handlers and their shared synchronous normalization.

- L169, 177, 183, 195, 199, 223, 224, 241 **SAFE** · The post frame owns dependency callbacks and the authentication variant that supplies the GitHub reference. The clock and ID functions are synchronous and the request and atomic inputs are values.
- L250, 256, 268, 288, 294 **SAFE** · The list frame owns authentication, request, query, store and iterator and awaits each step with the iterator still in scope.

### `src/analytics.cpp`

Reviewed log mutation and history listing, plus synchronous record codecs.

- L120, 127, 129, 141, 274, 284, 289, 294, 301, 307 **SAFE** · The mutation frame owns the callable and copied JSON. It consumes each prior record synchronously and retains the atomic operation until commit finishes.
- L312, 322, 333 **SAFE** · History holds the shared KV, iterator and output vector for the traversal, and each fetched entry is an owned value.

### `src/api_key_policy.cpp`

Reviewed window-lock promise ownership, quota reservation and compensation callbacks, KV traversal, policy cache and authentication.

- L166, 170, 175, 176, 181 **SAFE** · The lock frame copies the prior Promise before suspending and owns the operation callable. The tail is looked up afresh after the operation and never retained across suspension.
- L196, 207, 208, 213, 224, 225, 230, 239, 253, 257 **SAFE** · Release and compensation use owned keys, policy, KV and snapshots. Native reads own their store, and the lock owns the synchronous Task factory through settlement.
- L276, 278, 285, 286, 296, 300, 312, 327, 330 **SAFE** · Dispatch cancellation takes state and inputs by value. The callbacks only construct owning Tasks, and the sweeps retain their iterator or immutable ID snapshot through awaits.
- L349, 350, 351, 362, 379, 383, 400, 412, 416 **SAFE** · The lock owns the Task factories and each atomic operation stays in its awaiting frame. Structured bindings name entries of already-stored tuples that no other task mutates.
- L441, 444, 456, 461 **SAFE** · Reservation callbacks forward copied data into owning Tasks. The response clone stays live through JSON read and its parsed views are used only after the final microtask.
- L498, 501, 511, 513, 523, 525, 526, 534, 546, 549 **SAFE** · Admission, dispatch and release copy the state, KV and options. The coalesced admission result contains a separate active reservation state and the frame releases its copy on settlement.
- L592, 621, 630, 631, 632, 647, 658, 673, 687 **SAFE** · Each attempt owns its snapshots and keeps tuple and variant alternatives valid until the awaited full-window handling or atomic commit returns.
- L723, 728, 730, 735, 743, 749, 753, 764, 770, 774, 781, 788, 791, 800, 809, 836, 845 **SAFE** · The lock retains both Task-factory captures and their by-value arguments. Reads keep the KV and immutable iteration inputs until the respective wait completes.
- L853, 856, 857 **SAFE** · The deletion frame owns both iterators and the vector of copied keys. No other coroutine can mutate the vector being iterated.
- L862, 870, 872, 875, 881 **SAFE** · Authentication copies the cached policy immediately and never dereferences the shared cache entry after suspending for storage.

### `src/api_keys.cpp`

Reviewed key construction, token formatting and synchronous policy normalizers.

- L12, 14 **SAFE** · The key constructor copies each incoming view into owned key parts before the function returns.

### `src/auth/index.cpp`

Reviewed client and admin auth, external token verification, cache use, and method reporting.

- L54, 67, 72, 128, 137, 138, 148 **SAFE** · Fetch and verification own each request and token snapshot. The cache iterator is read only before the next await, and the body receiver remains in its awaiting frame.
- L176, 189, 195 **SAFE** · The GitHub authentication frame owns its policy variant, whose state remains valid across the queue write, and the queued path is an owning string.
- L297, 323, 333, 343, 352, 367, 390, 428 **SAFE** · The cookie adapter is synchronous and constructs a by-value Request. The other helpers own request or token and the logging callback through the work.
- L458, 466, 470, 474, 478, 488, 492, 495, 498 **SAFE** · The client auth frame and its logging callback both own Request handles. All branches await delegated authentication under those owners.
- L544, 547, 563, 567, 574, 584, 588, 597, 610, 622, 647 **SAFE** · Deploy-token fetches and loops retain token, URL and response in their frames, and the cache iterator dies before the remote verification await.
- L705, 737, 748, 750, 753, 757, 767, 772 **SAFE** · Admin auth owns its request, credentials and logger as values, and the cookie wrapper transfers its copied request to the called coroutine.
- L811, 841, 851, 864, 885, 890, 895, 902, 925 **SAFE** · Report generation holds auth and KV snapshots in the awaiting frame. JSON value-copying references a local stored record and invokes its helper synchronously.
- L137, 195 **DEFECT** · GitHub bearer verification and queue recording construct owning concatenation or path results in the awaited call. The GCC 12.2 porting rule requires naming each input or Task before suspension.

### `src/auth/local_admin.cpp`

Reviewed listener configuration, request-scoped peer binding, and URL-origin inspection. The fallback peer and request entries are copied before use.

- L112, 126, 171, 172 **SAFE** · The WeakMap key is request identity and its value is an owned address. The lookup pointer is immediately copied in a synchronous function.

### `src/auth/local_development_key.cpp`

Reviewed the provisioning timer and abort race, retained initializer, deletion guard and policy read.

- L97, 98, 102, 106, 107, 111 **DEFECT** · Orphaned provisioning abort race. Pricing settles first and the listener is removed, so nobody can settle the abort-only Promise. Its pending race reaction owns that same state and the winning key material until loop teardown. After registration succeeds, remove the listener and settle the unused private Promise during cleanup.
- L121, 132, 256, 257, 266, 269, 274, 275, 276, 280 **SAFE** · The initializer frame owns its callable and pricing signal even if the abort wins. The timer holds its controller by value, and normal completion clears the timer.
- L187, 191, 201, 208, 228, 236, 239, 243, 290, 303 **SAFE** · Provisioning and deletion checks retain the KV and each iterator or atomic operation. Their prefix and record collections are private frame storage.
- L310, 318, 321, 325 **SAFE** · The hash view is copied into an owning string and key before the second KV await, with the original JSON still live.

### `src/bounded_response_body.cpp`

Reviewed bounded reads, abort races and detached reader cancellation.

- L33, 34, 35, 36 **SAFE** · The newPromise executor invokes its reference captures synchronously. The installed listener owns its Reject and signal and is removed on normal and exceptional race completion.
- L41, 42, 43, 47 **DEFECT** · Orphaned bounded-body abort race. Each successful body read, including EOF, wins and removes the only callback able to settle the abort-only Promise. Its pending race reaction retains the winning ReadResult and chunk for the process loop lifetime. Create the private promise with withResolvers and settle it after unregistering the listener once the race has been installed.
- L78, 87, 92, 93, 94, 100, 103, 119, 120, 128 **SAFE** · The cleanup lambdas run in the owning reader frame. The plain reader.cancel wrapper captures stream state into the Task before releaseLock, so detached cancellation keeps its own stream owner.

### `src/responses_stream.cpp`

Reviewed stream framing, abort and timeout races, generator delegation, proxy lifecycle callback ownership, preflight and SSE heartbeats.

- L209, 212, 250, 251, 254, 258, 266, 271 **SAFE** · The abort listener owns only reader control and weakly references the signal. Disposal removes it, while detached cancel has already acquired a stream owner.
- L287, 288, 291, 294, 295, 299 **DEFECT** · Orphaned Responses inactivity race. The upstream read wins and the code unregisters the only inactivity rejection callback. The pending inactivity source owns its race reaction and the winning ReadResult, retaining a chunk for every ordinary read. Settle that private loser after removing the listener and only after its race has been installed.
- L308, 314, 315, 317, 325, 326, 332, 341, 354, 364 **SAFE** · Frames points into the consuming coroutine's own unchanged chunk while yielding. The generator captures are retained by the generator runtime and its delegated task owns the session.
- L391, 395, 400, 422, 423, 444, 454, 458, 470, 476, 478, 479 **SAFE** · The iterator and callback dispatchers put copied state, reason or event into Task frames before asynchronous use. Proxy pull also holds state after controller.close clears callbacks.
- L498, 499, 501, 502, 510, 515, 517, 529, 530, 532, 533, 536, 570 **SAFE** · The newPromise executor uses this only synchronously and its timer uses weak state. Cancellation and read wins resolve the synthetic heartbeat even when stopping the timer. A heartbeat win leaves the same read pending until a chunk or cancel.
- L575, 585, 589, 594, 605, 609, 614, 623, 624, 635, 636, 644, 646, 650 **SAFE** · Preflight and stream factories transfer values to generator and source holders. Escaping cancellation and pull tasks take copied iterator, stream control or source state before the wrapper returns.

### `src/auth/passkeys.cpp`

Reviewed attestation and certificate chains, CRL cache, passkey KV transactions and registration, session and login handlers. The parsing and cryptography helpers through the first certificate fetch and between attestation awaits are synchronous.

- L2757, 2772, 2773, 2775, 2788 **SAFE** · The fetch frame owns the certificate, URL and response. Cache iterators are discarded before fetch and the later insertion uses the owned key.
- L2811, 2812, 2816, 2835, 2839 **SAFE** · Both certificate collections are private to the path-validation frame and stay immutable while the checker uses a copied Certificate.
- L2926, 2972, 3085, 3182, 3642, 3665 **SAFE** · Attestation coroutine frames own the options and parsed objects backing their CBOR, certificate and JSON views. Each separately built validation Task owns its certificate lists.
- L3713, 3791, 3794, 3797, 3800, 3803, 3806 **SAFE** · Verification owns incoming JSON and the decoded authenticator material. It builds a separate Task with a by-value options record in each supported format branch.
- L4153, 4286, 4300 **SAFE** · Handle hashing retains the trimmed seed and delegates with a value. The store promise and request body parser own the store and request through completion.
- L4313, 4410, 4425, 4488, 4919, 5001 **DEFECT** · Passkey lookups and logout construct temporary KvKey values in the await full-expression. GCC 12.2 is prohibited there even though current KV adapters move the key to their own work. Bind each key before the await, with an explicit block for logout.
- L4316, 4324, 4333, 4345, 4352, 4366, 4372, 4378, 4383, 4402, 4468, 4476, 4479, 4485 **SAFE** · The concrete KV adapters move by-value inputs into owning operations and all receivers and list iterators stay in the awaiting frames. Each atomic receiver has a named owner until commit.
- L4524, 4542, 4545, 4569, 4573, 4583, 4590, 4595, 4600, 4620, 4642 **SAFE** · Session and registration-start operations take Request, KV, options and input snapshots by value, with parsed body and caller locals retained in the frame.
- L4682, 4686, 4699, 4709, 4732, 4738 **SAFE** · The parsed registration body, challenge and save variant are immutable frame locals backing any member references. Each child owns the copied inputs it uses.
- L4782, 4790, 4799, 4803, 4814, 4828, 4842, 4857 **SAFE** · Login-start owns the KV budget transaction, body and request metadata. The generated options and digest inputs are stored before the explicit microtask.
- L4882, 4891, 4895, 4909, 4914, 4930, 4935, 4940 **SAFE** · Login-finish retains the copied body and credential entry through counter and session updates. The assertion verifier uses its views synchronously while the caller owns the data.
- L4977, 4997, 5000, 5011, 5017, 5032, 5040, 5054, 5065 **SAFE** · Session, logout and administration keep their Request, KV handles, local body and iterator owners in scope through the corresponding awaits.

### `src/auth/passport.cpp`

Reviewed JWT verification, JWKS caching, OAuth callback and persistent-session routing, and bounded stream reads.

- L124, 131, 143, 155 **SAFE** · The bounded reader frame owns Response, body and reader through each drain or cancel and releases the reader lock afterwards.
- L176, 185, 187, 189, 193, 208 **SAFE** · Cache-hit keys are copied synchronously. The JWKS cursor and views spanning the microtask point into this frame's unchanged JSON, and each fetch owns its timeout signal.
- L293, 298 **SAFE** · The token and claims are frame values. A local map owns the key through the synchronous signature check before the final microtask.
- L367, 369, 373, 378 **SAFE** · The callback owns request, state and exchange response. It copies token and grant into downstream tasks before suspending.
- L445, 465, 498, 513, 526, 548 **SAFE** · Route frames own request, path, cookies and verifier state, and every authentication, revocation or callback Task receives its required values before the parent suspends.

### `src/auth/passport_sessions.cpp`

Reviewed session creation, replacement, introspection and revocation including stream and transaction owners.

- L92, 120, 132, 149, 157 **SAFE** · The token, response, reader, optional stream and timed request are values retained through the delegated operation.
- L185, 190, 191, 196, 202, 214, 219, 226, 231, 233 **SAFE** · Grant string views remain backed by the unchanged by-value grant JSON. Key, transaction and the optional replaced session are all owned in the creating frame or the called task.
- L248, 254, 256, 261, 265, 269, 274, 278, 282 **SAFE** · Introspection owns the KV, session record, response and parsed JSON through cancel or reread. Its copied record does not borrow the persistent store.
- L297, 303, 305, 308, 318, 320 **SAFE** · Revocation keeps KV and key through delete and passes the copied record and Response to the fetch and cancel tasks.

### `src/auth/relay.cpp`

Reviewed the synchronous origin parser, trusted host lookup and slash normalization. The static table uses literal storage, derived labels are local, and the result is an owning string. No lifetime site crosses a suspension or escapes as a callback.

### `src/authority/admission_state.cpp`

Reviewed the portable authority CAS loop and every admission mutation. The operation callbacks synchronously modify one private snapshot per retry. The RPC server and delivery outbox retain their shared AuthorityOperations receivers, and direct fixtures await with a named owner.

- L171, 178, 184 **SAFE** · The awaited transaction owns its action and mutable snapshot, and each CAS accepts its own state value. All current caller paths retain the PortableAuthority and therefore its store across both waits.
- L198, 250, 251, 257, 276, 280, 305, 310, 315, 320, 326, 350 **SAFE** · These normal lambdas borrow member coroutine arguments that remain live in the direct awaiting caller. The nested selectors consume snapshot pointers synchronously before the next wait.
- L355, 367, 371, 381, 387, 410, 415, 440, 444, 450, 455, 472, 476, 485, 490, 512, 517, 532, 534 **SAFE** · Mutations use references only inside synchronous actions held by the transaction frame. Outer arguments and receiver remain in the direct awaiting ownership chain, including cancel followed by settle.

### `src/authority/authority_rpc.cpp`

Reviewed the portable RPC command bridge, server receiver and client call chain. The delegate objects are retained by the shared-pointer owners or named direct callers.

- L188, 216, 221, 226, 231, 236, 241, 246, 252, 259, 264, 271, 276, 281, 286 **SAFE** · The RpcServer retains its AuthorityOperations dependency. The private execute frame owns the command backing its arguments view, and the selected operation receives decoded values before each direct await.
- L296, 304, 311, 317, 324, 340, 348, 354, 360, 366, 372, 381, 387, 394, 401 **SAFE** · The retained client owns the RPC transport while invoking. Inputs, argument arrays and replies are named frame values, and selection copies its reply views before returning.

### `src/authority/contracts.cpp`

Reviewed authority value codecs and snapshot shapes. Member references and viewed keys are consumed synchronously, and the exported serializers and parsers return owned values. No suspension or escaping callback.

### `src/authority/credential_publication.cpp`

Reviewed capability binding, asynchronous verifier use and the publication CAS retry. Current publisher callers hold a named shared owner through awaited publication.

- L103, 104, 124, 129 **SAFE** · The caller retains the publisher and its verifier. The publication input and identity are owned values and no references into the private snapshot survive the synchronous update or its CAS.

### `src/authority/delivery_outbox.cpp`

Reviewed the portable receipt facade and deferred observations.

- L12, 14, 19, 21, 26, 31 **SAFE** · The facade methods are synchronous Task factories. The child frames directly own the copied AuthorityOperations and handle through observation and settlement, even if the facade has been dropped.

### `src/authority/durable_store.cpp`

Reviewed SQL row snapshots and the synchronous durable implementation of the Promise-based store interface.

- L156, 204, 210, 221 **SAFE** · AuthoritySqlStorage transactionSync invokes both reference-capturing callbacks inline and never retains them. The read and CAS promises return already-copied values after those synchronous calls finish.

### `src/authority/native_api_key.cpp`

Reviewed validation, usage-window selectors and write-plan construction. All borrows refer to current JSON or records consumed synchronously, and plans own their records. No suspension or escaping callback.

### `src/authority/native_api_key_audit.cpp`

Reviewed API-key event field selection, cursor decoding and key construction. The field table is backed by literals and outputs own their values. No suspension or escaping callback.

### `src/authority/native_api_key_policy.cpp`

Reviewed verification, policy mutation planning, paid input normalization and key generation. JSON and string views are synchronous inputs, and output records contain copied values. No suspension or escaping callback.

### `src/authority/native_authority.cpp`

Reviewed native State ownership, control/runtime execution, routing, and synchronous SQLite callbacks.

- L48, 59, 67, 74, 75 **SAFE** · The public rt::async factory holds a shared State until the control task finishes. Initialization and command transaction callbacks run inline while their frame or constructor arguments are live.
- L115, 116, 118, 119, 214, 215, 219, 221, 231, 234 **SAFE** · The bound port captures State into the rt::async frame before entering a member coroutine. Routing and classification use named copied inputs, and SQL runs every capture synchronously after or between waits.
- L298, 299, 304, 305, 315, 321, 322, 323 **SAFE** · Every port or runner closure owns the shared State or a by-value facade. Task-factory callbacks put their own State owner in the child, and State does not own the port callbacks.

### `src/authority/native_authority_api.cpp`

Reviewed synchronous API admission, dispatch and status transactions. The snapshot lambda and row borrows are used within the method and the returned receipts own their values.

- L92 **SAFE** · The invoked snapshot lambda is a local read factory with access to this and the active frame. It is called inline and never escapes.

### `src/authority/native_authority_codex.cpp`

Reviewed synchronous pool selection, reset, signal and write operations. The identity pointer refers to the pool argument and callers consume it within the same operation.

- L14 **SAFE** · The identity finder uses its pool and slot synchronously and callers keep the local pool snapshot while using its result.

### `src/authority/native_authority_contracts.cpp`

Reviewed native authority argument and row codecs. Variant visitation invokes the local callback inline and outputs own their JSON. No suspension or escaping callback.

### `src/authority/native_authority_kernel.cpp`

Reviewed synchronous kernel quota transactions, policy snapshots and expiry handling.

- L77 **SAFE** · The decision callback reads the current frame and receiver during its immediate transaction decision.

### `src/authority/native_authority_paid.cpp`

Reviewed synchronous paid admission, settlement, reconciliation and deletion. The visitor runs inline and SQL reads supply materialized owned rows. No suspension or escaping callback.

### `src/authority/native_authority_principals.cpp`

Reviewed the principal control capability, synchronous commit closures, and shared preparation and cleanup work. The normal entry point keeps the enclosing NativeAuthority State and its SQL, initial settings and subcomponents alive.

- L53, 153, 160, 169, 182, 190, 253, 257, 262 **SAFE** · The control owner retains the principal receiver and asynchronous source callables throughout preparation. The create flight is a shared owner, parsed views refer to a private command, and the returned synchronous Commit owns its copied arguments while only borrowing the retained receiver.
- L267, 268, 282, 287, 289, 290, 295, 296, 299, 301 **SAFE** · The pending work is copied out of each map to an owned Promise and maps are looked up again by key after suspension. Commit and cleanup tasks remain awaited by a control frame retaining their receiver and borrowed SQL components.
- L306, 307, 308, 311, 319, 321, 322, 330, 331, 335, 341, 348 **SAFE** · Callback captures move or copy the command, binding and verified arguments into stable frames. SQL uses its reference captures inline. The admin variant pointer refers to the current frame's immutable verified value.

### `src/authority/native_authority_rpc.cpp`

Reviewed command parsing, server and client adapters, and escaping transport callbacks.

- L133, 143, 152, 161, 256, 257, 260, 264, 267, 277, 278 **SAFE** · Each server transport lambda is a noncoroutine factory and transfers a port or copied callable and binding to an owning child frame before returning. Each injected callable remains live through its direct wait.
- L175, 184, 193, 270, 271, 272, 273, 281, 282 **SAFE** · The synchronous facade copies each transport, epoch and argument object into the named client Task. The Task owns its encoded command and callback until the reply.

### `src/authority/native_authority_runtime.cpp`

Reviewed native runtime verification and routing, including retained-history authorization and synchronous row snapshots.

- L72, 121, 142 **SAFE** · The bound NativeAuthority port retains its enclosing State, runtime and injected verifier through settlement. The binding, request and intent offered to the verifier are value parameters of the awaiting frame.

### `src/authority/native_authority_sql.cpp`

Reviewed SQLite storage, initialization, pagination and row materialization.

- L75, 127, 128, 133 **SAFE** · The storage calls transactionSync inline and the initializer invokes its borrowed callback inside that transaction. Reads return copied rows and no callback is retained.

### `src/authority/native_codex.cpp`

Reviewed synchronous active-account selection, proof classification and fallback checks. Each selector only borrows the current input containers during the algorithm, and returns a value snapshot. No suspension or escaping callback.

### `src/authority/native_codex_429.cpp`

Reviewed 429 classification, bounded body ownership and retry evidence parsing.

- L312, 318, 319 **SAFE** · The response and header handle are values in the awaiting frame. The named body reader owns its copied argument and the classifier uses its owned byte snapshot after completion.

### `src/authority/native_codex_capacity.cpp`

Reviewed quota-class normalization, route-slot migration and resets. Predicates only borrow current groups during synchronous traversal, and selected slots and results are copied. No suspension or escaping callback.

### `src/authority/native_codex_contracts.cpp`

Reviewed synchronous pool, slot and proof codecs. Property views are consumed while the input JSON lives and results own decoded data. No suspension or escaping callback.

### `src/authority/native_codex_evaluation.cpp`

Reviewed synchronous quota-window lookup and account evaluation. Results copy their window/class evidence and no callback escapes.

### `src/authority/native_codex_mutations.cpp`

Reviewed synchronous slot reset, dispatch and proof mutations. Reference-capturing lambdas are immediately called or used as inline predicates. No suspension or escaping callback.

### `src/authority/native_contracts.cpp`

Reviewed native numeric, key and JSON shape helpers. Returned views are used synchronously by callers and no task or callback is created.

### `src/authority/native_kernel.cpp`

Reviewed usage arithmetic, terminal accounting and synchronous storage plan helpers. Plans own records and traversals use local inputs without suspension.

### `src/authority/native_kernel_contracts.cpp`

Reviewed kernel reservation, quota, terminal and configuration codecs. JSON or record borrows are confined to synchronous conversion and results own their data.

### `src/authority/native_paid.cpp`

Reviewed paid reservation, usage, reconciliation and deletion planning. Key and JSON borrows are processed synchronously during the plan construction, and results own their storage.

### `src/authority/native_paid_contracts.cpp`

Reviewed paid-attempt and billing codecs. JSON member borrows end within the synchronous conversion and outputs own the results.

### `src/authority/native_paid_policy.cpp`

Reviewed paid key projection and model lookup. Pointer and view use is confined to synchronous checks or copied into results.

### `src/authority/native_principal_contracts.cpp`

Reviewed principal request, grant and result codecs. Standard variant visitation consumes its captures synchronously and result conversion copies the data.

### `src/authority/native_runtime_contracts.cpp`

Reviewed runtime verification and routing codecs. Conversion and variant visitation use input references synchronously and return owned data.

### `src/catalog/index.cpp`

Reviewed catalog construction, paid-source joins, cache refresh and detached lease ownership.

- L32, 37, 45, 50, 66, 68, 69, 71, 74, 75, 78, 85, 99, 105, 111, 117, 127, 128 **SAFE** · Each joined or detached provider fetch takes ownership of its own options. Promise wait copies its own state, and catalog references refer to immutable frame-owned results while the full snapshot loads. The append lambda runs inline.
- L134, 138, 157, 160, 161, 165, 166, 167, 168, 169 **SAFE** · The degradation frame owns the injected callback through its wait. Refresh and fallback tasks retain the shared context or database and pass owning response inputs.
- L172, 173, 174, 176, 177, 178, 179, 180, 183, 184, 186, 187, 188, 190, 191, 192, 193, 194, 199, 200, 202, 203, 206, 207, 211, 213, 216, 218, 219, 220, 222 **SAFE** · Every refresh stage keeps the context containing the store. The provider error callback captures that context with no reverse edge and the response is a named frame value.
- L225, 227, 230, 232, 244, 245, 247, 248, 250, 251, 253, 255, 256, 257, 258, 260, 263, 264, 266 **SAFE** · The detached refresh directly owns the context and its shared KV. Normal success, rejection and recovery all reach heartbeat stop and wait for pending renewal before the context can be released. The spawn error callback owns its version.

### `src/catalog/models.cpp`

Reviewed catalog projection, model composition and normalized snapshot updates.

- L111, 117, 122, 141 **SAFE** · The refresh context owns the borrowed KV through the outer await. Current native KV get wrappers start store-owning children for the join. Parsed views and references are backed by the named tuple and transaction stays live through commit.

### `src/catalog/promotion.cpp`

Reviewed the scoped metadata promotion and atomic updates.

- L55, 56, 57, 58, 66 **SAFE** · The frame owns the shared KV and its input and plan snapshots, including the ready result used after commit. Native KV gets have independent storage owners if the join rejects early.

### `src/catalog/store.cpp`

Reviewed all KV reference parameters, catalog memo and chunk ownership, storage lifecycle and lease heartbeat.

- L82, 86, 90, 95, 99, 101, 103, 108, 117, 130, 134, 141, 147 **SAFE** · Catalog callers retain KV while these tasks run. Pruning traverses its private vector and keeps each list/atomic receiver live through its await. Compression helpers transfer or copy input into their child tasks.
- L237, 242, 243, 244, 260, 263 **SAFE** · The key factory is a frame value and native KV reads hold storage after a failed join. Chunk spans point into the owned entry vector, and gzip receives an owned byte copy.
- L274, 276, 289, 291, 301, 306 **SAFE** · The caller retains the KV. Memo hits are moved into private storage before insertion and no memo pointer crosses suspension. Chunk-key callbacks own version and generation.
- L550, 553, 560, 564, 575, 576, 578, 581, 585, 586, 587, 590, 595, 596, 759, 760, 763, 765, 767, 768 **SAFE** · The catalog context holds KV under the documented heartbeat precondition until stop has synchronously cleared the timer and awaited the last renewal. Renewal callbacks own state and clear the temporary renewal chain on settlement.
- L625, 627, 631, 637, 639, 656, 663, 667, 669, 689, 691, 694, 696, 702, 705, 707, 714, 719 **SAFE** · The context or named caller retains the borrowed KV. Writes, compression, digest and pruning use frame-owned inputs and named atomic receivers across waits.
- L727, 730, 736, 740, 745, 748, 755, 773, 777, 782, 789, 791, 795, 797, 800, 804, 807, 809, 812 **SAFE** · Generation and lease callers hold the KV through each directly awaited task. Each loop or retry uses copied rows and result values before the next await.

### `src/catalog/types.cpp`

Reviewed metadata, lease and protocol constants and record conversion. The synchronous codecs return owned values and register no callbacks.

### `src/config.cpp`

Reviewed synchronous model and transport environment configuration. Views and regex inputs are consumed during parsing and derived configuration owns its values. No suspension or escaping callback.

### `src/debug_routing.cpp`

Reviewed debug-route cache reads and mutation helpers.

- L89, 102, 105, 122, 137, 141 **SAFE** · The cache hit returns a copy before any await. Each in-flight load or write retains its KV, parsed values and options until the delegated task settles.

### `src/decisions/handlers.cpp`

Reviewed the Decisions route's request body, selection, shared dispatch telemetry and handler wrappers.

- L235, 245, 249, 257 **SAFE** · The handler copies Request and UsageContext on entry, and its parsed JSON, model and telemetry context stay live in the frame through body and admission waits.
- L267, 269, 272, 273, 275, 287, 294, 297 **SAFE** · Dispatch, headers and before-dispatch callbacks own the shared telemetry context, while fetch owns its input and injected fetcher. The handler keeps the context through completion accounting.

### `src/defaults.cpp`

Reviewed the default key constants and synchronous key construction. No suspension or escaping callback.

### `src/handler/admission.cpp`

Reviewed process-controller initialization, route classifications and synchronous overload responses. The controller lives in static storage and response details are copied.

### `src/handler/http.cpp`

Reviewed background registrar invocation, idempotency principal, normalization, request IDs and response decorators.

- L29, 32 **SAFE** · The waitUntil registrar is copied and called synchronously with a Promise value. A registered operation has to own its own inputs through settlement.
- L65, 83 **SAFE** · The coroutine retains its owning auth variant and the named principal lives through child construction. Hashing takes a string value.

### `src/handler/index.cpp`

Reviewed route tables, admin and auth adapters, inference dispatch and upgrade handoff.

- L43, 54, 66, 73, 177, 211, 235 **SAFE** · Route matching borrows stable immutable static tables and produces an entry pointer into those tables. Normal lambdas start child Tasks with value arguments.
- L135, 140, 153, 156, 163, 167, 174, 179, 184, 188, 199, 207, 213, 217, 224, 227, 231, 237, 259, 262, 265, 268, 271, 274, 281, 285 **SAFE** · Dispatcher frames own the request, path and contexts through their direct waits. Regex matches are local owned snapshots and the upgrade adapter passes its inputs by value.

### `src/handler/serve_handler.cpp`

Reviewed request-to-delivery abort forwarding and handoff.

- L12, 14, 16, 17, 18, 24, 28, 41, 42 **SAFE** · The serve coroutine copies the injected callable, and its armed scope guard removes the temporary abort listener on handoff or unwind. Transport completion owns the downstream controller through its success or failure.
- L13, 14, 16, 17, 24, 25 **DEFECT** · The abort listener owns the request signal that stores it. If lifecycle setup throws before returning the handoff guard or a direct caller drops the handoff, the unaborted signal retains itself and also pins the downstream controller. Capture the request signal weakly.

### `src/handler/terminal_log.cpp`

Reviewed body and delivery observation, streaming source ownership, coalesced quota terminal work and Sentinel persistence.

- L40, 46, 82, 89, 94, 163, 175, 182, 186, 193, 195, 196, 200, 207, 215, 219, 227 **SAFE** · Once write registration succeeds, the frame owns the inputs and observes the writes to settlement. The lexical scope guard uses its own live input and scratch JSON storage stays in the inline formatter.
- L61, 162, 174, 181, 193, 200, 207, 215, 219 **DEFECT** · A coroutine-valued injected writer can suspend before the join is installed. If a later writer throws synchronously, logTerminalRequest drops the exact invoked callable from its copied input, and the first writer can resume through its dead closure. Keep a shared owner of the invoked callback with each immediately registered writer completion.
- L272, 273, 278, 281, 287, 292, 294, 297, 311, 312, 328, 338, 351, 353, 357, 359, 360, 361 **SAFE** · Replay and logging tasks retain self. Each quota terminal operation stays alive through the registered finalizer, which clears the matching work slot and the temporary holder when it settles.
- L384, 386, 389, 390, 393 **SAFE** · Buffered handling owns state through response handoff and any registered completion reaction owns state through terminal logging.
- L417, 424, 426, 427, 436, 440, 446, 448, 451, 452, 458, 459, 460, 471, 474, 477, 480, 541, 542 **SAFE** · The stream runtime SourceAlgorithm::runAsync owns the pull callable and its state capture through the member task, including close or error clearing the stream's callbacks. Cancel is synchronous and the original reader remains owned.
- L519, 521, 531, 532, 533, 538 **DEFECT** · SSE completion keeps a body-outcome latch alive through its logging reaction. If transport header preparation throws before reading, runtime can finish a fallback response without ever cancelling the SSE body. The body latch retains the log, process permit and request drain indefinitely. Cancel the unconsumed body when native response setup fails.

### `src/handler/terminal_route.cpp`

Reviewed admission, quota gates, terminal accounting hooks, routes and background exception replay.

- L52, 69, 75, 94, 100, 101, 110, 166, 169, 176, 177, 183, 199, 206 **SAFE** · The outer route or scheduled omission owns state, request and identifiers. Permit callbacks own state and are transferred through responses, while borrowed admission helpers are awaited directly.
- L210, 213, 218, 221, 230, 233, 235, 238, 260, 264, 270, 281, 282, 286, 306, 307, 316, 323 **SAFE** · The outer route retains the receiver, and terminal settlement callbacks copy its shared state into logging. Both reservation tasks keep their receivers through waits.
- L331, 334, 344, 349, 353, 356, 363, 370, 374, 376, 379, 381, 385, 387, 389, 391, 398, 401, 404, 406, 408, 410, 415, 417, 419, 423, 425, 427, 431, 433, 435, 439, 441, 443, 445, 449, 451, 453, 455, 460, 461, 464, 465, 466, 470, 471, 472, 473, 474, 478, 490, 491, 497, 506 **SAFE** · Every route factory owns shared State in its direct awaiting frame and each downstream task gets an owning context or a borrow from that retained State. No sibling can outlive the frame through a fail-fast join here.
- L523, 524, 526, 529 **DEFECT** · An injected Task-returning persistence callable can be stored in the Promise-returning std::function through implicit Task adoption. A successful background registrar lets the options frame die while that coroutine is pending. Invoke a shared copy of the callable and keep that same copy in the persistence settlement chain.
- L536, 542, 547, 548, 551, 554, 558, 560, 562, 564, 567, 579, 587, 589, 591 **SAFE** · The owning route frame holds State, authentication, permit and quota context. Task factories pass values into children or run only while an owner in the awaiting chain remains live.

### `src/health.cpp`

Reviewed passive fan-out, provider bindings and the active-probe timeout race.

- L125, 134, 138, 140, 145, 154, 169, 202, 203, 210, 211, 213, 218, 220 **SAFE** · Auth hash children take a whole account or string copy, and independent health and quota operations own their arguments. Cache and endpoint bodies are named frame values.
- L229, 231, 243, 246, 249, 258, 270, 271, 275, 277, 283, 286, 288, 296 **SAFE** · Active probe tasks own account or credential, optional KV, signal and shared progress. Aggregation reads copies or uses shared progress on the same loop after child completion.
- L306, 311, 312, 313, 316, 317, 319 **DEFECT** · The probe or its failure can win, then finalization clears the only timer that settles the private timeout Promise. That pending source retains the combined outcome and the active result until the process loop ends. Use withResolvers and settle the timeout loser after clearing its timer.
- L306, 324, 326, 329, 332, 343, 344, 349, 350, 364, 373 **SAFE** · Coalescing copies the in-flight promise before waiting. Passive joins use independent tasks and subsequent views point into the named tuple or immutable frame snapshots.

### `src/http.cpp`

Reviewed synchronous OpenAI error/JSON creation, CORS and request-response helpers. Headers and other input views are consumed synchronously and outputs retain the required response/body handles.

### `src/inference_admission.cpp`

Reviewed release callbacks, fair queue updates and queued abort/timer ownership.

- L56, 57, 58, 84, 88, 149, 158, 178, 179, 205 **SAFE** · All queue and map mutations execute synchronously. Permit release owns the controller and normal grant, caller abort and timeout remove the waiter and its event listener before resolving.
- L170, 172, 176, 177 **DEFECT** · A queued waiter owns its listener and the listener strongly captures both that waiter and its controller. Dropping an isolated loop before the unreferenced wait timer fires drops the timer without invoking abort, so the controller, entries and signal remain in a cycle. Let registered waits use weak listener captures and put the lease of controller ownership in an external clearable timer.

### `src/inference_deadline.cpp`

Reviewed the three stream/paid timer factories and request-deadline signal composition.

- L15, 16, 24, 25, 32, 35, 38, 51, 52, 55, 59, 62, 65, 83, 84, 89, 91, 94, 97 **SAFE** · Timer callbacks copy the abort controller and active flag, and returned handles use timer identity. The timer owns no callback that retains its returned deadline handle, and clear or expiry releases the registration.

### `src/input_normalization.cpp`

Reviewed synchronous payload, tool, message and schema normalizers. Any reference-capturing algorithms run inline and normalization owns its output. No suspension or deferred callbacks.

### `src/images.cpp`

Reviewed request-body and multipart parsing, image fan-out and cancellation coordination, response aggregation and public adapters.

- L131, 227, 228, 236, 274, 285, 287, 293, 296, 302, 304, 312, 347, 354 **SAFE** · File tasks and form parsing retain their byte and request owners. The file loop traverses a private unchanged vector, and the body cleanup guard's references remain in its own frame.
- L376, 380, 382, 385, 388, 390, 445, 450, 454, 457, 463, 464, 468, 470, 548, 549, 551 **SAFE** · Fan-out callbacks and child tasks each own their shared state. The child waits with its context and dispatch closure live, allSettled waits for every child, and response reordering uses fresh local iterators.
- L480, 499, 512, 515, 521, 522, 528, 563, 566 **SAFE** · The facade copies context before making the Task, and the frame owns parsed variants, response vectors and upstream receiver handles. No other child mutates the vectors during traversal.

### `src/jev_compaction/compaction.cpp`

Reviewed compaction and rewrite tasks, request parsing, timed fetch callbacks, and stream wrapper cleanup.

- L185, 197, 213, 335, 355, 376 **SAFE** · Build and request tasks own their JSON and options. The shared asker and injected rewrite closure remain live through direct waits, and JSON member views point into the caller's immutable value.
- L261, 266, 268, 282, 381, 385, 388, 390, 392, 397, 398, 400, 401, 407 **SAFE** · Fetch and source adapters retain their callable through returned Tasks. The pending response reaction and stream own shared State, and abort callbacks use weak state with release on normal rejection, drain or cancel.
- L238, 246, 250, 385, 388, 390 **DEFECT** · A timed fetch wrapper can be discarded after returning a body that has buffered a chunk and while its external abort signal remains live. State destruction leaves the weak forwarding listener installed, and the weak timeout callback can no longer unregister it. Release on State destruction as well as normal stream completion.

### `src/jev_compaction/hybrid.cpp`

Reviewed non-streaming residue jobs, worker fan-out and synchronous transcript helpers.

- L113, 116, 120, 121, 123, 189, 190 **SAFE** · Each worker holds shared work plus a copied chunk and owns its fetch options and response. Shared vector sizes never change after dispatch, and workers use reserved distinct indices even if their parent join fails.

### `src/kv.cpp`

Reviewed coalesced KV open and mutable test opener configuration.

- L56, 57, 85, 86, 97, 98 **DEFECT** · An injected capturing Task opener suspends after being invoked directly out of the mutable global opener. setKvForTest can replace that function before the suspended open resumes. Copy the selected opener into the awaiting frame before invocation.
- L61, 72, 80, 82, 86 **SAFE** · Production uses the stable free-function opener and Promise settlement retains the store. The finalizer refers to process-local state, and the default/test resolved-value adapter owns its KV.

### `src/kv_conversion/cli.cpp`

Reviewed argument parsing, conversion dispatch and CLI error translation.

- L21, 31, 51, 52, 67 **SAFE** · The CLI frame owns input flags and the local validator runs synchronously. Conversion receives named options by value and the top-level wrapper directly awaits the CLI task.

### `src/kv_conversion/convert.cpp`

Reviewed staged SQLite source ownership, transcoding, snapshot callbacks and commit/rollback cleanup.

- L133, 152, 165, 222, 231 **SAFE** · Snapshot and transcode operations own their paths, options, row data and codec handles. They use named task results through hashing and conversion.
- L249, 252, 253, 254, 262 **SAFE** · The transfer owns the source reader, destination database, statement and cursor across per-row awaits. Each row is a value snapshot, no other task touches the private staging handles, and the CLI awaits transfer before closing anything.
- L456, 459, 469, 473, 478, 481, 482, 486, 488 **SAFE** · The snapshot frame holds the erased callable and reader for the entirety of its directly awaited invocation. Snapshot inputs and results are copied.
- L509, 516, 527, 531, 538, 545, 548, 571, 573, 586, 588, 591, 658 **SAFE** · The converter's signal check only reads its own options inline. The frame keeps snapshot and output handles until the named transfer and flush complete and owns rollback targets through abort.

### `src/kv_conversion/cursors.cpp`

Reviewed synchronous packing, cursor migration and direction/key parsing. Input spans are used only to create owned copy data. No suspension or escaping callback.

### `src/kv_conversion/legacy_codec.cpp`

Reviewed the offline codec subprocess, stdout/stderr streams, callback lifetime, transfer and close paths.

- L75, 78, 80, 82, 84, 101, 111, 191, 200, 201 **SAFE** · The public facade transfers its shared codec State to the exchange tasks. Buffer scans happen synchronously after owned reads, with the process reader and file reader retained in their awaiting frames.
- L120, 122, 123, 136, 137, 144, 155, 158, 159, 160, 161, 162, 174, 175, 176, 179, 202, 203 **SAFE** · The abort callback owns only the subprocess and is unregistered by close or abort. The subprocess read tasks capture stream state before being joined, and conversion serializes codec calls while retaining the codec owner.

### `src/kv_conversion/native_schema.cpp`

Reviewed SQLite adapters, schema and row metadata materialization. Live statements and iterators have shared database handles and synchronous callbacks consume local validation views.

### `src/kv_conversion/profile_json.cpp`

Reviewed entry/profile codecs and the asynchronous KV export stream.

- L370, 371, 373, 375, 383, 385 **SAFE** · Both capturing coroutine lambdas are stored by SourceAlgorithm, whose runAsync retains the exact closure through completion even after stream close clears registered callbacks. The closure shares the list iterator and native lists own storage.

### `src/kv_conversion/proof.cpp`

Reviewed offline proof fixtures, subprocess output joins, typed KV writes and the command entry point.

- L456, 506, 509, 510, 511, 512, 513, 567 **SAFE** · The subprocess and output responses create independent owning read operations. The result tuple is named before taking references and hashing takes an owning path.
- L593, 600, 604, 609, 614, 620, 625, 627, 629, 633, 637, 639 **SAFE** · The latest-state proof holds shared KV and output cells and owns all snapshots, atomic receivers and iteration state through writes and reads.
- L673, 676, 699, 701, 706, 708, 714, 720, 729, 732, 734, 744, 748, 753, 757, 763, 769, 771, 790, 797 **SAFE** · The outer proof owns all paths and fixture configuration and starts one named task at a time. Result and error views are local to the corresponding frame.

### `src/kv_conversion/typed_value.cpp`

Reviewed the synchronous typed graph encoder, decoder, key handling and graph slot traversal. Frames and decoded values have established local or graph owners. No asynchronous suspension or escaping callback.

### `src/lib/jev_compaction/client.cpp`

Reviewed Jev client's request assembly and response consumption.

- L24, 35, 36, 39 **SAFE** · The shared asker in askBatch retains the JevClient through ask, Fetch retains its own callable, and the response has a named owner during body parsing.

### `src/lib/jev_compaction/codex_items.cpp`

Reviewed synchronous incremental-input, last-turn and history conversions. Temporary views and variant borrows remain inside each call and results own their contents.

### `src/lib/jev_compaction/compact.cpp`

Reviewed batch and window fan-out, state traversal and compaction result assembly.

- L138, 142, 143 **SAFE** · Each batch owns its shared asker, state JSON and calls while its virtual member Task runs. The parsed answers are read from a named owned response.
- L334, 346, 353, 357, 374, 377, 378 **SAFE** · An escaping worker owns WindowWork. Its task vector never changes after launch, and each worker's batch traversal is private. Answer writes use no shared iterators across a wait.
- L419, 430, 431, 432 **SAFE** · Window and ordinary batch children receive owning values and the shared asker. Named results are traversed only after joining.

### `src/lib/jev_compaction/jev_codex_smoke.cpp`

Reviewed the offline smoke tool's fake server, subprocess output pumps, RPC timeouts, transport callbacks and shutdown. Reviewed CLI code by reading and did not invoke it.

- L180, 182, 183, 184, 188, 202 **DEFECT** · FakeChatUpstream owns its HttpServer handle, whose serving-state handle owns the callback capturing fake. Shutdown leaves both handles stored, and a startup rejection can also abandon the cycle. Capture a weak fake in the server function and retain a locked owner in each actual request Task.
- L315, 317, 318, 323, 328, 340, 361, 382, 384, 385 **SAFE** · AppServer startup or the two main owners explicitly keep the receiver through shutdown and all pump waits. The synchronous packet handler copies the pending-map entry before erasing it.
- L402, 404, 405, 408, 418, 436, 439, 440 **SAFE** · RPC waits keep their AppServer receiver and callables alive. The timeout always fires and a late RPC response or stdout close settles the reply, while process stop waits for exit and both pumps.
- L479, 480, 485, 496, 522, 537, 539 **SAFE** · Thread callbacks copy the RPC owner and ID into owned Tasks. Notification scans use fresh indexing and consume each borrow before yielding.
- L557, 562, 565, 583, 584, 586, 587, 592, 593, 596, 649, 654, 658, 663, 665, 667, 670, 675, 681, 684, 686, 696, 698, 702, 704, 722, 724, 733, 746, 779, 781, 785, 790, 807, 809, 811, 820, 835, 854, 855, 857, 861, 863, 864, 876 **SAFE** · Smoke startup, gateway stop and thread work keep copied fixture children and paths through each wait. Fetch owns its callable through use and normal teardown replaces it after subprocess waits.

### `src/lib/jev_compaction/jev_compaction_smoke.cpp`

Reviewed the local smoke run, owned mock transport callbacks, terminal polling and gateway lifecycle. Reviewed CLI code by reading and did not invoke it.

- L227, 241, 246, 250, 268, 269, 271, 272, 278, 279, 280, 287, 288 **SAFE** · Polling owns its predicate and the gateway handle's noncoroutine stop factory copies the server and store into the child. Store close occurs after the server shutdown wait.
- L294, 305, 321, 336, 337, 342 **SAFE** · The fake server task owns its request and shared call vector. Fixture preparation and reads keep the native-store and artifact inputs through completion.
- L349, 361, 368, 372, 388, 390, 392, 408, 412, 418, 421, 435, 441, 444, 464, 466, 468, 482, 490, 499, 502, 504, 506, 515 **SAFE** · Terminal polls retain shared console capture through their await, Fetch retains its callback and the outer runner keeps gateway, upstream and utility handles through normal cleanup.

### `src/lib/jev_compaction/request.cpp`

Reviewed synchronous Jev request serialization and response parsing. Header, payload and response values own their data. No suspension or escaping callback.

### `src/lib/jev_compaction/state.cpp`

Reviewed synchronous token estimation, tool-call matching, history reduction and rendering. Predicate and mutation callables execute within the input owner's scope and output state owns its data.

### `src/lib/jev_compaction/types.cpp`

Reviewed synchronous representation codecs and error translation. No retained callback or suspension.

### `src/metered_quota.cpp`

Reviewed provider fetch and decoding, quota cache, CAS and lease refresh, balance writes and history traversal.

- L132, 138, 139, 146, 594, 612 **SAFE** · The request frame and Fetch each retain their callable and owning network arguments. The Response is named, and parsed JSON pointers or credential views are consumed from owned values.
- L314, 322, 323, 338, 347, 355, 360, 371, 373, 392, 413, 414 **SAFE** · Cache and lease tasks retain their shared KV and snapshots. The synchronous invalidation wrapper immediately dispatches an independently owned native read before the join.
- L432, 433, 434, 435, 441, 443, 448, 457, 461, 467, 472, 476, 487 **SAFE** · Refresh holds its named tuple and atomic receiver. Owned option, signal and credential values remain live through provider observation, and hash and history children own their inputs.
- L684, 687, 690, 691, 701, 704, 712, 727, 741, 750, 751, 776, 786, 799, 805, 831, 872, 883, 893 **SAFE** · Quota callbacks and stale cache state live in the directly awaiting frame. History lists retain their iterator and use fresh owned entries, while control operations transfer owning values.

### `src/models/catalog.cpp`

Reviewed public and native model catalog projection, provider fan-out and capability routing.

- L44, 170, 173, 175, 182, 190, 198, 203, 205 **SAFE** · Each fetch or runtime snapshot child owns its options and arguments. Joining may fail early with independent siblings, and the results and KV are named value owners.
- L237, 238, 239, 246, 251, 257 **SAFE** · Local tuple values hold catalog and selection through later KV awaits. Merge algorithms execute synchronously, and no child shares a mutable projection container.

### `src/models/codex_models.cpp`

Reviewed model snapshots, capability hints, context limits and record transformation. String and JSON views are synchronous, and materialized snapshots own their contents. No suspension or escaping callback.

### `src/models/codex_models_availability.cpp`

Reviewed process caches, background refresh, KV merges and unsupported-model tracking.

- L287, 293, 299, 312, 325, 327, 328, 332, 333, 429 **SAFE** · Refresh is started independently with owning inputs and a finalizer pointing only to static state. Cache snapshot pointers from the synchronous getter are consumed before any suspension.
- L339, 353, 366, 370, 371, 375, 392, 394, 406, 409, 411, 478, 482 **SAFE** · The merge callback, database, entries and atomic operation are retained in their frame. Learned-map borrows are confined to a synchronous block and the test observer copies the in-flight Promise.

### `src/models/codex_models_fetch.cpp`

Reviewed single and union account discovery, bounded response handling, snapshot access and credentials validation.

- L49, 51, 53, 57, 60, 62, 64, 143, 156, 169, 175, 180, 182, 196 **SAFE** · The iteration borrows only from owned immutable account entries. Each auth/provider child receives copied values and its signal. Any shared-auth task continuing after abort owns its own input. The shared wrapper's loser retention is recorded under out-of-area observations.
- L210, 233, 235, 258, 274, 276, 292, 300, 302 **SAFE** · Validation and response readers own their Response and optional stream through reads and cancellation. Refresh inputs and parse outputs are values and no body receiver is a temporary.
- L330, 335, 342, 347, 381, 387, 442, 451, 467, 481, 493, 499 **SAFE** · Pool and URL collections are frame-owned and immutable while iterating. Callback and options snapshots stay live and failures are reported synchronously. Retried tasks own account, URL and response inputs.
- L544, 554, 555, 556, 582, 590, 591, 592, 593, 617 **SAFE** · The KV owner and copied reads outlive their awaits. Promise reactions return copies and the two named rows remain live through the atomic transaction.
- L712, 715, 730, 743, 756 **SAFE** · The validation frame retains copied auth, response and options, and starts separate named tasks for read and retry.

### `src/models/codex_models_whitelist.cpp`

Reviewed whitelist selection, retention and KV writes.

- L134 **DEFECT** · The strong-read options aggregate is constructed in the await full-expression, which the GCC 12.2 porting rule forbids. Bind options and the get Task to named locals before awaiting.
- L140 **SAFE** · The frame owns KV and serialized input and waits for the storage-owning native put wrapper.

### `src/models/metadata.cpp`

Reviewed synchronous model capability normalization, defaults and metadata merging. Input views are consumed synchronously and copied into the returned hints. No suspended or retained callback.

### `src/models/openrouter_models.cpp`

Reviewed model refresh coalescing, injected Fetch ownership and the lookup cache.

- L221, 222, 227, 234 **SAFE** · The request task and Fetch retain the callable and signal. Every body operation uses a named Response or owned stream.
- L158, 174, 179, 203, 206, 207, 239, 240, 245, 298, 306, 353, 356 **DEFECT** · A refresh replaces cache before building the new match index. If index construction throws, the refresh catches the exception and leaves the old index pointing into a retired snapshot; later metadata lookup may copy a freed row. Build the snapshot and index before publishing either shared pointer.
- L320, 326, 327, 332, 333, 337, 342, 346, 351, 354, 355, 356 **SAFE** · The module-state reference points to static storage, and the coroutine copies fetcher and pending Promise before suspension. A detached refresh gets its own inputs, and synchronous matching finishes before the shared snapshots can be replaced.

### `src/openai.cpp`

Reviewed default model and reasoning waits and synchronous request signal and schema accessors.

- L22, 27, 31, 46 **SAFE** · The defaults use named runtime snapshot values. Signal accessors copy the request/context signals synchronously before they return.

### `src/openai_schema.cpp`

Reviewed static allowlists and synchronous schema accessors. Views refer to static literals and no callbacks are registered.

### `src/openai_telemetry.cpp`

Reviewed response and request telemetry WeakMaps, aggregate state, callback dispatch and usage attachment.

- L100, 561, 562, 564, 565, 746, 759, 782 **SAFE** · The telemetry Task owns its handler callback, context copy and shared response state through the wait. The usage callbacks are invoked inline and the remaining telemetry helpers consume their pointers synchronously.

### `src/optional_telemetry_queue.cpp`

Reviewed synchronous payload sizing and error translation. Temporary child pointers are read while the caller's JSON remains unchanged. The asynchronous queue is implemented in the corresponding header.

### `src/quota_projection.cpp`

Reviewed synchronous balance/history projections and time-bin aggregators. Sort and erase predicates execute inline, and result bins own their data.

### `src/recent_model_context.cpp`

Reviewed synchronous context-window selection, source precedence and calculations. Views and pointers are used within each call, and projected results own their values.

### `src/release.cpp`

Reviewed release identifier declarations. The constants use static storage and there is no asynchronous operation.

### `src/request.cpp`

Reviewed one-shot raw body observers, bounded reads, rejection and JSON parsing.

- L42, 43, 47, 48, 56, 63, 64, 66 **SAFE** · The observer is copied before the WeakMap entry is erased. The synchronous callback is invoked with the request and bytes still owned by the caller, and the production observer holds its replay candidate weakly.
- L86, 88, 101, 103, 106, 125, 129, 140, 171 **SAFE** · The read frame owns its Request, stream, reader and chunk result until reader cleanup. Value borrows stem from private read results and no shared buffer is mutated across a wait.

### `src/request_policy.cpp`

Reviewed model routing metadata, provider joins and synchronous request field rules.

- L216, 219, 250, 257, 273, 274, 275, 276 **SAFE** · The frame owns model, route and snapshot while each provider task receives owning values. The joined result is named and metadata lookup consumes its element views synchronously.

### `src/responses_attempts.cpp`

Reviewed precommit iterator and options, primary and removed-provider attempts, semantic recovery and detached bookkeeping.

- L50, 51, 54, 58, 61, 64 **SAFE** · AsyncGenerator retains the capturing body callable until its generator Task completes, and that callable owns the delegate iterator copy.
- L82, 86, 93, 102, 105, 108, 115, 117, 120, 124, 129, 133, 137, 139, 143, 147, 149, 155, 159 **SAFE** · The current caller's shared request State outlives the awaited telemetry callbacks. The preparation frame owns response, options, iterator and event collections and invokes its inline fail callback.
- L179, 182, 197, 211, 214, 219, 232, 233, 236, 238, 248, 252, 257, 258 **SAFE** · The caller retains the usage context through the whole attempt, including before-dispatch and headers callbacks. The read and cancel children copy their circuit or response handles before leaving this frame.
- L265, 268, 270 **SAFE** · The deferred callback can contain a raw usage pointer. The checked-in circuit returns the already-settled value none, the recorder does no context access for none, and ordinary routing creates no probe.

### `src/responses_buffered.cpp`

Reviewed buffered iterators, accumulator, telemetry and cancellation.

- L74, 75, 78, 80, 84, 85, 87, 92, 99, 109, 112 **SAFE** · Generator and SourceAlgorithm holders own their callbacks, and the caller's options capture shared Delivery backing the usage pointer. Collection owns the source and awaits cleanup before releasing its frame.

### `src/responses_failover_stream.cpp`

Reviewed precommit iteration, event parsing and rewriting, stream delivery and lifecycle callbacks.

- L262, 276, 283, 287, 298, 301, 323 **SAFE** · Iterator helpers take copies and precommit owns buffers and event callbacks during direct awaits. Event references are used before the next loop iteration.
- L894, 916, 935, 942, 943, 956, 966, 979, 980, 984, 993, 1037, 1038 **SAFE** · Pull and cancel share flags, while each asynchronous pull owns State and controller. Original-event lookups become value snapshots before suspension and detached generator return owns its core.
- L944, 972 **DEFECT** · emitFailureTerminal receives an owning ResponsesStreamEvent aggregate by value directly in the await expression. The GCC 12.2 porting rule requires constructing the Task in a named statement before awaiting each call.

### `src/responses_handler.cpp`

Reviewed request-state construction, recovery, delivery and telemetry, including before-dispatch, event and terminal callbacks.

- L37, 38, 46, 48, 50, 52, 62, 69, 75, 77, 103, 112, 114, 116, 122, 140, 142, 144, 149, 150, 155, 161, 165, 168, 169, 171, 172, 173, 180, 183, 187, 206, 209, 214, 215, 223, 224 **SAFE** · The caller keeps its borrowed usage context in the directly awaiting frame. Recovery children each own State, responses and options, and variant borrows refer to named local snapshots.
- L233, 239, 241, 274, 324, 325, 326, 334, 341, 342, 343, 344, 355 **SAFE** · The detached capturing coroutine uses rt::async and shares Delivery. Buffered and stream callbacks own Delivery or complete in its owning frame. The terminal task accepts a copied Response.
- L315, 317, 318, 319, 360 **SAFE** · The interval closure owns a copied probe. The checked-in circuit provides no probe, so this timer cannot start on a current route. Terminal, cancel and failure callbacks clear an armed timer.
- L361, 365, 368, 372, 374, 375, 378, 379, 382, 383, 386, 388, 389, 392, 397, 398, 405, 407, 412, 415, 418, 420, 424, 426, 428 **SAFE** · Delegated request and telemetry tasks are directly awaited with their request, parsed body and context retained by the route/image caller. Streaming callbacks after handoff use a separate Delivery snapshot.

### `src/responses_native_headers.cpp`

Reviewed early-header dispatch wrapper and error capture.

- L26, 27, 55, 56, 65, 85 **SAFE** · The plain callback constructs a Task with copied request, headers and context. Header processing keeps its context and response through bounded reading, and the weak-map entry owns an error snapshot.

### `src/responses_request.cpp`

Reviewed parsed-body selection, model and reasoning metadata, and synchronous normalization.

- L61, 97, 103, 106, 110, 111, 145, 150 **SAFE** · Body and telemetry pointers borrow from the calling handler or image frame, which directly awaits these tasks. JSON member borrows refer to frame-owned snapshots, and the named metadata result stays live through reasoning normalization.

### `src/runtime_config.cpp`

Reviewed runtime configuration reads, coalescing and synchronous parsers.

- L204, 214, 225, 227, 343, 350 **SAFE** · Each waiter copies the Promise. Refresh retains stale config, KV and entry by value and only touches the process cache synchronously.

### `src/sentinel/incident_outbox.cpp`

Reviewed incident event creation and completion, degradation recording, index CAS, evidence binding and listing.

- L249, 251, 271, 334, 354, 364, 384, 412, 418, 422, 432 **SAFE** · The replay/degradation owners directly await the borrowing KV tasks. The frames retain their copied rows and inputs, and each atomic receiver survives until commit.
- L439, 443, 458, 464, 647, 652, 655, 657, 663, 672, 682, 690, 702, 727 **SAFE** · The environment task holds the shared KV through dispatch, and admin/replay callers retain the borrowed KV through listing and binding. UUID callbacks run inline, while keys, index and event snapshots and iterators stay named during awaits.

### `src/sentinel/replay_admin.cpp`

Reviewed authenticated replay listing, direct and paged read variants and output assembly.

- L131, 140, 155, 162 **SAFE** · The admin frame owns request, options, keys and shared KV while directly awaiting the borrowed-store listing and copying each result.

### `src/sentinel/replay_capture.cpp`

Reviewed the small translation unit used to link replay capture. No suspension or callback registration.

### `src/sentinel/replay_envelope.cpp`

Reviewed digest, compression, key import and synchronous replay envelope helpers.

- L300, 434, 446, 452, 466, 474 **SAFE** · Each crypto or compression frame owns byte vectors and key material. The synchronous decompressor callback copies each temporary span before returning, and the frame owns the assembled parts across its microtask.

### `src/sentinel/replay_limits.cpp`

Reviewed synchronous replay capacity arithmetic. No deferred operation or retained view.

### `src/sentinel/replay_model.cpp`

Reviewed replay key creation, value conversion, list shape validation and byte helpers. Spans and references are consumed synchronously and outputs own their data.

### `src/sentinel/replay_observation.cpp`

Reviewed bounded response cloning, SSE inspection, one-shot raw-body observation and retained replay inputs.

- L192, 196, 206, 207, 751 **SAFE** · The frame owns its cloned stream, reader and chunk buffers. The synchronous cancel wrapper copies its own stream owner before releasing the lock, and the response inspection retains its Response through cloning.
- L374, 595, 596, 597, 709, 764, 765 **SAFE** · The inline parser guard lives in the same frame as its receiver and index. Request observers own the candidate without a back-reference to Request and inspector callbacks own shared parser State, consuming passed spans synchronously.

### `src/sentinel/replay_read.cpp`

Reviewed encrypted listing and lookup, stale references, manifest and ledger reads, and authenticated decryption.

- L181, 189, 248, 272, 284, 290, 320, 324, 331, 348, 349, 356, 359, 368 **SAFE** · Admin and replay callers hold a shared KV owner while directly awaiting the borrowed-store helpers. Each reader retains its snapshots and named list/atomic receivers through the corresponding waits.
- L374, 377, 381, 384, 388, 413, 416, 420, 422, 424, 432, 449 **SAFE** · The caller retains KV while lookup tasks hold copied key, status and manifest values. The named status and tombstone entries back all borrowed views.
- L474, 476, 486, 513, 515 **SAFE** · Decrypt owns its export, key bytes and intermediate vectors. Hash, decompression and grouping children take independent values and cleanup guards run within that frame.

### `src/sentinel/replay_store.cpp`

Reviewed all persistence and dedupe stages, chunk ownership, cryptographic byte cleanup, KV lookups and environment adapters.

- L82, 86, 93, 99, 105, 119, 129, 133, 141, 208, 214, 220, 235, 251, 265, 267, 278, 290, 297, 309, 316, 320, 324 **SAFE** · The shared persistence dependency or awaiting caller retains borrowed KV. Stages own copied manifests, fingerprints and keys, and compression/encryption frames hold their own byte buffers through waits.
- L422, 424, 458, 463, 465, 466, 472, 478, 484, 485, 486, 487, 497, 503, 516, 519, 544, 547, 551, 568, 572, 573, 576, 595, 615, 621 **SAFE** · A shared immutable envelope retains store and chunks. Deletion starts independent storage-owning tasks, which may finish after a failed join. Atomic operations remain named through stage and publication commits.
- L661, 679, 714, 716, 725, 729, 740, 751, 788, 798, 799, 821, 841, 845, 853, 860, 865, 868 **SAFE** · Dedupe and persistence retain their dependency and metadata snapshots. In-frame zeroing guards run after reads and publication finish. The reference-taking API is a plain wrapper that copies before creating a Task.
- L905, 918, 926, 951, 959, 972, 980, 994, 999, 1009, 1015, 1020, 1024 **SAFE** · The environment wrapper constructs a Task with copied inputs. Its frame owns secret bytes, the store, incident entries and guards through ordered durable writes and settlement.

### `src/sentinel/upstream_capture.cpp`

Reviewed the captured upstream stream, attempt callbacks, sealing and disposal.

- L545, 572, 578, 580, 584, 624, 626, 656, 674, 675, 682, 709 **SAFE** · The stream runtime holds the capturing coroutine closure through each pull. The reader, recorder and attempt are shared owners in callbacks, with no reverse edge from recorded attempts to the wrapping stream. Retention and disposal update state synchronously.

### `src/sentinel/replay_retention.cpp`

Reviewed KV snapshot ownership and iterator, cleanup, fail-fast read, publication, eviction and maintenance paths.

- L102, 156, 249 **SAFE** · Joining and sorting use synchronous views and predicates. The temporary values and the sorted vector have owners throughout each call.
- L206, 213, 217, 233, 236 **SAFE** · The working frame owns each list cursor and collected candidates. The awaited callers retain the KV, and each cursor close is awaited before the cursor leaves scope.
- L257, 261, 263, 267, 271, 276, 280, 284, 298 **SAFE** · The input wrapper copies metadata before returning a Task. Directly awaited pruning keeps its candidate vector, version snapshots and atomic operation in the same frame.
- L305, 314, 317, 322, 323, 324, 325, 343 **SAFE** · Fail-fast siblings only read KV. Each runtime get copies storage and the key before suspending, readLedger performs its only KV use in the inline-started prefix, and references to joined values point into the named frame-owned tuple.
- L349, 352, 361, 369, 393, 394, 402 **SAFE** · Staging and quota helpers await only local KV work. Each iteration owns its rows and operation, while the calling persistence or maintenance frame holds the store.
- L435, 442, 445, 446, 447, 448, 449, 501, 505 **SAFE** · Publication wrappers copy rows and options. The eagerly invoked read helpers consume their KV borrow before suspension, and publication only reads references into its local joined tuple.
- L524, 528, 539, 545, 550, 554, 558, 565, 571, 576, 578, 585 **SAFE** · Abandonment owns its row and key snapshots. Its mutation callbacks are copied into the CAS Task and invoked synchronously. The caller retains KV until the awaited cleanup ends.
- L604, 605, 615, 619, 626, 636, 642, 644, 649, 654, 659, 667, 675, 680, 688, 690, 699, 710, 711, 717, 720, 728 **SAFE** · Reaping and payload deletion await frame-owned iterators and operations. The only candidate predicate runs synchronously, and range references point into immutable local vectors or values.
- L776, 777, 786, 792, 796, 814, 839, 844, 845, 846, 847, 848, 849, 850, 875, 887 **SAFE** · Finalization owns its row snapshots and atomic operation. The optional read and ledger read consume their KV borrow before suspension, and later tuple aliases refer to a named immutable local through the commit.
- L899, 903, 913, 915, 917, 924, 934, 942, 954, 955, 957, 961, 967, 976, 978, 986, 991, 994, 1003, 1007, 1009, 1014, 1017, 1026, 1030 **SAFE** · Selection owns the cursor and predicate, invokes the predicate synchronously and returns its own vector. Nested claims and cleanup use copied rows and named Tasks; the outer awaited frame retains KV.
- L1040, 1044, 1052, 1056, 1064, 1065, 1068, 1075, 1078, 1089, 1092, 1122, 1126, 1130, 1138, 1141, 1145, 1147, 1150, 1151, 1159, 1163, 1165, 1170, 1174 **SAFE** · Admission and maintenance keep owning options and snapshots in their frames. They await all local cleanup and CAS work, while the bootstrap helper separately manages its detached store owner.

### `src/sentinel/replay_retention_bootstrap.cpp`

Reviewed release and bootstrap batches, detached chunk-owner filling, weak KV identity guards and the bounded maintenance wait.

- L76, 78, 82, 84, 87, 92, 94 **SAFE** · An awaiting caller keeps KV alive. The release Task owns its reservation and key, and the copied CAS callbacks run synchronously while the operation is owned.
- L219, 226, 277, 280 **SAFE** · Entry members and delta references belong to the same coroutine frame; the legacy-release Task is constructed separately with owning arguments and directly awaited.
- L332, 360, 387, 400, 423, 425, 432, 435, 442, 446, 450, 464 **SAFE** · On the gateway loop the detached fill checks the KV identity before every fresh KV use. Native cursors and reads own their storage after creation. The fill owns the index, and map lookups copy its shared pointer before any suspension.
- L469, 472, 473, 476, 480 **DEFECT** · The owner fill can settle before the wait timer. Cleanup clears the only resolver, stranding the pending elapsed Promise and its race reaction for the process-loop lifetime. Resolve elapsed after clearing the timer, preserving the original outcome.
- L484, 491, 508, 513, 520 **SAFE** · The awaited preparation keeps its KV caller alive. The cursor, filled Promise and copied index handle stay owned while maintenance waits and later reads the index state.
- L536, 544, 546, 564, 569, 576, 581, 601, 608, 620, 626, 631, 636 **SAFE** · Bootstrap sweeps own their cursors and deltas, walk static prefix keys and only lend values within synchronous code. Every folded or closing Task is awaited before its cursor leaves scope.
- L668, 679, 697, 708 **SAFE** · The commit operation and materializations belong to the local frame, and the sweep's child receives an owned delta before suspension.
- L722, 725, 729, 746, 770, 776, 782, 787, 789, 793, 794, 803 **SAFE** · These directly awaited entry points borrow a store whose owning caller remains suspended. Ledger values and recursive work are established in named locals before awaiting.

### `src/sentinel/replay_retention_metadata.cpp`

Reviewed prefix scanning, status reconciliation and marker/failure reads and writes.

- L71, 97, 205 **SAFE** · Validation uses borrowed row members and field names only while their synchronous callers retain the values.
- L109, 125, 128, 132 **SAFE** · The frame owns the cursor and the array points to static prefix keys. Cursor close finishes inside its owner scope, and the awaited caller retains KV.
- L145, 147, 151, 153, 156, 160, 183, 193, 200 **SAFE** · The reconciliation chain directly awaits the scan and writes. It copies maintenance inputs, and each atomic operation remains frame-owned across its commit.
- L214, 219, 232 **SAFE** · Marker references point into the frame's immutable getMany result and end before the next suspension. The reconciliation child owns its options.

### `src/sentinel/replay_retention_schema.cpp`

Reviewed synchronous serialization and test injection, ledger snapshots, chunk iteration, and atomic mutation callbacks.

- L37, 39, 78, 83, 86, 234, 236, 245 **SAFE** · Text views and schema field lists are consumed synchronously. Parsed rows and preserved KV objects own their stored content.
- L326, 328 **SAFE** · The test injector is stored by value and invoked synchronously; current fixtures reset it after their awaited work.
- L417, 418, 432, 435, 439, 441 **SAFE** · The ledger helper's only use of its KV borrow starts a get that owns the native storage. The result and converted ledger are values local to the coroutine.
- L446, 454, 460, 465, 472, 476 **SAFE** · The two chunk helpers own their cursor and private key snapshots; each caller directly awaits them while retaining the store.
- L483, 485, 495 **SAFE** · The coroutine owns both synchronous mutation callbacks. Current callers return the same atomic operation covered by the frame's owning pointer through commit.
- L499, 501, 505, 509 **SAFE** · The awaiting caller retains KV; retention status construction uses only the frame-owned ledger snapshot after the read.

### `src/serve.cpp`

Reviewed startup, lifecycle signal registration, Mac maintenance, and ordering of request drain and KV closure.

- L20, 23 **SAFE** · Telemetry close is directly awaited, and the reporting snapshot is a value read after settlement.
- L42, 49, 50, 53, 59, 60, 65, 84, 87, 88, 89, 93 **SAFE** · Each maintenance Task owns the shared state and KV. Frame guards release its Promise slots, and the returned stop closure shares state until both task outcomes settle.
- L98, 104, 107, 118, 121, 124 **SAFE** · Provisioning and migration own their KV handles, and command-line options are copied into the directly awaited configuration coroutine.
- L136, 141, 146 **SAFE** · The runtime entry points delegate synchronously to process-static handlers and pass owning request and connection metadata.
- L155, 159, 160, 162, 163, 165, 166, 167, 169, 170, 171, 172, 173, 174, 181, 185, 187, 195, 196, 198, 199, 200, 201, 205, 206, 207, 211 **SAFE** · The process signal callbacks own copyable server handles. Startup retains the listener identities through ready and finished and removes them on success or unwind after registration.
- L159, 162, 163, 165 **DEFECT** · In the development launcher the second signal registration can throw after the first has been stored, before the removal guard exists. The process signal table then keeps the server handle and callback live after startup unwinds. Install the guard before either registration.
- L123, 124, 133, 156, 170, 174, 175, 192, 201, 216 **DEFECT** · Startup discards the migration Promise. If shutdown reaches KV close while that migration is suspended, its next read or atomic write uses a closed database even though a shared pointer retains the object. Keep the startup Promise and await it after server settlement and before closing KV, including the prepared launcher failure path.
- L225, 229, 231, 235, 239 **SAFE** · The options wrapper copies host and port before lazy startup begins. The main coroutine constructs owned arguments from the process argv vector before starting configuration.

### `src/static.cpp`

Reviewed static asset-map lifetime, process-wide configuration, request content negotiation and response assembly.

- L32, 140, 145, 157, 168 **SAFE** · Asset registration and path views are synchronous. The immutable process-static module owns the map, and each mapped asset has a shared owner.
- L174, 189, 220, 222 **SAFE** · Both asset coroutines own an asset pointer. The body bytes or text and extra headers remain values in their awaiting frames through response construction.
- L214, 215, 312, 317 **SAFE** · Header enumeration returns an owned vector. The ranking and sort callbacks consume references only inside synchronous operations on local containers.
- L343, 344, 347, 353, 360, 368, 373, 378 **SAFE** · Static lookup copies the shared asset pointer before awaiting. Root handling holds a request value, uses immutable module data, and retains the KV handle returned by the lookup.

### `src/systemone/handlers.cpp`

Reviewed request parsing, provider hooks and usage callbacks for the System One terminal route.

- L27, 28, 36, 48, 58, 61, 97, 110 **SAFE** · Static model and error views refer to literals. Parsed JSON entry pointers are consumed synchronously while input values remain owned.
- L124, 126, 132, 140, 141 **SAFE** · Request, parsed payload, dependencies and usage context are frame-owned. The usage call is awaited while its shared context remains live.
- L147, 148, 150, 151, 154, 155, 157, 158 **SAFE** · Provider hooks capture a shared context and are copied into the provider's owning frame. That frame awaits both the admission callback and the selected fetcher while retaining the exact callables it invoked.
- L109, 162, 165, 169, 170, 175, 178, 181, 184 **SAFE** · Detached health recording receives owned arguments and a free clock function. Completion recording is directly awaited with the shared context, and the public wrappers copy their override before returning the Task.

### `src/types.cpp`

Reviewed synchronous type parsing, serializers, and the variant visitor.

- L21, 49, 51, 62 **SAFE** · Stringified enum values refer to static literal storage. Parsing consumes the input view synchronously while its JSON string is alive.
- L801, 850, 862 **SAFE** · The variant visitor runs synchronously against the caller's value, and serializers return owned records or write into the caller's output.

### `src/upstream_wire.cpp`

Reviewed response provenance, error conversion, cancellation and buffered provider wire shaping.

- L128, 129, 130, 138, 199, 228, 234, 260, 276, 322, 329, 330, 331, 332, 338, 344, 348, 451 **SAFE** · Diagnostic views and JSON pointers are synchronous and point into live inputs or static literals. The provenance map stores only weak response identity and an owned label.
- L375, 381 **SAFE** · Cerebras SSE serialization appends synchronously to a local string, then gives the fully buffered result to the owning Response.
- L286, 289, 395, 397, 411, 414, 428, 430, 461, 463 **SAFE** · Error converters own their response, signal and diagnostic values. They await the bounded capture Task before reading frame-owned bytes and headers.
- L408, 409 **SAFE** · Stream cancellation synchronously captures the stream state or an owning rejection before returning the detached Task. The local stream wrapper can leave scope.

### `src/utils.cpp`

Reviewed byte and string views and both digest coroutines.

- L12, 32, 38, 40, 50, 74, 76 **SAFE** · Base64 and hex operations consume temporary or borrowed views during a synchronous call and return owning strings or byte vectors.
- L60, 63, 67, 70 **SAFE** · The digest frames own their input strings and digest buffers through their microtask yield.

### `include/uos/admin/api_key_audit.hpp`

Reviewed audit context values and the API for listing and handling change events.

- L25, 26, 27 **SAFE** · The listing adapter copies its borrowed input before returning and request handlers retain their shared KV argument.

### `include/uos/admin/api_key_mutations.hpp`

Reviewed the declarations for API key mutation and audit context propagation.

- L8, 9, 10, 11, 12, 13, 14, 15 **SAFE** · Each context overload copies the audit context into the child Task and transfers the request value before the caller can return.

### `include/uos/admin/api_keys.hpp`

Reviewed the input normalizers and API key administration interfaces.

- L21, 23, 25, 32, 34, 36, 38, 40, 44 **SAFE** · Normalizers read the caller's JSON synchronously and return owning strings or scalars.
- L30, 31, 48, 49, 50, 51, 52 **SAFE** · The create wrapper copies its context. Paid projections and history work receive owning records, keys or shared KV handles.

### `include/uos/admin/capacity_observation_sync.hpp`

Reviewed ingest options and the only injected callback.

- L24, 38, 39 **SAFE** · The wrapper copies options before delegation, and the stored clock function is later called synchronously.

### `include/uos/admin/chart_source_proxy.hpp`

Reviewed the shared capacity-push record and the proxy dependency interface.

- L16, 19, 23, 24, 26, 28, 29, 30, 31 **SAFE** · Proxy wrappers copy the dependency object, including shared push state. The detached push owns its inputs and releases the in-flight slot through its settlement callback.

### `include/uos/admin/claude_code.hpp`

Reviewed overloads, request ownership and injected credential, quota and clock dependencies.

- L11, 16, 17, 22, 24, 25, 28, 30, 31, 32 **SAFE** · The handlers receive request and options values. The clock is used synchronously, and the persistence operations remain within the awaiting frames that own their options.

### `include/uos/admin/codex.hpp`

Reviewed admin boundaries and callback aliases. The fail-fast catalog and refresh overrides at lines 35–49 are the interfaces for the three defects recorded under src/admin/codex.cpp.

- L20, 21, 22, 23, 30, 31, 32, 33, 34, 47, 50 **SAFE** · These request handlers take owning request or scalar arguments.
- L24, 25, 26, 27, 28, 29 **SAFE** · The telemetry and analytics interfaces take their callback objects by value, and the active callable remains in an awaiting frame for the child Task.

### `include/uos/admin/codex_reset.hpp`

Reviewed the two reset-consume overloads.

- L6, 7 **SAFE** · Requests and manual-reset dependency objects cross the boundary by value.

### `include/uos/admin/defaults.hpp`

Reviewed defaults and KV migration interfaces. The injected diagnostics slot at line 10 is the interface for the fail-fast defect in src/admin/defaults.cpp.

- L8, 9, 11 **SAFE** · Migration and ordinary defaults requests transfer Request values and take no borrowed asynchronous inputs.

### `include/uos/admin/error_log.hpp`

Reviewed the error input record and error, history and file-size interfaces.

- L66, 67 **SAFE** · The two error-recording overloads copy the input synchronously and make the optional shared KV owner part of the work.
- L126, 127, 128, 130 **SAFE** · History listing, status handling and request delegation receive scalar values, an owning override or a request value.

### `include/uos/admin/index.hpp`

Namespace imports only. The imported functions are already recorded with their declaring headers and implementations.

### `include/uos/admin/kernel.hpp`

Reviewed the admin kernel declarations against the already reviewed handlers.

- L7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 **SAFE** · The exported Tasks take no parameters or an owning Request. Fan-out ownership is inside the implementation.

### `include/uos/admin/sentinel_incident.hpp`

Reviewed the Sentinel incident administration dependency interface.

- L16, 17, 22, 23 **SAFE** · The handler owns the KV opening and listing callbacks and directly awaits both. A shared KV handle remains live through the borrowed list call.

### `include/uos/agent_messages.hpp`

Reviewed the request, KV and authentication test seam.

- L20, 21, 22, 23, 26, 27, 29, 30 **SAFE** · The handler frames own dependencies and the request. Authentication is directly awaited with the callable retained, and the clock and ID generator run synchronously.

### `include/uos/analytics.hpp`

Reviewed the key constructors and analytics persistence interfaces.

- L25, 26, 27, 28, 29 **SAFE** · Key constructors synchronously copy each string view into an owning KV key.
- L93, 94, 96, 97, 107, 108 **SAFE** · Log readers and writers receive owning IDs and records and an optional shared store through their by-value input or options.

### `include/uos/api_key_policy.hpp`

Reviewed quota decision and reservation callback interfaces.

- L56, 57, 74, 76 **SAFE** · The shipped reservation callbacks own their dependencies, and each returned Task copies the state it will use while cancel, release and dispatch are pending.
- L169, 170, 183, 184, 186, 187, 189, 190, 192, 193, 195, 203, 204 **SAFE** · Quota, usage and authentication Tasks take their policy, IDs, options and KV handles by value.

### `include/uos/api_keys.hpp`

Reviewed key and token helper declarations.

- L22, 23 **SAFE** · The key factories copy the view's characters before returning an owning key.

### `include/uos/auth/index.hpp`

Reviewed method variants, kind labels and authentication entry points.

- L23, 27, 35, 39, 45, 49, 53, 98, 99 **SAFE** · Authentication kind accessors return views of static literals, independent of how long the variant instance lives.
- L101, 103, 106, 108 **SAFE** · Each asynchronous authentication or route entry receives an owning request handle.

### `include/uos/auth/local_admin.hpp`

Reviewed loopback parsing and request-peer binding interfaces.

- L28, 30, 33, 35, 44, 45, 48, 59 **SAFE** · The borrowed arguments are consumed synchronously. Request-specific state retains only a weak request identity and owns the copied peer.

### `include/uos/auth/local_development_key.hpp`

Reviewed the pricing initializer interface and local provisioning options. The orphaned abort race itself is recorded in the implementation.

- L73, 78, 91, 92, 101, 102 **SAFE** · Pricing work owns the initializer callback and its signal independently of the caller that may finish on deadline; the callable stays in the frame awaiting the initializer.

### `include/uos/auth/passkeys.hpp`

Reviewed registration, session and user-management interfaces. The GCC comparison sites are recorded at their KV calls in the implementation.

- L74, 92, 108, 116, 139, 142, 143, 145, 146, 153, 154, 156, 158, 160, 166, 167, 169, 170, 172, 174 **SAFE** · Passkey, session and registration Tasks receive owning inputs and shared KV handles, and the literal cookie-name view has static storage.

### `include/uos/auth/passport.hpp`

Reviewed passport principal records and route interfaces.

- L16, 68, 71, 75 **SAFE** · The kind view points to a literal. Passport authentication and routing frames receive owning request and path values.

### `include/uos/auth/passport_sessions.hpp`

Reviewed the session lifecycle entry points.

- L34, 35, 37, 41 **SAFE** · Create, read and revoke operations own the grant or token and any replacement input throughout their asynchronous work.

### `include/uos/auth/relay.hpp`

Reviewed the relay origin parser interface.

- L13 **SAFE** · The parser consumes an optional string synchronously and returns an owning origin string.

### `include/uos/authority/admission_state.hpp`

Reviewed admission operation arguments, the portable receiver, and private CAS delegation.

- L46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 60, 61, 67, 69, 70, 71, 72, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 86, 89 **SAFE** · RPC and delivery adapters retain the shared authority; current direct callers retain it through the await. Inputs are values, and the owned store remains live for transaction retries.

### `include/uos/authority/authority_rpc.hpp`

Reviewed portable server/client interfaces and shared transport storage.

- L31, 37, 38, 42, 44, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 67, 69 **SAFE** · Current adapters keep the named RPC receivers alive and transfer values through the transport. Each receiver owns its delegated authority or transport.

### `include/uos/authority/contracts.hpp`

Reviewed the ordered record helper and authority store contract.

- L25, 26, 27, 38, 42, 243, 244 **SAFE** · Lookups borrow the record's own entries, used synchronously or against an immutable frame-local snapshot. Key iteration returns copied strings and the sorter runs inline.
- L215, 216, 217 **SAFE** · The authority owner retains the store during reads and CAS. Snapshot and update state pass by value.

### `include/uos/authority/credential_publication.hpp`

Reviewed the publication receiver and asynchronous verification interface.

- L49, 54, 57, 64 **SAFE** · The awaited publication has a named publisher owner that retains the verifier and store. The verifier receives a copied publication value.

### `include/uos/authority/delivery_outbox.hpp`

Reviewed the outbox interfaces and their forwarding ownership.

- L18, 20, 21, 24 **SAFE** · The synchronous methods copy the shared authority and handle into observation Tasks before returning, so the outbox can be released.

### `include/uos/authority/durable_store.hpp`

Reviewed synchronous SQL adapter pointers, cursor results and Promise-based store methods.

- L39, 43, 47, 53, 55, 56, 63 **SAFE** · The SQL storage contract invokes transaction callbacks synchronously. The durable store owns the storage and its cursor returns a materialized row vector.

### `include/uos/authority/native_api_key.hpp`

Reviewed usage provider constants, window helpers and admission-plan declarations.

- L23 **SAFE** · Provider views point to literals. Usage helpers consume input records synchronously and return owning plans.

### `include/uos/authority/native_api_key_audit.hpp`

Reviewed event inputs, cursor helpers and the audit-source callbacks.

- L54, 55, 56 **SAFE** · The event builder invokes the copied UUID, clock and release functions synchronously while their dependency owner is live.

### `include/uos/authority/native_api_key_policy.hpp`

Reviewed validation and write-plan interfaces. They accept borrowed JSON and records for synchronous calls and return owning policy values; there is no escaping callback.

### `include/uos/authority/native_authority.hpp`

Reviewed the native authority facade and conversion to owned runtime and control ports.

- L12, 15, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 **SAFE** · The facade copies its shared State into returned ports and Task factories. Parsed binding JSON is converted synchronously and each child retains the State it will access.

### `include/uos/authority/native_authority_api.hpp`

Reviewed the API subcomponent's SQL borrow and synchronous admission interfaces.

- L8, 16 **SAFE** · The enclosing native State owns SQL before constructing API and retains it throughout each synchronous command.

### `include/uos/authority/native_authority_codex.hpp`

Reviewed the Codex subcomponent's SQL, API and proof inputs.

- L9, 10, 25, 29 **SAFE** · The enclosing native State owns both dependencies. The signal method consumes the optional classification pointer synchronously while its frame-owned value remains alive.

### `include/uos/authority/native_authority_contracts.hpp`

Reviewed the native records and synchronous SQL adapter template.

- L512, 514, 515, 516, 518 **SAFE** · The adapter's transaction contract runs inline, so the reference to the template's local result remains valid. The SQL callback transfers its query, bindings and result rows by value.

### `include/uos/authority/native_authority_kernel.hpp`

Reviewed kernel command interfaces and their borrowed State members.

- L8, 9, 10, 22 **SAFE** · The enclosing authority State owns SQL, API and initial settings for the duration of each synchronous kernel operation.

### `include/uos/authority/native_authority_paid.hpp`

Reviewed paid accounting and reconciliation interfaces and their borrowed dependencies.

- L10, 11, 36 **SAFE** · The enclosing authority State owns SQL and API, and paid commands consume their borrowed arguments within synchronous storage transactions.

### `include/uos/authority/native_authority_principals.hpp`

Reviewed the principal subcomponent's callback and receiver boundary.

- L8, 10, 12 **SAFE** · The outer native authority Task keeps the enclosing State and its borrowed subcomponents alive, and the principal Task takes its binding and command by value.

### `include/uos/authority/native_authority_rpc.hpp`

Reviewed native RPC ports, callback-enabled transports and inline argument adapters.

- L34, 35, 38, 40, 41, 46, 47, 52, 53, 56, 61, 64, 84, 88 **SAFE** · Each issued server Task retains the bound port or copied callable through its wait, and the port itself owns its binding and native State.
- L69, 70, 71, 72, 73, 74, 76, 77, 97, 98, 99, 100 **SAFE** · The inline templates serialize arguments synchronously. The forwarded overload copies the object and transport into an owning Task before the temporary JSON leaves scope.

### `include/uos/authority/native_authority_runtime.hpp`

Reviewed the runtime verifier interfaces and the borrowing subcomponent.

- L7, 8, 9, 11, 13, 18, 21 **SAFE** · The bound State keeps the runtime receiver, verifier and all referenced subcomponents alive throughout direct asynchronous verification.

### `include/uos/authority/native_authority_sql.hpp`

Reviewed the inline transaction helper and materializing SQL adapters.

- L31, 32, 34, 38, 60 **SAFE** · The synchronous transaction contract keeps the local callback and receiver alive, while current transaction bodies return synchronous values and the initializer callback never escapes.
- L44, 49, 54, 55, 56, 57 **SAFE** · Read, write and paging templates consume temporary row and index references synchronously and return owning results.

### `include/uos/authority/native_codex.hpp`

Reviewed Codex selection and reset planning interfaces.

- L67 **SAFE** · The plan label points to a static literal and its associated record is an owning value.

### `include/uos/authority/native_codex_429.hpp`

Reviewed the retry classification interface and synchronous evidence declarations.

- L38 **SAFE** · The classifier takes an owning Response by value and copies its bounded result before using the response headers again.

### `include/uos/authority/native_codex_capacity.hpp`

Reviewed quota-class string views and the span-based capacity merge overloads.

- L68, 71, 77, 79, 81, 85, 88, 96, 99, 100, 101, 118, 134 **SAFE** · Capacity helpers consume views and spans synchronously, and the variadic wrapper keeps its backing vector live through the merge. Results and keys own their data.

### `include/uos/authority/native_codex_contracts.hpp`

Reviewed owning pool, capacity, slot and route records and their synchronous codecs.

- L232, 239, 263, 274, 281, 286, 344 **SAFE** · The alternative labels refer to static literals. Runtime data is stored in the owning record fields.

### `include/uos/authority/native_codex_evaluation.hpp`

Reviewed evaluation inputs, accumulator and returned plans. Container references and the optional persisted-state pointer are consumed synchronously.

### `include/uos/authority/native_codex_mutations.hpp`

Reviewed state and mutation plans from the synchronous proof and dispatch APIs.

- L102, 105 **SAFE** · Plan kind views point to static literals. The mutations take snapshots and produce owning values synchronously.

### `include/uos/authority/native_contracts.hpp`

Reviewed the native quota value contracts and synchronous helpers.

- L49, 58, 62, 67, 75, 79 **SAFE** · The plan labels refer to static literals, and variant members own their payloads.

### `include/uos/authority/native_kernel.hpp`

Reviewed synchronous usage-plan and kernel ID interfaces.

- L230, 233, 236 **SAFE** · Plan labels have static storage, and the plan records and optional results own their data.

### `include/uos/authority/native_kernel_contracts.hpp`

Reviewed quota result alternatives and synchronous serialization declarations.

- L144, 151, 159, 165, 174, 179, 190, 199, 204 **SAFE** · Each kind label is a static literal and the results carry owned state.

### `include/uos/authority/native_paid.hpp`

Reviewed synchronous paid usage, lease and rollup helpers, including borrowed key and correlation views.

- L53, 54, 55, 56, 57, 62, 66, 71, 105, 123, 163, 174, 209, 213, 218, 235, 236, 245 **SAFE** · Paid views are consumed within synchronous key or settlement planning. Key and plan values retain their own data, and static labels refer to literals.

### `include/uos/authority/native_paid_contracts.hpp`

Reviewed paid authority's row and outcome records and synchronous codecs.

- L227, 232, 237, 244, 285, 290, 321, 325, 329, 333 **SAFE** · Kind views refer to static literals and payment evidence travels in owning outcome records.

### `include/uos/authority/native_paid_policy.hpp`

Reviewed synchronous validators and paid-field projection. Input pointers are borrowed only during validation and the projection returns owned fields.

### `include/uos/authority/native_principal_contracts.hpp`

Reviewed the principal verifier, audit and provisioning callback declarations.

- L163, 169, 170, 171, 172, 173, 175, 176, 177, 178, 185 **SAFE** · The native State retains the stored verifier and source callables through their direct awaits. Bound runner callbacks copy their State into work and the remaining hooks are synchronous.

### `include/uos/authority/native_runtime_contracts.hpp`

Reviewed runtime, routing and retained-history verifier interfaces.

- L188, 189, 191, 193, 201 **SAFE** · The parent State retains all verifier callables, and arguments are passed by value. The method name views refer to static literals.

### `include/uos/bounded_response_body.hpp`

Reviewed the bounded body API and its response/signal options. The abandoned abort loser is classified at the race in the implementation.

- L45, 49 **SAFE** · The reader takes Response and options by value, so input handles survive while its Task awaits chunks.

### `include/uos/catalog/index.hpp`

Reviewed catalog handler dependencies and by-value request routing.

- L10, 11, 13, 14 **SAFE** · The handler and degradation wrapper copy their callbacks. The clock runs inline and the awaited degradation child holds the exact invoked callable.

### `include/uos/catalog/models.hpp`

Reviewed borrowed KV updates and the inline response model filter.

- L16 **SAFE** · The refresh context keeps the store alive throughout the awaited update, and the Task owns its remaining arguments.
- L19, 21, 24, 26 **SAFE** · The model filter walks caller-owned data synchronously and returns copied models.

### `include/uos/catalog/promotion.hpp`

Reviewed the catalog promotion boundary.

- L20 **SAFE** · The promotion Task owns its shared KV and promotion input from creation through commit.

### `include/uos/catalog/store.hpp`

Reviewed catalog KV borrows, refresh lease callbacks and parser views.

- L23 **SAFE** · The body parser consumes a view synchronously, then returns an owning JSON object.
- L26, 39, 43, 45, 55, 57, 59, 61 **SAFE** · The request or refresh Task retains the KV handle and directly awaits the helpers that borrow it. Each helper owns version, owner and input values.
- L49, 50 **SAFE** · The refresh state retains both the heartbeat controller and the shared KV context until the stop Task has drained renewal.

### `include/uos/catalog/types.hpp`

Reviewed the catalog snapshot types. They own stored chunks, tags and parsed JSON and declare no asynchronous callback.

### `include/uos/config.hpp`

Reviewed process configuration storage. Configuration values and returned build identity strings have owners, and this interface declares no suspension or escaping callback.

### `include/uos/debug_routing.hpp`

Reviewed debug route configuration interfaces and operation parameters.

- L30, 31, 36, 37 **SAFE** · The load and set methods own their routing choice and take durations or clock readings by value.

### `include/uos/decisions/handlers.hpp`

Reviewed the optional usage-context pointer and provider test dependencies.

- L14, 15, 18, 19 **SAFE** · The public functions copy the context and dependency object synchronously. The owned implementation holds the selected fetcher through its direct wait and calls the API key supplier inline.

### `include/uos/defaults.hpp`

Reviewed defaults keys and normalization declarations. Keys have static storage and synchronous normalization returns an owning string.

### `include/uos/handler/admission.hpp`

Reviewed admission controller access and local overload response interfaces.

- L27, 32, 39, 41, 49 **SAFE** · The route set and error label have static storage. The controller is a shared-state value and the borrowed overload cause is consumed synchronously.

### `include/uos/handler/http.hpp`

Reviewed delivery completion, background registration, principal resolution and routing declarations.

- L23, 29, 37, 40 **SAFE** · The delivery metadata owns its Promise handle. The registrar receives a Promise copy synchronously. The runtime owns the registrar slot and the scheduled task has independent ownership obligations.
- L54, 66, 68, 81, 95, 97, 100 **SAFE** · Principal inputs are passed by value and route normalization consumes borrowed text inline. Kind and header names refer to static literals.

### `include/uos/handler/index.hpp`

Reviewed the inline agent route table and its coroutine lambda.

- L18, 23, 24, 25, 26, 28, 29, 32, 33 **SAFE** · The route table is immutable process storage. Its only coroutine lambda is captureless, and its frame owns the Request and both awaited results.
- L36, 38 **SAFE** · The request and delivery or upgrade data pass by value into their handler frames.

### `include/uos/handler/serve_handler.hpp`

Reviewed the delivery handoff closure and injected request callback.

- L11, 14, 16, 18, 19, 21 **SAFE** · The returned handoff owns its delivery state and removes the request listener. Created handlers transfer the injected callback into each awaiting request Task before it starts.

### `include/uos/handler/terminal_log.hpp`

Reviewed terminal registration, delivery metadata and writer callback interfaces. Their multi-writer and body-delivery lifetime sites are classified in src/handler/terminal_log.cpp.

- L31, 33, 34, 35, 51, 55, 62, 65 **SAFE** · Callbacks, delivery handles and signal transfer by value into the terminal state. The quota finalizer retains it through the terminal await, and the release callback itself runs synchronously.

### `include/uos/handler/terminal_route.hpp`

Reviewed terminal route descriptions, handler inputs and the replay override declaration. The detached lifetime defect is recorded where persistInferenceExceptionReplay invokes the override.

- L25, 26, 28, 36 **SAFE** · The shipped route views refer to static literals. The handler copies the request, path and delivery context into its main Task.

### `include/uos/health.hpp`

Reviewed passive and active health entry points. The active probe's abandoned timeout is recorded at the race in its implementation.

- L9, 11, 12, 13, 14, 16 **SAFE** · The public health operations receive options by value or take no inputs.

### `include/uos/http.hpp`

Reviewed synchronous HTTP helpers and the response rate-limit constants.

- L13 **SAFE** · Header views refer to static literals. The HTTP response helpers consume borrowed inputs synchronously and return owning response handles.

### `include/uos/images.hpp`

Reviewed image dispatch injection, fan-out coordination and usage-context pointers.

- L16, 21, 22, 23, 25, 26, 29, 31 **SAFE** · The adapters copy the context and options before suspending, and fan-out callbacks share a coordinator whose children retain independent ownership through settlement.

### `include/uos/inference_admission.hpp`

Reviewed released permits and controller interfaces. The queue abandonment cycle is classified at its listener registration in the implementation.

- L71, 76, 83, 103, 104, 123 **SAFE** · The release and controller callbacks carry shared State. Each acquire takes a request value, and literal kind tags have static storage.

### `include/uos/inference_deadline.hpp`

Reviewed deadline handles and their timer controls.

- L50, 60, 65, 66, 67, 68, 75, 82 **SAFE** · The deadline handle owns its signal and control callbacks. Each active timer owns its own controller while the caller retains the clear and abort functions.

### `include/uos/input_normalization.hpp`

Reviewed synchronous normalization templates and exported constants.

- L25, 37, 41, 42, 43, 44, 45, 46 **SAFE** · The model constant refers to a literal. JSON input pointers and string views are used synchronously and normalization produces owned values.

### `include/uos/jev_compaction/compaction.hpp`

Reviewed the compaction input wrapper, injected asker and rewrite callback. Stream abort cleanup is classified in the implementation.

- L29, 38, 40, 44, 49, 50, 51 **SAFE** · The wrappers copy JSON and dependencies into the Task. The awaiting frame owns the asker and residue function through their calls.

### `include/uos/jev_compaction/hybrid.hpp`

Reviewed hybrid fetch injection and residue interfaces.

- L32, 37, 38 **SAFE** · The frame owns the summary and Fetch, and copies per-worker options and work state before dispatch.

### `include/uos/kv.hpp`

Reviewed KV startup and test overrides. Replacing a pending injected opener is classified in src/kv.cpp.

- L14, 17, 21 **SAFE** · Initialization and the resolved override take shared KV values, and coalesced consumers obtain their own Promise and store handles.

### `include/uos/kv_conversion/cli.hpp`

Reviewed the offline conversion command declaration.

- L11 **SAFE** · The argumentless entry Task constructs its own CLI options before awaiting conversion.

### `include/uos/kv_conversion/convert.hpp`

Reviewed snapshot callback type erasure and the conversion entry point.

- L51, 53, 65, 66, 67, 68, 69, 70, 71, 73, 74, 75 **SAFE** · The erased coroutine closure is moved into the snapshot Task that awaits it, retaining the original read callback, output cell and reader until completion.
- L154 **SAFE** · Options and paths enter the conversion Task by value.

### `include/uos/kv_conversion/cursors.hpp`

Reviewed the cursor transcoder span and return record.

- L27 **SAFE** · Cursor transcoding consumes the key span synchronously and returns owning converted data.

### `include/uos/kv_conversion/legacy_codec.hpp`

Reviewed file hashing, subprocess startup and facade method ownership.

- L12, 13, 27, 30, 31, 32, 33, 36, 40, 41 **SAFE** · The codec methods copy shared State into their Tasks before returning. Conversion owns the codec through close or abort, and path arguments enter opening Tasks by value.

### `include/uos/kv_conversion/native_schema.hpp`

Reviewed synchronous SQLite interfaces, returned iterators and adapter lifetimes.

- L28, 42, 51, 58, 64, 65, 66, 71, 76, 80, 94, 105 **SAFE** · Readers, prepared statements and iterators retain their underlying database or statement. Schema validation consumes its codec view synchronously.

### `include/uos/kv_conversion/profile_json.hpp`

Reviewed the stream-export boundary and synchronous profile codecs.

- L50 **SAFE** · Export creates its list cursor before returning and both stream algorithms retain that shared cursor. Native cursors own their ongoing storage access.

### `include/uos/kv_conversion/proof.hpp`

Reviewed offline proof entry points and argument ownership.

- L26, 31 **SAFE** · Proof work owns all path and digest inputs through the awaited offline stages.

### `include/uos/kv_conversion/typed_value.hpp`

Reviewed the typed-value codec. All inputs are borrowed for synchronous conversion and the resulting graphs own their data.

### `include/uos/lib/jev_compaction/client.hpp`

Reviewed the Jev client's options and explicit receiver-lifetime contract.

- L28, 30, 37, 38, 44, 45 **SAFE** · The constructor copies its options. The batch owns the JevClient through every virtual ask, and the Fetch and signal are owned handles.

### `include/uos/lib/jev_compaction/codex_items.hpp`

Reviewed Codex item alternatives, summary label, and synchronous conversion interfaces.

- L27, 35, 43, 52 **SAFE** · Summary and kind views refer to static string literals. Converted item records own their contents.

### `include/uos/lib/jev_compaction/compact.hpp`

Reviewed synchronous planning helpers and the asynchronous compaction interface.

- L22, 40, 41 **SAFE** · Text truncation uses a synchronous view. Compaction owns the transcript, options and shared asker across all batch waits.

### `include/uos/lib/jev_compaction/jev_codex_smoke.hpp`

Reviewed the offline Codex smoke command's entry point. The fake upstream ownership cycle is recorded in the implementation.

- L8 **SAFE** · The command Task takes no borrowed arguments and constructs its fixture objects in its own frame.

### `include/uos/lib/jev_compaction/jev_compaction_smoke.hpp`

Reviewed the local gateway smoke command declaration.

- L8 **SAFE** · The command Task takes no borrowed arguments and owns the local smoke-run options.

### `include/uos/lib/jev_compaction/request.hpp`

Reviewed request helper views and synchronous response and answer parsing.

- L16, 17, 39, 43 **SAFE** · Default views refer to literals. Reply text and answer fields are read synchronously and results are values.

### `include/uos/lib/jev_compaction/state.hpp`

Reviewed transcript state helpers and token estimation.

- L14, 25 **SAFE** · The context view refers to a static literal, and estimation consumes the caller's view synchronously.

### `include/uos/lib/jev_compaction/types.hpp`

Reviewed Jev value records and the virtual asker interface.

- L202, 203 **SAFE** · The batch frame owns the shared receiver and supplies owning state and question values to the directly awaited call.

### `include/uos/metered_quota.hpp`

Reviewed quota cache options, callbacks and KV/history API boundaries.

- L150, 152, 156, 162, 163, 175, 180, 184, 185, 198, 199, 201, 202, 204, 205 **SAFE** · The quota Tasks own credential and option snapshots. Fetch and KV are retained handles, and injected time and lease callbacks are invoked synchronously.
- L214, 225, 234, 235, 237, 238, 240, 241 **SAFE** · History and diagnostics Tasks own their options, state and any shared KV handles through settlement.

### `include/uos/models/catalog.hpp`

Reviewed public catalog, model capability and snapshot interfaces.

- L47, 48, 50, 51 **SAFE** · Optional requests are transferred by value and all other entry points construct their own snapshot state.

### `include/uos/models/codex_models.hpp`

Reviewed snapshot queries, static labels and version parser views.

- L34, 38, 134, 136, 139, 148, 169, 171 **SAFE** · Label views point to literals. Lookup and version parsing consume their input views synchronously; returned snapshots and records own their contents.

### `include/uos/models/codex_models_availability.hpp`

Reviewed account refresh, unsupported evidence and cache interfaces.

- L80, 81, 84, 85, 93, 98, 99, 131 **SAFE** · Read and mutation Tasks receive owning entries, IDs and options. Each copies its shared store or pending Promise before suspension.

### `include/uos/models/codex_models_fetch.hpp`

Reviewed model fetch and validation options, transport failure callback, and catalog persistence declarations.

- L28, 32, 33, 35, 42, 43, 45, 84, 85 **SAFE** · The fetch and validation Tasks own option, credential and KV values. The on-failure callable is stored by value and invoked synchronously.

### `include/uos/models/codex_models_whitelist.hpp`

Reviewed stored whitelist APIs. The inline strong-read argument in the implementation is separately flagged for GCC 12.2.

- L22, 23 **SAFE** · The read and write interfaces retain a shared KV and the stored ID vector is passed by value.

### `include/uos/models/metadata.hpp`

Reviewed model metadata source and match functions.

- L111, 112 **SAFE** · The lookup consumes model and source views synchronously and returns owning metadata fields.

### `include/uos/models/openrouter_models.hpp`

Reviewed the non-owning match index and catalog fetch options. The partial publication defect is recorded at the publisher in src/models/openrouter_models.cpp.

- L85, 86, 87, 88, 89, 90, 92, 107, 108 **SAFE** · The synchronous search borrows model rows only while their snapshot and the matching index are alive. The caller's cache publication must keep those paired.
- L113, 117, 124, 131, 134, 136 **SAFE** · Refresh holds its fetch and signal by value; cache snapshot access returns a shared owner that can survive cache replacement.

### `include/uos/openai.hpp`

Reviewed the default lookups, request signal APIs and static schema collections.

- L8, 9, 10, 11, 16, 21, 22 **SAFE** · Default lookups take no borrowed inputs. Signal and schema helper calls are synchronous and return copied handles or keys.

### `include/uos/openai_schema.hpp`

Reviewed static OpenAI field lists.

- L6, 8 **SAFE** · Both lists hold views of literal field names with static storage.

### `include/uos/openai_telemetry.hpp`

Reviewed usage-context callbacks, weak response association, wrapper Tasks and usage-recording boundaries.

- L37, 38, 39, 75, 83, 213, 216, 218 **SAFE** · Telemetry owns its callback values and shared record. Response registration stores weak response identity without making the record retain its own response through that map.
- L271, 273, 369, 372, 376 **SAFE** · Usage entry points consume their context and input pointers synchronously or copy fields into independently queued work before returning.
- L291 **SAFE** · The wrapper holds its copied handler closure and context through the directly awaited Task, and hands streams a separate shared record.

### `include/uos/optional_telemetry_queue.hpp`

Reviewed the complete inline queue, clock and writer hooks, wait races, in-flight cleanup, and bounded drain.

- L101, 104, 109, 113, 114, 121, 128, 151, 152, 186, 228, 230, 231, 470, 471, 472, 473 **SAFE** · The queue closures own shared State and each write Task owns State and its entry until the writer settles. Size and time callbacks run synchronously.
- L156, 241, 244, 245, 246, 247, 249 **SAFE** · Write settlement takes a fresh iterator before erasing and releases the charge in the same synchronous callback. In-flight writes keep their owner and charge until settlement.
- L256, 259, 260, 261, 266, 267, 271 **SAFE** · The waiter copies Promise handles while scanning inFlight, so mutations on settlement cannot invalidate the handles retained by its race.
- L258, 262, 263, 264, 266, 267, 271, 275 **DEFECT** · A normal writer completion wins, then cancelTimeout clears the only settlement callback for the timeout Promise. Its self-owning race reaction and combined outcome stay on the process loop for each successful drain wait. Clear the timer and settle the private timeout Promise during cleanup.
- L307, 318, 319, 328, 332, 342, 343, 356, 358, 360, 364, 399, 403, 409, 410 **SAFE** · Drain frames own State and use fresh queue lookups across waits. The detached completion holds the matching drain Promise and State until it retires and settles that slot.

### `include/uos/quota_projection.hpp`

Reviewed projection constants, output records, and synchronous aggregation APIs. Result rows own their data and there is no Task or retained callback.

### `include/uos/recent_model_context.hpp`

Reviewed context estimates and stored-record interfaces. All options and borrowed views are processed synchronously and projections are owning values.

### `include/uos/release.hpp`

Reviewed the build identity declaration.

- L6 **SAFE** · The release SHA view points to build-time literal storage.

### `include/uos/request.hpp`

Reviewed one-shot body observation and JSON reader interfaces.

- L17, 19, 36, 39, 41, 48 **SAFE** · The request map copies both callbacks and erases the chosen entry before synchronous invocation. Captured body bytes cross the callback boundary as a shared owner.
- L51, 54 **SAFE** · Both JSON reader entry Tasks own their Request and take body/document limits by value.

### `include/uos/request_policy.hpp`

Reviewed model defaults, metadata lookup and synchronous policy helpers.

- L45, 56, 81, 103 **SAFE** · The asynchronous metadata lookup takes model and route strings by value. The static labels refer to literals.

### `include/uos/responses_attempts.hpp`

Reviewed prepared/primary attempt handles, usage pointers and provider options.

- L18, 19, 27, 41, 43, 61, 70, 78, 81, 83 **SAFE** · The route keeps the borrowed usage context during direct attempts. Attempt work owns response, body, controls and generator; the detached circuit seam's constraints are recorded at its implementation.

### `include/uos/responses_buffered.hpp`

Reviewed buffered result callbacks and collection interfaces.

- L8, 9, 10, 11, 13, 14 **SAFE** · The collector owns its attempt and options through its awaited iterator. Event and failure borrows are passed only to synchronous callbacks.

### `include/uos/responses_failover_stream.hpp`

Reviewed precommit options, event constructors, and stream lifecycle callbacks.

- L45, 46, 51, 53, 63, 81, 95 **SAFE** · Precommit owns its iterator and options, and invokes event and progress callbacks synchronously. Event builders consume their string views within the call.
- L114, 115, 116, 117, 118 **SAFE** · The streamed options are copied into State, async callback work retains the exact callable and event values, and synchronous controls run with State live. The GCC await-expression issue is recorded at each affected implementation call.

### `include/uos/responses_handler.hpp`

Reviewed ordinary and pre-parsed request interfaces.

- L5, 7, 9, 11, 13 **SAFE** · Production route and image caller frames retain their context and parsed body through the direct awaits. Stream handoff captures owned copies before the caller can finish.

### `include/uos/responses_native_headers.hpp`

Reviewed the early-header error Task and associated request metadata declarations.

- L11 **SAFE** · The early-header recovery takes its request, response and telemetry context by value.

### `include/uos/responses_request.hpp`

Reviewed request parsing, metadata and reasoning interfaces.

- L62, 65, 72, 73, 78 **SAFE** · Current callers directly await with their parsed body and usage context alive. The result owns its metadata and deadline controls.

### `include/uos/responses_stream.hpp`

Reviewed event identity, generator cancellation, the stored callback wrapper, and stream creation interfaces.

- L31, 33, 34, 55, 67, 93 **SAFE** · Event and exception identities have explicit owners. The cancellation wrapper creates work with its own iterator and error pointers borrow their live Reason.
- L100, 101, 107, 111, 118, 119, 120, 125, 126, 127 **SAFE** · The synchronous function runs inline. The async wrapper copies the shared callable into its child frame before invoking it and owns each event or reason it passes.
- L125 **DEFECT** · The callback wrapper passes an owning ResponsesStreamEvent or OwnedResponsesStreamFailureDetails aggregate by value inside the await expression. The GCC 12.2 profile forbids this form. Construct the returned Task in a named statement and then await it.
- L135, 138, 142, 147, 149, 150, 156, 157, 158, 165, 168 **SAFE** · Stream and preflight wrappers copy their signals, source and options. Escaping callbacks retain their own callable and arguments through asynchronous use.

### `include/uos/runtime_config.hpp`

Reviewed the runtime configuration loader and cache override interface.

- L92, 93, 94 **SAFE** · Refresh takes an optional shared store by value, and returns an owning configuration snapshot.

### `include/uos/sentinel/incident_outbox.hpp`

Reviewed incident dependency callbacks, event/read Tasks and fingerprint wrapper.

- L54, 59, 60, 61, 62, 63, 76, 77, 79, 80, 96, 98, 101, 102 **SAFE** · Current callers hold KV through their direct waits. The Tasks own options and events, Fetch retains its callable and the clock, UUID and signal suppliers are invoked synchronously.
- L108, 109, 111, 112, 236, 262, 263, 290, 311, 312 **SAFE** · Environment and admin owners keep shared KV handles. Fingerprinting serializes its input before returning the owning hash Task, and list/bind Tasks own their input values.

### `include/uos/sentinel/replay_admin.hpp`

Reviewed captured replay lookup and injected dependency interfaces.

- L18, 19, 21, 23, 29, 30 **SAFE** · The public adapter copies dependencies. The admin frame owns the exact opening or list callable and the shared KV through each direct wait.

### `include/uos/sentinel/replay_capture.hpp`

Namespace imports only. The replay and capture functions are recorded at their defining interfaces and implementations.

### `include/uos/sentinel/replay_envelope.hpp`

Reviewed the replay codec views and asynchronous crypto and compression interfaces.

- L59, 80, 81, 116, 119 **SAFE** · Synchronous readers consume spans while the caller owns their bytes, and encoded, decoded and split results own their data.
- L56, 90, 92, 95, 97 **SAFE** · The asynchronous codec entry points move or copy byte buffers and digest inputs into their own Tasks.

### `include/uos/sentinel/replay_limits.hpp`

Reviewed bound constants and synchronous charge calculations. These values and helpers neither suspend nor retain any inputs.

### `include/uos/sentinel/replay_model.hpp`

Reviewed view-backed literals, shared capture buffers and synchronous byte helpers.

- L100, 122, 147, 207, 277, 278, 294, 297, 300, 301 **SAFE** · Body buffers are explicitly shared. Constant views refer to literals, and the salt is an owning byte vector built by an immediately invoked lambda.
- L321, 323, 324 **SAFE** · Clone and concat consume input spans synchronously and return owned byte copies.

### `include/uos/sentinel/replay_observation.hpp`

Reviewed persistence dependencies, request capture APIs and the response inspector callback interface.

- L52, 54, 56, 57, 58, 67, 74, 77, 87, 91, 95 **SAFE** · Capture records carry shared candidate and body owners, while the body observer holds the candidate weakly. Persistence takes its own key and dependency data, and the option functions are synchronous.
- L99, 102, 107, 108, 111, 113 **SAFE** · The buffered inspection Task owns its Response. SSE inspector callbacks retain shared parsing state and consume byte spans during synchronous calls.

### `include/uos/sentinel/replay_read.hpp`

Reviewed live replay listings and offline decryption declarations.

- L42, 61, 72, 73, 84, 85, 89, 93 **SAFE** · Admin and replay caller frames keep the borrowed store while awaiting reads. Listings own their cursor and key values, and decryption takes its export and key bytes by value.

### `include/uos/sentinel/replay_retention.hpp`

Reviewed retention, status, publication, expiry and maintenance declarations.

- L74, 78, 79, 105, 124, 150, 177, 198, 216, 225 **SAFE** · Wrappers copy row, key and option inputs before constructing a Task. Current directly awaiting callers retain the borrowed KV through each retention operation.

### `include/uos/sentinel/replay_retention_bootstrap.hpp`

Reviewed legacy release, resumable sweep and bootstrap mutation interfaces. The abandoned wait timer is recorded in the implementation.

- L39, 53, 57, 70, 72 **SAFE** · The foreground bootstrap path keeps the store through its direct await and gives each Task owning keys, ledger values or numeric options.

### `include/uos/sentinel/replay_retention_metadata.hpp`

Reviewed metadata matching, recount and throttle interfaces.

- L54, 55, 69 **SAFE** · The recount Tasks take maintenance inputs by value and run within an awaiting caller that retains its KV.

### `include/uos/sentinel/replay_retention_schema.hpp`

Reviewed schema labels, fault and transaction callbacks, and borrowed KV operations.

- L41, 42, 43, 44, 45, 215, 316, 318, 321 **SAFE** · Status views point to literals. Fault and mutation callbacks are invoked synchronously, and current atomic application callbacks return the operation the frame owns.
- L289, 301, 302, 311, 313, 326, 327 **SAFE** · Read and chunk helpers await only owned cursor or storage operations. The caller retains KV, or the eager ledger read consumes the borrow before suspension when running in a fail-fast join.

### `include/uos/sentinel/replay_store.hpp`

Reviewed replay persistence and omission wrappers and their borrowed store contract.

- L30, 39, 41, 45, 48, 54, 56 **SAFE** · The wrappers copy reference arguments before constructing a Task. Ordinary persistence takes a shared store in its owned dependencies and direct status writers retain KV through the await.

### `include/uos/sentinel/upstream_capture.hpp`

Reviewed safe header labels, request-owned recorders and attempt callbacks.

- L53, 122, 123, 128, 129, 130, 174 **SAFE** · Header views refer to static literals. Recorder and attempt closures share their state, and body wrappers own their reader without a reverse ownership edge from recorded attempts.

### `include/uos/serve.hpp`

Reviewed maintenance stop, request dispatch, initialization and startup interfaces. Signal and KV shutdown ordering defects are recorded in the implementation.

- L13, 15, 17, 23, 24, 25, 27, 40, 43 **SAFE** · The stop closure owns shared maintenance state. Requests and connection information enter as values, and the startup wrapper copies host and port before returning its Task.

### `include/uos/static.hpp`

Reviewed the static resource and root route declarations.

- L16, 20 **SAFE** · Asset paths and requests enter their coroutine by value, and asset lookup acquires its shared asset owner before awaiting.

### `include/uos/systemone/handlers.hpp`

Reviewed the model literal and injected provider interface.

- L13, 19, 20, 24, 25 **SAFE** · The model view refers to a literal. The wrapper copies options and context values, the provider invocation retains its fetch callable, and the key supplier runs inline.

### `include/uos/types.hpp`

Reviewed value codecs, enum conversion and message content type tags.

- L54, 56, 400, 407, 413, 422, 435 **SAFE** · Stringified enum and type tag views point to static literals. The parser uses its input view synchronously and record codecs produce owned results.

### `include/uos/upstream_wire.hpp`

Reviewed provider request ID and error converter interfaces.

- L22, 27, 33 **SAFE** · Formatters and normalizers consume views synchronously. The warning label is static literal storage.
- L43, 45, 51, 53 **SAFE** · Each asynchronous converter owns its response, signal and any diagnostic values while the bounded body read runs.

### `include/uos/utils.hpp`

Reviewed the byte and string views and digest declarations.

- L22, 24, 26, 29, 35, 38 **SAFE** · Encoding functions borrow input only during synchronous conversion and return owning strings or byte buffers.
- L31, 33 **SAFE** · The digest Tasks receive and hold owning strings through their yield.

<!-- SITE-LEDGER-END -->

## Out-of-area observations

- `src/codex/auth_refresh.cpp:536-549`. Shared auth can win its race, then its finalizer removes the only listener able to settle the private abort Promise. The pending abort reaction retains the winning auth state until loop teardown. The Codex owner should settle the abandoned private latch after detaching it while keeping the shared auth work observed.
- `include/uos/rt/async.hpp:855-861` and `src/rt/async.cpp:410-425`. A pending race source owns its reaction, which owns both the source and the combined result. An abandoned synthetic loser therefore pins the winner until `Loop::Impl` clears pending reactions on teardown. The default loop is process-long (`src/rt/async.cpp:399-403`). Runtime owner should address pending-source reaction ownership or disarm race-only observers after settlement while still observing late rejections. The local diffs below settle only an abandoned private latch. Actual sibling work keeps the same semantics.
- `src/rt/kv.cpp:73`. `GenericAtomicOperation::commit` returns an awaited aggregate directly. The port's named-result rule also applies there. Request callers in this audit generally keep the receiver in their awaiting frame. Runtime owner should bind the result to a named local before returning it.

## Verification and handoff

The baseline is `6d28e858e7ab8b8b1036db4a4adfdade0465ed40`, tree `6abb54014cf79ba7c64b24be4d28dde250c40ccf`. All 143 owned source and 143 owned header blobs matched that baseline. The changed paths are this report, `analysis/lifetime/d/sites.json`, `analysis/lifetime/d/audit.py`, and `analysis/lifetime/d/tidy.py`. The selected gates are ledger reconciliation against the baseline and the pinned Clang receipt, Python parsing, changed-file formatting and `git diff --check`. The three focused controls used separate scratch builds on the prepared runner. The full native ASan shards and the qualified GCC 12.2 SDK are outside the measured evidence.

Formatting used Bun 1.4.0 and Prettier 3.9.6 through the repository task. The scratch tree holds the checked-in formatter scripts, manifests and lockfiles. Both dependency installs used `bun --no-env-file install --frozen-lockfile --ignore-scripts --cache-dir "$format_dir/cache" --no-summary`, once in the root and once in `tools/lint`.

```sh
repo=$PWD
format_dir=/tmp/cppgw-lifetime-d-format.LCMtG1kE
format_run=$(mktemp -d "$format_dir/run.XXXXXXXX")
mkdir "$format_run/home" "$format_run/tmp"
(
  cd "$format_dir/tree"
  env -i PATH=/home/runner/.bun/bin:/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$format_run/home" TMPDIR="$format_run/tmp" \
    bun --no-env-file run format:check "$repo/cpp/gateway/docs/findings/cppgw-lifetime-d-20261118.md" \
    "$repo/cpp/gateway/analysis/lifetime/d/sites.json"
)
python3 -B -c 'import ast; from pathlib import Path; paths = sorted(Path("cpp/gateway/analysis/lifetime/d").glob("*.py")); [ast.parse(path.read_text(), filename=str(path)) for path in paths]; print(f"{len(paths)} Python scripts parse")'
git diff --check 6d28e858e7ab8b8b1036db4a4adfdade0465ed40
```

Audit commits are on the job branch. The configured GitHub remote permitted anonymous fetch, while `GIT_TERMINAL_PROMPT=0 timeout 20 git push --dry-run origin HEAD:refs/heads/agent-jobs/cppgw-lifetime-d-20261118` returned `fatal: could not read Username for 'https://github.com': terminal prompts disabled`. The refreshed `origin/development` was `a3a96bc5fe96ba383936746fae23e64daa69562f`, which excludes both the integration baseline and the audit branch. Publication and integration are pending in the enclosing job. The proposed source fixes and their regression coverage are pending maintainer dispatch.

## Ranked defects and counts

### P1 · OpenRouter refresh can publish an index whose rows have been freed

`src/models/openrouter_models.cpp:203-207,239-245,353-356`. A refresh drops the prior cache owner and then allocating or building the new index throws. The catch resumes with the new snapshot and the old raw-pointer index. A later lookup can dereference a row from the retired snapshot.

```diff
       auto snapshot = std::make_shared<const OpenRouterModelsSnapshot>(OpenRouterModelsSnapshot{std::move(models), rt::dateNow()});
+      auto index = std::make_shared<const OpenRouterMatchIndex>(buildMatchIndex(snapshot->models));
       ModuleState& state = moduleState();
       state.cache = snapshot;
-      state.index = std::make_shared<const OpenRouterMatchIndex>(buildMatchIndex(snapshot->models));
+      state.index = std::move(index);
```

### P1 · Request-stream memory retention

`src/responses_stream.cpp:287-299` and `src/bounded_response_body.cpp:33-47`. A normal read wins. The reader then removes the only registration that can settle the synthetic deadline or abort loser. The pending source owns its own reaction and the combined Promise stores the winning chunk until loop teardown. This happens on each winning read, including EOF.

Use the same small edit in each reader. In `readWithDeadline` the resolver replaces the optional `inactive` Promise and its executor. In `abortableRead` preserve the `if (signal.aborted()) onAbort();` check. The post-race flag avoids rejecting a private Promise that has no observer if race construction failed.

```diff
-      rt::Promise<ReadResult> aborted = rt::newPromise<ReadResult>([&onAbort, &signal](const rt::Resolve<ReadResult>&, const rt::Reject& reject) {
-        onAbort = rt::EventListener([reject, signal] { reject(abortError(signal.reason())); });
-        signal.addEventListener("abort", onAbort, {.once = true});
-        if (signal.aborted()) onAbort();
-      });
+      auto aborted = rt::withResolvers<ReadResult>();
+      onAbort = rt::EventListener([reject = aborted.reject, signal] { reject(abortError(signal.reason())); });
+      signal.addEventListener("abort", onAbort, {.once = true});
+      if (signal.aborted()) onAbort();
+      bool observed = false;
@@
-        std::vector<rt::Promise<ReadResult>> racers{reader.readPromise(), aborted};
+        std::vector<rt::Promise<ReadResult>> racers{reader.readPromise(), aborted.promise};
         rt::Promise<ReadResult> race = rt::race(std::move(racers));
+        observed = true;
         result = co_await race.wait();
@@
       signal.removeEventListener("abort", onAbort);
+      if (observed) aborted.reject();
```

```diff
-      std::optional<rt::Promise<ReadResult>> inactive;
+      auto inactive = rt::withResolvers<ReadResult>();
+      bool observed = false;
@@
-        inactive = rt::newPromise<ReadResult>([&abortTimeout, &timeout](const rt::Resolve<ReadResult>&, const rt::Reject& reject) {
-          abortTimeout = rt::EventListener([reject] {
-            reject(rt::makeReason(ResponsesStreamError("Upstream Responses stream became inactive.", {.cause = nullptr, .kind = "inactivity_timeout"})));
-          });
-          timeout.addEventListener("abort", abortTimeout, {.once = true});
+        abortTimeout = rt::EventListener([reject = inactive.reject] {
+          reject(rt::makeReason(ResponsesStreamError("Upstream Responses stream became inactive.", {.cause = nullptr, .kind = "inactivity_timeout"})));
         });
-        std::vector<rt::Promise<ReadResult>> racers{read, *inactive};
+        timeout.addEventListener("abort", abortTimeout, {.once = true});
+        std::vector<rt::Promise<ReadResult>> racers{read, inactive.promise};
         rt::Promise<ReadResult> race = rt::race(std::move(racers));
+        observed = true;
         result = co_await race.wait();
@@
       timeout.removeEventListener("abort", abortTimeout);
+      if (observed) inactive.reject();
```

### P1 · Transport setup can strand the SSE body outcome and request drain

`src/handler/terminal_log.cpp:519-538`. A terminal SSE wrapper registers logging that needs both the transport outcome and the body outcome. Native header setup can throw before reading the body, and the fallback failure path can finish delivery without cancelling the wrapper. Both layers keep the request drain, terminal state and admission permit waiting on the private body outcome indefinitely.

The source edit belongs to the transport owner. The Web Headers adapter permits values that node:http rejects, such as DEL, so the error is reachable without allocation failure. In `src/runtime/http.cpp:1783-1795`, name `response.body()` before preparing the headers, and dispatch cancellation on header setup failure. Preserve the original failure and the separate transport receipt.

```diff
+      const std::optional<rt::ByteStream> body = response.body();
       if (responseInterrupted(*outgoing, signal)) {
-        if (const std::optional<rt::ByteStream> body = response.body()) rt::spawn(body->cancel(signal.reason()), rt::ignoreRejection);
+        if (body) rt::spawn(body->cancel(signal.reason()), rt::ignoreRejection);
         co_return;
       }
-      prepareResponseHeaders(request, response, *outgoing);
-      const std::optional<rt::ByteStream> body = response.body();
+      try {
+        prepareResponseHeaders(request, response, *outgoing);
+      } catch (...) {
+        if (body) rt::spawn(body->cancel(std::current_exception()), rt::ignoreRejection);
+        throw;
+      }
```

### P2 · Injected admin Tasks retain the parent callback address

`src/admin/codex.cpp:208-209,216-221,239-240` and `src/admin/defaults.cpp:53-54`. Any failing sibling lets the join return while an injected capturing coroutine is suspended. The corresponding handler frame owns the callable and is destroyed during unwind, leaving the pending coroutine without its closure. Normal route dispatch constructs empty dependency overrides. Keep each injected callable in an owning child frame before starting it.

```diff
-    dependencies.buildCatalog ? dependencies.buildCatalog() : models::catalog::buildModelCatalogSnapshot()
+    dependencies.buildCatalog ? rt::async(dependencies.buildCatalog) : models::catalog::buildModelCatalogSnapshot()
@@
-    dependencies.refreshEnrichment ? dependencies.refreshEnrichment() : models::openrouter_models::fetchOpenRouterModels(eo)
+    dependencies.refreshEnrichment ? rt::async(dependencies.refreshEnrichment) : models::openrouter_models::fetchOpenRouterModels(eo)
@@
-    dependencies.refreshOpenlux ? dependencies.refreshOpenlux(mo) : provider::metered::fetchMeteredModels(mo)
+    dependencies.refreshOpenlux ? rt::async([refresh = dependencies.refreshOpenlux, mo] { return refresh(mo); }) : provider::metered::fetchMeteredModels(mo)
@@
-    dependencies.getMeteredQuotaDiagnostics ? dependencies.getMeteredQuotaDiagnostics() : metered_quota::getMeteredQuotaDiagnostics()
+    dependencies.getMeteredQuotaDiagnostics ? rt::async(dependencies.getMeteredQuotaDiagnostics) : metered_quota::getMeteredQuotaDiagnostics()
```

Apply the `refreshOpenlux` form to `refreshSurplus`, `refreshLithos` and `refreshClaude` with the corresponding copied options. Apply the `buildCatalog` form at both sites.

### P2 · Terminal writer setup failure can release a suspended writer closure

`src/handler/terminal_log.cpp:61,162-219`. An injected writer returning a capturing Task suspends during batch setup. A subsequent writer throws synchronously, so the log frame and its copy of the invoked callable leave scope before the first writer resumes. Attach the callback owner immediately to each injected writer's own settlement.

```diff
     rt::Task<void> logTerminalRequest(LogInput data) {
-      auto& input = data.input;
+      const auto callbacks = std::make_shared<WithTerminalRequestLogInput>(std::move(data.input));
+      auto& input = *callbacks;
+      const auto invokeWriter = [callbacks](const auto& writer, auto&&... args) {
+        auto pending = writer(std::forward<decltype(args)>(args)...);
+        return pending.finally_([callbacks] { (void)callbacks; });
+      };
@@
-      auto telemetryWrite = input.recordTelemetry ? input.recordTelemetry(telemetryEvent)
+      auto telemetryWrite = input.recordTelemetry ? invokeWriter(input.recordTelemetry, telemetryEvent)
@@
-      auto cacheAnalyticsWrite = input.recordCacheAnalytics ? input.recordCacheAnalytics(analyticsEvent)
+      auto cacheAnalyticsWrite = input.recordCacheAnalytics ? invokeWriter(input.recordCacheAnalytics, analyticsEvent)
@@
-      const auto usageRollupWrite = (input.recordUsageRollup ? input.recordUsageRollup(rollupInput)
+      const auto usageRollupWrite = (input.recordUsageRollup ? invokeWriter(input.recordUsageRollup, rollupInput)
@@
-        replayWrite = input.persistSentinelReplay ? input.persistSentinelReplay(*input.sentinelReplayInput, replayObservation, clientObservation)
+        replayWrite = input.persistSentinelReplay ? invokeWriter(input.persistSentinelReplay, *input.sentinelReplayInput, replayObservation, clientObservation)
@@
-        ? input.recordSentinelDegradation(rt::dateNow()) : rt::Promise<bool>::resolve(false);
+        ? invokeWriter(input.recordSentinelDegradation, rt::dateNow()) : rt::Promise<bool>::resolve(false);
@@
-      const auto adminErrorWrite = input.recordAdminError ? input.recordAdminError(errorInput) : rt::Promise(admin::error_log::recordAdminError(errorInput));
+      const auto adminErrorWrite = input.recordAdminError ? invokeWriter(input.recordAdminError, errorInput) : rt::Promise(admin::error_log::recordAdminError(errorInput));
```

### P2 · Detached exception replay borrows its injected callable

`src/handler/terminal_route.cpp:523-529`. An injected capturing Task can convert to the hook's Promise return type. Registration can return successfully while persistence remains suspended. On parent return the options frame releases the coroutine's closure. The default persistence task has owning parameters.

```diff
-    rt::Promise<SentinelReplayPersistResult> persisted = options.persistSentinelReplay
-      ? options.persistSentinelReplay(replaySnapshot, observation)
+    auto persist = std::make_shared<decltype(options.persistSentinelReplay)>(std::move(options.persistSentinelReplay));
+    rt::Promise<SentinelReplayPersistResult> persisted = *persist
+      ? (*persist)(replaySnapshot, observation)
       : rt::Promise<SentinelReplayPersistResult>(persistSentinelReplayFromEnvironment(replaySnapshot, observation));
-    auto persistence = persisted.then(ignoreSuccess, ignoreFailure).then([replaySnapshot] {
+    auto persistence = persisted.then(ignoreSuccess, ignoreFailure).then([replaySnapshot, persist] {
+      (void)persist;
       zeroSentinelReplayInput(&replaySnapshot);
     });
```

### P2 · Successful active health probes retain cleared timeout losers

`src/health.cpp:306-319`. The provider probe wins, then its finalizer clears the only timeout callback. The pending race source retains the combined Promise, including its probe summary, on the process loop. Each independently completed active probe repeats the retention.

```diff
-      auto timeout = rt::newPromise<json::object>([controller, progress, timer](rt::Resolve<json::object> resolve, rt::Reject) {
-        *timer = rt::setTimeout([controller, progress, resolve] { controller.abort(rt::makeReason(rt::DOMException("Active upstream health probe timed out.", "TimeoutError"))); resolve(timeoutProbe(*progress)); }, activeUpstreamHealthTimeoutMs);
-      });
+      auto timeout = rt::withResolvers<json::object>();
+      *timer = rt::setTimeout([controller, progress, resolve = timeout.resolve] { controller.abort(rt::makeReason(rt::DOMException("Active upstream health probe timed out.", "TimeoutError"))); resolve(timeoutProbe(*progress)); }, activeUpstreamHealthTimeoutMs);
@@
-      std::vector<rt::Promise<json::object>> racers; racers.emplace_back(std::move(task)); racers.push_back(timeout);
-      auto pending = rt::race(std::move(racers)).finally_([timer] { if (*timer) rt::clearTimeout(*timer); upstreamProbeInFlight = std::nullopt; });
+      std::vector<rt::Promise<json::object>> racers; racers.emplace_back(std::move(task)); racers.push_back(timeout.promise);
+      auto pending = rt::race(std::move(racers)).finally_([timer, reject = timeout.reject] {
+        if (*timer) rt::clearTimeout(*timer);
+        reject();
+        upstreamProbeInFlight = std::nullopt;
+      });
```

### P2 · Telemetry queue retains one timeout Promise per successful wait

`include/uos/optional_telemetry_queue.hpp:256-276`. A writer settles first, then the drain clears the only callback that can settle `timedOut`. The pending timeout retains the race outcome and its own reaction. The live-loop control measured 280 retained bytes per successful wait and zero with this edit.

```diff
-      std::function<void()> cancelTimeout = [] {};
@@
-      const rt::Promise<bool> timedOut = rt::newPromise<bool>([&cancelTimeout, timeoutMs](rt::Resolve<bool> resolve, rt::Reject) {
-        const rt::Timeout timer = rt::setTimeout([resolve] { resolve(false); }, timeoutMs);
-        cancelTimeout = [timer] { rt::clearTimeout(timer); };
-      });
-      std::vector<rt::Promise<bool>> racers = {progressed, timedOut};
+      const auto timedOut = rt::withResolvers<bool>();
+      const auto timer = rt::setTimeout([resolve = timedOut.resolve] { resolve(false); }, timeoutMs);
+      std::vector<rt::Promise<bool>> racers = {progressed, timedOut.promise};
@@
-      cancelTimeout();
+      rt::clearTimeout(timer);
+      timedOut.resolve(false);
```

### P3 · Local provisioning retains the losing abort source

`src/auth/local_development_key.cpp:94-113`. Pricing finishes before abort and the listener is removed. The abort-only source still retains the combined Promise with copied local key material until loop teardown. The existing timer cleanup and late initializer observation stay in place.

```diff
       rt::Reason failure;
+      bool observed = false;
@@
         const rt::Promise<T> racing = rt::race(std::move(racers));
+        observed = true;
@@
       signal.removeEventListener("abort", onAbort);
+      if (observed) reject();
```

### P3 · Replay bootstrap wait retains a cleared timeout source

`src/sentinel/replay_retention_bootstrap.cpp:469-482`. Index filling settles before maintenance's timer. Maintenance clears the timer and drops the only resolver, stranding the private Promise and its race reaction. Settle the private latch after clearing the timer, leaving the original result and the index fill's ownership intact.

```diff
       rt::clearTimeout(timer);
+      elapsed.resolve();
       if (failure) std::rethrow_exception(failure);
```

### P3 · Queued admission listener survives loop teardown

`src/inference_admission.cpp:149-179`. A dropped loop clears the queue-timeout timer without running its abort callback. The controller owns each waiter, and the waiter and signal own a listener that strongly captures both. No remaining action can detach it once the loop and external owners are gone.

Keep the original queue deadline and the preference for a simultaneous caller abort. Make the explicit exact timer the outside owner of pending waiters. The runtime clears the timer callback on normal settlement and on loop teardown even when the waiter still holds a Timeout handle. The caller-signal listener holds weak references, locks them inline and uses the existing settlement path.

```diff
-    std::optional<rt::AbortSignal> waitSignal;
+    std::optional<rt::Timeout> timeout;
@@
-      if (waiter->waitSignal && waiter->onWaitAbort) waiter->waitSignal->removeEventListener("abort", *waiter->onWaitAbort);
-      waiter->waitSignal.reset();
+      if (waiter->callerSignal && waiter->onWaitAbort) waiter->callerSignal->removeEventListener("abort", *waiter->onWaitAbort);
+      if (waiter->timeout) rt::clearTimeout(*waiter->timeout);
@@
-          const rt::EventListener onWaitAbort([self, waiter, callerSignal, queuedAtMs] { /* settle caller abort or timeout */ });
+          const rt::EventListener onWaitAbort([controller = std::weak_ptr(self), pending = std::weak_ptr(waiter), queuedAtMs] {
+            const auto owner = controller.lock();
+            const auto live = pending.lock();
+            if (owner && live) owner->settleWaiter(live, InferenceAdmissionCallerAborted{.waitedMs = elapsedMs(queuedAtMs)});
+          });
+          waiter->timeout = rt::detail::createExactTimer([self, waiter, queuedAtMs] {
+            if (waiter->callerSignal && waiter->callerSignal->aborted())
+              self->settleWaiter(waiter, InferenceAdmissionCallerAborted{.waitedMs = elapsedMs(queuedAtMs)});
+            else
+              self->settleWaiter(waiter, InferenceAdmissionLocalOverload{.cause = "queue_wait_timeout", .waitedMs = elapsedMs(queuedAtMs)});
+          }, maxQueueWaitMs);
+          waiter->timeout->unref();
@@
-          waitSignal.addEventListener("abort", onWaitAbort);
+          if (callerSignal) callerSignal->addEventListener("abort", onWaitAbort);
           enqueueWaiter(waiter);
-          if (waitSignal.aborted()) onWaitAbort();
+          if (callerSignal && callerSignal->aborted()) onWaitAbort();
```

Remove the replaced combined-signal construction. The module already includes `uos/rt/clock.hpp`.

### P3 · Replacing the injected KV opener invalidates a suspended closure

`src/kv.cpp:53-57,94-98`. A capturing coroutine opener suspends and the test hook replaces `selectedOpener`. The pending open resumes to a destroyed callable. The production default is a free-function opener. Keep a copy of the exact invoked callable in its awaiting frame.

```diff
-        rt::Promise<std::shared_ptr<rt::Kv>> opening(state().selectedOpener());
+        const auto opener = state().selectedOpener;
+        rt::Promise<std::shared_ptr<rt::Kv>> opening(opener());
```

### P3 · An abandoned timed compaction body leaves a weak abort listener

`src/jev_compaction/compaction.cpp:238-250,385-390`. A response body can be dropped after it has buffered a chunk, while an external abort signal is reused. After the wrapper state dies, the weak timeout callback cannot unregister the listener. Later calls on the same live signal accumulate dead listeners.

```diff
     struct TimeoutFetchState {
+      ~TimeoutFetchState() {
+        try { release(); } catch (...) {}
+      }
```

### P3 · Smoke fake upstream and server retain each other

`src/lib/jev_compaction/jev_codex_smoke.cpp:179-188`. The fake owns its server handle and the server owns a handler that captures the fake. Server shutdown does not release the fake's handle, so dropping the fixture leaves both and the request history alive. Use a weak fake in the server registration and a strong owner in each actual request.

```diff
-        runtime::http::ServeHandler handle = [fake](rt::Request request, runtime::http::ServeHandlerInfo) { return fake->handle(request); };
+        runtime::http::ServeHandler handle = [weak = std::weak_ptr(fake)](rt::Request request, runtime::http::ServeHandlerInfo) {
+          if (auto owner = weak.lock()) {
+            return rt::async([owner = std::move(owner), request = std::move(request)] { return owner->handle(request); });
+          }
+          return rt::fulfilledTask(rt::Response("Fixture closed", {.status = 503}));
+        };
```

### P3 · Development startup can leave a signal handler registered

`src/serve.cpp:159-169`. The process signal API stores its listener before it installs the signal wait. If either registration throws, the development launcher's removal guard has yet to run its constructor. The process signal table can retain the server and its callback after startup unwinds.

```diff
-      rt::process::once("SIGINT", shutdown);
-      rt::process::once("SIGTERM", shutdown);
       {
         auto removeListeners = rt::scopeExit([shutdown] {
           rt::process::removeListener("SIGINT", shutdown);
           rt::process::removeListener("SIGTERM", shutdown);
         });
+        rt::process::once("SIGINT", shutdown);
+        rt::process::once("SIGTERM", shutdown);
         co_await server.ready.wait();
```

### P3 · Shutdown can close KV while the startup migration is pending

`src/serve.cpp:118-133,155-175,181-216`. The launchers start the reset settings migration and discard its Promise. A shutdown that reaches KV closure before migration finishes leaves a live coroutine with a shared handle to a closed database. Retain its Promise and join it during cleanup after the server settles. Listening can still start concurrently.

```diff
-      (void)initializeModule();
+      const auto migration = initializeModule().catch_(rt::ignoreRejection);
@@
         co_await server.finished.wait();
+        co_await migration.wait();
@@
       rt::Reason failure;
       std::optional<rt::process::Listener> shutdown;
+      std::optional<rt::Promise<void>> migration;
       try {
-        (void)initializeModule();
+        migration = initializeModule().catch_(rt::ignoreRejection);
@@
       try {
+        if (migration) co_await migration->wait();
         rt::Promise<void> drained(shutdownOptionalTelemetry());
```

### P3 · A delivery abort listener retains its own request signal

`src/handler/serve_handler.cpp:7-25`. Registration gives the request signal a callback that captures the same signal. If setup fails before the handoff guard is armed, or a direct caller drops the handoff, an unaborted request retains its own signal and the downstream controller. Keep the strong signal owner in the handoff closure and use a weak signal in the registered listener.

```diff
+#include <memory>
@@
-      abortBeforeHandoff = rt::EventListener([requestSignal, downstream] { downstream.abort(requestSignal.reason()); });
+      const std::weak_ptr source = requestSignal.state();
+      abortBeforeHandoff = rt::EventListener([source, downstream] {
+        if (auto live = source.lock()) downstream.abort(rt::AbortSignal(std::move(live)).reason());
+      });
```

### P3 · GCC 12.2 await-expression violations

Manual inspection identified eight sites covered by the mandatory comparison-profile rule. A qualified GCC 12.2 execution result is unavailable.

`src/admin/defaults.cpp:99`. The commit result's member is accessed within the await expression. Bind the commit result before reading it.

```diff
-    bool committed = false; rt::Reason failure; try { committed = (co_await atomic->commit()).ok; } catch (...) { failure = std::current_exception(); }
+    bool committed = false; rt::Reason failure; try { const auto result = co_await atomic->commit(); committed = result.ok; } catch (...) { failure = std::current_exception(); }
```

`src/auth/passkeys.cpp:4313, 4410, 4425, 4488, 4919, 5001`. The lookups construct a temporary KV key within the awaited call expression. Current native adapters copy the receiver's storage and move the key, and Clang 21 reported no failure. Factor the key out for the required GCC comparison profile. The logout equivalent uses an explicit conditional block.

```diff
-      const rt::KvEntryMaybe<> userIdEntry = co_await kv->get(passkeyHandleKey(handle));
+      const rt::KvKey handleKey = passkeyHandleKey(handle);
+      const rt::KvEntryMaybe<> userIdEntry = co_await kv->get(handleKey);
```

`src/admin/codex.cpp:71`. The debug-routing delete call constructs a string as the coroutine starts. Name the scenario before starting that await.

```diff
-  if (req.method() == "DELETE") { const auto routing = co_await debug_routing::setDebugRoutingConfig(std::string("normal"), 0); co_return http::json(200, json::object{{"routing", json::value_from(routing)}}, rt::Headers{{"Cache-Control", "no-store"}}); }
+  if (req.method() == "DELETE") {
+    const std::string scenario = "normal";
+    const auto routing = co_await debug_routing::setDebugRoutingConfig(scenario, 0);
+    co_return http::json(200, json::object{{"routing", json::value_from(routing)}}, rt::Headers{{"Cache-Control", "no-store"}});
+  }
```

`src/admin/kernel.cpp:380,384,406,439`. Cohort hashing, reset/overage reads and an overage write construct owning strings within the await. Bind the concatenation and each account ID before the call.

```diff
-    const auto accountCohortId = co_await utils::sha256Hex(std::string("uos-prompt-cache-account-cohort-v1\0", 35) + account.account_id);
+    const std::string cohortInput = std::string("uos-prompt-cache-account-cohort-v1\0", 35) + account.account_id;
+    const auto accountCohortId = co_await utils::sha256Hex(cohortInput);
@@
-    const auto usage = co_await cs::readCodexResetUsage(kv, std::string(*rt::getString(account, "account_id_hash")));
+    const std::string accountIdHash(*rt::getString(account, "account_id_hash"));
+    const auto usage = co_await cs::readCodexResetUsage(kv, accountIdHash);
@@
-    const auto allow = co_await co::readOverageUsage(kv, std::string(*rt::getString(account, "account_id_hash")));
+    const std::string accountIdHash(*rt::getString(account, "account_id_hash"));
+    const auto allow = co_await co::readOverageUsage(kv, accountIdHash);
@@
-    co_await co::writeOverageUsage(kv, std::string(*accountIdHash), *allow); co_await refreshOverageUsageRoutingCache();
+    const std::string hash(*accountIdHash);
+    co_await co::writeOverageUsage(kv, hash, *allow);
+    co_await refreshOverageUsageRoutingCache();
```

`src/auth/index.cpp:137,195`. The bearer cache hash and policy queue construct owned text in the await expression. Bind each string before awaiting the consuming call.

```diff
-      const std::string cacheKey = co_await utils::sha256Base64Url(token + ":" + owner + "/" + repo);
+      const std::string cacheInput = token + ":" + owner + "/" + repo;
+      const std::string cacheKey = co_await utils::sha256Base64Url(cacheInput);
@@
-        co_await kernel::policy_queue::recordKernelPolicyQueue(owner, repo, getRequestPath(req));
+        const std::string path = getRequestPath(req);
+        co_await kernel::policy_queue::recordKernelPolicyQueue(owner, repo, path);
```

`src/models/codex_models_whitelist.cpp:134`. The strong-read options aggregate lives in the await full-expression. Create the get Task with named options before awaiting.

```diff
-    const auto entry = co_await kv->get(CODEX_MODELS_WHITELIST_KV_KEY, {.consistency = rt::KvConsistencyLevel::strong});
+    const rt::KvGetOptions options{.consistency = rt::KvConsistencyLevel::strong};
+    auto reading = kv->get(CODEX_MODELS_WHITELIST_KV_KEY, options);
+    const auto entry = co_await std::move(reading);
```

`src/responses_failover_stream.cpp:944,972`. Both failure paths pass an owning `ResponsesStreamEvent` aggregate by value within an await. Construct the Task in a named statement at both sites.

```diff
-        co_await emitFailureTerminal(state, controller, std::move(failure), std::move(error), std::move(upstreamTerminal));
+        auto emitting = emitFailureTerminal(state, controller, std::move(failure), std::move(error), std::move(upstreamTerminal));
+        co_await std::move(emitting);
@@
-      co_await emitFailureTerminal(state, controller, std::move(failure), std::move(error), std::move(noUpstreamTerminal));
+      auto emitting = emitFailureTerminal(state, controller, std::move(failure), std::move(error), std::move(noUpstreamTerminal));
+      co_await std::move(emitting);
```

`include/uos/responses_stream.hpp:125`. The asynchronous callback wrapper can pass an owning event or failure-details aggregate by value. Bind the Task before awaiting while the shared function remains in the frame.

```diff
-    static rt::Task<void> run(std::shared_ptr<const Async> function, A... args) { co_await (*function)(std::move(args)...); }
+    static rt::Task<void> run(std::shared_ptr<const Async> function, A... args) {
+      auto pending = (*function)(std::move(args)...);
+      co_await std::move(pending);
+    }
```

| Defect class                                     |  Sites |
| ------------------------------------------------ | -----: |
| Catalog index invalidation                       |      1 |
| Abandoned private race sources                   |      6 |
| Suspended callbacks whose owners can be released |      7 |
| Other callback and resource lifecycle failures   |      7 |
| GCC 12.2 await-expression violations             |      8 |
| **Total**                                        | **29** |

Severity totals are 4 P1, 8 P2 and 17 P3 sites. Site counts group repeated calls with the same mechanism in one file or operation.

286/286 files reviewed. 481 sites: 452 SAFE, 29 DEFECT, 0 UNCLEAR. <!-- AUDIT-COUNTS -->
