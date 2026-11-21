# Service-manager confinement work log

Base `claude/cpp-gateway-port` at `116f1e972397d80321ee223f3e63f88db9061e97`.

- [x] Read the native startup, sandbox, service, packaging and CI guidance. The Ubuntu ARM64 runner reports Landlock ABI 8 and supports the Linux namespace test boundary.
- [x] Commit the absent-Landlock decision, startup checks, diagnostics and native regression tests. The forced probe uses a seccomp filter linked into test code only.
- [x] Commit the hardened systemd unit and Raspberry Pi operator instructions.
- [ ] Record Release, ASan and offline systemd results with the candidate commit and tree. Request the committed-diff review.
