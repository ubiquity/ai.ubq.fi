# C++ gateway release workflow findings

## Work contract v1

Build downloadable Linux ARM64 and Apple Silicon packages for `workflow_dispatch` and `cpp-gateway-v*` tags. Linux uses the Bookworm portable build, with Debian 12 and 13 container proofs. Arch Linux ARM and Raspberry Pi OS use an offline seal against the target's libraries and retain the existing Linux kernel requirements. macOS uses `tools/macos-build.sh --suite rt` and the existing entrypoints. Darwin production startup currently fails closed and remains outside the owned release files.

Owned files are `.github/workflows/cpp-gateway-release.yml`, `cpp/gateway/packaging/`, `cpp/gateway/docs/RELEASES.md` and this report. Preserve the application, tests, authentication, accounting, storage and OS confinement. Leave live state, service activation and provider traffic outside this work. Use an empty test home, fresh scratch and credential-free local mocks.

Acceptance requires byte-identical Linux archives from the same commit, an independent compile and KV rebuild, the same Debian proof command as the workflow, `actionlint`, and install and rollback instructions for all three targets. Record the commit and tree, exact commands, counts, timings, and any untested targets.

## Release changes

- The workflow runs on manual dispatch and `cpp-gateway-v*` tag pushes. It uploads separate Linux ARM64 and Apple Silicon archives with relative `SHA256SUMS`. The Linux download includes `proof.log`. Checkout and artifact upload use immutable revisions of the existing v4 actions.
- The portable Bookworm build records the commit timestamp, compiler and dependency versions in `VERSION`. The archive has deterministic ordering, timestamps and ownership. The package verifies the original KV build receipt before recording the actual commands and exit codes with the temporary path normalized. Rust compilation maps that path out of its binary. The package carries the native KV workspace and registry licences, including 231 notice files from or for 129 registry packages.
- The Apple Silicon packaging entry point uses `tools/macos-build.sh --suite rt --require-clean` with SQLite 3.54.0 and ICU 78. It links the three existing entrypoints from the tested application archive and the static dependencies, then checks the resulting Mach-O architecture, system dylib closure and ad hoc signature. `VERSION` lists the executable minimum OS versions. I checked the published `arm64_tahoe` bottle digests and confirmed the expected static archives for Boost, OpenSSL, ICU, Brotli, Zstd and zlib. The Boost 1.92.0 and LLVM compiler runtime licences come from their upstream releases. [Mac build guide](../MACOS.md)
- The release guide covers download, checksum, installation, sealing, and rollback. Current Arch Linux ARM provides OpenSSL 3 as `openssl-3`. [Release guide](../RELEASES.md)

## Linux build and verification

The acceptance checkout was commit `4e0c5d9c6a29de4d0017bf866b751be530388e77`, tree
`3e80f9d25af215ca820931639e8d3ca763a5117e`, dated `2026-11-18T12:08:37Z`. The runner was Ubuntu 24.04.5 ARM64
with four CPUs and Linux `7.0.0-1017-azure`, which exposed Landlock ABI 8. The build used Debian Clang 21.1.5,
Boost 1.92.0, SQLite 3.54.0 and ICU 78.3 in the Bookworm container. The dependency stages were prepared outside the checkout.

The first build and proof ran the two Linux `run` blocks verbatim using `bash --noprofile --norc -eo pipefail` with
`RUNNER_TEMP=/var/tmp/cppgw-release.ETONLl/a`. These are the commands inside those steps, followed by the independent
rebuild and checks. Each step used an empty home and a fresh temporary directory.

```sh
release="$RUNNER_TEMP/cpp-gateway-linux"
(
  scratch=$(mktemp -d "$RUNNER_TEMP/cpp-gateway-build.XXXXXX")
  trap 'rm -rf -- "$scratch"' EXIT
  mkdir "$release" "$scratch/home" "$scratch/tmp"
  env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$scratch/home" TMPDIR="$scratch/tmp" \
    bash cpp/gateway/packaging/portable/build.sh "$release/uos-gateway-linux-arm64.tar.gz"
  (cd "$release" && sha256sum uos-gateway-linux-arm64.tar.gz > SHA256SUMS)
)
(
  scratch=$(mktemp -d "$RUNNER_TEMP/cpp-gateway-proof.XXXXXX")
  trap 'rm -rf -- "$scratch"' EXIT
  mkdir "$scratch/home" "$scratch/tmp"
  env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$scratch/home" TMPDIR="$scratch/tmp" \
    bash cpp/gateway/packaging/portable/verify.sh "$release/uos-gateway-linux-arm64.tar.gz" "$release/evidence" \
    2>&1 | tee "$release/proof.log"
)

work=/var/tmp/cppgw-release.ETONLl
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$work/b-home" TMPDIR="$work/b-tmp" PORTABLE_REBUILD=1 \
  timeout --kill-after=10s 45m bash cpp/gateway/packaging/portable/build.sh "$work/b/uos-gateway-linux-arm64.tar.gz"
cmp "$release/uos-gateway-linux-arm64.tar.gz" "$work/b/uos-gateway-linux-arm64.tar.gz"
(cd "$release" && sha256sum -c SHA256SUMS)
```

