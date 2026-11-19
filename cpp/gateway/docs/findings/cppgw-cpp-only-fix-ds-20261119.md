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

## Exact commands and measurements

(in progress: fuzz campaign and release tests)

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
