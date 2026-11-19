# Native gateway input limits

Base branch `agent-jobs/cppgw-combine-1-20261119-44443689630-1`, commit
`e97ed985ef6c39377783e0263b10d15767de1432`, tree `95f338db7da4f5a6afc5a69c1d947c242350bfce`.

## Work and verification

- Read the native gateway README, CI driver guidance and porting rules. Inspected the clean checkout and started locating library call sites.
- `uname -m`, `cat /etc/os-release`, `g++-14 --version`, `pkg-config --modversion sqlite3 icu-uc`, and `bun --version` identified this host as Ubuntu 24.04.5 ARM64 with GCC 14.2, SQLite 3.45.1, ICU 74.2 and Bun 1.4.0. The qualified dependencies and isolated test runs are pending.
- Added encoded and decoded byte limits to the strict and Buffer base64 adapters. The encoded ceiling is the padded size of the existing 50 MiB image allowance. The safe decoder receives an output buffer capped at 50 MiB. The test covers both limits independently, including an extra decoded byte with the same encoded size.
- Bounded request bodies at the HTTP connection, including chunk declarations before payload reaches llhttp. Kept 16,408 bytes and 198 fields for server heads and trailers, the 4 KiB chunk metadata line and the larger image-edit body allowances. Added parser cases for the exact limits and one beyond.
- Applied the shared head and body limits in the response parser used by Fetch and WebSocket handshakes. Fixed-length bodies and chunk declarations are checked before payload, close-delimited responses stop at the byte limit, and chunk lines and trailers have separate allowances. Added split-head and exact-boundary parser cases.
- Added cumulative compressed and decoded byte budgets and an expansion ratio to the gzip, Brotli and Zstd paths. Fetch keeps a 16 MiB ceiling and stored gzip keeps room for replay envelopes at 64 MiB. Zstd limits its decoding window to 16 MiB. Client HTTP content encodings reach the application as raw request bytes under the existing behavior. Added budget, gzip and Brotli checks to the existing library fixture.
- Bound all QuickJS-ng patterns and subjects before conversion or execution. Fixed image, SSE framing and compaction patterns use the corresponding bounded application inputs. Connected the engine deadline to the header with an initial 1,500 ms allowance. Final tuning awaits measurements in both build profiles.
- Added Ada guards for URL and authority length, query bytes and parameter count. Long remote image paths fit the existing UTF-16 reference allowance. Query mutations check their operands and preserve the last valid value when the resulting query exceeds its budget. The URL fixture exercises exact boundaries.
- Moved the existing multipart boundary limit into the header and enforced total, part, header and field limits before copying part data or decoding text. Bounded the explicit simdjson allowance at the existing image-edit exception while keeping smaller caller limits. Added exact-boundary cases to the HTTP and JSON fixtures.
- Applied the shared CBOR size and depth before the decoder advances. Added 64 KiB certificate and 256 KiB decoded attestation-chain checks before X.509 parsing and PEM conversion. Preserved the existing ASN.1 depth, element and CRL size ceilings. The certificate fixture pads a known DER certificate to the boundary and checks one byte beyond.
- Set OpenSSL's pre-decode peer certificate-message ceiling on the shared TLS context. The aggregate TLS check also enforces the individual 64 KiB cap. Bounded local PEM stores and each decoded trust anchor, and applied the same DER byte cap to RSA SPKI and PKCS#8 imports. Added context and DER import checks.
- Applied the 16 MiB WebSocket message cap, kept smaller caller limits, and counted empty continuation frames toward a 4,096-frame ceiling. Shared sockets now expire after 60 seconds without a read or completed write. Added exact message and fragment boundary tests and a fake-clock idle test.
- Bounded cumulative chunk metadata and consecutive informational responses. Added an 8 MiB limit for decoded upstream SSE lines across socket reads, with the same allowance in the framing regexes for provider streams without a trustworthy media type. The distinct response-size error keeps passkey revocation checks fail-closed on decoder budget failures.
- Added a common text-input ceiling at the SIMD converters and a separate 16 MiB ICU input ceiling. UTF-8 validation reports oversized input as invalid and decoding raises the usual text error type. The non-throwing length helper keeps exact length by counting directly for oversized strings. Added exact-limit Unicode fixture cases.
- Bounded stored queue JSON and encoded KV cursors before parsing, reused the shared base64 adapter for the cursor wire format, and mapped a stored regex resource rejection to the existing malformed-value error. Added boundary checks in the durable queue recovery and cursor fixtures.
- Applied the same Ada query count at the inbound HTTP boundary. An oversized query now receives HTTP 400 before request construction instead of reaching the generic handler failure path.
- Added the library boundary inventory to `docs/SECURITY-LIMITS.md` and checked Ada input after Unicode normalization where its size can expand.
- Bounded composed Fetch request heads before Beast header comparisons and transmission. Added exact-size and field-count tests for outgoing heads and a loopback check for the inbound 413.
- Added Zstd output and window boundary cases using frames generated by the linked compressor, plus a stored gzip test that crosses the decoded ceiling in a second member.
- Kept direct coverage of QuickJS-ng’s allocation ceiling after the smaller subject check began rejecting the old oversized examples earlier. Added checks for the deadline supplied to the library and the full image subject allowance.
- The focused Release run exposed two compatibility cases. Fetch keeps its original 1.5 MiB head ceiling for the existing 1 MiB response field fixture. The multipart ceiling is 2,048 parts to cover the existing 1,124-part MIME table fixture. Also kept Zstd’s exact final full output buffer from being mistaken for the start of another frame.
- Release shard 1 exposed another compatibility case in `sentinel_upstream_capture_differential_test`. A 4 MiB stored chunk encodes to 5,592,408 base64 bytes. The fixed canonical-base64 pattern now uses the shared encoded-byte ceiling and a bounded ASCII scan. The existing fixture still tests Sentinel’s smaller aggregate bound and its ordinary rejection.
- Release shard 2 exposed the same allowance issue in the base64url alphabet normalization used by `sentinel_upstream_capture_test`. The four existing literal substitutions now use a bounded standard-library replacement on large encoded inputs. The fixture continues to verify the exact retained byte count and both truncation flags.
- Release shard 3 stopped during fixture registration because the checked-in compaction replay expands from 163,003 to 71,469,423 bytes. Its existing 72 MiB caller allowance now bounds fixture-only gzip and JSON loading. Runtime request and stored gzip callers keep their central caps. The test data and assertions remain intact.
- ASan shard 3 found one more fixed pattern that needed time to preserve existing behavior. A timing-only `bash "$scratch/run-gate.sh" asan 4 codex_supervisor_brief_test` run passed 37 cases with a diagnostic 1,800 ms deadline and timed the supervisor’s credential-key scan at up to 593.402 ms. The first scoped budget, 750 ms, still failed in the four-worker shard. The single matching pattern now receives five ordinary budgets (1,250 ms). Its subject size, compilation deadline and the other redaction patterns keep the ordinary limits. The diagnostic code was removed before acceptance.

