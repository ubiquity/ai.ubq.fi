# Apple Silicon C++ gateway release findings

## Work contract v1

Execute the Apple Silicon job's `run` blocks in workflow order, then prove two clean builds of the same commit
produce identical archives. Extract into fresh directories, exercise a local gateway health or version request,
check the Mach-O dependencies and third-party licences, and follow the Mac install and rollback instructions.
`actionlint` validates the workflow. Record the exact tree, commands, measurements and remaining boundaries.

Changes are limited to `packaging/macos/`, the workflow's `macos` job, the Mac sections of `RELEASES.md`, and this
report. Fix the Darwin build driver only if a reproduced failure requires it. Preserve Linux startup and the native
runtime tests. Use an empty home, fresh temporary directories, a scrubbed environment and local mocks for runtime
checks. Production service activation, database conversion and provider traffic are out of scope.

The starting commit is `23e097b75cd5c657797f5325ab4363b5bb27b828`, tree `531ff31ff2d1c3a9c226bb63b0ee038598027df7`.
GitHub Actions run `44307885223`, attempt 1, is a native arm64 runner with macOS 26.7.2 (25G332) and Apple Clang
21.0.0 (`clang-2100.1.1.101`). The prior release report covers its own Linux receipt. Mac linkage and the local
smoke surface are the outstanding acceptance checks.

## First run and repair

The original macOS install block passed in 21.17 seconds. The original build block exited 1 after 525.10 seconds,
with 367 out of 369 production translation units compiled. Apple Clang diagnosed `-Wsign-conversion` in the
vendored simdutf header included by `src/rt/base64.cpp` and in ICU headers included by `src/rt/string.cpp`. The
Darwin driver now marks those vendor inputs as system headers and records the vendored header hashes explicitly
in the object cache. First-party compiler flags and test expectations are retained.

The Mac archive exposes an offline version command backed by the packaged source commit. The Mac-only entrypoint
passes all other arguments to the existing gateway entrypoint. The workflow now checks the installed executable's
version from a fresh extraction under a network-denying Seatbelt profile.

At commit `21c922c6d62fedee77556115c64386b3beb246f3`, the corrected build compiled all 369 production sources and
passed 36 runtime binaries and 458 cases. It produced an archive, but the extracted version command failed with
`dyld: missing LC_UUID load command`. The workflow rehearsal failed after 1872.08 seconds. The linker had been told to omit
the UUID. The packager now uses Apple's `-reproducible` flag and the default content-derived UUID. Two signed link
probes with that flag had the same UUID, the same SHA-256 and exit status 0 when executed.

## Commands

From the repository root, I extracted each macOS YAML `run` value with `yq -r` and ran the two scripts in
workflow order. The original install block and the final install block were byte-identical. The runner
already had Bun 1.4.0. I installed the additional tools from the Mac guide and the workflow checkers.

```sh
HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ANALYTICS=1 brew install cmake ninja actionlint shellcheck
xcrun clang++ --version
xcrun --show-sdk-version
sw_vers
bun --version
```

The install block used the workflow Homebrew environment and `/bin/bash -e`.

```sh
test "$(uname -m)" = arm64
brew install boost openssl@3 sqlite zlib icu4c@78 brotli zstd python pkgconf
```

The original build block used `RUNNER_TEMP=/private/tmp/cppgw-release-macos.jnNBB5/original`
and `/bin/bash --noprofile --norc -eo pipefail`. It ran from the clean report-start commit
`9d603cf03ccdecaa7fc5d012fdd46f43dda83360`, tree `334b80a7e168c5e84c801e9b88490a659f2fa864`.

```sh
release="$RUNNER_TEMP/cpp-gateway-macos"
mkdir "$release"
python3 -B cpp/gateway/packaging/macos/build.py "$release/uos-gateway-macos-arm64.tar.gz"
(cd "$release" && shasum -a 256 uos-gateway-macos-arm64.tar.gz > SHA256SUMS)
```

The final build block was identical in both final runs. Each used new build, smoke and output directories.

