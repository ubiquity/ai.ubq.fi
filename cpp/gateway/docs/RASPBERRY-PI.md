# Portable ARM64 gateway on Raspberry Pi OS

The portable release targets 64-bit Raspberry Pi OS Bookworm and Trixie with an ARMv8-A CPU. The default requires Landlock ABI 8 or newer. Operators on ABI 3–7 can explicitly select the reduced policy described below. Build on a Linux ARM64 machine with Docker and Buildx. Use a Bookworm userspace for the glibc 2.36 baseline. Trixie uses glibc 2.41.

## Build and verify

From a committed checkout, run both commands with writable output paths outside the source tree.

```sh
bash cpp/gateway/packaging/portable/build.sh /var/tmp/uos-gateway-arm64.tar.gz
bash cpp/gateway/packaging/portable/verify.sh /var/tmp/uos-gateway-arm64.tar.gz /var/tmp/uos-gateway-pi-evidence
```

The build uses `git archive HEAD` for its input, writes the archive only after the binary and native-artifact checks, and prints its SHA-256. The package includes the gateway and offline KV tools, immutable static assets, the existing native launcher, the frozen KV and confinement artifacts, and their licenses and manifests. The build profile records the commit, tree, compiler, baseline and Debian dependency versions. Docker keeps build cache outside the checkout. Downloads and compilation happen during package preparation.

The proof installs the documented Debian libraries and seal tools in clean Debian 12 and 13 ARM64 images. It seals the same archive in each image, captures `readelf --version-info`, `ldd -v` and `ldd -r` for all three executables, and runs the sealed gateway with Docker's default seccomp profile and `--network=none`. Each runtime starts as an unprivileged user when the proof caller is unprivileged. It checks `/health`, unauthenticated rejection, an authenticated small chat, durable usage across a graceful restart, and the observed cgroup limits. The evidence includes JSON for two CPUs at both 512 MiB and 256 MiB, with swap disabled. [Earlier runner findings](findings/cppgw-pi-compat-20261118.md) · [Final qualification](findings/cppgw-final-20261119.md)

## Linkage

The compiler is Clang 21 on Debian 12, with Bookworm's libstdc++ and libgcc linked statically. ARMv8-A is the compilation baseline. The ELF interpreter is `/lib/ld-linux-aarch64.so.1`. The package checks for the baseline's GLIBC version ceiling and rejects runtime search paths and dynamic libstdc++/libgcc dependencies in the executables.

| Component                                | Bookworm version                | Release selection                                                                                 |
| ---------------------------------------- | ------------------------------- | ------------------------------------------------------------------------------------------------- |
| Boost.JSON and Boost.Container           | 1.74 for Bookworm's Boost suite | Static 1.92.0 from the verified upstream archive                                                  |
| simdjson and simdutf                     | Vendored in this source tree    | Static 5.2.0 and 9.4.0 from the CMake vendor registry                                             |
| llhttp, Ada, QCBOR, QuickJS-ng libregexp | Vendored in this source tree    | Static 9.4.4, 4.0.0, 1.6.1 and 0.18.0 from the CMake vendor registry                              |
| SQLite                                   | 3.40.1                          | Static 3.54.0 with `SQLITE_DQS=3`, matching the durable store's differential fixtures             |
| ICU                                      | 72.1                            | Static ICU 78.3, including data for case mapping, collation and URL IDNA                          |
| OpenSSL                                  | 3.0.x                           | Shared `libssl.so.3` and `libcrypto.so.3`. Trixie supplies the same SONAMEs through `libssl3t64`. |
| zlib, Brotli and Zstd                    | 1.2.13, 1.0.9 and 1.5.4         | Shared Debian libraries. The package and codec tests qualify the APIs used by the gateway.        |
| glibc and libm                           | 2.36                            | Shared Debian runtime and dynamic loader                                                          |

Trixie also ships older SQLite (3.46.1) and ICU (76.1) than this source requires. Keeping those two libraries in the binary fixes the storage and Unicode versions across both systems. glibc keeps the host's normal resolver and runtime integration. System packages deliver shared-library security updates. Static-library updates require a package rebuild.

