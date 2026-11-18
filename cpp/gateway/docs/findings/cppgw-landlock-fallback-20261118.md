# Linux Landlock compatibility on ARM64

## Work contract v1

- Start from `33ea9b20d1f02b96bb86a24c0530641ccd4fd525`. Add an operator-only opt-in for Landlock ABI 3–7, with a real pre-thread kernel boundary. Keep the ABI 8 default requirement, the existing filesystem grants and the seccomp filter.
- Use the kernel ABI query as the authority. ABI 3 provides filesystem and truncation controls, ABI 4 adds TCP port controls, ABI 5 adds device `ioctl`, ABI 6 adds abstract Unix socket and signal scoping, ABI 7 adds kernel audit controls, and ABI 8 adds Landlock thread synchronization. ABI 9 and newer retain the existing pathname Unix socket access check. Seccomp thread synchronization is required in every mode.
- Own the Linux permissions module, a new reduced-policy test, the two runtime guides and this report. The startup call sites named in the brief live at `runtime/startup.cpp` and `entrypoints/gateway.cpp` on the supplied branch. Limit startup edits to the confinement bootstrap. Keep the original full-policy tests, build registry, other runtime code, providers and TypeScript byte-identical.
- Verify the permissions link closures in release and with ASan+UBSan, the complete ARM64 release suite, and strict compilation of the actual startup call sites. Tests use the driver's isolated network, empty home and fresh temporary directory. No service activation, real provider traffic or state conversion is in scope.
- Resolve the existing TCP listener and egress contracts before selecting port rules. Report actual probe, build and test results, including any host qualification still outstanding.

## Environment

Ubuntu 24.04 ARM64 runner, Linux `7.0.0-1017-azure`, four CPUs. The runner already provides GCC 14.2.0. Prerequisites and build outputs are prepared outside the checkout.