```sh
release="$RUNNER_TEMP/cpp-gateway-macos"
scratch=$(mktemp -d "$RUNNER_TEMP/cpp-gateway-macos.XXXXXX")
trap 'rm -rf -- "$scratch"' EXIT
mkdir "$release" "$scratch/build-home" "$scratch/build-tmp"
env -i PATH=/opt/homebrew/bin:/usr/bin:/bin HOME="$scratch/build-home" TMPDIR="$scratch/build-tmp" \
  python3 -B cpp/gateway/packaging/macos/build.py "$release/uos-gateway-macos-arm64.tar.gz"
(cd "$release" && shasum -a 256 uos-gateway-macos-arm64.tar.gz > SHA256SUMS)
mkdir "$scratch/unpacked" "$scratch/home" "$scratch/tmp"
tar --no-same-owner --no-same-permissions -xzf "$release/uos-gateway-macos-arm64.tar.gz" -C "$scratch/unpacked"
expected="uos-gateway $(git rev-parse HEAD)"
version=$(cd "$scratch/unpacked" && env -i PATH=/usr/bin:/bin HOME="$scratch/home" TMPDIR="$scratch/tmp" \
  /usr/bin/sandbox-exec -p '(version 1)(allow default)(deny network*)' ./bin/uos-gateway --version)
test "$version" = "$expected"
printf '%s\n' "$version"
```

These are the final launch and comparison commands. The two passes also used different umasks.

```sh
work=/private/tmp/cppgw-release-macos.jnNBB5/verified
yq -r '.jobs.macos.steps[] | select(.name == "Install dependencies for the Apple Silicon build") | .run' \
  .github/workflows/cpp-gateway-release.yml > "$work/install.bash"
yq -r '.jobs.macos.steps[] | select(.name == "Build, test and package for Apple Silicon") | .run' \
  .github/workflows/cpp-gateway-release.yml > "$work/build.bash"
for attempt in a b; do
  trial="$work/$attempt"
  mkdir "$trial"
  (
    if test "$attempt" = a; then umask 022; else umask 077; fi
    HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ANALYTICS=1 /usr/bin/time -p /bin/bash -e "$work/install.bash" \
      > "$trial/install.log" 2>&1 &&
    RUNNER_TEMP="$trial" /usr/bin/time -p /bin/bash --noprofile --norc -eo pipefail "$work/build.bash" \
      > "$trial/build.log" 2>&1
  ) || exit 1
done
cmp "$work/a/cpp-gateway-macos/uos-gateway-macos-arm64.tar.gz" "$work/b/cpp-gateway-macos/uos-gateway-macos-arm64.tar.gz"
shasum -a 256 "$work"/{a,b}/cpp-gateway-macos/uos-gateway-macos-arm64.tar.gz
```

## Package receipts

Both builds used clean commit `824a7616061c52dc241d71ba1901f443adad7a8a`, tree
`a4e5f1ba90a6b03a9da9192ec045b136d3f11aa6`. The driver confirmed unchanged source identity and inputs on both
passes. The workflow now selects `macos-26`. The runner used Apple Clang 21.0.0 and macOS SDK 26.5. The
package records a minimum of macOS 26.0 for all three executables. The static inputs listed in `VERSION` were
Boost 1.92.0_1, OpenSSL 3.5.9, SQLite 3.54.0, ICU 78.3_1, Brotli 1.2.1, Zstd 1.5.7_1, zlib 1.4,
simdjson 5.2.0 and simdutf 9.4.0. The prepared SQLite source was checked against its pinned SHA-256
`68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d`.

| Result                            | Build A               | Build B               |
| --------------------------------- | --------------------- | --------------------- |
| Homebrew step elapsed             | 1.30 s                | 1.93 s                |
| Build and extracted smoke elapsed | 816.39 s              | 816.38 s              |
| Production translation units      | 369 / 369             | 369 / 369             |
| Runtime translation units         | 36 / 36               | 36 / 36               |
| Runtime binaries                  | 36 / 36               | 36 / 36               |
| Runtime cases                     | 458 / 458             | 458 / 458             |
| Driver run ID                     | `1795009255961159000` | `1795010073825644000` |