| Gate                                   | Elapsed   | Result                                                   |
| -------------------------------------- | --------- | -------------------------------------------------------- |
| Workflow build commands                | 167.21 s  | Archive produced using warmed C++ objects                |
| Workflow Debian proof commands         | 44.35 s   | 4/4 capped runs passed                                   |
| Independent gateway and native rebuild | 1053.15 s | 381/381 Ninja actions and a fresh KV build, same archive |

The second build reused the prepared compiler and library stages and skipped the C++ cache. `cmp` exited 0. Both
archives were 83,060,686 bytes and had SHA-256 `c84d463d566cc5b2e5c58f96db7875701e75e97a5e6e0f31be7862fdcf17073c`.
The archive audit counted 307 files and 156 directories, checked the three executable modes, uid and gid zero, the
commit timestamp on every tar entry, and the hash of the packaged KV command log. The gateway binary was 67,058,144
bytes with SHA-256 `c5349ae9bc1e50859db1b525ffaecc0aa6ab71fe4faf88777d2d62386dffa630`. The proof log SHA-256 was
`d3643d4f9c19b4df2f8b62aef13af841b2474245f79d6d58b8586a1184ca62f4`.

The existing proof sealed the same archive against glibc 2.36 on Debian 12 and 2.41 on Debian 13. Loader versions and
relocations resolved for the packaged executables and native addon version checks passed. Every run used two CPUs, zero
swap, a network-disabled container, fresh state and a loopback mock. Health reached HTTP 200 before load, after load
and after reopen. Unauthenticated chat returned 401. All 4,644 successful requests were accounted for after restart and
no memory limit or OOM events were recorded.

| Base      | Limit (MiB) | Chats and reopened settlements | Gateway peak RSS (MiB) | Cgroup peak (MiB) | Median requests/s |
| --------- | ----------- | ------------------------------ | ---------------------- | ----------------- | ----------------- |
| Debian 12 | 512         | 1161 / 1161                    | 37.40                  | 36.68             | 416.1             |
| Debian 12 | 256         | 1161 / 1161                    | 37.25                  | 36.58             | 405.9             |
| Debian 13 | 512         | 1161 / 1161                    | 38.63                  | 37.15             | 418.9             |
| Debian 13 | 256         | 1161 / 1161                    | 38.85                  | 37.12             | 403.8             |

Additional changed-surface checks passed with the following commands. Formatting ran on copies of the same tracked
files with the two frozen Bun lockfiles and Bun 1.4.0 in an external directory. A Linux test of the Mac archive writer
also produced equal bytes when input file times and permission bits varied.

```sh
actionlint -oneline .github/workflows/cpp-gateway-release.yml  # v1.7.9
shellcheck cpp/gateway/packaging/portable/{build,native}.sh
python3 -B -c 'import ast, sys; from pathlib import Path; [ast.parse(Path(p).read_text(), filename=p) for p in sys.argv[1:]]' \
  cpp/gateway/packaging/portable/{native-receipt,payload}.py cpp/gateway/packaging/macos/build.py
bun --no-env-file run format:check .github/workflows/cpp-gateway-release.yml cpp/gateway/docs/RELEASES.md \
  cpp/gateway/docs/findings/cppgw-release-workflow-20261118.md cpp/gateway/packaging/{portable,macos}/INSTALL.md
git diff --check da56e2277e0b0ef6ee0da3b3f504dfea16069bd2..HEAD
```

## Outstanding checks and handoff

The macOS job is unexecuted. Its workflow still requires an Apple Silicon run. The existing executable startup requires
Linux ELF, Linux native artifacts and Linux confinement, so the macOS archive is for development installation. The
Linux proof health response still reports `unknown` release identity from the existing runtime. Build identity is recorded
in `VERSION` and bound by the proof digests. Physical Raspberry Pi and Arch VPS installation and service acceptance
remain untested. [Native startup](../../runtime/startup.cpp) [Raspberry Pi requirements](../RASPBERRY-PI.md)

The regular HTTPS push dry run failed because GitHub authentication was unavailable. The work remains committed on the
clean local `agent-job` branch for coordinated integration. The local build and proof logs are under
`/var/tmp/cppgw-release.ETONLl/`. The acceptance SHA above identifies the packaging tree before this findings update.

## Summary

Added the ARM64 release workflow, local packagers, and install and rollback guide. Linux produced byte-identical
archives and passed all four Debian 12 and 13 proof runs. `actionlint` passed. The macOS job is unexecuted, and its
development package retains the current Linux startup requirement. The commits remain local because GitHub
authentication was unavailable.
