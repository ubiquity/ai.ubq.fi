# Gateway coroutine and callback lifetime audit, area D

## Contract v1

Audit the C++20 sources directly under `src/`, the twelve assigned trees and the corresponding `include/uos/` headers. The baseline is commit `6d28e858e7ab8b8b1036db4a4adfdade0465ed40`, tree `6abb54014cf79ba7c64b24be4d28dde250c40ccf`, from `agent-jobs/cppgw-integrate-x1-20261118-44302040764-1`. Paths and line numbers below are relative to `cpp/gateway/`.

The owned deliverables are this report, `analysis/lifetime/d/sites.json`, and audit scripts in that directory. Source fixes, production and test edits, TypeScript, workflow changes, deployment, credential handling and live provider probes are outside this audit. Rejected and selected promises retain the existing no-sibling-cancellation semantics. Use only local or in-memory fixtures with a clean environment, empty home and fresh temp directory. Put toolchains, dependencies and outputs outside the checkout.

| Surface | Ownership traced |
| --- | --- |
| HTTP and inference handlers | Request and response frames, detached jobs, stream and delivery callbacks, and shutdown |
| Admin, auth, catalog and Sentinel | Awaited versus escaping KV or fetch work, shared caches, abort and replay callbacks |
| Authority and KV conversion | Local and RPC modes, queued work, SQLite and file handles, conversion streams |
| Models, compaction and other helpers | Owning arguments, views, iterators, generator and callback holders |

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

| Archive | SHA-256 | Command run in an external scratch directory |
| --- | --- | --- |
| [Boost 1.92.0](https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2) | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` | `./bootstrap.sh --with-libraries=json,container` then `./b2 -j2 --with-json --with-container variant=release link=static runtime-link=shared cxxstd=20 stage`. Produced 869,382-byte JSON and 163,406-byte Container archives. |
| [SQLite 3.54.0](https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz) | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` | `./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'`, `make -j1`, `sudo -n make install`, `sudo -n ldconfig`. `pkg-config --modversion sqlite3` returned 3.54.0. |
| [ICU 78.3](https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz) | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` | `../icu/source/configure --prefix="$icu/prefix" --disable-tests --disable-samples --disable-extras --disable-static`, then `make -j1`, `make install`. The staged pkg-config returned 78.3. |

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
git diff --check 6d28e858e7ab8b8b1036db4a4adfdade0465ed40
```

Each audit entry binds its source coordinates to the baseline Git blobs. `audit.py verify` reconciles the checklist, Markdown site entries and JSON, validates lines, and requires every lexical `co_await` line to have a verdict in a reviewed file. `audit.py pending` prints the restart order. Finish each file with `audit.py record`, which writes both ledgers and makes one Git commit per reviewed file.

