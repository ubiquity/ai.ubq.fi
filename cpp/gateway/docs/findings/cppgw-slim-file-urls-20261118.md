# File URL conversion and trusted resource paths

## Work contract v1

Resolve packaged static assets and the offline codec helper from the configured resource root, with the existing working-directory fallback in fixtures. Resolve development startup inputs from the source file and permissions inputs from the executable. Preserve the external POSIX file URL checks, SQLite URI flags, startup checks, helper pins and coroutine lifetime rules.

The brief assigns the file URL adapter regions in `rt/url`, static resource resolution in `static.cpp`, and the path construction regions in `kv_conversion/legacy_codec.cpp`, `storage/native_addon.cpp`, `runtime/native_kv_bootstrap.cpp` and `runtime/permissions_linux.cpp`. Their tests and an appendix to `DEVIATIONS-FROM-BUN.md` are in scope. Provider behavior, stored-value codecs, broader URL parsing, deployment, and the TypeScript gateway are outside this work.

The starting checkout is `7bde4d44a00ed5322901a59e98f80a5f60bafa50` with tree `0ded9458c6cbf069d117bdfabeaaa946810ea6ab`. The incoming [Ada receipt](cppgw-std-ada-20261117.md) reported 441 release binaries and 5,025 passing cases. Its sanitizer run reached the same totals after three runner-specific retries. Local baseline results and the exact toolchain will be recorded below.

Acceptance uses `bash cpp/gateway/tools/ci-build.sh --config release` for the baseline and final tree, and `--config asan` for the final link closures. Every test links `src/rt/url.cpp`, so the final profiles run the full registry. The listed asset, conversion, and startup tests cover the changed callers. Reserved-byte path checks cover the remaining file URL boundary. The runner uses disposable fixture state, an empty home, and Linux network, mount, and PID namespaces. The work ends after reconciling the baseline and final receipts, scope, line counts, and branch handoff.

## Resource mapping

Before editing, I resolved all 12 preload strings and 41 static asset entries against the synthetic `src/static.ts` path. The 12 preloads and 39 text asset entries reached the same filenames under `static/`, including `static/docs/llms-agents.md`. The two binary asset entries reached `favicon.png` and `favicon-32.png` at the root. All paths pointed to tracked files.

| Consumer                 | Filesystem location                                                         |
| ------------------------ | --------------------------------------------------------------------------- |
| Static files             | `resourceRoot()/static/<filename>` and the two root favicons                |
| Legacy codec             | `resourceRoot()/src/kv-conversion/v8-helper.mjs`                            |
| Default native KV inputs | `tools/native-kv/inputs.json` in the checkout containing `native_addon.cpp` |
| Development bootstrap    | The checkout containing `native_kv_bootstrap.cpp`                           |
| Development permissions  | `.uos-permissions/{manifest.json,linux-arm64.node}` beside the executable   |

The starting filesystem module exposes `rt::path::resolve` for lexical absolute paths. The starting Ada adapter already uses it, `rt::isUtf8`, and Ada's percent codec. Ada decodes only complete hexadecimal escapes, and the UTF-8 check runs over the decoded bytes. The URL adapter retains both file conversion entry points and the `PathLike` overload contract.

## Refactor and line counts

The static module holds native paths for both its preloaded text and its on-demand assets. The codec child receives the helper path in both invocations. The source-relative native KV defaults use the shared lexical resolver, and Linux confinement joins the names beside the executable. Explicit URL bindings and SQLite URIs continue through the POSIX adapter. The encoded-slash guard checks both case spellings before Ada decodes the pathname.

Counts are physical lines including comments and blank lines. The production denominator is the tracked `.cpp`, `.hpp`, and `.h` files in `src`, `include`, `runtime`, and `entrypoints` under `cpp/gateway`.

| File                                       |  Before |   After | Removed |
| ------------------------------------------ | ------: | ------: | ------: |
| `src/kv_conversion/legacy_codec.cpp`       |     211 |     206 |       5 |
| `src/rt/url.cpp`                           |     463 |     461 |       2 |
| `src/runtime/native_kv_bootstrap.cpp`      |     150 |     143 |       7 |
| `src/runtime/permissions_linux.cpp`        |     318 |     317 |       1 |
| `src/static.cpp`                           |     399 |     384 |      15 |
| `src/storage/native_addon.cpp`             |     212 |     205 |       7 |
| Six changed files                          |   1,753 |   1,716 |      37 |
| All 745 production source and header files | 190,126 | 190,089 |      37 |

## Verification setup

