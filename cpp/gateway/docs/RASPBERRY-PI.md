# Portable ARM64 gateway on Raspberry Pi OS

The portable release targets 64-bit Raspberry Pi OS Bookworm and Trixie with an ARMv8-A CPU. The default requires Landlock ABI 8 or newer. Operators on ABI 3–7 can explicitly select the reduced policy. On kernels built without Landlock, use the opt-in systemd unit below for testing. Build on a Linux ARM64 machine with Docker and Buildx. Use a Bookworm userspace for the glibc 2.36 baseline. Trixie uses glibc 2.41.

## Build and verify

From a committed checkout, run both commands with writable output paths outside the source tree.

```sh
bash cpp/gateway/packaging/portable/build.sh /var/tmp/uos-gateway-arm64.tar.gz
bash cpp/gateway/packaging/portable/verify.sh /var/tmp/uos-gateway-arm64.tar.gz /var/tmp/uos-gateway-pi-evidence
```

The build uses `git archive HEAD` for its input, writes the archive only after the executable and asset checks, and prints its SHA-256. The package includes the C++ gateway, immutable static assets, the launcher, licenses and verification manifests. The build profile records the commit, tree, compiler, baseline and Debian dependency versions. Docker keeps build cache outside the checkout. Downloads and compilation happen during package preparation.

The proof installs the documented Debian libraries and seal tools in clean Debian 12 and 13 ARM64 images. It seals the same archive in each image, captures `readelf --version-info`, `ldd -v` and `ldd -r` for the gateway executable, and runs the sealed gateway with Docker's default seccomp profile and `--network=none`. Each runtime starts as an unprivileged user when the proof caller is unprivileged. It checks `/health`, unauthenticated rejection, an authenticated small chat, durable usage across a graceful restart, and the observed cgroup limits. The evidence includes JSON for two CPUs at both 512 MiB and 256 MiB, with swap disabled. [Runner findings](findings/cppgw-pi-compat-20261118.md)

## Linkage

The compiler is Clang 21 on Debian 12, with Bookworm's libstdc++ and libgcc linked statically. ARMv8-A is the compilation baseline. The ELF interpreter is `/lib/ld-linux-aarch64.so.1`. The package checks for the baseline's GLIBC version ceiling and rejects runtime search paths and dynamic libstdc++/libgcc dependencies in the executables.

| Component                      | Bookworm version                | Release selection                                                                                 |
| ------------------------------ | ------------------------------- | ------------------------------------------------------------------------------------------------- |
| Boost.JSON and Boost.Container | 1.74 for Bookworm's Boost suite | Static 1.92.0 from the verified upstream archive                                                  |
| simdjson and simdutf           | Vendored in this source tree    | Static 5.2.0 and 9.4.0 from the CMake vendor registry                                             |
| llhttp                         | Vendored in this source tree    | Static 9.4.4 from the CMake vendor registry                                                       |
| SQLite                         | 3.40.1                          | Static 3.54.0 with `SQLITE_DQS=3`, matching the durable store's differential fixtures             |
| ICU                            | 72.1                            | Static ICU 78.3, including data for case mapping, collation and URL IDNA                          |
| OpenSSL                        | 3.0.x                           | Shared `libssl.so.3` and `libcrypto.so.3`. Trixie supplies the same SONAMEs through `libssl3t64`. |
| zlib, Brotli and Zstd          | 1.2.13, 1.0.9 and 1.5.4         | Shared Debian libraries. The package and codec tests qualify the APIs used by the gateway.        |
| glibc and libm                 | 2.36                            | Shared Debian runtime and dynamic loader                                                          |

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

Use a new destination with a canonical existing parent. Sealing verifies the payload and binds the host loader and library paths and hashes into a new release manifest. The command prints `packageRoot` and `packageManifestSha256` for the private launch manifest. A shared library change requires sealing a new release directory before starting the gateway again. Both OS versions use the same executable bytes.

Use `runtime/launch-schema.json` from the sealed package. Supply both values from sealing, `schema` set to `uos-gateway-launch-v1`, a canonical owner-private `stateRoot`, `hostname` set to `127.0.0.1` and the chosen port other than 7999 or 8000. The manifest must be a private regular file owned by the gateway user and the package and state roots must be disjoint. Outside the isolated proof, `.data/kv-bun.sqlite3` must already exist in the private state root with the `bun-jsc-v1` codec marker. Invoke the sealed package's `ops/launch-native.sh` with the absolute manifest path. Keep the release under separate deployment ownership or on a read-only mount. [Native runtime and state requirements](RUNTIME.md)

## Kernel and filesystem requirements

Use a usr-merged system. The loader and libraries must resolve under `/usr/lib/`, including Debian's `/usr/lib/aarch64-linux-gnu` directory. The existing Arch package tool looks up libraries in flat directories and binds the building host's library hashes. The portable sealer accounts for Debian's multiarch layout while preserving the executable's closure verification. The runtime reads the selected package, standard OS trust and resolver data, and `/proc/self`. Both confinement paths direct SQLite temporary files to the private state `.data` directory.

