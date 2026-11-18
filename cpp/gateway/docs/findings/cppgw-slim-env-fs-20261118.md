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

`git grep -nE '\\b(loadEnvFile|parseEnvFile)\\b' -- cpp/gateway` found only
the declarations and implementation, the ordinary env fixture, the existing
env fuzz fixture and documentation or metadata. The original snapshot and
lookups retain their implementation. Enumeration uses a stable sort and the
whole-view fixture uses a scoped swap. The
runtime fixture keeps all 124 original dotenv inputs and exercises explicit
OS precedence, present-but-empty values and nested restoration. The existing
fuzz target exercises snapshot and override restoration with its arbitrary
input bytes. Its descriptive `entry` field still names the retired parser
in the out-of-scope fuzz registry. Neither fuzz driver reads that field.

Environment production files went from **281 to 185 physical lines**
(**96 removed**).

The first implementation extended outside the assigned snapshot and lookup
regions. The line-range audit restored those regions from the supplied base
and the committed code stays within the brief. The earlier focused and
partial-build logs are retained as superseded evidence.

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

| Archive | SHA-256 |
| ------- | ------- |
| `boost-1.92.0-b2-nodocs.tar.xz` | `ea7b982002cc9dfbe59b0b217b206f470dc75f3de0bb2973d844118934d82411` |
| `sqlite-autoconf-3540000.tar.gz` | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |
| `icu4c-78.3-sources.tgz` | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |

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
`bun --no-env-file run format:check` in an external scratch copy.

## Verification in progress

The full test registry contains 441 tests. Every test links both edited
runtime source files. Qualification runs the twelve named release gates, the
unsharded release suite and the unsharded Clang ASan/UBSan suite. The release
and sanitizer drivers use two and four compiler jobs, respectively. Each run writes
its compile log, incremental test counts and exact-tree receipt to scratch.
The cold suites are expected to take roughly 30 to 60 minutes on the four
runner CPUs. Keep exact diagnostics for failed controls, retry recoverable
runner failures, and leave findings owned by other lanes on their existing
branches.
