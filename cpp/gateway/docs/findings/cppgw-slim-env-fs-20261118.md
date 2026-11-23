# Slim C++ gateway environment and filesystem helpers

## Work contract v1

Base commit `6393434b110e2d1afe93e6c7005848219336d9b9`, tree
`89b42863ac7dd6a07610e2d15aa320efc42547af`. Retire the fixture-only dotenv
entrypoints first, then consolidate filesystem text conversion and lexical
path operations. Each brief gets a separate buildable commit.

| Surface       | Required behavior                                                                                            |
| ------------- | ------------------------------------------------------------------------------------------------------------ |
| Environment   | First-use OS snapshot, explicit empty values, enumeration, nested overrides, and inherited child environment |
| File text I/O | Shared UTF-8 codecs, retained BOM and NUL, replacement of malformed input, and existing descriptor offsets   |
| Lexical paths | POSIX byte handling, distinct join and resolve semantics, and containment checks at the existing callers     |

Owned paths are the assigned regions of `src/rt/env.cpp`, `include/uos/rt/env.hpp`,
`src/rt/fs.cpp` and `include/uos/rt/fs.hpp`, their runtime fixtures, an appendix
in `docs/DEVIATIONS-FROM-BUN.md` and this report. Deployment, credentials,
provider traffic, descriptor security, TLS, subprocess construction, and other
lanes' files are outside the contract. Gateway wire formats, accounting, stored
values, authentication and validation decisions are invariants.

Use `bash cpp/gateway/tools/ci-build.sh --config release` for the named
fixtures and the whole registry, and `--config asan` for every test linking an
edited runtime object. Each test runs in a credential-free loopback namespace
with isolated home and temporary directories. Record exact receipts and
physical production line counts. The runner is Linux ARM64. Apple Silicon
execution depends on a matching host and its qualified fixture boundary.

## Progress

### Environment

The gateway and both offline CLIs require explicit manifests. The draft
native unit uses a systemd `EnvironmentFile` and its launcher passes the
resulting OS environment to the executable. The deployed Bun launcher owns
its explicit repository-root dotenv policy. TLS and child environment
construction are outside the edited closure.

`git grep -nE '\b(loadEnvFile|parseEnvFile)\b' -- cpp/gateway` found only
the declarations and implementation, the ordinary env fixture, the existing
env fuzz fixture and documentation or metadata. The original snapshot and
lookups retain their implementation. Enumeration uses a stable sort and the
whole-view fixture uses a scoped swap. The runtime fixture keeps all 124
original dotenv inputs and exercises explicit OS precedence, present-but-empty
values and nested restoration. The existing
fuzz target exercises snapshot and override restoration with its arbitrary
input bytes. Its descriptive `entry` field still names the retired parser
in the out-of-scope fuzz registry. Neither fuzz driver reads that field.

Environment production files went from **281 to 185 physical lines**
(**96 removed**).

The first implementation extended outside the assigned snapshot and lookup
regions. The line-range audit restored those regions from the supplied base
and the committed code stays within the brief. The earlier focused and
partial-build logs are retained as superseded evidence. The final env fuzz
target passed 10,000 inputs with Clang ASan, UBSan and LeakSanitizer.

### Filesystem and paths

The supplied base already had the codec switch and initial standard path
migration from `6384cf2d`. This brief removes the remaining `getcwd` buffer
loop, uses native path objects for resolution and relative comparisons, and
exposes one overload set each for joining and resolving. Single-segment join
retains normalization coverage. Test-only filename and extension adapters
give way to direct standard path operations in the original vectors and fuzz
fixture. The fixture retains the suffix expectations. The new cases cover
literal percent and backslash bytes, sibling-prefix escapes, deleted cwd,
exact text bytes from both read overloads, descriptor offsets and encoded
surrogate pairs and lone surrogates.

Both filesystem containment callers were traced. The Linux permissions
caller canonicalizes before its overlap comparison. Native KV rejects an
equal path, parent paths and absolute results before validating every
directory and file on the remaining path. The existing native addon
fixtures include outside-directory, parent traversal and symlink cases.

Filesystem production files went from **1,025 to 987 physical lines**
(**38 removed**). The text I/O change accounts for 2 lines and the lexical
adapters account for 36. The earlier 257-line removal is outside this diff.
The combined reduction from the assigned base is **134 production lines**.

The final lexical probe passed 370/370 assertions on Linux with GCC 14.2
and libstdc++, and again with Clang 21.1.8 and libc++. The baseline and
refactor produced the same 4,181 serialized path results for the generated
37-input corpus. Its result SHA-256 is
`2b4b7d8a7bcc1de37c35497e50a17401793de9d525a082ac894c43c6b8cdae74`.
The existing path fuzz target passed 10,000 inputs under ASan, UBSan and
LeakSanitizer with seed `18112026` and maximum length 16,384.

