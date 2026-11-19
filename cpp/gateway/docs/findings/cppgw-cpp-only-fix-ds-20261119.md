# C++-only cleanup repair: fuzz build and packaged release identity

Job: `cppgw-cpp-only-fix-ds-20261119`. Branch: `agent-jobs/cppgw-cpp-only-20261119-44610457899-1`.
Start commit: `f92b14a97818ff81bf95cbfb2642e8ff13b4ab13`. Comparison base: `ca045a83ebaab6c50c20567d1a825638abac0891`.

## What was done

### Deleted-path audit

`git diff --diff-filter=D --name-only ca045a83ebaab6c50c20567d1a825638abac0891 HEAD` reported 56 deleted paths.
I searched the tracked tree for each full path, each `cpp/gateway`-relative path and each distinctive basename, excluding
prose in findings reports. The stale references that were consumed by build, test, fuzz or package tooling were:

1. `cpp/gateway/fuzz/targets.json`: both `passkeys_cbor_library` and `passkeys_asn1_library` still listed
   `src/storage/native_addon.cpp` in `extra_sources`. Removed both entries; the remaining storage closure is
   `src/storage/kv.cpp` plus `src/storage/native_kv.cpp` and `src/storage/sqlite_readonly.cpp`.
2. `cpp/gateway/bench/request_path/build.py`: the benchmark builder still tried to compile and link
   `entrypoints/kv-convert.cpp` and `entrypoints/kv-proof.cpp`. It now builds only `entrypoints/gateway.cpp`.

Verified clean: `cpp/gateway/fuzz/ci.py`, `cpp/gateway/fuzz/seeds.json`, `cpp/gateway/fuzz/seed.py`,
`cpp/gateway/tools/vendor_sources.py`, `cpp/gateway/tools/links.py` and `cpp/gateway/tests/LINKS.txt` data,
`cpp/gateway/tools/cmake-sources.py`, `cpp/gateway/PORT-MANIFEST.json`, `cpp/gateway/PORTED.txt`,
`cpp/gateway/cmake/**`, `cpp/gateway/CMakeLists.txt`, `cpp/gateway/packaging/**`,
`.github/workflows/cpp-gateway-ci.yml`, `.github/workflows/cpp-gateway-release.yml` and `cpp/gateway/.clang-tidy`.
All 19 fuzz targets in `cmake/extra-targets.json` and every source in `tests/LINKS.txt` resolve to an existing file.
`tools/cmake-sources.py` emits 322 module sources and 44 `src/rt` sources, 366 unique files, with no deleted path.

### Packaged release identity

`src/release.cpp` previously always defined `RELEASE_GIT_SHA = "unknown"`, and `health.cpp` exposes it as
`release.git_sha`. The portable packager already bound the full commit and tree in `VERSION` and
`build-profile.json`, but never compiled it into the executable. The macOS packager bound the commit only into the
`--version` wrapper, not into `release.cpp`.

- `src/release.cpp` now uses the compile definition `UOS_PACKAGE_GIT_COMMIT` and keeps `"unknown"` only when a
  source-tree test/development build does not define it.
- `CMakeLists.txt` accepts the validated `UOS_GATEWAY_GIT_COMMIT` cache value and propagates
  `UOS_PACKAGE_GIT_COMMIT` to the gateway objects.
- `packaging/portable/Dockerfile` takes the already-passed `SOURCE_COMMIT`, validates that it is 40 lowercase hex
  characters and passes it to CMake. `packaging/portable/build.sh` now fails with exit 64 outside a Git checkout or
  without a full lowercase `HEAD` commit.
- `packaging/portable/payload.py` refuses an unbound or malformed commit/tree before writing `VERSION`.
- `packaging/portable/proof.py` reads `build-profile.json`, requires a full commit, and checks both
  `release.git_sha` and `x-uos-git-sha` equal that commit and are never `unknown`.
- `tools/macos-build.sh` accepts `--release-commit` (validated full lowercase SHA) and compiles
  `UOS_PACKAGE_GIT_COMMIT`. `packaging/macos/build.py` passes the checked-out commit and asserts the linked binary
  prints `uos-gateway <commit>` for `--version`.
- Added `tests/release_identity_test.cpp` (registered in `tests/LINKS.txt`) and extended `tests/health_test.cpp` so
  a packaged build asserts the health field equals `UOS_PACKAGE_GIT_COMMIT` and is never `unknown`.