## Focused sanitizer control

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
- [ ] `src/lib/jev_compaction/compact.cpp`
- [ ] `src/lib/jev_compaction/jev_codex_smoke.cpp`
- [ ] `src/lib/jev_compaction/jev_compaction_smoke.cpp`
- [ ] `src/lib/jev_compaction/request.cpp`
- [ ] `src/lib/jev_compaction/state.cpp`
- [ ] `src/lib/jev_compaction/types.cpp`
- [ ] `src/metered_quota.cpp`
- [ ] `src/models/catalog.cpp`
- [ ] `src/models/codex_models.cpp`
- [ ] `src/models/codex_models_availability.cpp`
- [ ] `src/models/codex_models_fetch.cpp`
- [ ] `src/models/codex_models_whitelist.cpp`
- [ ] `src/models/metadata.cpp`
- [ ] `src/models/openrouter_models.cpp`
- [ ] `src/openai.cpp`
- [ ] `src/openai_schema.cpp`
- [ ] `src/openai_telemetry.cpp`
- [ ] `src/optional_telemetry_queue.cpp`
- [ ] `src/quota_projection.cpp`
- [ ] `src/recent_model_context.cpp`
- [ ] `src/release.cpp`
- [ ] `src/request.cpp`
- [ ] `src/request_policy.cpp`
- [ ] `src/responses_attempts.cpp`
- [ ] `src/responses_buffered.cpp`
- [ ] `src/responses_failover_stream.cpp`
- [ ] `src/responses_handler.cpp`
- [ ] `src/responses_native_headers.cpp`
- [ ] `src/responses_request.cpp`
- [x] `src/responses_stream.cpp`
- [ ] `src/runtime_config.cpp`
- [ ] `src/sentinel/incident_outbox.cpp`
- [ ] `src/sentinel/replay_admin.cpp`
- [ ] `src/sentinel/replay_capture.cpp`
- [ ] `src/sentinel/replay_envelope.cpp`
- [ ] `src/sentinel/replay_limits.cpp`
- [ ] `src/sentinel/replay_model.cpp`
- [ ] `src/sentinel/replay_observation.cpp`
- [ ] `src/sentinel/replay_read.cpp`
- [ ] `src/sentinel/replay_retention.cpp`
- [ ] `src/sentinel/replay_retention_bootstrap.cpp`
- [ ] `src/sentinel/replay_retention_metadata.cpp`
- [ ] `src/sentinel/replay_retention_schema.cpp`
- [ ] `src/sentinel/replay_store.cpp`
- [ ] `src/sentinel/upstream_capture.cpp`
- [ ] `src/serve.cpp`
- [ ] `src/static.cpp`
- [ ] `src/systemone/handlers.cpp`
- [ ] `src/types.cpp`
- [ ] `src/upstream_wire.cpp`
- [ ] `src/utils.cpp`
- [ ] `include/uos/admin/api_key_audit.hpp`
- [ ] `include/uos/admin/api_key_mutations.hpp`
- [ ] `include/uos/admin/api_keys.hpp`
- [ ] `include/uos/admin/capacity_observation_sync.hpp`
- [ ] `include/uos/admin/chart_source_proxy.hpp`
- [ ] `include/uos/admin/claude_code.hpp`
- [ ] `include/uos/admin/codex.hpp`
- [ ] `include/uos/admin/codex_reset.hpp`
- [ ] `include/uos/admin/defaults.hpp`
- [ ] `include/uos/admin/error_log.hpp`
- [ ] `include/uos/admin/index.hpp`
- [ ] `include/uos/admin/kernel.hpp`
- [ ] `include/uos/admin/sentinel_incident.hpp`
- [ ] `include/uos/agent_messages.hpp`
- [ ] `include/uos/analytics.hpp`
- [ ] `include/uos/api_key_policy.hpp`
- [ ] `include/uos/api_keys.hpp`
- [ ] `include/uos/auth/index.hpp`
- [ ] `include/uos/auth/local_admin.hpp`
- [ ] `include/uos/auth/local_development_key.hpp`
- [ ] `include/uos/auth/passkeys.hpp`
- [ ] `include/uos/auth/passport.hpp`
- [ ] `include/uos/auth/passport_sessions.hpp`
- [ ] `include/uos/auth/relay.hpp`
- [ ] `include/uos/authority/admission_state.hpp`
- [ ] `include/uos/authority/authority_rpc.hpp`
- [ ] `include/uos/authority/contracts.hpp`
- [ ] `include/uos/authority/credential_publication.hpp`
- [ ] `include/uos/authority/delivery_outbox.hpp`
- [ ] `include/uos/authority/durable_store.hpp`
- [ ] `include/uos/authority/native_api_key.hpp`
- [ ] `include/uos/authority/native_api_key_audit.hpp`
- [ ] `include/uos/authority/native_api_key_policy.hpp`
- [ ] `include/uos/authority/native_authority.hpp`
- [ ] `include/uos/authority/native_authority_api.hpp`
- [ ] `include/uos/authority/native_authority_codex.hpp`
- [ ] `include/uos/authority/native_authority_contracts.hpp`
- [ ] `include/uos/authority/native_authority_kernel.hpp`
- [ ] `include/uos/authority/native_authority_paid.hpp`
- [ ] `include/uos/authority/native_authority_principals.hpp`
- [ ] `include/uos/authority/native_authority_rpc.hpp`
- [ ] `include/uos/authority/native_authority_runtime.hpp`
- [ ] `include/uos/authority/native_authority_sql.hpp`
- [ ] `include/uos/authority/native_codex.hpp`
- [ ] `include/uos/authority/native_codex_429.hpp`
- [ ] `include/uos/authority/native_codex_capacity.hpp`
- [ ] `include/uos/authority/native_codex_contracts.hpp`
- [ ] `include/uos/authority/native_codex_evaluation.hpp`
- [ ] `include/uos/authority/native_codex_mutations.hpp`
- [ ] `include/uos/authority/native_contracts.hpp`
- [ ] `include/uos/authority/native_kernel.hpp`
- [ ] `include/uos/authority/native_kernel_contracts.hpp`
- [ ] `include/uos/authority/native_paid.hpp`
- [ ] `include/uos/authority/native_paid_contracts.hpp`
- [ ] `include/uos/authority/native_paid_policy.hpp`
- [ ] `include/uos/authority/native_principal_contracts.hpp`
- [ ] `include/uos/authority/native_runtime_contracts.hpp`
- [ ] `include/uos/bounded_response_body.hpp`
- [ ] `include/uos/catalog/index.hpp`
- [ ] `include/uos/catalog/models.hpp`
- [ ] `include/uos/catalog/promotion.hpp`
- [ ] `include/uos/catalog/store.hpp`
- [ ] `include/uos/catalog/types.hpp`
- [ ] `include/uos/config.hpp`
- [ ] `include/uos/debug_routing.hpp`
- [ ] `include/uos/decisions/handlers.hpp`
- [ ] `include/uos/defaults.hpp`
- [ ] `include/uos/handler/admission.hpp`
- [ ] `include/uos/handler/http.hpp`
- [ ] `include/uos/handler/index.hpp`
- [ ] `include/uos/handler/serve_handler.hpp`
- [ ] `include/uos/handler/terminal_log.hpp`
- [ ] `include/uos/handler/terminal_route.hpp`
- [ ] `include/uos/health.hpp`
- [ ] `include/uos/http.hpp`
- [ ] `include/uos/images.hpp`
- [ ] `include/uos/inference_admission.hpp`
- [ ] `include/uos/inference_deadline.hpp`
- [ ] `include/uos/input_normalization.hpp`
- [ ] `include/uos/jev_compaction/compaction.hpp`
- [ ] `include/uos/jev_compaction/hybrid.hpp`
- [ ] `include/uos/kv.hpp`
- [ ] `include/uos/kv_conversion/cli.hpp`
- [ ] `include/uos/kv_conversion/convert.hpp`
- [ ] `include/uos/kv_conversion/cursors.hpp`
- [ ] `include/uos/kv_conversion/legacy_codec.hpp`
- [ ] `include/uos/kv_conversion/native_schema.hpp`
- [ ] `include/uos/kv_conversion/profile_json.hpp`
- [ ] `include/uos/kv_conversion/proof.hpp`
- [ ] `include/uos/kv_conversion/typed_value.hpp`
- [ ] `include/uos/lib/jev_compaction/client.hpp`
- [ ] `include/uos/lib/jev_compaction/codex_items.hpp`
- [ ] `include/uos/lib/jev_compaction/compact.hpp`
- [ ] `include/uos/lib/jev_compaction/jev_codex_smoke.hpp`
- [ ] `include/uos/lib/jev_compaction/jev_compaction_smoke.hpp`
- [ ] `include/uos/lib/jev_compaction/request.hpp`
- [ ] `include/uos/lib/jev_compaction/state.hpp`
- [ ] `include/uos/lib/jev_compaction/types.hpp`
- [ ] `include/uos/metered_quota.hpp`
- [ ] `include/uos/models/catalog.hpp`
- [ ] `include/uos/models/codex_models.hpp`
- [ ] `include/uos/models/codex_models_availability.hpp`
- [ ] `include/uos/models/codex_models_fetch.hpp`
- [ ] `include/uos/models/codex_models_whitelist.hpp`
- [ ] `include/uos/models/metadata.hpp`
- [ ] `include/uos/models/openrouter_models.hpp`
- [ ] `include/uos/openai.hpp`
- [ ] `include/uos/openai_schema.hpp`
- [ ] `include/uos/openai_telemetry.hpp`
- [ ] `include/uos/optional_telemetry_queue.hpp`
- [ ] `include/uos/quota_projection.hpp`
- [ ] `include/uos/recent_model_context.hpp`
- [ ] `include/uos/release.hpp`
- [ ] `include/uos/request.hpp`
- [ ] `include/uos/request_policy.hpp`
- [ ] `include/uos/responses_attempts.hpp`
- [ ] `include/uos/responses_buffered.hpp`
- [ ] `include/uos/responses_failover_stream.hpp`
- [ ] `include/uos/responses_handler.hpp`
- [ ] `include/uos/responses_native_headers.hpp`
- [ ] `include/uos/responses_request.hpp`
- [ ] `include/uos/responses_stream.hpp`
- [ ] `include/uos/runtime_config.hpp`
- [ ] `include/uos/sentinel/incident_outbox.hpp`
- [ ] `include/uos/sentinel/replay_admin.hpp`
- [ ] `include/uos/sentinel/replay_capture.hpp`
- [ ] `include/uos/sentinel/replay_envelope.hpp`
- [ ] `include/uos/sentinel/replay_limits.hpp`
- [ ] `include/uos/sentinel/replay_model.hpp`
- [ ] `include/uos/sentinel/replay_observation.hpp`
- [ ] `include/uos/sentinel/replay_read.hpp`
- [ ] `include/uos/sentinel/replay_retention.hpp`
- [ ] `include/uos/sentinel/replay_retention_bootstrap.hpp`
- [ ] `include/uos/sentinel/replay_retention_metadata.hpp`
- [ ] `include/uos/sentinel/replay_retention_schema.hpp`
- [ ] `include/uos/sentinel/replay_store.hpp`
- [ ] `include/uos/sentinel/upstream_capture.hpp`
- [ ] `include/uos/serve.hpp`
- [ ] `include/uos/static.hpp`
- [ ] `include/uos/systemone/handlers.hpp`
- [ ] `include/uos/types.hpp`
- [ ] `include/uos/upstream_wire.hpp`
- [ ] `include/uos/utils.hpp`

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