## Runner and exact commands

The scratch root is `/var/tmp/cppgw-input-limits-20261119.v8RZKRLB`. The four-core ARM64 runner uses Clang 21.1.8, OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5. The SQLite probes inside the production fixture boundary read 3.54.0. ICU 78.3 and Boost 1.92.0 came from verified source archives. The Boost build used GCC 14.2.0 for the static archives.

| Archive                                                                                    | SHA-256                                                            |
| ------------------------------------------------------------------------------------------ | ------------------------------------------------------------------ |
| `https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2`                     | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` |
| `https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz` | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |
| `https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz`                               | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |

Downloaded each archive using `curl -q -fLsS --retry 3 <url> -o <archive>`, verified with `sha256sum -c`, and extracted it with `tar -xf`. The signed Noble LLVM 21 repository used key fingerprint `6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`.

```sh
sudo apt-get -o Acquire::Retries=2 -o Acquire::https::Timeout=20 update -qq
sudo apt-get install -y -qq clang-21 libclang-rt-21-dev mold libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev util-linux iproute2 pkg-config

# In $scratch/boost_1_92_0. The user config selects /usr/bin/g++-14.
./bootstrap.sh --with-libraries=json,container
./b2 -j2 --user-config="$scratch/boost-user-config.jam" --with-json --with-container variant=release link=static threading=multi toolset=gcc-14 stage

# In $scratch/icu-build.
../icu/source/configure --prefix="$scratch/icu78" --disable-tests --disable-samples --disable-extras --disable-static
make -j2
make install

# In $scratch/sqlite-autoconf-3540000.
./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1
make install
```

`run-gate.sh` in the scratch root takes a profile, worker count and optional driver selection. It uses the following test command, with a new `session` directory containing empty `home` and `tmp` directories for each invocation. The driver further isolates each binary in real Linux network, mount and PID namespaces with loopback and a fresh tmpfs. The sanitizer flags supplement the driver's address, undefined-behavior and leak checks.

```sh
env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/lib/llvm-21/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
  HOME="$session/home" TMPDIR="$session/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS="$workers" \
  UOS_CI_BUILD_DIR="$scratch/ci" UOS_CI_SQLITE_PREFIX="$scratch/sqlite" \
  PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
  UOS_CI_CFLAGS="$instrument" UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0 $instrument" \
  UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
  bash cpp/gateway/tools/ci-build.sh --config "$profile" "$@"
