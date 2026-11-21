# Service-manager confinement work log

Base `claude/cpp-gateway-port` at `116f1e972397d80321ee223f3e63f88db9061e97`.

- [x] Read the native startup, sandbox, service, packaging and CI guidance. The Ubuntu ARM64 runner reports Landlock ABI 8 and supports the Linux namespace test boundary.
- [x] Commit the absent-Landlock decision, startup checks, diagnostics and native regression tests. The forced probe uses a seccomp filter linked into test code only.
- [x] Commit the hardened systemd unit and Raspberry Pi operator instructions.
- [x] Verify the exact unit offline and check its live mount layout in disposable systemd containers.
- [ ] Record Release and ASan results and the completed committed-diff review.

The Ubuntu systemd container exposed a writable `/run` and `/run/lock` even with `ProtectSystem=strict`. Adding `/run` to `ReadOnlyPaths` made both mounts read-only while keeping the state writable. The check uses the mount visible to the process, since the kernel also lists hidden mounts below some read-only binds.

In a network-isolated Debian 12 container, systemd 252.39 on the runner kernel mounted the three `ProtectHome=tmpfs` masks writable. The unit now specifies the three read-only tmpfs masks explicitly as well. The revised masks were empty or inaccessible from the service, and the state mount was the only writable non-API mount.

## Systemd and startup checks

The unit from candidate `ca3e04f5bc79051bed9411fcc3b23492b66646cb` (tree `950a480249f978f916408a1efc0ef5694411a367`) had SHA-256 `f42f3c252f15c91633dbb2b4cc1f628a7fc6380582eb044779e4744f6e527b6a`. With the launcher staged under a fixture `/srv/uos-native`, `systemd-analyze verify --man=no /etc/systemd/system/uos-gateway-native-no-landlock.service` passed in Ubuntu 26.04 and Debian 12 containers. `systemd-analyze security --offline=yes /etc/systemd/system/uos-gateway-native-no-landlock.service` reported exposure **3.0** on systemd 259.9 and **3.2** on systemd 252.39.

The exact Ubuntu unit ran a native probe linked with the gateway's Linux permission code. The probe forced `ENOSYS` using the test-only filter and reported `mode=service-manager TSYNC=1 no-new-privileges=1 exec-denied=1`. An unset switch and the value `true` preserved the ABI-query refusal. With the opt-in, a writable `/run/lock`, an extra read-only bind under `/home`, and a readable `.ssh` outside state each produced the corresponding refusal. A `.config` under the private state passed.

The runner has Landlock ABI 8. The absence-path tests use a test-only ABI-query override. The containers and fixtures use the runner kernel. Raspberry Pi hardware verification is pending.

Local logs are in `/var/tmp/cppgw-no-landlock-ci` and `/var/tmp/cppgw-no-landlock-systemd`. The C++ driver uses the restored compiler cache and retains objects between bounded invocations.