Linux 6.6 has Landlock ABI 3 and Linux 6.12 has ABI 6 upstream when Landlock is enabled. Landlock ABI 8 introduces `LANDLOCK_RESTRICT_SELF_TSYNC` in upstream Linux 7.0. Keep seccomp filter support with thread synchronization in every mode. On ABI 3–7, set `UOS_GATEWAY_ALLOW_REDUCED_CONFINEMENT=1` in the native launcher's environment. The reduced policy applies the kernel's available rights before bootstrap starts other threads, which then inherit the restrictions. Verify the effective policy in `GET /health`. The Docker proofs use the runner kernel for these checks. [Linux confinement](RUNTIME.md#linux-confinement) · [Landlock userspace API](https://docs.kernel.org/userspace-api/landlock.html)

On an ARM64 target, query the active ABI directly before sealing.

```sh
uname -m
getconf GNU_LIBC_VERSION
python3 - <<'PY'
import ctypes
import errno

libc = ctypes.CDLL(None, use_errno=True)
abi = libc.syscall(444, 0, 0, 1)
if abi >= 0:
    print("Landlock ABI", abi)
else:
    error = ctypes.get_errno()
    print(errno.errorcode.get(error, error))
PY
```

Use the memory results for the measured small-chat workload. Raspberry Pi throughput and sustained write latency depend on the device CPU, storage and production workload. The proof uses fresh private state and runner storage. [Measurements and scope](findings/cppgw-pi-compat-20261118.md)

## Start under systemd without Landlock

Use these steps when the probe prints `ENOSYS` or `EOPNOTSUPP`. They use a dedicated service account and a fresh `/srv/uos-native` layout for a test gateway. This mode relies on systemd for filesystem confinement and leaves outbound TCP ports unrestricted by the gateway. [Security limits](SECURITY-LIMITS.md#service-manager-confinement)

1. Install the Bookworm or Trixie packages from [Prepare a local release](#prepare-a-local-release), then create the service account and its private state directories.

   ```sh
   sudo useradd --system --user-group --no-create-home --home-dir /nonexistent \
     --shell /usr/sbin/nologin uos-gateway
   sudo install -d -m 0755 /srv/uos-native
   sudo install -d -o uos-gateway -g uos-gateway -m 0700 /srv/uos-native/state /srv/uos-native/state/.data
   ```

2. Verify and extract the ARM64 archive as above. Seal it as root into the new directory selected by the unit. Keep the printed `packageManifestSha256`.

   ```sh
   sudo env -i PATH=/usr/bin:/bin LC_ALL=C python3 -B "$bundle/seal.py" /srv/uos-native/package
   ```

3. Copy a stopped, checkpointed test database with the `bun-jsc-v1` marker into the state directory. Replace the source path with your approved test snapshot.

   ```sh
   sudo install -o uos-gateway -g uos-gateway -m 0600 /path/to/test/kv-bun.sqlite3 \
     /srv/uos-native/state/.data/kv-bun.sqlite3
   sudo install -o uos-gateway -g uos-gateway -m 0600 /dev/null /srv/uos-native/launch.json
   sudoedit /srv/uos-native/launch.json
   ```

   Fill in the hash printed by the sealer. Pick a free loopback port if 18080 is in use.

   ```json
   {
     "schema": "uos-gateway-launch-v1",
     "packageRoot": "/srv/uos-native/package",
     "packageManifestSha256": "paste-the-hash-printed-by-the-sealer",
     "stateRoot": "/srv/uos-native/state",
     "hostname": "127.0.0.1",
     "port": 18080
   }
   ```

4. Create the root-only environment file. Add the line `UOS_GATEWAY_ALLOW_NO_LANDLOCK=1` and the approved test gateway environment. Keep Antigravity disabled. Systemd reads this file before applying the mount sandbox.

   ```sh
   sudo install -o root -g root -m 0600 /dev/null /srv/uos-native/operator.env
   sudoedit /srv/uos-native/operator.env
   ```

5. From the checkout for the package's commit, install and verify the unit. Start it, check for `"confinement":"service-manager"` in the response, and read the startup warning. Use the port from your launch manifest in the health request.

   ```sh
   sudo install -m 0644 cpp/gateway/ops/uos-gateway-native-no-landlock.service /etc/systemd/system/
   sudo systemd-analyze verify /etc/systemd/system/uos-gateway-native-no-landlock.service
   sudo systemctl daemon-reload
   sudo systemctl start uos-gateway-native-no-landlock.service
   curl -fsS http://127.0.0.1:18080/health
   sudo journalctl -u uos-gateway-native-no-landlock.service -n 30 --no-pager
   ```

   The unit includes a commented `MemoryMax=256M` setting for small boards. Match the limit to the test workload.