## Verification identity

Code, fuzz and package evidence were produced from commit `508e62cc77452aa36c920a9a04b028ded82a0e54`,
tree `1ec7e5321d91fd4b46eabc82cf575dbebc4d3119`. Every later commit in this job changes only documentation,
this findings report or the acceptance prose; no compilation unit, fuzz list, test registration or package
script differs from that verified state. The checkout is clean at the final report commit.

## Exact commands and measurements

### Registration and source inventory

- `git diff --diff-filter=D --name-only ca045a83ebaab6c50c20567d1a825638abac0891 HEAD` -> 56 deleted paths.
- `python3 -B cpp/gateway/tools/links.py --check` -> 450 registered tests, exit 0.
- `python3 -B cpp/gateway/tools/cmake-sources.py /tmp/sources.cmake` -> `{"module_count": 322, "rt_count": 44, "source_count": 366}`.
- A Python check of every source in `cmake/extra-targets.json` and every source in `tests/LINKS.txt` found 0 missing files.
- `python3 -B -c 'import json; print(len(json.load(open("cpp/gateway/fuzz/targets.json"))["targets"]))'` -> 25 targets.
- `git diff --check` -> clean.

### Release tests (Clang 21.1.8, release, fresh namespaces, empty home/temp)

Environment: `UOS_CI_SQLITE_PREFIX=/usr/local`, Boost 1.92.0 staged at `/tmp/boost-stage`,
`UOS_CI_CXXFLAGS='-isystem /tmp/boost-stage/include'`, `UOS_CI_LDFLAGS='-L/tmp/boost-stage/lib'`.

- `bash cpp/gateway/tools/ci-build.sh --config release release_identity` -> 366/366 production objects,
  `tests/release_identity_test.cpp` PASS 1/1 cases, 711.668 s including the cold object phase.
- `bash cpp/gateway/tools/ci-build.sh --config release tests/health_test.cpp` -> PASS 23/23 cases.
- `bash cpp/gateway/tools/ci-build.sh --config release runtime_native_startup_test` -> PASS 1/1 case.
- `bash cpp/gateway/tools/ci-build.sh --config release config_differential` -> 3 tests, PASS 23/23 cases.

### Portable package and sealed verifier

Package built from commit `508e62cc77452aa36c920a9a04b028ded82a0e54`, tree
`1ec7e5321d91fd4b46eabc82cf575dbebc4d3119`:

```sh
PORTABLE_JOBS=2 TMPDIR=/tmp bash cpp/gateway/packaging/portable/build.sh /tmp/uos-gateway-portable-508e62cc.tar.gz
```

- Archive SHA-256: `e2f2d7564b77ecbb5b097f901548cda4e25f2a634a1f910dd3bd223004fb280d`.
- `packaging/portable/verify.sh` exit 0 on Debian 12 and 13 at 512 MiB and 256 MiB.
- Gateway binary SHA-256: `d54a1fe7125334e37257b3a11e7bde92c9fae587044380905d1a67aed5fe75b3`.
- Every proof reported `release.git_sha = 508e62cc77452aa36c920a9a04b028ded82a0e54` and
  `release.deployment_id = vps-508e62cc77452aa36c920a9a04b028ded82a0e54` in both initial and loaded health,
  never `unknown`, and `x-uos-git-sha` matched the packaged commit.
- Same binary and manifest verified in all four proofs; no OOM events; two integrity refusals per proof.

### Fuzz campaign

The target list came from the workflow's own discovery expression over `cpp/gateway/fuzz/targets.json`.
Each target was built and run by the workflow's `ci.py` command, one target at a time, with Clang 21.1.8,
four compile jobs, an empty `HOME`, a fresh `TMPDIR`, a credential-free `env -i` environment and only local
loopback/libFuzzer activity. The exact per-target invocation was:

```sh
env -i PATH=/usr/lib/llvm-21/bin:/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8   HOME=<fresh>/home TMPDIR=<fresh>/tmp CPLUS_INCLUDE_PATH=/tmp/boost-stage/include   LIBRARY_PATH=/tmp/boost-stage/lib LDFLAGS=-L/tmp/boost-stage/lib   python3 -B cpp/gateway/fuzz/ci.py <target> --seconds 20 --jobs 4 --compiler clang++-21 --cc clang-21   --out <fresh>/campaign
```

