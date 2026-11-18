# Native gateway owner guide verification, 2026-11-18

## Work contract v1

Write the owner entry point and architecture guide for the C++20 gateway at starting commit
`33ea9b20d1f02b96bb86a24c0530641ccd4fd525`. Own `cpp/gateway/README.md`, `cpp/gateway/docs/ARCHITECTURE.md`,
and this report. Preserve all other tracked files, including the TypeScript gateway, C++ application and fixtures,
existing documents, and workflows. Use the source for current behavior and exact-build receipts for measured results.

The platform matrix is Linux ARM64 for release and production confinement, Linux x86_64 for portable build and local
fixtures, Apple Silicon for the recorded development suite, and Raspberry Pi for an unqualified target. Use local mocks
only. Production traffic, service activation, credential changes, and database conversion are outside this work.

Verify the README's commands with the portable Release driver and record the observed platform, compiler, host libraries,
counts, failures and launch outcome. The driver uses an external build directory and isolated Linux fixtures. The
namespace setup, installed library versions, and x86_64 startup outcome need local confirmation. The work ends with a
verified Markdown and relative-link check, `git diff --check`, and a complete findings record.

## Initial inventory

`bash cpp/gateway/tools/ci-build.sh --list | wc -l` returned `441` on Ubuntu 26.04.1 LTS, Linux x86_64.
The suite-repair report records 441 Release binaries and 5,031 cases passed on Linux ARM64 at the last source/test
commit, `380a9b0a19fca261c910967ee24078f1b22aec9a`. [Suite-repair receipts](cppgw-suite-repair-20261118.md)

## Runner and commands

The runner had four CPUs, Clang 21.1.8, Ninja 1.13.2, mold 2.40.4, Boost 1.90.0, OpenSSL 3.5.5, zlib 1.3.1,
ICU 78.2, Brotli 1.2.0 and Zstd 1.5.7. The [owner guide](../../README.md) records the apt and pinned SQLite
commands run here. The SQLite archive matched the published SHA-256. `pkg-config --modversion sqlite3` printed
`3.54.0`, the shared library in the test environment reported `3.54.0` with `DQS=3` compiled in, and the CLI
query below printed `3.54.0|double quoted string`.

```sh
/usr/local/bin/sqlite3 :memory: 'select sqlite_version(), "double quoted string";'
bash cpp/gateway/tools/ci-build.sh --list | wc -l
```

The unprivileged namespace probe failed at `uid_map`. The portable driver used its `sudo -n` fallback and
dropped back to the runner uid for each test. Its kernel controls proved loopback connectivity and no outbound
route. It supplied a fresh empty home and temp directory and used the fixture environment allowlist.

```sh
build_dir=$(mktemp -d /tmp/uos-gateway-build.XXXXXX)
env -i PATH="$PATH" LC_ALL=C LD_LIBRARY_PATH=/usr/local/lib CXX=clang++ UOS_CI_JOBS=4 \
  UOS_CI_BUILD_DIR="$build_dir" bash cpp/gateway/tools/ci-build.sh --config release >"$build_dir/driver.log" 2>&1
```

The build directory was `/tmp/uos-gateway-build.SuKRVT`, with the driver receipt and logs under
`clang++-4ab551e14be2/release/`. The driver digest covers the compiled sources, tests and driver inputs.

## Launch command check

The portable driver compiles the service modules and builds standalone tests. For a separate launch preflight,
the scratch probe compiled `runtime/startup.cpp` and `entrypoints/gateway.cpp` with the driver's generated Clang
Release flags and linked them with all 369 production objects, both vendor archives and the same host libraries.
It copied the launcher next to the scratch `bin/uos-gateway` and used the driver's real isolation helper. The
manifest used an intentionally wrong package-manifest hash, an empty private state directory and port 0.

```sh
env -i PATH="$PATH" LC_ALL=C LD_LIBRARY_PATH=/usr/local/lib python3 -B \
  /tmp/cppgw-guide-format.8FaOcD/launch-probe.py cpp/gateway /tmp/uos-gateway-build.SuKRVT/clang++-4ab551e14be2/release
```

The probe executed `bash "$package_root/ops/launch-native.sh" "$launch_manifest"` with the two scratch paths.
The executable SHA-256 was `6905eb378fac21994c107309ca5e1686b2a104846ca8f2d098989b8ed92a9345`. The process
exited 1 with `Native startup refused: package manifest integrity check failed` and the private `.data` stayed
empty. The control printed `loopback: true` and `outside_errno: 101`. This verifies the command and the early
integrity check on x64. The prior sealed ARM64 package's paired smoke supplies successful startup, authenticated
request, reopen and shutdown evidence. [Launcher](../../ops/launch-native.sh) · [Smoke results](../PERFORMANCE.md)

## Documentation discrepancies