The runtime driver selected `--suite rt --require-clean` with three compile jobs. Seatbelt accepted loopback
and denied external networking in both runs. The result receipts are under `$work/{a,b}/receipt/`. The
`results.json` SHA-256 values are `32cfa25546b4e1285e385cbc4cfef76c8a874ab27f0d7d2fe8086578fcf1e7ad`
for A and `316e96e3b9c5b45e6dcb44e9459ba24a158b52384f058fd60c282ac723a09dea` for B.

`cmp` exited 0. Both archives are 140,078,145 bytes and have SHA-256
`5acac9c3ffbd1ce5561371cda22ddf18a13742c3f6373bfac0461f2d7b5e4d08`. The archive contains
56 files and 14 directories. Names are sorted, uid and gid are zero, and the owner and group names are
empty. The USTAR timestamp is the commit epoch `1795009210` for every member. The gzip timestamp is zero.
Executables and directories have mode `0755`. Other files have mode `0644`.

| Packaged file        | Bytes       | SHA-256                                                            |
| -------------------- | ----------- | ------------------------------------------------------------------ |
| `bin/uos-gateway`    | 197,488,552 | `a19159f17f48ab3cccf968b18d6e9db3cbcb9173047d5cfd836fdb72f3ea3208` |
| `bin/uos-kv-convert` | 197,487,936 | `bb379e8bbfd154f83db9c314e14f0860683f008d381726ee8d858072ac430b75` |
| `bin/uos-kv-proof`   | 197,487,936 | `0e152356b820bcb845780d9894573611adf2a8192c9cabd9c768574271dcac03` |
| `VERSION`            |             | `83eadb42f6221fa82e887602d5e10881d4a09f2c1f3b925e8cbb1bf63f061e89` |
| `INSTALL.md`         |             | `25f0518d15d019eed05dd8c0f421792e5dfd3384e1a6c0f859e3104806204541` |

All 13 licence and notice files matched their inputs with `cmp`. They cover Boost, Brotli, compiler-rt, ICU,
OpenSSL, simdjson, simdutf, SQLite, zlib and Zstd. `otool -L` listed only
`/usr/lib/libc++.1.dylib` and `/usr/lib/libSystem.B.dylib` for every executable. Each also passed
`lipo -archs`, `vtool -show-build` and `codesign --verify --strict`. The UUIDs in gateway, convert
and proof order were `5909DB8E-75B7-357C-83F8-DAB44E995ADE`, `E94AAAE4-3066-3621-9967-AF4BA0EE9892` and
`01B15CDA-606C-379A-A457-EB82E382BF71`.

## Install, local execution and rollback

I ran the macOS checksum, install and rollback shell blocks from `RELEASES.md` with a scratch installation root.
Both checksums returned `OK`. `file` reported arm64 Mach-O for all three executables in each installation.
The rollback selected the first extracted directory and rechecked its version. The two directories were separate
extractions of the same verified build identity.

I also ran the extracted gateway directly from the first installation. A local HTTP mock listened on an assigned
IPv4 loopback port. The test supplied a private launch manifest pointing to that mock, a new private state directory
and the extracted package. Both subprocesses ran under a network-denying Seatbelt profile. Each had its own empty
`HOME` and fresh `TMPDIR`, with only those variables and `PATH` set.

- `./bin/uos-gateway --version` returned `uos-gateway 824a7616061c52dc241d71ba1901f443adad7a8a` and exited 0.
- `./bin/uos-gateway --runtime-manifest "$manifest"` exited 1. The original startup preflight refused the missing
  Linux `manifest.sha256`. The mock accepted zero connections, and the isolated homes, temp directories and mock
  state stayed empty.

The check commands and output are retained in the external scratch directory.