- L86, 95, 101, 103, 117, 130, 134, 141, 147 **SAFE** · Catalog callers retain KV while these tasks run. Pruning traverses its private vector and keeps each list/atomic receiver live through its await. Compression helpers transfer or copy input into their child tasks.
- L237, 242, 244, 260, 263 **SAFE** · The key factory is a frame value and native KV reads hold storage after a failed join. Chunk spans point into the owned entry vector, and gzip receives an owned byte copy.
- L274, 276, 289, 291, 301, 306 **SAFE** · The caller retains the KV. Memo hits are moved into private storage before insertion and no memo pointer crosses suspension. Chunk-key callbacks own version and generation.
- L550, 553, 560, 564, 575, 576, 578, 581, 585, 586, 587, 590, 595, 596, 759, 760, 763, 765, 767, 768 **SAFE** · The catalog context holds KV under the documented heartbeat precondition until stop has synchronously cleared the timer and awaited the last renewal. Renewal callbacks own state and clear the temporary renewal chain on settlement.
- L625, 627, 637, 639, 656, 663, 667, 669, 689, 691, 694, 696, 702, 705, 707, 714, 719 **SAFE** · The context or named caller retains the borrowed KV. Writes, compression, digest and pruning use frame-owned inputs and named atomic receivers across waits.
- L727, 730, 736, 740, 748, 755, 773, 777, 782, 789, 791, 797, 800, 807, 809, 812 **SAFE** · Generation and lease callers hold the KV through each directly awaited task. Each loop or retry uses copied rows and result values before the next await.

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

