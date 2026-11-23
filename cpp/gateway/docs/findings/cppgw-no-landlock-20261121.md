# Service-manager confinement work log

Base `claude/cpp-gateway-port` at `116f1e972397d80321ee223f3e63f88db9061e97`.

- [x] Read the native startup, sandbox, service, packaging and CI guidance. The Ubuntu ARM64 runner reports Landlock ABI 8 and supports the Linux namespace test boundary.
- [x] Commit the absent-Landlock decision, startup checks, diagnostics and native regression tests. The forced probe uses a seccomp filter linked into test code only.
- [x] Commit the hardened systemd unit and Raspberry Pi operator instructions.
- [x] Verify the exact unit offline and check its live mount layout in disposable systemd containers.
- [x] Record Release and ASan results, the full startup smoke, and the completed committed-diff review.

The Ubuntu systemd container exposed a writable `/run` and `/run/lock` even with `ProtectSystem=strict`. Adding `/run` to `ReadOnlyPaths` made both mounts read-only while keeping the state writable. The check uses the mount visible to the process, since the kernel also lists hidden mounts below some read-only binds.

In a network-isolated Debian 12 container, systemd 252.39 on the runner kernel mounted the three `ProtectHome=tmpfs` masks writable. The unit now specifies the three read-only tmpfs masks explicitly as well. The revised masks were empty or inaccessible from the service, and the state mount was the only writable non-API mount.

## Systemd and startup checks

The unit from candidate `ca3e04f5bc79051bed9411fcc3b23492b66646cb` (tree `950a480249f978f916408a1efc0ef5694411a367`) had SHA-256 `f42f3c252f15c91633dbb2b4cc1f628a7fc6380582eb044779e4744f6e527b6a`. With the launcher staged under a fixture `/srv/uos-native`, `systemd-analyze verify --man=no /etc/systemd/system/uos-gateway-native-no-landlock.service` passed in Ubuntu 26.04 and Debian 12 containers. `systemd-analyze security --offline=yes /etc/systemd/system/uos-gateway-native-no-landlock.service` reported exposure **3.0** on systemd 259.9 and **3.2** on systemd 252.39.

The exact Ubuntu unit ran a native probe linked with the gateway's Linux permission code. The probe forced `ENOSYS` using the test-only filter and reported `mode=service-manager TSYNC=1 no-new-privileges=1 exec-denied=1`. An unset switch and the value `true` preserved the ABI-query refusal. With the opt-in, a writable `/run/lock`, an extra read-only bind under `/home`, and a readable `.ssh` outside state each produced the corresponding refusal. A `.config` under the private state passed.

The supplied unit reached loopback mocks for unicast DNS and IPv4 TCP port 443 on Ubuntu and Debian 12. The fixtures used Docker's `--network=none` boundary and a credential-free environment.

The full Release gateway smoke ran the actual entry point and verified its fixture package and runtime closure. It used an Ubuntu ARM64 executable with SHA-256 `f8de85ef4bd7e0dcc6abad92c5420b6e2b8ca3661a48509c0ee64b7520fff955`, built from the Release objects and bound to `ca3e04f5bc79051bed9411fcc3b23492b66646cb`. The test override forced the ABI query to return `EOPNOTSUPP` before the normal launcher. With the value `true`, startup refused before creating a SQLite database or fixture binding. With `1`, startup logged one warning and `GET /health` returned HTTP 200 with `service-manager`. Keeping `1` and running the same binary on the runner's real ABI 8 returned `landlock`. Both successful runs drained on stop.

```sh
proof=/var/tmp/cppgw-no-landlock-systemd
commit=ca3e04f5bc79051bed9411fcc3b23492b66646cb
tree=950a480249f978f916408a1efc0ef5694411a367
bash "$proof/link-gateway.sh" /var/tmp/cppgw-no-landlock-ci/clang++-0c4b5b1681c2/release release "$commit"
bash "$proof/exercise-gateway.sh" release "$commit" "$tree"
```

The runner has Landlock ABI 8. The absence-path tests use a test-only ABI-query override. The containers and fixtures use the runner kernel. Raspberry Pi hardware verification is pending.

Local logs are in `/var/tmp/cppgw-no-landlock-ci` and `/var/tmp/cppgw-no-landlock-systemd`. The C++ driver uses the restored compiler cache and retains objects between bounded invocations.

## Native test receipts

The final focused commands ran at `6b6999d8276c1f5b608cfc0dbe04afaaeb349c5e`, tree `f467b5ab31018fb973bf3e1179113dde2f8ac4af`. All six receipts recorded source digest `f423f19ae0e759e512836b90772a69d4ab616f5b2990888dee312acbe8bf1d04`, unchanged inputs, and 367 production objects. The runner used Ubuntu 26.04 ARM64, Clang 21.1.8 and the pinned SQLite 3.54.0 with the workflow's download hash and configure flags. The driver used Linux network, mount and PID namespaces via sudo, an empty home, fresh temp and a credential-free environment.

```sh
export CC=clang CXX=clang++ UOS_CI_JOBS=3 UOS_CI_BUILD_DIR=/var/tmp/cppgw-no-landlock-ci
for config in release asan; do
  for selection in permissions health_test runtime_native_startup; do
    timeout --kill-after=8s 240s bash cpp/gateway/tools/ci-build.sh --config "$config" "$selection"
  done
done
python3 -B cpp/gateway/tools/links.py --check
git diff --check claude/cpp-gateway-port..HEAD
```

| Filter | Release | ASan |
| --- | --- | --- |
| `permissions` | 4 tests, 17 cases passed | 4 tests, 17 cases passed |
| `health_test` | 2 tests, 25 cases passed | 2 tests, 25 cases passed |
| `runtime_native_startup` | 1 test, 1 case passed | 1 test, 1 case passed |

The existing seccomp hash comparison and Landlock tests passed with their original assertions. The registration gate checked 453 tests and the whitespace check passed.

## Committed-diff review

The native Codex 0.171.0 CLI completed the review with model `gpt-6.1-sol` at base `116f1e972397d80321ee223f3e63f88db9061e97` and head `ca3e04f5bc79051bed9411fcc3b23492b66646cb`. The follow-up commits record verification only.

```sh
codex review --base origin/claude/cpp-gateway-port \
  -c 'model="gpt-6.1-sol"' -c 'review_model="gpt-6.1-sol"' \
  -c 'sandbox_mode="read-only"' -c 'shell_environment_policy.inherit="none"' \
  -c 'shell_environment_policy.set={PATH="/usr/bin:/bin"}'
```

> No actionable defects were identified in the changes relative to the supplied merge base.