```sh
/bin/bash --noprofile --norc -eo pipefail "$work/verify-installed.bash" "$work"
python3 -B "$work/record-build-proof.py" "$work"
release=$(tail -1 "$work/audit-install/install-a.log")
/usr/bin/tar -tzf "$work/a/cpp-gateway-macos/uos-gateway-macos-arm64.tar.gz" > "$work/archive-members.txt"
for executable in uos-gateway uos-kv-convert uos-kv-proof; do
  /usr/bin/otool -L "$release/bin/$executable" &&
  /usr/bin/vtool -show-build "$release/bin/$executable" &&
  /usr/bin/lipo -archs "$release/bin/$executable" &&
  /usr/bin/codesign --verify --strict "$release/bin/$executable" || exit
done > "$work/extracted-mach-o.log"
```

The install rehearsal extracts into `$work/audit-install/home/uos-gateway/releases/`. Its exact executed commands
are in `$work/install-docs.bash` and `$work/audit-install/rollback-docs.bash`. The mock commands and diagnostics
are in `$work/audit-install/audit.log`.

The runtime audit log SHA-256 is `9f776d8e36ce965037378dc4374cd8fe0f6a1844819135a72c8a9160d26111cf`.
The installed directory changed from `macos.U3sMgH` to `macos.N69hzG` in the rollback rehearsal.

## Static checks and handoff

The scoped gates covered the workflow, the Darwin build driver and packager, and the changed documentation. The
formatter used pinned Bun 1.4.0 and frozen installs from both lockfiles in an external copy.

```sh
actionlint -oneline .github/workflows/cpp-gateway-release.yml
shellcheck cpp/gateway/tools/macos-build.sh
bash -n cpp/gateway/tools/macos-build.sh
python3 -B - <<'PYTHON'
import ast
from pathlib import Path
package = Path("cpp/gateway/packaging/macos/build.py")
driver = Path("cpp/gateway/tools/macos-build.sh")
ast.parse(package.read_text(), filename=str(package))
ast.parse(driver.read_text().split("<<'PYTHON'\n", 1)[1].rsplit("\nPYTHON", 1)[0], filename=str(driver))
PYTHON
bun --no-env-file run format:check .github/workflows/cpp-gateway-release.yml cpp/gateway/docs/RELEASES.md \
  cpp/gateway/docs/findings/cppgw-release-macos-20261118.md cpp/gateway/packaging/macos/INSTALL.md
git diff --check 23e097b75cd5c657797f5325ab4363b5bb27b828..HEAD
```

All passed, including `actionlint` 1.7.12. The archive receipts bind the package commit above. The later findings
commit records those results.

The Mac package can run the offline version command. Serving and KV execution still use the sealed Linux startup
path in `cpp/gateway/runtime/startup.cpp`. That path and the native KV and permissions artifacts are outside this
package work. The full application test suite, hosted artifact upload, older macOS versions and production service
acceptance remain unverified. The prior release job carries the Linux proof.

The GitHub push dry run exited 128 because GitHub authentication was unavailable. I fetched
`origin/development` at `2ac5aae69b0f0bac77cf1c6f329995cf1187616c`. The packaged commit is outside its
ancestry, so the work remains committed on the clean local `agent-job` branch. The release archives and receipts
are outside the checkout, under `/private/tmp/cppgw-release-macos.jnNBB5/verified`.

```sh
git fetch --no-tags origin refs/heads/development:refs/remotes/origin/development
git merge-base --is-ancestor 824a7616061c52dc241d71ba1901f443adad7a8a origin/development
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/usr/bin/false git push --dry-run origin \
  HEAD:refs/heads/agent-jobs/cppgw-release-macos-20261118-44307885223-1
```

## Summary

The `macos-26` job commands passed on Apple Silicon. Two independent builds with content-derived Mach-O UUIDs passed
36 runtime binaries and 458 cases each and produced byte-identical archives. The extracted gateway returned its
commit via `--version` and exited 0 with zero mock connections. The system-library, licence, install, rollback and
`actionlint` checks passed. Gateway serving and KV operations still require Linux. GitHub authentication blocked
push and integration.