## Commands and dependencies

The runner was Ubuntu 24.04.5 on Linux AArch64 with four CPUs. GCC 14.2.0
ran release and Clang 21.1.8 ran address and undefined-behavior sanitizers.
Mold 2.30.0 linked the tests. The installed OpenSSL, zlib, Brotli and Zstd
versions were 3.0.13, 1.3, 1.1.0 and 1.5.5. Boost, SQLite and ICU were
built from verified official archives outside the checkout.

| Archive                          | SHA-256                                                            |
| -------------------------------- | ------------------------------------------------------------------ |
| `boost-1.92.0-b2-nodocs.tar.xz`  | `ea7b982002cc9dfbe59b0b217b206f470dc75f3de0bb2973d844118934d82411` |
| `sqlite-autoconf-3540000.tar.gz` | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |
| `icu4c-78.3-sources.tgz`         | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |

`/tmp/cppgw-slim-env-fs.6AJbTT` held the dependencies, the `gate` wrapper,
build objects, evidence and probe sources. The preparation commands were:

```bash
sudo -n apt-get -y install clang-21 libclang-rt-21-dev libssl-dev zlib1g-dev \
  libicu-dev libbrotli-dev libzstd-dev util-linux mold
sudo -n apt-get -y install libc++-21-dev libc++abi-21-dev

# In the verified Boost source directory
./bootstrap.sh --with-toolset=gcc --with-libraries=json,container
./b2 -j2 toolset=gcc variant=release link=static threading=multi \
  --with-json --with-container stage

# In the verified SQLite source directory
./configure --prefix="$scratch/sqlite/install" --disable-shared --disable-readline \
  CFLAGS='-O2 -DSQLITE_DQS=3'
make -j2
make install

# In the verified ICU source directory
./configure --prefix="$scratch/icu/install" --enable-static --disable-shared \
  --disable-samples --disable-tests
make -j4
make install
```

`gate` runs `env -i` with fresh `HOME` and `TMPDIR`, sets `TZ=UTC`, and selects
the SQLite and ICU `pkg-config` paths. It supplies `-isystem` for Boost,
`-L` for its static archives, and an external `UOS_CI_BUILD_DIR`. The Clang
configuration adds `--system-header-prefix=third_party/`,
`-gline-tables-only` and `-fno-sanitize-recover=all` to the driver's sanitizer
flags. The driver then proves the per-binary network boundary and supplies
private mount and PID namespaces to each test.

```bash
scratch=/tmp/cppgw-slim-env-fs.6AJbTT
for test in tests/rt/env_test.cpp tests/rt/process_test.cpp \
  tests/runtime_config_differential_test.cpp tests/jev_compaction_smoke_test.cpp; do
  bash "$scratch/gate" release 2 "$test"
done
for test in tests/rt/fs_test.cpp tests/rt/text_codec_test.cpp \
  tests/static_differential_test.cpp tests/native_addon_differential_test.cpp \
  tests/sqlite_readonly_test.cpp tests/kv_conversion_convert_differential_test.cpp \
  tests/permissions_linux_test.cpp tests/runtime_native_startup_test.cpp; do
  bash "$scratch/gate" release 2 "$test"
done
bash "$scratch/gate" release 2
bash "$scratch/gate" asan 4
```

The wrapper ends in `bash cpp/gateway/tools/ci-build.sh --config "$profile" "$@"`.
The named gates map the env helper to process, runtime config and compaction,
and the filesystem helper to text codecs, static files, bindings, SQLite,
conversion and confinement. The remaining closure is the whole test registry.
The two updated fuzz harnesses were built with `clang++-21 -std=c++20 -O1
-gline-tables-only -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all`
and the strict warning flags. Runs used `-runs=10000 -seed=18112026
-max_len=16384` under the Linux namespace and cleared-environment boundary.
The report and appended deviations used the frozen Prettier dependency via
`bun --no-env-file run format:check` in an external scratch copy. The whole
deviations file and its supplied base both have pre-existing Prettier differences.
Formatting both scratch copies confirmed that all differences precede the appendix.

## Named release gates

All 12 named binaries passed all 96 cases on Linux ARM64. The receipts bind
commit `af9d96832c48d938b1f46a87e46bac6ae27e5df3`, tree
`ac162da5c72a31608df627c753926651a6de056d` and stable inputs.