```

`instrument` is empty for Release and `-gline-tables-only -fno-sanitize-recover=all` for ASan. The early `$scratch/run-gate.sh release 2 rt/base64_test` and the corresponding `asan` run each observed 1 passing binary and 9 passing cases in 1,456.949 s and 1,948.448 s respectively. Source inputs changed during these cache-warming runs, and the driver invalidated both receipts. The first focused timing compile found a stale ASN.1 constant name in the distinguished-name reader. The source now uses `kCertificateBytes` there.

`python3 -B cpp/gateway/tools/links.py --check` resolved 451 registered binaries. Bun 1.4.0 installed Prettier into a scratch copy of `package.json` and `bun.lock` using `bun --no-env-file install --frozen-lockfile --ignore-scripts`. Selected Markdown files passed `bun --no-env-file --no-install run --cwd "$scratch/formatter" format:check -- "$PWD/cpp/gateway/docs/SECURITY-LIMITS.md" "$PWD/cpp/gateway/docs/findings/cppgw-input-limits-20261119.md"`. `git diff --check` passed.

## Regex deadline measurement

Timing probes in the scratch build measured each `Matcher` lifetime from its deadline initialization to destruction. The probes logged elapsed microseconds, subject bytes, search count and a category. They did not record patterns or subject text. The diagnostic command was `bash "$scratch/run-focused.sh" <release|asan> 2 '(/rt/(base64|crypto|fetch_transport|http|json|kv|kv_sqlite|regex|text_codec|url|websocket_lifetime)_test|/legacy/(fetch_response_library|passkeys_asn1_library|passkeys_cbor|runtime_http_llhttp|string_icu|websocket_library)_test|/(images|jev_compaction|runtime_http|runtime_websocket)_test)\.cpp$'`. It selected exact fixture link closures from the CI driver and kept its real namespace isolation. The temporary instrumentation was removed before the acceptance runs.

| Operation                                                                      |          Subject |    Release |                        ASan |
| ------------------------------------------------------------------------------ | ---------------: | ---------: | --------------------------: |
| Jev token estimation in the floor-bound compaction fixture, 1,284,750 searches |  8,101,176 bytes | 425.964 ms |                1,374.532 ms |
| Large multipart image data URL capture                                         | 22,369,646 bytes | 139.858 ms |                  684.177 ms |
| Full image subject ceiling before the ASCII fast path                          | 70,953,644 bytes | 399.991 ms | Interrupted at 1,500.449 ms |
| Existing accepted million-byte regexp fixture                                  |  1,000,000 bytes |  43.022 ms |                  140.644 ms |

The engine uses 10 ms for compilation, 250 ms for ordinary matching and 1,800 ms for the fixed image, compaction and SSE patterns when the subject exceeds 1 MiB. The fixed supervisor credential-key pattern uses 1,250 ms after its sanitizer fixture took 593 ms alone and exceeded the first 750 ms budget in the four-worker run. The old engine used 2,500 ms. The generic million-byte fixture alone took 141 ms under sanitizers, and the large compaction operation took 1,375 ms. The allowances provide about 109 ms and 425 ms of scheduling margin respectively. The fixed large ASCII image pattern uses a bounded linear scan with the same captures. Other patterns retain QuickJS-ng execution and its polling and allocation ceilings.

The timing diagnostics observed Release 16 passing binaries, three failing binaries and two build failures, with 322 of 325 reported cases passing. ASan observed 16 passing binaries, four failing binaries and one build failure, with 348 of 353 cases passing. They overlapped source edits and both receipts were invalidated. The compatibility and compile fixes are listed above. The acceptance totals below come from separate, uninstrumented runs.

## Acceptance runs

The first focused uninstrumented checks used `577ada6b3463f6cd3c9a181877c8f13ff6ff39fa`, tree `1d3dec2e8fb331f9a17f419ce3f0b57ff55b732b`. They each passed **4 binaries and 62 cases** and covered `fetch_response_library_test`, `gzip_test`, `http_test` and `regex_test`. The Release driver took 437.388 s and ASan took 601.783 s. Both receipts recorded unchanged source SHA-256 `7c4de595fe1b2e26608362c9e6f596b19834602df0843f9319d8552bc8caa6bb`.

```sh
bash "$scratch/run-smoke.sh" release 1 '^tests/(legacy/fetch_response_library|rt/(gzip|http|regex))_test\.cpp$'
bash "$scratch/run-smoke.sh" asan 1 '^tests/(legacy/fetch_response_library|rt/(gzip|http|regex))_test\.cpp$'
bash "$scratch/run-shards.sh" release 2
bash "$scratch/run-shards.sh" asan 2
```

The smoke wrapper uses the driver with only the registered test closures and every runtime object. `run-shards.sh` invokes `bash "$scratch/run-gate.sh" "$profile" 2 --shard "$shard/4"` for shards 1 through 4 in each profile. It keeps each `driver.log`, `results.tsv` and `receipt.json` in `final-<profile>-<shard>`.

The first full Release shard observed 112 of 113 binaries and 1,197 of 1,198 cases passing. The sole failure was the stored Sentinel base64 compatibility case described above. Commit `36fc3ddaf898ee0573cbd88a5b00a15da5543753`, tree `4bc704e799797a1fa77f3f7087f584d7d02b4169`, passed that shard with **113 binaries and 1,199 cases**, source SHA-256 `68b151c9a6e7cdb80860d0df9e01b563a1caa6809d048d19e72d9a10d7a397f6`. The later base64url normalization, trusted replay loader and supervisor redaction fixes led to the final matrix below.

All eight final receipts recorded code commit `b26c9e4c606bd4a7fcdb1340b62da76784528b57`, tree `44a34e3b2996987b115bdc5d8c4f5344e1d31f1d`, and source SHA-256 `4720db1121dade35e9b215019b0b8d5650987c0b4319faaa816cc854f503066e` before and after each run. Both profiles passed **451 binaries and 5,222 cases**. The compaction forwarding fixture passed 17 cases, the image fixture 38, Jev compaction 24, supervisor brief 37, and both Sentinel upstream capture fixtures 15 each in each profile.

```sh
bash "$scratch/run-selected-shards.sh" asan 4 3 4 1 2
bash "$scratch/run-selected-shards.sh" release 4
python3 -B "$scratch/read-final-receipts.py" "$scratch"
python3 -B cpp/gateway/tools/links.py --check
git diff --check origin/agent-jobs/cppgw-combine-1-20261119-44443689630-1 HEAD
```

The final shard wrapper invokes `bash "$scratch/run-gate.sh" "$profile" 4 --shard "$shard/4"` in the specified order, using the environment above. The Release pass used shard order 1, 2, 3, 4. All final measurements are incremental. The receipt check verified identical input digests, the exact commit and tree, zero compile failures, zero failing or timed-out binaries and non-overlapping selections of 451 tests per profile.

| Profile and shard | Passing binaries | Passing cases | Compile (s) | Link and test (s) | Driver wall (s) |
| ----------------- | ---------------: | ------------: | ----------: | ----------------: | --------------: |
| Release 1/4       |              113 |         1,200 |       8.625 |            16.655 |          25.803 |
| Release 2/4       |              113 |         1,339 |       0.009 |            34.644 |          35.177 |
| Release 3/4       |              113 |         1,493 |       0.009 |            23.248 |          23.795 |
| Release 4/4       |              112 |         1,190 |       0.009 |            11.794 |          12.336 |
| ASan 1/4          |              113 |         1,200 |      11.791 |            96.750 |         109.120 |
| ASan 2/4          |              113 |         1,339 |      10.937 |           109.069 |         120.535 |
| ASan 3/4          |              113 |         1,493 |       2.297 |            44.285 |          47.169 |
| ASan 4/4          |              112 |         1,190 |     102.334 |            31.489 |         134.410 |

## Committed review

The base was fetched using `git fetch --no-tags origin refs/heads/agent-jobs/cppgw-combine-1-20261119-44443689630-1:refs/remotes/origin/agent-jobs/cppgw-combine-1-20261119-44443689630-1`. The local command `codex review --base origin/agent-jobs/cppgw-combine-1-20261119-44443689630-1` used the configured `ubiquity/deepseek-flash` model, for heads `db0dd9101fac487160366d47d87ba6a471dac69c` and `577ada6b3463f6cd3c9a181877c8f13ff6ff39fa`. Both runs reported `bwrap: loopback: Failed RTM_NEWADDR: Operation not permitted` before reading the diff. The system bubblewrap package and a read-only probe reproduced the failure.

The fallback piped a prompt with exact base and head, followed by `git diff --no-ext-diff --no-color "$review_base" "$review_head"`, into `timeout -k 15s 1200s codex review -` with the same model. The completed reviews used the committed diffs and identified these findings verbatim.

`db0dd9101fac487160366d47d87ba6a471dac69c`

```text
- [P1] Raising the regex deadline 180x permits client CPU stalls — /home/runner/work/_temp/agent-jobs/cppgw-input-limits-20261119/checkout/cpp/gateway/include/uos/rt/limits.hpp:28-28
  `kRegexDeadline` increases from 10 ms to 1,800 ms, so every client-influenced regex operation can now occupy a worker for 180x longer. Since request paths, fields, model IDs, headers and large image strings flow through this adapter, a single request can chain several multi-second matches and compile/match operations; the tests only justify a larger budget for fixed compaction/image patterns, not for all generic client subjects. Use a separate deadline for those trusted patterns or retain a much tighter generic deadline.
