# Linux Landlock compatibility on ARM64

## Work contract v1

- Start from `33ea9b20d1f02b96bb86a24c0530641ccd4fd525`. Add an operator-only opt-in for Landlock ABI 3–7, with a real pre-thread kernel boundary. Keep the ABI 8 default requirement, the existing filesystem grants and the seccomp filter.
- Use the kernel ABI query as the authority. ABI 3 provides filesystem and truncation controls, ABI 4 adds TCP port controls, ABI 5 adds device `ioctl`, ABI 6 adds abstract Unix socket and signal scoping, ABI 7 adds kernel audit controls, and ABI 8 adds Landlock thread synchronization. ABI 9 and newer retain the existing pathname Unix socket access check. Seccomp thread synchronization is required in every mode.
- Own the Linux permissions module, a new reduced-policy test, the two runtime guides and this report. The startup call sites named in the brief live at `runtime/startup.cpp` and `entrypoints/gateway.cpp` on the supplied branch. Limit startup edits to the confinement bootstrap. Keep the original full-policy tests, build registry, other runtime code, providers and TypeScript byte-identical.
- Verify the permissions link closures in release and with ASan+UBSan, the complete ARM64 release suite, and strict compilation of the actual startup call sites. Tests use the driver's isolated network, empty home and fresh temporary directory. No service activation, real provider traffic or state conversion is in scope.
- Resolve the existing TCP listener and egress contracts before selecting port rules. Report actual probe, build and test results, including any host qualification still outstanding.

## Environment

Ubuntu 24.04 ARM64 runner, Linux `7.0.0-1017-azure`, four CPUs. The runner already provides GCC 14.2.0. Prerequisites and build outputs are prepared outside the checkout.

## Continuation and work contract v2

The inherited `b99f24ca6f3e1b610f084da9fa55332897c20d4a` commit added the report. The worktree was clean. Clang 21.1.8 compiled the untouched permissions implementation, the original permissions test, and both native startup units with `-std=c++20 -O2 -Wall -Wextra -Wshadow -Wconversion -Werror` and the driver’s release definitions. A raw `landlock_create_ruleset(..., LANDLOCK_CREATE_RULESET_VERSION)` returned ABI 8 on the ARM64 runner. The tracked launcher call sites are `cpp/gateway/runtime/startup.cpp` and `cpp/gateway/entrypoints/gateway.cpp`.

- The default keeps the current filesystem rights and seccomp program and requires Landlock TSYNC. The exact startup environment value `UOS_GATEWAY_ALLOW_REDUCED_CONFINEMENT=1` selects the ABI-adaptive ruleset. ABI 3–7 requires a verified single-threaded bootstrap. Every accepted ABI uses the same seccomp program with TSYNC. ABI 7 uses the kernel's default audit settings. A test-only C++ argument can lower the queried ABI.
- The opt-in TCP policy permits bind on the manifest's validated listener port. Normal egress permits TCP 53 for DNS and 443 for the current HTTPS/WSS upstreams. The isolated fixture can connect to its separately validated loopback mock port. Landlock controls port numbers, and redirect targets on other TCP ports will be denied in this mode. TLS trust loads synchronously before confinement so configured CA bundles keep their existing preflight access.
- The existing confinement receipt and a single startup line report the applied controls. The HTTP health response has no confinement field. Existing path and syscall grants, full-policy tests, provider routing, package verification, wire contracts and the build registry stay within their prior contracts.
- Use the driver for the four ARM64 release shards, the permissions link closures for the focused sanitizer gate, and strict compilation for the launcher units that the portable driver omits. External scratch holds dependencies, objects, and complete command logs. Expect a cold run to take several minutes per profile with four cores. The driver logs object counts and per-test results. Diagnose the first new failure and retain exact receipts. Stop with a committed bounded diff and a documented qualification or delivery blocker if one remains.

## Implementation and verification

The opted-in policy uses ABI-specific Landlock ruleset lengths and rights. It checks the real thread group before applying Landlock on ABI 3–7. For ABI 8 and newer it uses Landlock TSYNC. The existing seccomp filter source is byte-identical to the baseline. Launch preflight runs synchronously, including the existing configured TLS trust reads, before transport installation or event-loop startup. The original permissions test is byte-identical to the baseline.

The verified implementation is commit `feda6ebb556316b7993d77638ac23ec09016dd93`, tree `d9e52b1ed94ea20c1a961a3efd2d5363dd935467`. The runner used Clang 21.1.8, Boost.JSON and Boost.Container 1.92.0 static archives, OpenSSL 3.0.13, SQLite 3.54.0 with `SQLITE_DQS=3`, ICU 78.3, zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5. The driver used its loopback-only network and private mount and PID namespaces via sudo, dropped back to runner UID 1001, and started each test with an empty home, a fresh temporary directory and no provider variables.