| Test                                      | Cases passed | Wall seconds |
| ----------------------------------------- | -----------: | -----------: |
| `rt/env_test`                             |          7/7 |      217.443 |
| `rt/process_test`                         |        10/10 |        9.753 |
| `runtime_config_differential_test`        |          8/8 |        1.417 |
| `jev_compaction_smoke_test`               |          6/6 |       17.081 |
| `rt/fs_test`                              |        13/13 |       15.392 |
| `rt/text_codec_test`                      |          6/6 |        1.255 |
| `static_differential_test`                |          1/1 |        6.342 |
| `native_addon_differential_test`          |          4/4 |        7.529 |
| `sqlite_readonly_test`                    |          5/5 |        6.816 |
| `kv_conversion_convert_differential_test` |        13/13 |        6.700 |
| `permissions_linux_test`                  |          5/5 |        6.799 |
| `runtime_native_startup_test`             |        18/18 |        7.264 |

## Complete ARM64 runs

Every binary in the 441-test registry links both edited runtime objects.
Both unsharded runs used the tested commit and tree above and compiled all
369 production objects, including 44 runtime objects. The source input
SHA-256 was `959474754fdcb4c94b4693fcccc19c78fc2d39a42cb497250440826e39ef30bb`
in both receipts, before and after execution. Each binary emitted one
confirmed fixture-network receipt. The sanitizer logs contained no ASan,
UBSan or LeakSanitizer diagnostics.

| Profile          | Binaries passed | Cases passed | Build failures | Timeouts | Object seconds | Link and test seconds | Total seconds | Exit |
| ---------------- | --------------: | -----------: | -------------: | -------: | -------------: | --------------------: | ------------: | ---: |
| GCC release      |         438/441 |  5,027/5,030 |              0 |        0 |       2549.340 |               139.237 |      2689.300 |    1 |
| Clang ASan/UBSan |         438/441 |  5,027/5,030 |              0 |        0 |       2730.013 |               230.556 |      2961.092 |    1 |

The profiles ran concurrently and reused previously compiled objects.
Their aggregate wall times describe incremental runs. The full receipts
and individual diagnostics are in the external evidence directory.

| Receipt                     | SHA-256                                                            |
| --------------------------- | ------------------------------------------------------------------ |
| `evidence/release-all.json` | `eb6bfab88b30e4bb21645e35b140182dbc6598f7af029e319c0dddec73020d44` |
| `evidence/asan-all.json`    | `5299d57be46eaa3b6bf098c2fdbc430099239118acaa090779397700be1c3a40` |

## Baseline findings and handoff

Each full run exited 1 on the same three cases documented for the supplied
base. Each row accounts for one failed case in both profiles.
[Prior small-helper qualification](cppgw-std-small-helpers-20261117.md#combined-build-and-regression-evidence)

| Fixture                                 | Current diagnostic                                                                                                                             |
| --------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| `claude_code_usage_schema_test`         | The legacy string `"0"` leaves the normalized deadline null. The Bun vector expects `946684800000`.                                            |
| `codex_external_auth_differential_test` | The legacy refresh strings `"Nov 16 2026"`, `" 2026-11-16 "` and `"1"` are rejected in one differential case.                                  |
| `rt/http_test`                          | Two malformed redirects throw `TypeError` with `location` and an omitted rejected value. The vector expects `Location` and the rejected bytes. |

The prior ICU 74.2 IDNA discrepancies did not reproduce with ICU 78.3.
The IDNA binary passed both cases in each profile. The three date and HTTP
cases remain with the integration job. The platform guide and the env fuzz
registry's descriptive `entry` field still name the retired dotenv API.
Apple Silicon execution awaits a matching host and the qualified native
fixture procedure. [Mac qualification](../MACOS.md)

The environment and filesystem commits are `a6d30b146aa6755225906a3ce346b00acec7884b`
and `af9d96832c48d938b1f46a87e46bac6ae27e5df3` on the local `agent-job` branch.
`git fetch --no-tags origin refs/heads/development:refs/remotes/origin/development`
returned `2ac5aae69b0f0bac77cf1c6f329995cf1187616c`.
`GIT_TERMINAL_PROMPT=0 git push --dry-run origin HEAD:refs/heads/agent-jobs/cppgw-slim-env-fs-20261118`
failed because GitHub required authentication. Publication, pull request
integration and the `development` checkout handoff remain pending.

## Summary

Removed 134 first-party production lines across the two briefs. Environment
went from 281 to 185 lines and filesystem/path from 1,025 to 987. All 12
named ARM64 release fixtures passed all 96 cases. Each full ARM64 profile
passed 438 of 441 binaries and 5,027 of 5,030 cases, with the same three
documented baseline failures. GitHub authentication blocked publication.
Apple Silicon qualification remains pending.
