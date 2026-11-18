# C++ gateway library integration, stage 1

## Work contract v1

Start at small-helpers tip `6393434b110e2d1afe93e6c7005848219336d9b9`.
Merge the accepted ICU branch first and the accepted llhttp server branch
second, with two-parent commits. Own the shared registries, build paths and
fixture reconciliation needed under `cpp/gateway/`. Preserve the accepted
vendor bytes and leave TypeScript, hosted workflows, other `cpp/` paths and
parallel integration lanes outside these edits. The handoff is the stage 1
branch for the maintainer's next integration.

| Surface               | Acceptance                                                                                                                            |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| Unicode               | ICU casing, collation and IDNA with the build's actual Unicode data version                                                           |
| HTTP server           | Strict standard request parsing and original status, size, framing, raw-field, dispatch and delivery contracts for supported requests |
| Date and Headers      | Dates received by gateway callers, rejection of malformed input and bounded error messages                                            |
| Other runtime helpers | Preserve the merged helper branch's console, path, filesystem and environment contracts                                               |
| Application           | Unchanged authentication, validation, stored data, accounting and provider routing                                                    |

Resolve the supported date formats from TypeScript and C++ call sites before
editing date expectations. Check each wire difference against HTTP and URI rules
and repository client evidence. The actual merge conflicts, compiler behavior
and fixture results are open at discovery.

Prepare the ARM64 compiler, Boost 1.92.0 and SQLite 3.54.0 outside the
checkout. Use the same ICU 78.3 as the selected native release if available.
Compile C vendor files with the C compiler. Test with a credential-free
environment and the driver's isolated loopback fixtures. Verify all four
`bash cpp/gateway/tools/ci-build.sh --config release --shard N/4` selections,
the affected sanitizer selection, vendor archive registration and the changed
documents. Stop at a clean stage branch with both merges, exact receipts and the
runtime source line-count comparison.

## Progress

The runner is Ubuntu 24.04.5 on AArch64 with four CPUs. The checkout's
`agent-job` branch matches the specified small-helpers tip. Both incoming
branch tips have been fetched from `origin`.

## Continuation and merge resolutions

The resumed checkout started at `e6bfbcab395600c153bf695e25854fff8462da90`.
`git log --first-parent --merges -2 --format='%H %P %s'` and a fresh fetch
verified both two-parent commits and their accepted parent tips.

| Order | Merge                                      | Accepted second parent                                   |
| ----- | ------------------------------------------ | -------------------------------------------------------- |
| 1     | `d7976742f4aa351478d6b975c47c0b7c0399d28f` | ICU `b54357a92c0f2669335d83f04047c3f7de32f925`           |
| 2     | `e6bfbcab395600c153bf695e25854fff8462da90` | llhttp server `441f5ff457d03ce47b472f0b8025274ac38b543b` |

`git show --remerge-diff` found one add/add documentation conflict in each
merge, both in `docs/DEVIATIONS-FROM-BUN.md`. The first merge kept the helper
sections and appended ICU case mapping and collation. The second preserved both
sides with conflict markers. Commit `0bc13c83` removed the markers and duplicate
title, and retained every section. The fuzz registry contains both new targets.
Git reported no textual conflicts in CMake, the vendor registry or `tests/LINKS.txt`.
Both new standalone tests resolve through their existing Links declarations.

The inherited llhttp merge already incorporates the CMake, SDK unit, Mac,
portable CI and benchmark archive changes. `tools/vendor_sources.py` and
`cmake/vendors.json` resolve the same three llhttp files and consumer closures.
The portable driver uses separate `CC` and `CXX` rules and keeps the C++ warning
set off vendor compilation. CMake configured with `-DUOS_GATEWAY_TESTS=ON` on
this runner and built `uos_vendor_llhttp` with the C compiler. `ar t` returned
`llhttp.c.o`, `api.c.o` and `http.c.o`. `git diff --exit-code` against the
accepted llhttp parent found identical bytes throughout `third_party/llhttp`.

## Date formats checked before fixture changes

The audit used `rg -n -i 'retry-after|rate.?limit.reset|resets_at|last_refresh|expires_at'`
and `rg -n 'Date::parse|Date\.parse|new Date\('` in `src/`, `serve.ts` and the
corresponding C++ modules. Local provider and native-auth fixtures supply the examples.

