# Live Codex login for a same-host follower

Base is `fe308a3435a58cd2a0462a3117f54cdce80d3b97` on `claude/cpp-gateway-port`. The runner is Ubuntu 26.04 ARM64. All inference and usage fixtures use synthetic credentials and local mocks.

- [x] Share credential validation between stopped import and the live reader.
- [x] Open the live source without write access and confine it to a separate state tree.
- [x] Wire startup, demand-driven recovery, durable adoption and health.
- [ ] Verify with a concurrent WAL writer, loopback inference and Linux confinement.
- [ ] Document operator setup and record Release, ASan/UBSan and registration receipts.
