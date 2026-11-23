# Native gateway residual review

Base `74062589fbb209d376a39d119cc00b5a4140d694` on `agent-jobs/cppgw-input-limits-20261119-44456009556-1`.

## Status

| Defect                                                 | Verdict | Commit and regression                                                                                                                                                                                  |
| ------------------------------------------------------ | ------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Admin default-model field and application input limits | FIXED   | `b58afd3d1d835a8b86f25149232540f98f31f8bc`. Regressions in `admin_defaults_differential_test`, `admin_codex_reset_test`, `openai_compat_part11_test`, `images_test` and `codex_supervisor_brief_test`. |
| SSE body settlement after rejected response headers    | FIXED   | `f90d511bf4e5e8adfb15cff8c20d5f9e1e49a8d4`. Regression in `serve_delivery_http_test`.                                                                                                                  |

## Input limits

With a runtime configuration present, a trimmed model above 1 MiB reached the whitespace regex before the catalog lookup. Defaults now use the existing 200 UTF-16 code unit model ID limit from Decisions and return the exact unknown-model response. The guard applies to submitted model changes. The regression compares the error body to the recorded unknown-model case and checks the stored configuration.

The application audit found the same ordering on the banked-reset account hash, chat and Responses model IDs entering provider matching, multipart image numbers (`n`, `partial_images`, `output_compression`) and oversized MIME types in inline image URLs. Those fields now use their ordinary invalid-request paths before the regex or ICU call. Decisions, SystemOne and agent message tags already check their shorter field bounds. The HTTP parser checks query limits before constructing the application URL. Passkey login and registration have a 64 KiB body limit and Claude credential submission catches account-selector rejection as HTTP 400.

The supervisor brief route also matched JSON source and thread IDs before checking their lengths. It now checks the same 32- and 200-byte maximums encoded in the ASCII patterns. `codex_supervisor_brief_test` covers both validation responses. The supervisor output route receives these IDs in the bounded HTTP query string.

## SSE header rejection

Native HTTP rejects DEL in response headers after Fetch Headers has accepted it. Before the fix, this path emitted the fallback 500 with the SSE body still pending. The transport now cancels the original body with the header error before rethrowing it to the existing fallback handler. The loopback test checks the fallback status, content type and body, one upstream cancellation, one terminal settlement, one release and one interrupted delivery log. The server drains the request before the test completes.

## Verification

