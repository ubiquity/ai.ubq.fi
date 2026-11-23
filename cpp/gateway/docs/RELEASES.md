# Download and install C++ gateway packages

Use the Linux ARM64 package for Arch Linux ARM or 64-bit Raspberry Pi OS. Use the macOS ARM64 package on Apple Silicon
to install the development executable and check the gateway version offline. Linux service startup needs glibc 2.36
or newer and seccomp thread synchronization, with Landlock ABI 8 or newer by default. Gateway serving requires the sealed Linux package.

## Build and download in GitHub Actions

1. In the repository, open **Actions → C++ gateway ARM64 packages → Run workflow**. Select the branch you intend to
   build. The workflow must be available on the default branch for manual runs. To build a tagged commit instead, push a
   tag beginning with `cpp-gateway-v`, for example `cpp-gateway-v0.1.0`, pointing to a commit containing the workflow.
2. Open the run and check the commit shown at the top. Wait for your platform's job to succeed. The Linux job checks
   the package in Debian 12 and 13 containers. The macOS job runs the runtime tests and checks the extracted gateway's
   offline version command.
3. On that run's **Summary** page, download `uos-gateway-linux-arm64` or `uos-gateway-macos-arm64` from **Artifacts**.
   Unzip the download to find the `.tar.gz` archive and `SHA256SUMS`. The Linux download also includes `proof.log`.
   Download each platform into its own directory so its checksum file stays with the correct archive.

Open a terminal in the directory containing the downloaded archive and verify it before extracting it:

```sh
# Linux
sha256sum -c SHA256SUMS

# macOS
shasum -a 256 -c SHA256SUMS
```

Continue only if the command prints `OK` for the expected archive. The archive's `VERSION` records the source commit,
its UTC timestamp, the compiler and the selected library versions.

## Install on Arch Linux ARM or Raspberry Pi OS

Run `uname -m` and `getconf GNU_LIBC_VERSION`. The expected architecture is `aarch64` and the minimum reported glibc
version is `2.36`. Install the runtime and sealing prerequisites for your distribution:

```sh
# Arch Linux ARM, on an up-to-date system
sudo pacman -S --needed openssl-3 zlib brotli zstd ca-certificates python binutils

# Raspberry Pi OS based on Debian 12 (Bookworm)
sudo apt-get install libssl3 zlib1g libbrotli1 libzstd1 ca-certificates python3 binutils

# Raspberry Pi OS based on Debian 13 (Trixie)
sudo apt-get install libssl3t64 zlib1g libbrotli1 libzstd1 ca-certificates python3 binutils
```

Query the active Landlock ABI before planning a launch:

```sh
python3 -c 'import ctypes; print(ctypes.CDLL(None).syscall(444, 0, 0, 1))'
```

The default requires `8` or higher. On ABI 3–7, explicitly set `UOS_GATEWAY_ALLOW_REDUCED_CONFINEMENT=1` in the
gateway's launch environment to use the rules supported by that kernel. ABI 3 lacks Landlock TCP port rules. Seccomp
thread synchronization remains required. The Debian proofs use the Actions runner's kernel. [Raspberry Pi requirements](RASPBERRY-PI.md)

From the verified download directory, extract the archive and seal it against the libraries on the target machine:

```sh
mkdir -p "$HOME/uos-gateway/releases"
install_root=$(cd "$HOME/uos-gateway" && pwd -P)
bundle=$(mktemp -d "$install_root/bundle.XXXXXX")
tar --no-same-owner --no-same-permissions -xzf uos-gateway-linux-arm64.tar.gz -C "$bundle"
commit=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["gitCommit"])' "$bundle/VERSION")
release="$install_root/releases/$commit"
env -i PATH=/usr/bin:/bin LC_ALL=C python3 -B "$bundle/seal.py" "$release"
```

The last command creates a new release directory and prints `packageRoot` and `packageManifestSha256`. Keep these values with the release. If sealing reports a missing library, install the
matching distribution package and retry. After upgrading shared libraries, use a new release directory and seal again
to record their new paths and hashes.

