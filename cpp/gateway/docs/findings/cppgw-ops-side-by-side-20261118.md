# Native gateway side-by-side operator kit

Base commit `b54357a92c0f2669335d83f04047c3f7de32f925` on `agent-jobs/cppgw-std-icu-20261117-44188151110-1`.

## Work contract v1

Deliver a Linux systemd template, bounded loopback request replay, and a runbook from verified package to the owner's cutover decision. The side-by-side lane uses isolated synthetic state and credentials. The existing production ingress stays on Bun. Deployment, live credentials, live data conversion, and real upstream traffic are outside this job.

Only `cpp/gateway/ops/side-by-side/`, `cpp/gateway/docs/SIDE-BY-SIDE.md` and this report belong to the job. Application, packaging, build, existing operations, and workflow paths are outside its scope.

The replay lane covers only audited reads. Provider probes, inference, account changes, and accounting belong to the existing isolated mock harness. The runbook will bind health, authentication, settlement, storage, and rollback evidence to the actual artifacts and state. Linux ARM64 package startup and host resource headroom require target-host evidence. Apple Silicon service startup and Raspberry Pi qualification have separate platform gates.

The final checks are `bash -n`, ShellCheck, `systemd-analyze verify` of the rendered unit, changed-document formatting, `git diff --check`, and bounded loopback controls for the replay script with an empty home and fresh temporary directory. This job ends with a committed, validated kit and an explicit handoff for deployment and cutover decisions.

## Continuation and deliverables

Resumed at `bc422b3f429a448629002d416cd512fdbb8c9723`, tree `91086704e39ca6b3d6b55d5856165d97ce2b16a4`. That commit carried the earlier unit and replay draft. Contract v1 still applies. The four-path delta from the ICU base consists of this append-only report, [the runbook](../SIDE-BY-SIDE.md), [the unit template](../../ops/side-by-side/uos-gateway-side-by-side.service.in) and [the replay script](../../ops/side-by-side/replay.sh).

The unit uses a dedicated account, sealed package launcher, private marked `.data`, sparse environment, loopback-only IP filter and an explicit TCP port bind rule. It asserts cgroup v2 CPU and memory support and disables swap for the observation unit. Bash replay validates the complete sample before it connects, retains bounded raw responses in a private temporary directory, and masks only the two valid health identity strings during comparison. Using response files also prevents shell command substitution from dropping embedded NUL bytes before JSON validation. The runbook pairs the public reads with the existing native/Bun mock harness and defines owner decision gates for credentials, stopped-state proof, ingress and latest-state recovery.

## Inherited tree build on the disposable runner

Ubuntu 26.04 x64, four jobs, Clang 21.1.8, Boost 1.90, ICU 78.2, SQLite 3.54.0 and mold 2.40.4. Scratch was `/tmp/cppgw-side-by-side.pdsHlT`. Installed the available dependencies with `sudo -n apt-get install -y libbrotli-dev mold` and `sudo -n env NEEDRESTART_MODE=l apt-get install -y libboost-json-dev libboost-container-dev libssl-dev libsqlite3-dev zlib1g-dev libicu-dev libzstd-dev`. The first probe stopped when it found missing Boost headers and resumed after that install. SQLite was prepared outside the checkout from the archive and SHA-256 listed in [CI.md](../CI.md), using `CFLAGS='-O2 -DSQLITE_DQS=3' ./configure --prefix="$cppgw_work/sqlite" --disable-static --disable-readline`, `make -j2` and `make install`.

The portable driver compiles every production translation unit even with one test selected. `tests/serve_test.cpp` exercises the serve lifecycle. The driver used the actual Linux network, mount and PID namespace helper with a loopback positive control, outside-route `ENETUNREACH`, private tmpfs and empty `HOME=/tmp/h`, `TMPDIR=/tmp/t`. The build used a separate empty home, a fresh temporary directory and a credential-free environment.

```bash
cppgw_work=/tmp/cppgw-side-by-side.pdsHlT
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$cppgw_work/home" TMPDIR="$cppgw_work/tmp" LANG=C.UTF-8 TZ=UTC \
  CXX=clang++ UOS_CI_JOBS=4 UOS_CI_BUILD_DIR="$cppgw_work/build" \
  PKG_CONFIG_PATH="$cppgw_work/sqlite/lib/pkgconfig" UOS_CI_LDFLAGS="-Wl,-rpath,$cppgw_work/sqlite/lib" \
  bash cpp/gateway/tools/ci-build.sh --config release tests/serve_test.cpp
```