The gateway gates use commit `d35a8a8bbc9a986bded5bba2d9c0f0f1a11c4ea1`, tree `089c2bdc294cce1383565ab575cf9d1c5aa5d9bd`. The runner is Ubuntu 24.04.5 on Linux ARM64 with four CPUs. Clang 21.1.8 and mold 2.30.0 linked Boost.JSON and Boost.Container 1.92.0, OpenSSL 3.0.13, zlib 1.3, ICU 78.3, Brotli 1.1.0, Zstd 1.5.5 and SQLite 3.54.0. The SQLite probe ran inside the fixture namespace. The scratch root is `/var/tmp/cppgw-residuals-1-20261119`. [Dependency recipes and archive digests](cppgw-input-limits-20261119.md#runner-and-exact-commands)

The signed LLVM repository used fingerprint `6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`. The Boost, ICU and SQLite archives passed the linked SHA-256 checks before extraction. Boost used GCC 14.2.0 and the linked static library recipe. ICU used two make workers. SQLite used one make worker and the documented `-O2 -DSQLITE_DQS=3` build.

The wrappers supply an empty home, fresh temporary directory and credential-free environment for each invocation. The driver proves loopback and outside-route denial before running each test in fresh network, mount and PID namespaces. The sanitizer configuration enables ASan, UBSan and LSan.

```sh
scratch=/var/tmp/cppgw-residuals-1-20261119
session=$(mktemp -d "$scratch/session.XXXXXXXX")
mkdir "$session/home" "$session/tmp"
instrument= # ASan uses -gline-tables-only -fno-sanitize-recover=all
env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/lib/llvm-21/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
  HOME="$session/home" TMPDIR="$session/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS="$jobs" \
  UOS_CI_BUILD_DIR="$scratch/ci" UOS_CI_SQLITE_PREFIX="$scratch/sqlite" \
  PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
  UOS_CI_CFLAGS="$instrument" UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0 $instrument" \
  UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
  bash cpp/gateway/tools/ci-build.sh --config "$profile" "$@"
```

`run-gate.sh` takes the profile, worker count and driver arguments. `run-linked.sh` uses the same environment and invokes `python3 -B "$scratch/run-linked.py" 74062589 "$skip" "$selection" --config "$profile"`. The selector intersects each test and its resolved `links.py` closure with the changed C++ paths from `git diff --name-only 74062589 HEAD -- cpp/gateway`. It passes that test list into the standard driver. It selected 134 affected binaries. The Release shard includes 39, and the Release linked run covers the other 95. An explicit comma-separated selection is used for the quick focused runs.

```sh
focus=tests/admin_defaults_differential_test.cpp,tests/admin_codex_reset_test.cpp,tests/codex_supervisor_brief_test.cpp,tests/images_test.cpp,tests/openai_compat_part11_test.cpp,tests/serve_delivery_http_test.cpp
bash "$scratch/run-linked.sh" asan 2 - "$focus"
bash "$scratch/run-linked.sh" asan 2 - -
bash "$scratch/run-gate.sh" release 2 --shard 3/4
bash "$scratch/run-linked.sh" release 2 3 -
python3 cpp/gateway/tools/links.py --check
git diff --check 74062589 HEAD
bun --no-env-file --no-install run --cwd "$scratch/formatter" format:check -- \
  "$PWD/cpp/gateway/conformance/cases.json" "$PWD/cpp/gateway/docs/CONFORMANCE.md" \
  "$PWD/cpp/gateway/docs/DEVIATIONS-FROM-BUN.md" "$PWD/cpp/gateway/docs/findings/cppgw-residuals-1-20261119.md"
```

The registration check resolved all 451 binaries. Bun 1.4.0 installed Prettier in the scratch copy of `package.json` and `bun.lock` with `bun --no-env-file install --frozen-lockfile --ignore-scripts`. The comparison controls used `python3 -B "$scratch/check-conformance.py"` under the same Linux fixture namespace and ran `python3 -B cpp/gateway/conformance/compare_test.py -v`. All ten controls passed. The manifest loader validated 186 cases and two runtime-specific contracts. Control process wall time on the tested head was 0.469 s.

### ASan before the SSE fix

`bash "$scratch/check-sse-baseline.sh"` read `cpp/gateway/src/runtime/http.cpp` from commit `37aa9be4f1b263b6f8dfd39876d3f008d46eb8a7`, SHA-256 `d5d89593d5daaa0066602326f4627399f986b921ff66af86fa8e30289bc758f9`. It mounted that file over the transport source in a private mount namespace, dropped privileges and ran `bash "$scratch/run-gate.sh" asan 2 serve_delivery_http_test`. The receipt identifies the fixture head `67d73f6adbed3e492c6bcea710f8d27ec530c1e2` and tree `872adc0a10c4572e86cbb699d15df832e5541a4d`, with source digest `40fdf8c8086af45cae126a25c915ebe042d93c616b50b871992841ff0214ebc3` for the private overlay. The binary compiled and reported 8 passes and 1 failure in 167.900 s for the driver. The rejected SSE response recorded zero upstream cancellations where the test required one. The old object was moved out of the cache before testing the fix.

### Terminal results

The finished gates recorded source digest `e53ce15d98dcb40adeb17df97d060da88c35920c4f0da25217062b28fd2aa70b`. These are incremental builds that reuse production objects. The earlier cache warmups and first shard run overlapped edits to the source tree. The driver invalidated those receipts.

| Gate                                        | Passing binaries | Passing cases | Object build | Link and test | Driver wall time |
| ------------------------------------------- | ---------------: | ------------: | -----------: | ------------: | ---------------: |
| Release shard 3/4                           |              113 |         1,494 |     45.025 s |      36.104 s |         81.668 s |
| Release affected binaries outside shard 3/4 |               95 |         1,205 |    569.007 s |      47.399 s |        616.997 s |
| Focused sanitizer run                       |                6 |           140 |    131.439 s |      19.799 s |        151.712 s |
| All affected sanitizer binaries             |              134 |         1,775 |  1,158.225 s |     108.990 s |      1,267.858 s |

Release ran 208 distinct binaries and 2,699 cases, including all 134 affected binaries and their 1,775 cases. All four receipts recorded unchanged source inputs and successful linkage and execution for every selected binary. The fixed SSE delivery binary passed all nine cases in 0.167 s in the focused sanitizer run. The evidence commit adds this report to the tested code tree.

| Receipt in `$scratch/evidence` | SHA-256                                                            |
| ------------------------------ | ------------------------------------------------------------------ |
| `release-shard3.json`          | `e51aa6d3225aea29d85a3b8a27476b7834b4572dc4115bd85dd079ac65b3af71` |
| `release-linked.json`          | `57a1b60c8122ef76a05f32c46d63c71e3fde15e26dd12557905e6552ad52e051` |
| `asan-focus.json`              | `3a23cfe5d8746d8008c5b371aad7589b255e8cf4f69dd3fe46b47c420e99e756` |
| `asan-linked.json`             | `dc33023cd48e52d5c59152513a74cce56dc4752f5c18f477ea6fc4b5c14472ee` |
| `asan-before-sse.json`         | `d4791b7784fd974e6bd52aa4f63037e7ec9af3b9e6baf53eea7c518aab33b7a4` |

### Committed review

The base was refreshed with `git fetch --no-tags origin refs/heads/agent-jobs/cppgw-input-limits-20261119-44456009556-1:refs/remotes/origin/agent-jobs/cppgw-input-limits-20261119-44456009556-1`. The review used the local head and the model recorded by the previous review, `ubiquity/deepseek-flash`.

```sh
base=origin/agent-jobs/cppgw-input-limits-20261119-44456009556-1
sudo -n unshare --net -- setpriv --reuid="$(id -u)" --regid="$(id -g)" --clear-groups --no-new-privs \
  env -i PATH="$PATH" HOME="$run/home" TMPDIR="$run/tmp" LANG=C.UTF-8 \
  timeout -k 5s 30s codex review --base "$base" -c 'model="ubiquity/deepseek-flash"' -c 'review_model="ubiquity/deepseek-flash"'
```

The attempt for the tested head and tree timed out with exit 124. The restricted review process reported `failed to connect to Responses WebSocket stage=Unknown` on each reconnection. There were no complete review findings.

### Outside this change

- The admin passkey-user update and API-key mutation handlers can pass oversized IDs to a KV lookup. The native KV adapter's separate stored-key contract rejects the lookup with `KeyTooLong` before SQLite runs it. The application paths can propagate that error. [Passkey users](../../src/auth/passkeys.cpp#L4206) · [API-key mutations](../../src/admin/api_key_mutations.cpp#L279) · [KV boundary](../../src/rt/kv_sdk.cpp#L88)
- The older regex limit descriptions still state a 2.5-second matching deadline. The current generic and scoped deadlines are documented in the security limits inventory. [Runtime](../RUNTIME.md#input-limits) · [Regex](../rt/regex.md) · [Current limits](../SECURITY-LIMITS.md#regular-expressions)
- The packaged Bun/native comparison remains outstanding. This run covers the two native request regressions, comparator controls, and the pinned reference's recorded statuses and dispatches.

## Summary

- Oversized admin, inference, and image fields now return their usual validation errors.
- Rejected SSE headers cancel the original stream once and preserve the fallback HTTP 500.

All 134 affected binaries passed in Release and under sanitizers. Release shard 3/4, registration, and conformance controls also passed. The packaged Bun comparison remains pending, and the isolated committed-diff review timed out.
