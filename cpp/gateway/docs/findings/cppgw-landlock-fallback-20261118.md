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