The unchanged driver exited 1 after compiling 368 of 369 production objects in 1,082.046 s. `src/rt/base64.cpp` includes `third_party/simdutf/simdutf.h` by tree path. Clang raised `-Werror,-Wsign-conversion` in that vendor header, so the fixture could not link. The failed receipt has SHA-256 `5d0ed6d0151fdca1d3826fce8712f112efc50bb920039e84bd2636f7a6ab7411`. The same prefix flag already appears in `tools/ci_sanitize.py` and `fuzz/ci.py`.

Used a scratch controller to call the same `driver.main()` and add `--system-header-prefix=third_party/` to just the Ninja compile edge for `src/rt/base64.cpp`. It reused the other objects from the failed run and retained the first-party warning flags and original isolation and receipt logic. The scratch controller's SHA-256 is `20bc1f206cbb80eedf941adc34d8c14f18571a60eaf3b1676d9389e428c3cf13`.

```bash
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$cppgw_work/home" TMPDIR="$cppgw_work/tmp" LANG=C.UTF-8 TZ=UTC \
  CXX=clang++ UOS_CI_JOBS=4 UOS_CI_BUILD_DIR="$cppgw_work/build" \
  PKG_CONFIG_PATH="$cppgw_work/sqlite/lib/pkgconfig" UOS_CI_LDFLAGS="-Wl,-rpath,$cppgw_work/sqlite/lib" \
  python3 -BI "$cppgw_work/scoped-driver.py" cpp/gateway/cmake/ci/driver.py --config release tests/serve_test.cpp
```

The continuation completed the 369-object build, exited 0, and passed 1 of 1 test binaries and 5 of 5 cases. The one-object rebuild took 1.341 s and the driver took 2.105 s end to end. The fixture link and execution took 0.315 s and 0.064 s. The receipt SHA-256 is `d69ac4b245451bf3a6ab7d2b2f171e060b2462c86aa3fc73fad367b045d9b6e4`. Both receipts matched the before/after source digest `cde5c54835e8431ccf1128f47e49cef3fea46700a1e0b4d5f3e0a93e9c35fc42`, also recorded in the inherited ICU report. The scoped receipt captured commit `b3b42428702ca5e0b4d6d6299cf792da4e32615d`, tree `fa3b00dd8da8501a41856f85864daee06a584c42`. Later edits were confined to the runbook and report.

Linked the gateway entrypoint from the same portable driver flags and libraries, the 369 `obj/src` objects, both vendor archives, and two fresh objects. The arguments below use the exact receipt arrays. The arguments `modules` and `vendors` were the sorted `obj/src` `.o` paths and the two `vendor_archives` keys prefixed with the build directory. The mold link took 0.29 s and reported 70,480 KiB maximum child RSS. `readelf -h` identified the 42,509,256-byte binary as x86-64. Its SHA-256 is `eb4f6fa102736f8b71b4cdf47dc76eae994e31256297e9e87a06da110388948a`.

```bash
clang++ "${flags[@]}" -c cpp/gateway/runtime/startup.cpp -o "$output/startup.o"
clang++ "${flags[@]}" -c cpp/gateway/entrypoints/gateway.cpp -o "$output/gateway.o"
/usr/bin/time -f 'Gateway link elapsed %es; peak child RSS %M KiB' \
  clang++ "${flags[@]}" -o "$output/uos-gateway" "$output/startup.o" "$output/gateway.o" \
  "${modules[@]}" "${vendors[@]}" "${libs[@]}"
```

ARM64 package startup and effective target-host confinement remain operator checks. The selected x64 fixture did not include `permissions_linux_test` or `runtime_native_startup_test`.

## Kit verification

The kit artifact commit is `b48f3ad2708540ff67b766190718a554a1127e26`, tree `ca2a7ec8042ad58e9d64942bd2c7d94524c6d854`. Only the Bash script, the systemd template, and two Markdown documents are in scope. Bash 5.3.9 and ShellCheck 0.11.0 each returned 0 with empty output for the script. All eight Bash examples parsed and all 20 local Markdown links resolved. Bun 1.4.0 checked the two changed documents in scratch with frozen root and lint lockfiles. `git diff --check` passed.

```bash
bash -n cpp/gateway/ops/side-by-side/replay.sh
shellcheck cpp/gateway/ops/side-by-side/replay.sh
bun --no-env-file run format:check cpp/gateway/docs/SIDE-BY-SIDE.md \
  cpp/gateway/docs/findings/cppgw-ops-side-by-side-20261118.md
git diff --check b54357a9..HEAD
```