- **JSON implementation.** `BUILD.md`, `RUNTIME.md`, `OPEN-STAGES.md`, the replacement plan and `PERFORMANCE.md`
  describe DOM or its historical fallback in earlier packages and checkpoints. This tree selects simdjson 5.2.0
  On-Demand and Boost.JSON serialization primitives. `docs/rt/values.md` also describes the old parser's numeric-key
  ordering, 10,000-level parse bound and exact JavaScriptCore errors. The current parser preserves input order and
  enforces 16 MiB and depth 128 by default with `SyntaxError("Invalid JSON")` on refusal. The guide uses the code
  and the newer suite-repair findings for current behavior and keeps the performance data attached to its build.
  [Build](../BUILD.md) · [Values](../rt/values.md) · [Parser](../../src/rt/json_simdjson.hpp) ·
  [Serializer](../../src/rt/json.cpp) · [Deviations](../DEVIATIONS-FROM-BUN.md) · [Suite repair](cppgw-suite-repair-20261118.md)
- **Base64 and Unicode.** The replacement plan and `OPEN-STAGES.md` describe Base64 decode and bulk UTF work as
  candidates. The selected simdutf adapter calls the library for encode, decode, validation and transcoding.
  `docs/rt/web.md` gives the Latin-1-only IDNA limit of the comparison profile. The native URL implementation
  uses ICU UTS #46. The Ada report is a candidate receipt and the current URL source still owns parsing.
  [Plan](../OSS-REPLACEMENT-PLAN.md) · [Open stages](../OPEN-STAGES.md) · [Web](../rt/web.md) ·
  [Vendor registry](../../cmake/vendors.json) · [Base64](../../src/rt/base64.cpp) · [URL](../../src/rt/url.cpp) ·
  [Ada findings](cppgw-oss-ada-url-20261117.md)
- **Input defaults and current enforcement.** The `RUNTIME.md` v40 table distinguishes proposed generic limits
  from wired limits. The existing HTTP server uses 16,408 bytes and 198 fields on the request head. The common
  body reader and several routes set their own caps. The WebSocket idle limit, the new generic regex limits and
  the new CBOR and certificate limits still depend on integrations. The guide reports only enforced limits.
  [Runtime table](../RUNTIME.md) · [HTTP parser](../../src/runtime/http.cpp) · [Body reader](../../src/request.cpp) ·
  [Limits](../../include/uos/rt/limits.hpp)
- **Launch and passkeys.** `docs/rt/platform.md` says the C++ entry point explicitly reads the repository `.env`
  and labels WebAuthn unimplemented. The packaged launcher and native entry point pass the manifest and inherit
  process configuration. The draft unit supplies an explicit `EnvironmentFile`. `src/auth/passkeys.cpp` implements
  passkey verification and has differential coverage. The launcher and implementation files take precedence.
  [Platform reference](../rt/platform.md) · [Gateway entry](../../entrypoints/gateway.cpp) ·
  [Draft unit](../../ops/uos-gateway-native.service) · [Passkey verification](../../src/auth/passkeys.cpp) ·
  [Passkey fixture](../../tests/passkeys_webauthn_differential_test.cpp)
- **Durable storage.** The storage reference's row format says the reader accepts “every form Bun writes.” Its
  own limits section and the current materializer show refused cycles and sparse lengths beyond its dense limit.
  `docs/rt/kv-durable.md` and `docs/rt/sqlite.md` also describe the older SDK's load-on-use SQLite 3.40.1 path.
  The native profiles use the installed SQLite library directly. The native startup fixture's opening comment
  still says the backend is unported. Its preparation-only scope is accurate for that test and the linked store
  is implemented and tested elsewhere. [Durable KV](../rt/kv-durable.md) · [SQLite](../rt/sqlite.md) ·
  [Reader](../../src/rt/kv_jsc_deserialize.cpp) · [Native library binding](../../src/rt/sqlite.cpp) ·
  [Startup fixture](../../tests/runtime_native_startup_test.cpp) · [Interop fixture](../../tests/kv_durable_interop_test.cpp)
- **Test instructions and receipts.** `TESTING.md` and the host-safety section of `PORTING.md` are for the original
  constrained host and use `unit.sh`. The portable driver and its isolation apply to the borrowed runner. The
  port notes' 436 targets and 4,995 cases, the release docs' 5,024 cases and `CI.md`'s pending runs belong to older
  scopes. The latest suite-repair report supplies the ARM64 totals. Git records `tools/ci-build.sh` as `100755`
  at this base. The earlier workflow found `100644` and repaired it. The guide uses the requested Bash invocation.
  [Testing](../TESTING.md) · [Port notes](../PORT-NOTES.md) · [CI](../CI.md) · [CI workflow receipt](cppgw-ci-workflow-20261118.md)
- **Evidence links.** Build, runtime, performance, open-stage and deviations documents link to runner-local
  `/tmp` or untracked `.data` receipts absent from this checkout. The guides link checked-in summaries. The
  packager still copies `docs/RUNTIME.md` into a sealed package as its README. [Build](../BUILD.md) ·
  [Performance](../PERFORMANCE.md) · [Packager](../../tools/package.py)

The pre-existing storage limitations, ARM64 confinement requirement and Ubuntu multiarch host-library lookup
finding remain outside the owned paths. The request benchmark report documents its local workaround for the last
one. [Storage blockers](../OPEN-STAGES.md) · [Benchmark finding](cppgw-perf-request-path-20261117.md)