For a separately approved Linux launch, keep the sealed directory under deployment ownership or on a read-only mount.
Prepare a separate state directory containing an existing `.data/kv-bun.sqlite3` with the `bun-jsc-v1` codec marker. Both the
state directory and its `.data` directory must be private (mode `0700`) and owned by the gateway user. Stop any other
process using the database and keep the legacy `.data/kv.sqlite3` intact. Use a private configuration file (mode `0600`)
owned by the gateway user and fill in both printed values and the canonical state path:

```json
{
  "schema": "uos-gateway-launch-v1",
  "packageRoot": "/absolute/path/printed/by/the/sealer",
  "packageManifestSha256": "paste-the-package-manifest-sha256",
  "stateRoot": "/absolute/path/to/private/state",
  "hostname": "127.0.0.1",
  "port": 18080
}
```

Run `"$release/ops/launch-native.sh" /absolute/path/to/launch.json` as the gateway user with its approved environment.
In another terminal, `curl -f http://127.0.0.1:18080/health` checks the listener.
[Native startup](RUNTIME.md) · [Owner cutover checklist](CUTOVER.md)

## Install the files on an Apple Silicon Mac

In a native Terminal, extract to a new directory from the verified download directory.

```sh
test "$(uname -m)" = arm64
mkdir -p "$HOME/uos-gateway/releases"
release=$(mktemp -d "$HOME/uos-gateway/releases/macos.XXXXXX")
tar --no-same-owner --no-same-permissions -xzf uos-gateway-macos-arm64.tar.gz -C "$release"
cat "$release/VERSION"
file "$release/bin/uos-gateway"
"$release/bin/uos-gateway" --version
printf '%s\n' "$release"
```

`VERSION` lists the minimum macOS version. Check for an ARM64 Mach-O executable and compare the full
commit printed by `--version` with `gitCommit` in `VERSION`. Save the printed directory for rollback. Keep your
existing service on its supported release. [Known Darwin boundaries](MACOS.md)

## Roll back

Keep the previous archive and its checksum, sealed or extracted directory, and launch manifest. To roll back a Linux
launch, stop the candidate gracefully and let its writes finish. Select the previous compatible release and its launch
manifest, which must point to the current stopped `bun-jsc-v1` state. If the host libraries changed, reseal the previous archive
and use its new binding. Then restart the previous release and check its health and authenticated access.

Use the latest stopped database when switching runtimes. A return to legacy Deno after Bun writes requires reverse
conversion, a verified private rollback receipt, and a supported legacy release/profile. The offline tools are at [STATE A](https://github.com/ubiquity/ai.ubq.fi/tree/ca045a83ebaab6c50c20567d1a825638abac0891).

### Roll back the Mac file installation

Set `previous_release` to the directory printed when you installed the previous archive. Select it and check its
recorded commit.

```sh
previous_release=/absolute/path/to/previous/extracted/directory
release="$previous_release"
cat "$release/VERSION"
"$release/bin/uos-gateway" --version
```

## Build locally

Run from the repository root after choosing a committed checkout. On Linux ARM64 with Docker Buildx, the first command
below builds the Bookworm package. On Apple Silicon with Xcode and the Homebrew packages described in the Mac guide,
the second command builds SQLite 3.54.0, runs the runtime gate and packages the executable. The destination directory
must already exist outside the checkout and the archive path must be new. [Mac build prerequisites](MACOS.md)

```sh
bash cpp/gateway/packaging/portable/build.sh /var/tmp/uos-gateway-linux-arm64.tar.gz
python3 -B cpp/gateway/packaging/macos/build.py /var/tmp/uos-gateway-macos-arm64.tar.gz
```

Generate `SHA256SUMS` with `sha256sum uos-gateway-linux-arm64.tar.gz > SHA256SUMS` on Linux or
`shasum -a 256 uos-gateway-macos-arm64.tar.gz > SHA256SUMS` on macOS from the corresponding output directory.
For Linux, `bash cpp/gateway/packaging/portable/verify.sh /path/to/archive.tar.gz /path/to/new-evidence-dir` runs both
proofs. The proofs require an ARM64 Docker host with Landlock ABI 8 or newer.

Repeated builds use the committed source, normalized archive metadata and the commit timestamp. Compare builds with
the same toolchain and dependency revisions. On Linux, set `PORTABLE_REBUILD=1` to rerun the gateway compile with the dependency stages reused and the compiler cache bypassed.
