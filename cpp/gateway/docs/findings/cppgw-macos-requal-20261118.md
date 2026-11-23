# Apple Silicon requalification after simdjson, simdutf and ICU

## Work contract v1

Requalify the C++ gateway on arm64 macOS from source commit
`b54357a92c0f2669335d83f04047c3f7de32f925` (tree `d0aabad25d3dbe02f2f33ff519e5b77715d19d37`).
Use Apple Clang and libc++, the registry-selected vendor sources and Homebrew ICU 78, the Linux major.
The native SQLite fixtures require the separately prepared 3.54.0 library.

| Surface                     | Acceptance                                                                                                                                                                                                        |
| --------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Production and vendor build | Every production translation unit compiles with the port warning set, vendor sources compile on their own warning flags, and production links resolve the selected archives and ICU.                              |
| Runtime and application     | Run the runtime suite first, then every registered binary in the full suite. Record real harness totals and first diagnostic lines for any failure.                                                               |
| Network and platform        | Seatbelt permits loopback fixtures and denies external networking. Test environments have private empty homes and fresh temp directories. Linux contracts and security decisions retain their existing semantics. |
| Documented startup          | Measure the gateway's idle RSS if the production launcher admits Darwin. Record its exact blocker otherwise.                                                                                                      |

Owned paths are `tools/macos-build.sh`, `docs/MACOS.md`, this report, Darwin branches in
`include/`, `src/` and `tests/`, and standard-library portability changes with identical Linux behavior.
Production activation, provider calls, credentials, live state, deployment, shared Linux build
tools, workflows, vendor source changes, other agents' paths and application TypeScript are outside
this unit. Use the native compile and suite gates plus script syntax, changed Markdown formatting
and `git diff --check`.

The runner reports macOS 26.7.2 build 25G332 on arm64, Apple Clang 21.0.0
(`clang-2100.1.1.101`), 3 CPUs and 7 GiB of RAM. The starting script already knows the vendor and
ICU metadata. The first compile will establish whether current sources and dependencies still
match that wiring. The documented launcher has Linux-native requirements to verify from source
and the executable. Initial build and test budget is 30 minutes, using three compile slots and
one build handle at a time. Object-count progress and external logs identify the active phase.
Retain diagnostics and use incremental reruns for owned repairs. Stop local qualification after
the clean full result and any exact remaining failures have been recorded.

## Progress

Read the current CI, build, runtime, OSS replacement, porting and macOS instructions. Starting
from a clean checkout. Build outputs, logs and dependency downloads are external to the checkout.

The first native invocation at `3ea476b30eda57a7607aef841368fd3ecddf4e44` was stopped
after Apple Clang diagnosed three signedness errors inside Homebrew ICU 78.3 headers reached
through `src/rt/string.cpp:10`. It reached no tests. The compiler reported the following error first.

```text
/opt/homebrew/opt/icu4c@78/include/unicode/uset.h:1498:49: error: implicit conversion changes signedness: 'int' to 'size_t' (aka 'unsigned long') [-Werror,-Wsign-conversion]
```

The other two were in `unicode/unistr.h:358`. The Darwin driver now adds native dependency
headers as system includes. The port sources retain fatal conversion warnings.

## Dependency preparation and checks

The host uses Darwin 25.6.0, Xcode 26.6, macOS SDK 26.5 and libc++. Homebrew supplied Boost
1.92.0_1, OpenSSL 3.5.9, zlib 1.4, Brotli 1.2.1, Zstd 1.5.7 and `icu4c@78` 78.3_1 with ICU 78.3.
Homebrew's unversioned ICU points to 79.1. The gateway's Mach-O imports resolve the ICU 78.3
`libicuuc.78.dylib` and `libicui18n.78.dylib` under the `icu4c@78` prefix.

The build used the existing `-O0 -g0` macOS profile and three build workers. Commands ran from the
repository root. Package preparation used a sparse environment and a cache in the external job
directory.

```bash
UOS_MAC_BUILD=/tmp/cppgw-macos-requal.H8Fbf5
uos_path=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin
env -i PATH="$uos_path" HOME="$UOS_MAC_BUILD/prep-home" TMPDIR="$UOS_MAC_BUILD/prep-tmp" \
  HOMEBREW_CACHE="$UOS_MAC_BUILD/brew-cache" HOMEBREW_NO_AUTO_UPDATE=1 \
  HOMEBREW_NO_ANALYTICS=1 HOMEBREW_NO_INSTALL_CLEANUP=1 brew install boost zlib icu4c@78
curl --fail --silent --show-error --location \
  https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip -o "$UOS_MAC_BUILD/sqlite.zip"
shasum -a 256 "$UOS_MAC_BUILD/sqlite.zip"
bash cpp/gateway/tools/ci-build.sh --list
bash -n cpp/gateway/tools/macos-build.sh
```