Result: **25/25 targets built, 25/25 campaigns passed, 0 findings**. Aggregate compile time 2,870.61 s
(min 85.203 s, max 186.274 s). Aggregate campaign wall time 550.351 s; libFuzzer reported 535 fuzzed seconds
across the 25 targets (21-22 s each). The only crash artifact was the known empty ARM64 libFuzzer fork-parent
exit leak, counted separately by `ci.py`; every target reported one expected fork-exit leak and no input
finding. The two targets that failed hosted CI, `passkeys_cbor_library` and `passkeys_asn1_library`, both built
and passed in 100.411 s / 102.399 s and 21.657 s / 21.904 s respectively.

Per-target measurements:

| Target | Result | Build s | Campaign s | Fuzzed s | Findings | Corpus |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| base64 | passed | 163.540 | 21.994 | 21 | 0 | 277 |
| bigint_library | passed | 110.842 | 21.884 | 21 | 0 | 355 |
| console_format | passed | 149.787 | 22.535 | 22 | 0 | 354 |
| date_parse | passed | 162.241 | 21.678 | 21 | 0 | 83 |
| env_file | passed | 161.124 | 22.023 | 21 | 0 | 12 |
| fetch_response | passed | 163.653 | 22.389 | 22 | 0 | 699 |
| fetch_response_library | passed | 163.214 | 22.569 | 22 | 0 | 720 |
| headers_library | passed | 161.826 | 22.130 | 22 | 0 | 83 |
| http_request | passed | 186.274 | 22.195 | 22 | 0 | 462 |
| http_server_library | passed | 108.237 | 21.780 | 21 | 0 | 689 |
| json | passed | 85.453 | 21.720 | 21 | 0 | 774 |
| kv_jsc | passed | 86.763 | 22.119 | 22 | 0 | 1017 |
| passkeys | passed | 102.554 | 21.735 | 21 | 0 | 310 |
| passkeys_asn1_library | passed | 102.399 | 21.904 | 21 | 0 | 128 |
| passkeys_cbor_library | passed | 100.411 | 21.657 | 21 | 0 | 238 |
| posix_path | passed | 85.573 | 21.720 | 21 | 0 | 121 |
| regex | passed | 86.239 | 22.289 | 22 | 0 | 1829 |
| regex_library | passed | 85.203 | 22.378 | 22 | 0 | 2200 |
| sse | passed | 87.508 | 21.806 | 21 | 0 | 570 |
| string_icu_library | passed | 86.290 | 21.722 | 21 | 0 | 507 |
| url | passed | 85.266 | 22.101 | 22 | 0 | 1180 |
| url_library | passed | 85.321 | 22.575 | 22 | 0 | 1352 |
| utf8 | passed | 85.637 | 21.758 | 21 | 0 | 535 |
| websocket | passed | 86.416 | 22.012 | 21 | 0 | 321 |
| websocket_library | passed | 88.839 | 21.678 | 21 | 0 | 226 |

## Unfinished / out-of-scope defects

- `cpp/gateway/analysis/tidy-baseline.json` and `cpp/gateway/analysis/lifetime/{b,d}/sites.json` are historical
  audit evidence from earlier commits. They still contain keys for deleted sources (`native_addon.cpp`,
  `native_kv_bootstrap.cpp`, the offline CLI/codec/proof files and their deleted tests). `tools/tidy.sh --audit`
  reads only `configuration_yaml` and its hash, not these file lists, so they do not drive lint selection. I did not
  rewrite that evidence.
- `tests/native_addon_{darwin,linux_x64}_vectors.inc` and `tests/kv_conversion_cli_differential_vectors.inc` remain
  as recorded fixture evidence whose owning tests were removed by the cleanup. No current test or build list
  references them. The cleanup report explicitly retained recorded C++ fixtures as evidence.
- Documentation prose outside findings still links deleted files, notably `docs/ARCHITECTURE.md`
  (`src/storage/native_addon.cpp`), `docs/PORT-NOTES.md` (`tests/helpers/linked_native_kv.hpp`) and
  `docs/RELEASE-REPAIRS.md` (offline CLI/codec/proof paths). These are prose, not build inputs.