- L12, 14, 16, 17, 18, 24, 28, 41, 42 **SAFE** · The request abort listener momentarily owns its signal and the downstream controller, with the scope guard removing it on handoff or exception. The transport owns completion and always delivers or rejects it on the normal serve paths. The serve Task copies its injected callable.

### `src/handler/terminal_log.cpp`

Reviewed body and delivery observation, streaming source ownership, coalesced quota terminal work and Sentinel persistence.

- L40, 46, 82, 89, 94, 163, 175, 182, 186, 193, 195, 196, 200, 207, 215, 219, 227 **SAFE** · The log frame owns inputs and injected writers and observes the write promises together. The lexical scope guard runs against its own live input, and scratch JSON storage is used only by the inline formatter.
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
- L306, 324, 326, 329, 332, 343, 344, 350, 364, 373 **SAFE** · Coalescing copies the in-flight promise before waiting. Passive joins use independent tasks and subsequent views point into the named tuple or immutable frame snapshots.

### `src/http.cpp`

Reviewed synchronous OpenAI error/JSON creation, CORS and request-response helpers. Headers and other input views are consumed synchronously and outputs retain the required response/body handles.

### `src/inference_admission.cpp`

Reviewed release callbacks, fair queue updates and queued abort/timer ownership.

- L56, 57, 58, 84, 88, 149, 158, 178, 179, 205 **SAFE** · All queue and map mutations execute synchronously. Permit release owns the controller and normal grant, caller abort and timeout remove the waiter and its event listener before resolving.
- L170, 172, 176, 177 **DEFECT** · A queued waiter owns its listener and the listener strongly captures both that waiter and its controller. Dropping an isolated loop before the unreferenced wait timer fires drops the timer without invoking abort, so the controller, entries and signal remain in a cycle. Let registered waits use weak listener captures and put the lease of controller ownership in an external clearable timer.

### `src/inference_deadline.cpp`

Reviewed the three stream/paid timer factories and request-deadline signal composition.