| Source                                                                                | Formats and existing handling                                                                                                                                                                                           |
| ------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Codex, Claude Code, Lithos and Voyage `Retry-After`                                   | Delay seconds take the numeric path. Absolute values use IMF-fixdate such as `Mon, 16 Nov 2026 06:00:00 GMT`. `authority/native-codex-429` additionally requires a canonical round trip for a durable reset identity.   |
| Lithos and Cerebras reset headers                                                     | `provider/lithos-rate-limits` parses explicit duration units such as `3.23s`, `750ms` and `1m30s`. `provider/cerebras-rate-limits` parses numerical windows separately.                                                 |
| Codex 429 reset body and JWT access expiry                                            | `authority/native-codex-429` and `codex/auth` read epoch seconds as JSON numbers and convert to milliseconds.                                                                                                           |
| Codex native `last_refresh`, banked-credit `expires_at`, and Claude usage `resets_at` | The native-auth generation and credit fixtures use offset-bearing ISO 8601, including fractional seconds. Claude usage snapshots cover ISO and IMF-fixdate, and retain raw reset text when a deadline cannot be parsed. |
| Persisted observation and expiry times                                                | `observed_at_ms`, `expires_at_ms` and numeric KV Date nodes use epoch milliseconds directly.                                                                                                                            |

The inherited date code covers these formats. The audit found the three synthetic
`last_refresh` spellings and the single-character Claude reset in the differential
fixtures only. They need no change to the production date grammar.

## Build and fixture reconciliation

The resumed portable release gate was
`bash cpp/gateway/tools/ci-build.sh --config release legacy/runtime_http_llhttp_test`.
Clang 21.1.8 first compiled 367 of 369 production sources. Diagnostics came
from inline code in simdutf's header and ICU's `uset.h` and `unistr.h`. Commit
`3a150ca0` scoped `-Wsign-conversion` exceptions to the upstream header
inclusions and retained dependency tracking and the first-party warning set.
The repeat compiled 369 of 369 production sources and passed the parser
binary's seven cases. The initial object phase took 617.292 s. The incremental
object phase took 1.857 s.

`ninja -t commands` showed three `clang-21 -std=c11` commands without the
port's warning flags, followed by one archive command. `ar t` confirmed all
three members in CMake, the portable driver and the SDK unit make rule exercised
with a runner compiler shim. The older `tools/ci_sanitize.py` discovers the
same three C files. Its versioned Clang path formerly looked for
`clangcc-21`. Commit `c9f96a85` retains the compiler version and supports an
explicit `CC`. All three C11 objects compiled through that path. The Bash
entrypoint change in `b6259914` fixed a permission error in the shard control.
The driver controls passed 7/7 in debug and 7/7 under ASan/UBSan.

| Test and commit                                      | Decision                                                                                                                                                                                                                                                                                    |
| ---------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `runtime_http_differential_test` (`d80f2591`)        | Expect 400 and connection close before dispatch when a request carries a transfer coding without final chunked framing. RFC 9112 §6.3 requires this response. Reject repeated equal Content-Length fields as permitted by RFC 9110 §8.6.                                                    |
| Same HTTP test                                       | DEL is excluded from the HTTP field value grammar in RFC 9110 §5.5. Raw UTF-8 request-target bytes must be percent-encoded per RFC 9112 §3.2 and RFC 3986. The original four inputs still run and the added valid-header companion retains coverage of OWS, empty fields and Latin-1 bytes. |
| `claude_code_usage_schema_test` (`a1d26ab5`)         | Keep the raw synthetic `"0"` and expect a null normalized deadline. The ISO and IMF-fixdate windows retain their original deadlines.                                                                                                                                                        |
| `codex_external_auth_differential_test` (`90e7732e`) | Reject the synthetic `"Nov 16 2026"`, `" 2026-11-16 "` and `"1"` values. The other 51 inputs retain their original expectations, including the ISO fractional and offset cases, the canonical HTTP date and the official access-only fixture.                                               |
| `rt/http_test` (`7ef86f45`)                          | Both malformed redirects still require `TypeError` and the `location` field identity. The new message excludes the rejected bytes.                                                                                                                                                          |
| `rt/url_idna_test`                                   | Link the already qualified ICU **78.3**. Keep all curated and exhaustive assertions. The Ubuntu ICU 74.2 data accounts for the differing results in the incoming lane reports.                                                                                                              |

The recorded Codex client uses typed HTTP POST requests to `responses` and
serializes `last_refresh` as Chrono `DateTime<Utc>`. The supported OpenAI
and Codex routes use ASCII path components and normal HTTP body framing. The
client contracts and local wire controls gave no evidence of reliance on the
four malformed requests. The percent-encoded URI and valid Latin-1 header paths
are covered by the parser fixture.

