# Portable ARM64 gateway package findings

## Work contract v1

Build a portable release from `b54357a92c0f2669335d83f04047c3f7de32f925` for 64-bit Raspberry Pi OS Bookworm and Trixie. Compile on Debian 12 ARM64 against glibc 2.36, statically link the C++ runtime and libraries that require newer versions, and qualify the remaining Debian shared dependencies. Preserve the real packaged entrypoint, storage codec, authentication, accounting and OS confinement. The owned paths are `packaging/portable/`, `docs/RASPBERRY-PI.md` and this report. Application sources, production deployment, provider traffic and live state are outside this task.

Acceptance requires a one-command build, symbol and loader checks on clean Debian 12 and 13 ARM64, authenticated health and small chat with a loopback mock, and memory and throughput receipts with two CPUs at 512 MiB and 256 MiB. The source checkout lacks `bench/request_path/` and its performance report. The tracked `tests/production_smoke/` harness provides an existing packaged gateway launcher and scripted upstream. Discovery still needs to resolve the host-library closure, artifact preparation and kernel requirements before publishing a package.