## Prepare a local release

Bookworm requires `libssl3`, `zlib1g`, `libbrotli1`, `libzstd1` and `ca-certificates` in addition to the base `libc6` and `libc-bin`. On Trixie use `libssl3t64` for OpenSSL. `python3` and `binutils` are also required for offline sealing and the proof. The gateway launcher uses `/bin/sh` and `/usr/bin/env`.

```sh
# Bookworm. Use libssl3t64 on Trixie.
sudo apt-get install --no-install-recommends libssl3 zlib1g libbrotli1 libzstd1 ca-certificates python3 binutils

# Compare this hash with the build receipt before extracting the archive.
sha256sum /var/tmp/uos-gateway-arm64.tar.gz
bundle=$(mktemp -d)
tar --no-same-owner --no-same-permissions -xzf /var/tmp/uos-gateway-arm64.tar.gz -C "$bundle"
env -i PATH=/usr/bin:/bin LC_ALL=C python3 -B "$bundle/seal.py" /absolute/releases/uos-gateway-arm64
```

Use a new destination with a canonical existing parent. Sealing verifies the payload and binds the actual Debian multiarch loader and library paths and hashes into a new release manifest. The command prints `packageRoot`, `packageManifestSha256` and `nativeKvProvenanceSha256` for the private launch manifest. A shared library change requires sealing a new release directory before starting the gateway again. Both OS versions use the same executable bytes.

Use `runtime/launch-schema.json` from the sealed package. Supply the three values from sealing, `schema` set to `uos-gateway-launch-v1`, a canonical owner-private `stateRoot`, `hostname` set to `127.0.0.1` and the chosen port other than 7999 or 8000. The manifest must be a private regular file owned by the gateway user and the package and state roots must be disjoint. Outside the isolated proof, `.data/kv-bun.sqlite3` must already exist in the private state root with the Bun codec marker. Invoke the sealed package's `ops/launch-native.sh` with the absolute manifest path. Keep the release under separate deployment ownership or on a read-only mount. A reviewed systemd unit supplies an explicit operator-owned `EnvironmentFile=`. The binary performs no automatic `.env` discovery. [Native runtime and state requirements](RUNTIME.md) · [Service draft](../ops/uos-gateway-native.service)

## Kernel and filesystem requirements

Use a usr-merged system. The loader and libraries must resolve under `/usr/lib/`, including Debian's `/usr/lib/aarch64-linux-gnu` directory. The existing Arch package tool looks up libraries in flat directories and binds the building host's library hashes. The portable sealer accounts for Debian's multiarch layout while preserving the executable's closure verification. The existing runtime reads the selected package, standard OS trust and resolver data, and `/proc/self`. It gives write access to the private state `.data` directory and directs SQLite temporary files there.

Linux 6.6 has Landlock ABI 3 and Linux 6.12 has ABI 6 upstream. Landlock ABI 8 introduces `LANDLOCK_RESTRICT_SELF_TSYNC` in upstream Linux 7.0. Enable Landlock in the kernel and the active LSM list, and retain seccomp filter support with thread synchronization. On ABI 3–7, set `UOS_GATEWAY_ALLOW_REDUCED_CONFINEMENT=1` in the native launcher's environment. The reduced policy applies the kernel's available rights before bootstrap starts other threads, which then inherit the restrictions. Verify the effective policy in the startup record. The Docker proofs use the runner kernel for these checks. Boot, confinement and service behavior on a physical Pi remain unverified in the final Linux ARM64 run. [Linux confinement](RUNTIME.md#linux-confinement) · [Landlock userspace API](https://docs.kernel.org/userspace-api/landlock.html)

On an ARM64 target, query the active ABI directly before sealing.

```sh
uname -m
getconf GNU_LIBC_VERSION
python3 -c 'import ctypes; print(ctypes.CDLL(None).syscall(444, 0, 0, 1))'
```

Use the memory results for the measured small-chat workload. Raspberry Pi throughput and sustained write latency depend on the device CPU, storage and production workload. The proof uses fresh private state and runner storage. [Measurements and scope](findings/cppgw-pi-compat-20261118.md)