- [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html)
- [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html)
- [RFC 3986](https://www.rfc-editor.org/rfc/rfc3986.html)
- [Pinned Codex responses client](https://github.com/openai/codex/blob/7b44ac1661433ae3a6f375fde873fa7123c935fe/codex-rs/codex-api/src/endpoint/responses.rs)
- [Pinned Codex auth storage](https://github.com/openai/codex/blob/7b44ac1661433ae3a6f375fde873fa7123c935fe/codex-rs/login/src/auth/storage.rs)

## Production source size

Counts are physical lines of the 52 tracked C++ source and private header files
under `src/rt` and the 8 under `src/runtime`, measured from Git blobs with
`git ls-tree -r --name-only <commit>` and `git show <commit>:<path>`. They
include blank and comment lines. Vendored and test sources are outside these
directories.

| Directory     | Baseline `55afcc8b1` | Stage 1 |        Change |
| ------------- | -------------------: | ------: | ------------: |
| `src/rt`      |               31,605 |  29,246 |        −2,359 |
| `src/runtime` |                2,966 |   3,004 |           +38 |
| Total         |               34,571 |  32,250 | −2,321 (6.7%) |

## Linux ARM64 runner and commands

Ubuntu 24.04.5 on AArch64 with four CPUs. The C and C++ compilers were Clang
21.1.8, with mold 2.30.0, Boost 1.92.0, OpenSSL 3.0.13, SQLite 3.54.0, zlib 1.3,
Brotli 1.1.0, Zstd 1.5.5 and Bun 1.4.0. Relinking the IDNA fixture with its
recorded command and inspecting `ldd` resolved `libicuuc.so.78.3`,
`libicui18n.so.78.3` and `libicudata.so.78.3` from the staged ICU directory.

The dependency scratch directory was `/var/tmp/cppgw-integrate-s1.J2gz51dr`.
Clang came from the signed apt.llvm.org Noble 21 archive.
The apt install command was `sudo apt-get install -y clang-21 libclang-rt-21-dev llvm-21 libbrotli-dev libssl-dev libicu-dev libzstd-dev zlib1g-dev mold`.
The three downloaded release archives passed `sha256sum --check` before extraction.

| Archive                          | SHA-256                                                            | Build commands                                                                                                                                                          |
| -------------------------------- | ------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `boost_1_92_0.tar.bz2`           | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` | `./bootstrap.sh --with-libraries=json,container` and `./b2 -j2 --with-json --with-container variant=release link=static runtime-link=shared cxxstd=20 stage`            |
| `icu4c-78.3-sources.tgz`         | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` | Out-of-tree `../icu/source/configure --prefix="$scratch/icu78" --disable-tests --disable-samples --disable-extras --disable-static`, then `make -j2` and `make install` |
| `sqlite-autoconf-3540000.tar.gz` | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` | `./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'`, then `make -j1` and `make install`                            |

The runner used the driver's real Linux network, mount and PID namespaces via
passwordless sudo and dropped back to uid 1001. Each binary ran with an empty
home, a fresh tmpfs and loopback only. The link step bound both staged runtime
libraries through RPATH so fixture subprocesses could clear their environment.

```bash
scratch=/var/tmp/cppgw-integrate-s1.J2gz51dr
for shard in 1 2 3 4; do
  bash "$scratch/ci-env.sh" bash cpp/gateway/tools/ci-build.sh --config release --shard "$shard/4"
done
CPP_STAGE1_JOBS=4 CPP_STAGE1_LINE_TABLES=1 bash "$scratch/ci-env.sh" python3 -B "$scratch/asan-stage1.py"
bash "$scratch/ci-env.sh" python3 -B cpp/gateway/cmake/ci/test_driver.py
bash "$scratch/ci-env.sh" env UOS_CI_SELFTEST_CONFIG=asan python3 -B cpp/gateway/cmake/ci/test_driver.py
python3 -B cpp/gateway/tools/links.py --check
(cd cpp/gateway/third_party/llhttp && sha256sum --check SHA256SUMS)
git diff --check 55afcc8b1..HEAD -- . ':(exclude)cpp/gateway/third_party'
```

The scratch `ci-env.sh` launched commands through `env -i` with a fresh
`HOME` and `TMPDIR`, `TZ=UTC`, `LC_ALL=C.UTF-8`, `CC=clang-21`,
`CXX=clang++-21`, `UOS_CI_JOBS=4` and `UOS_CI_BUILD_DIR="$scratch/ci"`.
Its PATH was `/usr/local/bin:/usr/bin:/bin:/home/runner/.bun/bin:/usr/lib/llvm-21/bin`.
`PKG_CONFIG_PATH` was `$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig`.
`UOS_CI_CXXFLAGS` added `-isystem $scratch/boost_1_92_0` and
`UOS_CI_LDFLAGS` added
`-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib`.

The sanitizer selector uses `tools/links.py` to select all 88 test closures
containing `src/runtime/http.cpp`. It adds the standalone parser and ICU
regressions, `tests/rt/{console,date,env,fs,headers,http,string,url_idna}_test.cpp`,
and the affected application fixtures. These are
`claude_code_models_differential`, `claude_code_usage_schema`,
`claude_code_usage`, `codex_banked_reset_provider_differential`,
`codex_external_auth_differential`, `codex_native_auth`,
`kv_conversion_profile_json_differential`, `lithos_wiring` and
`runtime_native_startup`. The wrapper supplies that sorted list to the original
driver with `--config asan`. It uses the driver's normal compiler, linker,
isolation and receipt paths. C and C++ debug flags include
`-gline-tables-only` alongside the profile's ASan and UBSan flags. The first
sanitizer compile was stopped after 18 of 496 edges to raise the job
count from two to four. The measured run reused those objects.

## Release receipts

Commit `c9f96a85623c855ade3e89c222b8a553bb307e35`, Git tree
`cc4195c017e1d06b021b979000793c04ae457c19`. The driver's input SHA-256
was `a4f3b87fcb31ee733f8fe12ecf7aaef7579ea9f419f6b4826018d0fd5ff0558a`
throughout. The four selections are disjoint and cover all 443 tests. Each
command exited 0 with 369 production objects, zero failing or unbuilt binaries
and zero timeouts.

| Shard | Passing binaries | Passing cases | Object phase | Link/test phase |     Total |
| ----- | ---------------: | ------------: | -----------: | --------------: | --------: |
| 1/4   |          111/111 |   1,329/1,329 |    519.617 s |        27.731 s | 547.676 s |
| 2/4   |          111/111 |   1,296/1,296 |    516.133 s |        43.398 s | 560.203 s |
| 3/4   |          111/111 |   1,342/1,342 |    540.379 s |        30.456 s | 571.534 s |
| 4/4   |          110/110 |   1,070/1,070 |    465.676 s |        20.104 s | 486.471 s |
| Total |          443/443 |   5,037/5,037 |              |                 |           |

The cached object phase compiles each shard's test objects and its new
fixture helpers. Measurements include concurrent sanitizer compilation.
The machine-readable receipts were saved as `receipts/release-{1,2,3,4}.json`
in the runner scratch directory.

| Receipt          | SHA-256                                                            |
| ---------------- | ------------------------------------------------------------------ |
| `release-1.json` | `2e89fa36541dfbceebe7e07446bda09a52e1e82527d50c94a868703c5562640b` |
| `release-2.json` | `03edd270d731db1d0b87c7e25c3b7a3f63fd09150c48ac8340a1b89d3a027c1a` |
| `release-3.json` | `9a9e9d1492a76b3dcf78a53c7d8373c4ac78441d0e37ec51a0ff8dfc3c4005cb` |
| `release-4.json` | `243690dbf4910c88987cc520ce2e4559d9cec17877808ed26781e623ee39a1c6` |

The HTTP wire binary passed all 107 comparisons. The IDNA fixture passed both
test cases with 1,260 curated vectors and 2,228,224 exhaustive scalar records
across 1,088 blocks.

## Sanitizer receipt

ASan/UBSan passed 105/105 binaries and 1,191/1,191 cases with zero failures,
build failures, timeouts or sanitizer findings. The driver compiled 369/369
production objects and exited 0. The incremental object phase took 2,423.845 s,
link and test took 71.828 s, and the total was 2,496.387 s. The receipt matches
the release commit, Git tree and input digest above.
The SHA-256 of `receipts/asan-final.json` was
`3ea7bbc80aeb1f08ca8487698b2db1d4847467fbbe4d2c818e359a46c0b2b4a9`.

The receipt audit verified that the release shards together cover the full
driver inventory exactly once and that every release and sanitizer selection
has the same input digest as before execution. Commits after the tested tree
only append this report. The three edited documents passed the repository's
`bun --no-env-file --no-install run format:check` from a scratch setup with
both frozen lockfiles. The first-party diff passed `git diff --check`, and
all pinned vendor bytes matched the accepted branches.

## Handoff

The clean local stage 1 branch is
`agent-jobs/cppgw-integrate-s1-20261118-44220704314-1`. GitHub publication
is pending because the push preflight could not authenticate to `origin`
and returned exit 128. The acceptance runs were on Linux ARM64. macOS and the
owner's historical SDK were unavailable on this runner.

Stage 1 preserves both accepted merges and is green on Linux ARM64 with
443/443 release binaries and 5,037/5,037 cases across four shards, plus
105/105 targeted ASan/UBSan binaries and 1,191/1,191 cases. First-party
runtime production source fell from 34,571 to 32,250 lines.