```sh
scratch=/tmp/cppgw-landlock.VMsArCYv
export CXX=clang++-21 LD_LIBRARY_PATH=/usr/local/lib PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
export UOS_CI_BUILD_DIR="$scratch/build" UOS_CI_JOBS=4
export UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0"
export UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -licudata"
for shard in 1 2 3 4; do
  bash cpp/gateway/tools/ci-build.sh --config release --shard "$shard/4"
done
python3 -B cpp/gateway/tools/links.py --list-tests | xargs -r python3 -B cpp/gateway/tools/links.py --with-test
nice -n 6 python3 -B "$scratch/focused-asan.py"
```

The external sanitizer harness resolved the two permissions link closures with `links.py`. It used the driver's ASan+UBSan compiler flags, Ninja object builder, link runner and actual isolated test launcher. This built 47 first-party objects in 687.456 seconds and passed 2/2 binaries and 10/10 cases in 692.214 seconds including the build. ASan leak detection was enabled and UBSan errors were fatal. The source hash before and after both tests was `345615c05ae3e9bf3777d7ffea4fb21192e3c7c05711915360fa2f8da167ab31`.

On the ABI 8 host the tests installed real rulesets capped at ABI 3, 4, 5, 6, 7 and 8. At ABI 3, the confined child and a newly created thread both got `EACCES` when trying to write outside the grant. The tests also verified default and below-floor refusals, the seccomp availability requirement, refusal when a sibling thread already exists, and loopback TCP access on the granted port with denial on an ungranted port from ABI 4. ABI 9 was checked in the policy matrix.

The four release receipts match the same Git tree and input hash. They cover each of the 442 binaries exactly once. Each shard built or reused all 369 production objects. Both permissions binaries passed 5/5 cases in release.

| Release shard | Binaries passed | Cases passed | Object phase (s) | Total wall time (s) |
| ------------- | --------------: | -----------: | ---------------: | ------------------: |
| 1/4           |             111 |         1215 |          973.339 |            1006.276 |
| 2/4           |             111 |         1211 |          243.607 |             263.169 |
| 3/4           |             110 |         1265 |          236.248 |             248.119 |
| 4/4           |             110 |         1345 |          237.751 |             257.933 |
| Total         |             442 |         5036 |                  |            1775.497 |

The launcher units are outside the portable driver's `src/` inventory. Both were compiled with the release warnings and definitions below. The launcher was then linked with all 369 driver-built production objects and both vendor archives. The 371-object link took 0.672 seconds. The isolated binary invocation without arguments reported the expected manifest usage refusal and exited 1 in 0.067 seconds.

```sh
for source in runtime/startup.cpp entrypoints/gateway.cpp; do
  clang++-21 -std=c++20 -O2 -Wall -Wextra -Wshadow -Wconversion -Werror \
    -DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING -DBOOST_JSON_NO_LIB \
    -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB -DUOS_GATEWAY_NATIVE_LIBRARIES=1 \
    -Icpp/gateway/include -Icpp/gateway -isystem "$scratch/boost_1_92_0" \
    -isystem /usr/local/include --system-header-prefix=third_party/ \
    -c "cpp/gateway/$source" -o "$scratch/launcher-check/${source##*/}.o"
done
python3 -B "$scratch/verify-launcher.py"
bun --no-env-file run format:check cpp/gateway/docs/RUNTIME.md cpp/gateway/docs/DEVIATIONS-FROM-BUN.md cpp/gateway/docs/findings/cppgw-landlock-fallback-20261118.md
git diff --check b99f24ca..HEAD
```

The launcher harness linked the actual entrypoint and startup object with the Release objects and driver library flags. Its invocation used the driver's real Linux namespace launcher. Formatting used Bun 1.4.0 and the repository's locked Prettier 3.9.6 in external scratch.

## Handoff

The verification used ABI caps on the Linux 7.0 ARM64 runner. A physical Raspberry Pi and a packaged deployment are outside these receipts. The Raspberry Pi checklist in `docs/BUILD.md` still lists an ABI 8 requirement. That file is outside the owned paths.

The intended branch for delivery is `origin/agent-jobs/cppgw-landlock-fallback-20261118-44221821464-1`. The HTTPS push preflight with `GIT_TERMINAL_PROMPT=0 git -c core.askPass=/bin/false push --dry-run origin HEAD:refs/heads/agent-jobs/cppgw-landlock-fallback-20261118-44221821464-1` failed because GitHub credentials were unavailable. The task base is outside the fetched `development` history. The implementation and verification record are committed on the assigned local checkout for the branch handoff.

**Summary.** `UOS_GATEWAY_ALLOW_REDUCED_CONFINEMENT=1` enables the available Landlock protections down to ABI 3 on Linux ARM64. The default requires ABI 8. The full release suite passed 442 binaries and 5,036 cases. Both permissions binaries passed ASan+UBSan with 10 cases total. The capped ABI 3 child and its later thread both got `EACCES` on out-of-grant writes. GitHub authentication blocked publication.