```

The 10 ms declaration was unused on the base. The revision adopts the suggested separation with a 10 ms compile deadline, 250 ms for ordinary matches and 1,800 ms for fixed patterns above the generic subject limit. The earlier full-matrix object builds were stopped when this revision was made.

`577ada6b3463f6cd3c9a181877c8f13ff6ff39fa`

```text
- [P1] Preserve the 2.5 s regex matching budget — /home/runner/work/_temp/agent-jobs/cppgw-input-limits-20261119/checkout/cpp/gateway/include/uos/rt/limits.hpp:28-29
  The old matcher enforced a flat 2.5 s timeout, but these constants are now the executed budgets: ordinary patterns get 250 ms, and even the image/compaction/SSE patterns that admit subjects above 1 MiB get only 1.8 s. Any request whose regex previously completed between 250 ms and 2.5 s, or between 1.8 s and 2.5 s for a scoped pattern, will now fail at the timeout path; the recorded ASan run already consumed 1.37 s on the compaction scan and was interrupted at 1.5 s on the full image subject, leaving little margin. Keep the previous 2.5 s execution budget while retaining the subject-size caps.
```

Lowering the enforced deadline is an explicit requirement. The revised full-image case passed the uninstrumented sanitizer check. The compaction allowance follows the recorded 1,375 ms operation plus scheduling margin.

On final code commit `b26c9e4c606bd4a7fcdb1340b62da76784528b57`, `timeout -k 5s 90s codex review --base origin/agent-jobs/cppgw-combine-1-20261119-44443689630-1` reproduced the sandbox failure with `ubiquity/deepseek-flash`. The earlier stdin retry for `36fc3ddaf898ee0573cbd88a5b00a15da5543753` exited 124 after reconnects. The final stdin fallback sent the committed follow-up diff from `577ada6b3463f6cd3c9a181877c8f13ff6ff39fa` using `git diff --no-ext-diff --no-color "$review_base" "$review_head" -- cpp/gateway/src cpp/gateway/include cpp/gateway/tests` and `timeout -k 5s 120s codex review -`. It also exited 124. The final review remains incomplete.

## Outside the owned paths

- `docs/RUNTIME.md` and `docs/rt/regex.md` still describe several limits from the combined base, including the 2.5-second regex deadline. The current inventory lives in `SECURITY-LIMITS.md`.
- Some provider parsers search a growing SSE buffer from the beginning on each read. The transport and runtime regex checks now bound that buffer and its lines. Changes to the provider parsing loops were outside the assigned paths.
- The admin default-model normalizer runs its whitespace regex before validating against the catalog. A model field above the generic regex subject ceiling can therefore reach the generic HTTP 500 handler instead of the route’s usual validation response. This path was identified by source inspection. [Normalizer](../../src/admin/api_keys.cpp#L260) · [Admin defaults caller](../../src/admin/defaults.cpp#L70)

## Summary

The native gateway applies shared input limits to HTTP, WebSocket, decompression, parsing and stored encodings. Existing image, compaction and Sentinel fixtures retain their bounded allowances.

Regex compilation uses 10 ms and ordinary matches use 250 ms. Measured exceptions are 1.25 seconds for the fixed credential-redaction pattern and 1.8 seconds for fixed patterns on larger inputs.

All four Release and all four ASan, UBSan and LSan shards passed on ARM64, with 451 binaries and 5,222 cases per profile. Registration and formatting passed. The final committed-diff review remains incomplete. An oversized admin default-model field can still reach HTTP 500 in the out-of-scope caller.