- L15, 16, 24, 25, 32, 35, 38, 51, 52, 59, 62, 65, 83, 84, 91, 94, 97 **SAFE** · Timer callbacks copy the abort controller and active flag, and returned handles use timer identity. The timer owns no callback that retains its returned deadline handle, and clear or expiry releases the registration.

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
- L120, 123, 137, 144, 155, 158, 159, 160, 162, 174, 175, 176, 179, 202, 203 **SAFE** · The abort callback owns only the subprocess and is unregistered by close or abort. The subprocess read tasks capture stream state before being joined, and conversion serializes codec calls while retaining the codec owner.

### `src/kv_conversion/native_schema.cpp`

Reviewed SQLite adapters, schema and row metadata materialization. Live statements and iterators have shared database handles and synchronous callbacks consume local validation views.

### `src/kv_conversion/profile_json.cpp`

Reviewed entry/profile codecs and the asynchronous KV export stream.

- L370, 371, 373, 375, 383, 385 **SAFE** · Both capturing coroutine lambdas are stored by SourceAlgorithm, whose runAsync retains the exact closure through completion even after stream close clears registered callbacks. The closure shares the list iterator and native lists own storage.

### `src/kv_conversion/proof.cpp`

Reviewed offline proof fixtures, subprocess output joins, typed KV writes and the command entry point.

- L456, 506, 509, 510, 511, 513, 567 **SAFE** · The subprocess and output responses create independent owning read operations. The result tuple is named before taking references and hashing takes an owning path.
- L593, 600, 604, 609, 614, 620, 625, 627, 629, 633, 637, 639 **SAFE** · The latest-state proof holds shared KV and output cells and owns all snapshots, atomic receivers and iteration state through writes and reads.
- L673, 676, 699, 701, 706, 708, 714, 720, 729, 732, 734, 744, 753, 757, 763, 769, 771, 790, 797 **SAFE** · The outer proof owns all paths and fixture configuration and starts one named task at a time. Result and error views are local to the corresponding frame.

### `src/kv_conversion/typed_value.cpp`

Reviewed the synchronous typed graph encoder, decoder, key handling and graph slot traversal. Frames and decoded values have established local or graph owners. No asynchronous suspension or escaping callback.

### `src/lib/jev_compaction/client.cpp`

Reviewed Jev client's request assembly and response consumption.

- L24, 35, 36, 39 **SAFE** · The shared asker in askBatch retains the JevClient through ask, Fetch retains its own callable, and the response has a named owner during body parsing.

### `src/lib/jev_compaction/codex_items.cpp`

Reviewed synchronous incremental-input, last-turn and history conversions. Temporary views and variant borrows remain inside each call and results own their contents.

<!-- SITE-LEDGER-END -->

## Out-of-area observations

- `include/uos/rt/async.hpp:855-861` and `src/rt/async.cpp:410-425`. A pending race source owns its reaction, which owns both the source and the combined result. An abandoned synthetic loser therefore pins the winner until `Loop::Impl` clears pending reactions on teardown. The default loop is process-long (`src/rt/async.cpp:399-403`). Runtime owner should address pending-source reaction ownership or disarm race-only observers after settlement while still observing late rejections. The local diffs below settle only an abandoned private latch. Actual sibling work keeps the same semantics.
- `src/rt/kv.cpp:73`. `GenericAtomicOperation::commit` returns an awaited aggregate directly. The port's named-result rule also applies there. Request callers in this audit generally keep the receiver in their awaiting frame. Runtime owner should bind the result to a named local before returning it.

## Ranked defects and counts

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

### P3 · Queued admission listener survives loop teardown

`src/inference_admission.cpp:149-179`. A dropped loop clears the queue-timeout timer without running its abort callback. The controller owns each waiter, and the waiter and signal own a listener that strongly captures both. No remaining action can detach it once the loop and external owners are gone.

Keep the original queue deadline and the preference for a simultaneous caller abort. Make the explicit exact timer the outside owner of pending waiters. The waiter retains a clearable timer identity, with no strong path back to the closure. The caller-signal listener holds weak references, locks them inline and uses the existing settlement path.

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

Remove the replaced combined-signal construction and add the corresponding `uos/rt/clock.hpp` dependency if needed.

### P3 · Awaited temporary member access under GCC 12.2 rules

`src/admin/defaults.cpp:99`. The commit result is immediately dereferenced in the await expression. This violates `PORTING.md`'s named-result requirement, even though no failure was observed with Clang 21.

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

93/286 files reviewed. 194 sites: 179 SAFE, 15 DEFECT, 0 UNCLEAR. <!-- AUDIT-COUNTS -->