The SQLite ZIP SHA-256 was
`68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d`. The recipe verified the hash
before extraction, copied `sqlite3.h` to the prepared prefix, and built the official 3.54.0 dylib
with Apple Clang. The dylib SHA-256 was `5c6850f0c08474a3aab9d8007d1039c7ae8168411ee7d2eba1b6a5a5d3ea5887`.
[SQLite preparation commands](../MACOS.md#prepare-a-clean-development-machine)

```bash
sqlite_prefix="$UOS_MAC_BUILD/sqlite-3.54.0"
xcrun clang -O2 -dynamiclib -DSQLITE_THREADSAFE=1 -DSQLITE_DQS=3 \
  -DSQLITE_ENABLE_COLUMN_METADATA=1 "-Wl,-install_name,$sqlite_prefix/lib/libsqlite3.dylib" \
  "$UOS_MAC_BUILD/sqlite-amalgamation-3540000/sqlite3.c" -o "$sqlite_prefix/lib/libsqlite3.dylib"
set -euo pipefail
for suite in rt all; do
  mkdir "$UOS_MAC_BUILD/$suite-home" "$UOS_MAC_BUILD/$suite-tmp"
  env -i PATH="$uos_path" HOME="$UOS_MAC_BUILD/$suite-home" TMPDIR="$UOS_MAC_BUILD/$suite-tmp" \
    /usr/bin/time -l bash cpp/gateway/tools/macos-build.sh --build-root "$UOS_MAC_BUILD/native" \
    --sqlite-prefix "$sqlite_prefix" --jobs 3 --suite "$suite" --require-clean \
    >"$UOS_MAC_BUILD/$suite.log" 2>&1
done
```

Each binary used a private HOME, TMPDIR and working directory with a sparse environment. Both
Seatbelt preflights reported `loopback accepted; external network denied`. The C++ test registry
listed 442 binaries. The macOS driver uses system include paths for the installed libraries and
limits the port warning set to first-party code. It records both vendor archives and all three
production executable links. The diff covers the driver and these two qualification documents.

## Verification receipts

Both runs exited 0 from clean commit `b05ecda8d3727b56b791a363ec344ba2f1093230`
and tree `c1c4aa854fb7488c05aa80d7b589bc776fc717f1`. Post-run commit, tree and input hashes
matched each run's initial values. The full run executed each of the 442 registered binaries once.

| Gate                                                   | Passed                                                                            | Measured wall time        |
| ------------------------------------------------------ | --------------------------------------------------------------------------------- | ------------------------- |
| Production compile and link                            | 373/373 production translation units, 2/2 vendor sources and 3/3 executable links | Included in both commands |
| Standalone runtime gate, cold object cache             | 36/36 binaries, 458/458 cases                                                     | 652.31 s                  |
| Full gate, reusing production and runtime test objects | 442/442 binaries, 5,034/5,034 cases                                               | 1,206.90 s                |
| Application phase of the full gate                     | 406/406 binaries, 4,576/4,576 cases                                               | Included in the full gate |

All 464 test and support translation units compiled. The production archive has 369 members
and occupies 680,759,040 bytes. The gateway executable is 155,948,072 bytes and linked in 1.769 s
on the full run. The two offline CLI executables are 155,947,584 bytes each. `time -l` reported
maximum resident set sizes of 1,289,830,400 bytes for the cold runtime driver command and
1,445,593,088 bytes for the full driver command, including the build tools.

The runtime-only receipt is in `native/runs/1794965009363943000`. The full receipt is in
`native/runs/1794965661617890000`, relative to `$UOS_MAC_BUILD`.

| Full-run artifact                          | SHA-256                                                            |
| ------------------------------------------ | ------------------------------------------------------------------ |
| `provenance.json`                          | `8287081130667f179e79479fe5d3a47de44fdbf1d24f1e49edf5083910959755` |
| `results.json`                             | `07a1e37a784231610d259bc0c219506262f38807d7b05ff1c1f2398b78b89ed4` |
| `libuos_gateway.a`                         | `1e3290826b63b176f9c24116dc6e0e156434b51d374aea794ea04a211bd67590` |
| `libuos_vendor_simdjson.a` (350,736 bytes) | `d6d83d5ae38bd659e09f7e002c988b1c0d254b8d76f2ee65f07cbdb30abdc514` |
| `libuos_vendor_simdutf.a` (623,144 bytes)  | `1692b0a0fac7c4dfab171f0b19c8c9753870cdee7dfdfe55e9bfe3c08ef38210` |
| `uos-gateway`                              | `8e3f9d04fc846d3850ff2df568e46dd02c1601ac44867baeb9f7b2c7f4c261f1` |
| `uos-kv-convert`                           | `eb8c46b221707d670e419427546e30fa97c806ccb2ed3e3dc43415644da0e1ec` |
| `uos-kv-proof`                             | `b4a82a81055d3bb9a4c89e1c58df9282ad4e72abd5e368668aef7562294a786d` |

The compiler receipts show the full port warning set on `json.cpp`, `base64.cpp` and `string.cpp`,
and no port warning flags on the two vendor translation units. The JSON consumer uses
`UOS_GATEWAY_SIMDJSON`. JSON passed **25/25** harness cases and base64 passed **6/6**.
ICU's shared case-mapping and collation fixtures passed **19/19** string cases and **3/3** legacy
cases. Both IDNA cases passed, covering 1,260 curated vectors and 2,228,224 code point and host
context records across 1,088 blocks. The same Unicode expectations passed on Linux ICU 78.3.
The Linux run's aggregate of 5,027 cases includes its own platform fixture selection.
[Linux ICU verification](cppgw-std-icu-20261117.md#verification)

## Documented launch

`file` identified all three production executables as Mach-O 64-bit arm64. `otool -L` confirmed
libc++, the prepared SQLite dylib and both ICU 78.3 libraries in the gateway. The documented
`uos-gateway --runtime-manifest <absolute path>` was exercised with a synthetic manifest, a
private empty state directory and a hash-bound fixture containing the three Mach-O binaries. The
runtime closure described the actual arm64 Darwin platform.
`$UOS_MAC_BUILD/launch-check.log` records the command and result.

```sh
/usr/bin/sandbox-exec -p '(version 1)(allow default)(deny network*)(allow network-inbound (local ip "localhost:*"))(allow network-outbound (remote ip "localhost:*"))(allow network-bind (local ip "localhost:*"))(allow network* (local unix-socket) (remote unix-socket))' \
  /private/tmp/cppgw-macos-requal.H8Fbf5/launch-check.ggyns2vv/package/bin/uos-gateway \
  --runtime-manifest /private/tmp/cppgw-macos-requal.H8Fbf5/launch-check.ggyns2vv/launch.json
```

```text
error: Native startup refused: runtime closure platform is unsupported
exit=1, state_entries=0
```

The Linux ARM64 runtime closure gate executes before state is opened or a listener is created.
The documented launcher also requires Linux ELF modules and real Linux confinement. A Darwin
launcher and idle gateway RSS remain open. [Runtime closure gate](../../runtime/startup.cpp#L188)

## Handoff

The change-scoped gates are the native commands above, changed Markdown formatting with
`bun --no-env-file run format:check` and `git diff --check`. Formatting uses Bun 1.4.0 and the
repository's frozen root and lint locks in external scratch. The final Markdown commit leaves all
driver input hashes from the passing run intact.

The task starts from an ICU branch whose base is outside `origin/development`. At fetched
`6bd78fc5e3b0783809691c6bd45d25ac45e9696e`, `development` lacks `cpp/gateway`. The runner
also lacks GitHub write authentication. The branch push dry-run returned
`fatal: could not read Username for 'https://github.com': terminal prompts disabled`.
The commits remain on the clean `agent-job` checkout for the branch integration owner. Local Codex
review would require a provider connection, which the job excludes. Verification ran on the disposable Mac.

## Summary

- Apple Silicon compiled 373/373 production sources and linked 3/3 executables. The full suite passed 442/442 binaries and 5,034/5,034 cases.
- Homebrew ICU 78.3 matched Linux ICU 78.3 on the shared Unicode fixtures.
- The Linux-only launcher blocked idle RSS measurement. GitHub write authentication was unavailable, so the commits remain local.