The 28 replay controls passed with curl 8.18.0 and jq 1.8.1. They verified zero traffic on dry run and invalid samples, allowed GET/HEAD with normalized object keys and health identity, disabled curlrc, proxy and redirects, preserved array-order and other payload differences, and rejected malformed or multiple JSON bodies, wrong and unexpected statuses, oversized chunked responses, timeout and a closed port. The harness also checked absence of auth/cookies/request bodies and removal of temporary response files. Its SHA-256 is `abd917bcb2df74d2729389a11610904629b5ea6fa07b661f873eadfd168c809d`. Invocation from repository root used an empty `$control_work/home` and fresh `$control_work/tmp`.

```bash
sudo -n unshare --net -- /bin/sh -eu -c '
  ip link set lo up
  exec setpriv --reuid="$1" --regid="$2" --clear-groups env -i \
    PATH=/usr/bin:/bin HOME="$3/home" TMPDIR="$3/tmp" LANG=C.UTF-8 TZ=UTC \
    python3 -I "$4" "$5" "$3"
' sh "$(id -u)" "$(id -g)" "$control_work" "$cppgw_work/replay-controls.py" \
  "$PWD/cpp/gateway/ops/side-by-side/replay.sh"
```

The runbook's state setup created mode-0700 private directories, a mode-0600 codec-marked database and a mode-0600 launch manifest with the expected paths and port. A second seed refused the existing database and its digest stayed intact. `systemctl --root="$offline_root" link /opt/units/uos-gateway-side-by-side.service` and `systemctl --root="$offline_root" disable uos-gateway-side-by-side.service` added and removed only the synthetic systemd symlink.

Rendered the unit with the current runner uid/group names, distinct scratch package, state and Bun roots, and port `18080`. Systemd `259.9-0ubuntu0.1` returned 0 for `systemd-analyze verify --recursive-errors=one "$cppgw_work/unit-check/uos-gateway-side-by-side.service"`. The unit SHA-256 is `08b0d8eeb35ac5a2a60a991d373544b4443f2beeebe0ccc195ee1ac9673cb28b`. The output was 357 bytes, all from installed distribution units.

```text
/usr/lib/systemd/system/xfs_scrub_all.service:26: Support for option CPUAccounting= has been removed and it is ignored
/usr/lib/systemd/system/system-xfs_scrub.slice:15: Support for option CPUAccounting= has been removed and it is ignored
/usr/lib/systemd/system/walinuxagent.service:21: Support for option CPUAccounting= has been removed and it is ignored
```

`systemd-analyze condition 'AssertControlGroupController=v2' 'AssertControlGroupController=cpu memory'` passed on the runner. All test execution used runner scratch and local fixtures. The recorded systemd evidence is static verification and offline link/unlink.

## Findings and handoff

- Build tooling owner. The portable driver's vendor-header classification is missing the scoped simdutf include used by the consumer. The exact failure and scratch workaround are above. Keep first-party warnings fatal when repairing the driver.
- Release and storage owners. `src/release.cpp` defaults to `unknown`; production needs a full build-bound SHA and deployment identity. Sparse JSC arrays can allocate memory per hole. The existing cyclic typed-value proof refuses graphs it cannot represent. A complete stopped-copy forward and reverse rehearsal and a latest-state rollback receipt remain required. [Runtime blockers](../RUNTIME.md#cutover-requirements)
- Operator. Qualify the exact Linux ARM64 package and effective unit confinement, run the same-binary native/Bun mock smoke, compare host headroom and preserve the results for the owner go/no-go decision. Service activation and cutover belong to that decision.
- Branch collection. `git push origin HEAD:refs/heads/agent-jobs/cppgw-ops-side-by-side-20261118-44214118327-1` with `GIT_TERMINAL_PROMPT=0` returned 128 because GitHub credentials were unavailable. The fetched `origin/development` at `6bd78fc5e3b0783809691c6bd45d25ac45e9696e` has no `cpp/gateway` tree. The C++ port branch at `45118332b287b7ba9534bb66ab1518f1990a9d9e` also lacks the inherited ICU tip. The job commits are available on clean `agent-job` for collection with the related port work.

### Summary

- Committed the [runbook](../SIDE-BY-SIDE.md), isolated systemd template and dry-run replay for public health and OpenAPI reads.
- On x64, all 369 production objects compiled and the gateway linked using a scratch-only vendored-header flag. The serve fixture passed 5/5 cases. ShellCheck, `bash -n`, 28 replay controls, Markdown formatting and rendered-unit verification passed.
- GitHub credentials blocked publication. ARM64 startup and confinement remain to be qualified. Cutover requires the full release identity and proof for sparse arrays, cyclic values and latest-state rollback.