The runner is Ubuntu 24.04.5 on Linux `7.0.0-1017-azure` AArch64. Clang and LLD 21.1.8 came from the signed apt.llvm.org Noble repository (`llvm-toolchain-noble-21`). The installed native dependencies were OpenSSL 3.0.13, zlib 1.3, ICU 74.2, Brotli 1.1.0 and Zstd 1.5.5. The source builds used GCC 14.2.0 for Boost JSON and Container and the documented `SQLITE_DQS=3` for SQLite. Using a static SQLite archive keeps linkage independent of the test namespace's private `/tmp`.

```bash
job=/tmp/cppgw-slim-file-urls-20261118
sudo apt-get update
sudo apt-get install -y g++-14 ninja-build pkg-config libssl-dev zlib1g-dev \
  libicu-dev libbrotli-dev libzstd-dev util-linux mold
sudo apt-get install -y clang-21 lld-21 libclang-rt-21-dev
curl -fLsS https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2 -o "$job/prep/boost_1_92_0.tar.bz2"
printf '%s  %s\n' 5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c \
  "$job/prep/boost_1_92_0.tar.bz2" | sha256sum --check
tar -xjf "$job/prep/boost_1_92_0.tar.bz2" -C "$job"
printf '%s\n' 'using gcc : 14 : /usr/bin/g++-14 ;' > "$job/prep/boost-user-config.jam"
(
  cd "$job/boost_1_92_0"
  bash ./bootstrap.sh --with-libraries=json,container
  ./b2 --user-config="$job/prep/boost-user-config.jam" toolset=gcc-14 \
    --with-json --with-container link=static threading=multi variant=release -j3 stage
)
curl -fLsS https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz -o "$job/prep/sqlite-autoconf-3540000.tar.gz"
printf '%s  %s\n' 134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed \
  "$job/prep/sqlite-autoconf-3540000.tar.gz" | sha256sum --check
tar -xzf "$job/prep/sqlite-autoconf-3540000.tar.gz" -C "$job"
(
  cd "$job/sqlite-autoconf-3540000"
  ./configure --prefix="$job/sqlite" --disable-shared --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
  make -j1
  make install
)
mkdir -p "$job/pkgconfig"
sed "s|^Libs:.*|Libs: $job/sqlite/lib/libsqlite3.a -lm -lz|" \
  "$job/sqlite/lib/pkgconfig/sqlite3.pc" > "$job/pkgconfig/sqlite3.pc"
```

The two clean profiles share the four cores. This baseline is a cold build. The final run reuses unaffected objects from the same absolute path. Progress is visible in the driver's compile log and incremental `results.tsv`, and each profile writes a terminal `receipt.json`. The earlier sanitizer run needed more than 300 seconds for `sentinel_replay_retention_metadata_test`, so the timeout here is 720 seconds for both profiles. Clang's simdutf header prefix applies existing outside-lane signedness recovery only to that vendored header. First-party warnings remain fatal.

```bash
git worktree add --detach '../file-url qualification %?#' 7bde4d44a00ed5322901a59e98f80a5f60bafa50

run_ci() {
  local profile=$1
  shift
  local flags="-isystem $job/boost_1_92_0 --system-header-prefix=third_party/simdutf/ -ffunction-sections -fdata-sections -pthread"
  if [[ "$profile" == asan ]]; then
    flags="$flags -gline-tables-only -fno-sanitize-recover=all"
  fi
  local scratch
  scratch=$(mktemp -d "$job/ci-env.XXXXXXXX")
  mkdir "$scratch/home" "$scratch/tmp"
  env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 TZ=UTC HOME="$scratch/home" TMPDIR="$scratch/tmp" \
    CXX=/usr/bin/clang++-21 PKG_CONFIG_PATH="$job/pkgconfig" UOS_CI_BUILD_DIR="$job/ci" \
    UOS_CI_JOBS=2 UOS_CI_TIMEOUT=720 UOS_CI_CXXFLAGS="$flags" \
    UOS_CI_LDFLAGS="-L$job/boost_1_92_0/stage/lib --ld-path=/usr/bin/ld.lld-21 -Wl,--threads=1,--gc-sections" \
    bash cpp/gateway/tools/ci-build.sh --config "$profile" "$@"
}

cd '../file-url qualification %?#'
run_ci release
run_ci asan
```

Both baseline profiles entered their own private network, mount, and PID namespaces via `sudo -n`. The fixture helper verified loopback communication and outside route denial before running each binary. The driver runs with an empty fixture `HOME`, a fresh `TMPDIR`, and an allowlisted environment with the provider settings absent.
