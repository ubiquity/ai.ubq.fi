# Integrated C++ gateway follow-ups on Apple Silicon

## Work contract v1

Qualify the integrated C++ gateway with Apple Clang and libc++ on arm64 macOS. The runner's clean `agent-job`
checkout starts at `824687e6b5e2f561ebc588f3456f83e33e1b3228` (tree `3cf7060f40468468b031b292e551f945e2f141c0`),
the tip of `agent-jobs/cppgw-followups-1-20261118-44398005611-1`.

- Compile the native source and registered vendors, including QuickJS-ng libregexp, and link all three executables.
- Verify the C++20 calendar, helper cleanup and filesystem paths in the registered fixtures. Preserve validation,
  authentication, wire and storage contracts, and accounting.
- Use the Darwin Seatbelt boundary, an empty home, a fresh temporary directory and a credential-free environment.
  Provider fixtures use loopback mocks.
- Run the macOS workflow's Python build entry point and check the extracted arm64 gateway's `--version` against the
  source commit.

Owned paths are `tools/macos-build.sh`, `docs/MACOS.md`, this report, Darwin-guarded paths under `include`,
`src` and `tests`, and standard-library portability fixes that leave Linux behavior intact. The Python packager,
vendor sources and registry, workflow files, TypeScript, other `cpp/` directories and service activation are outside
this unit. Keep all build output and dependency downloads in external scratch.

Use `bash cpp/gateway/tools/macos-build.sh` with `--suite rt --require-clean`, then `--suite all --require-clean`,
then `python3 -B cpp/gateway/packaging/macos/build.py` and an offline `--version` check. For any changed source,
rerun its affected fixture before the full gate. Check the shell driver with `bash -n` if edited. Check changed
Markdown with `bun --no-env-file run format:check` and the final diff with `git diff --check`.

At the start of this job, the new calendar, regex vendor and package linkage awaited qualification on this host.
Use their observed diagnostics to decide whether a fix is needed. Budget 10 minutes for dependency preparation, 25 for the cold runtime build,
35 for the full suite, and 30 for packaging. Keep one build driver active at a time and follow compile progress
and the terminal receipts. Reuse valid objects after a repair and retain each failed gate's first diagnostics.
Stop with exact passing suite and package receipts or a diagnosis of each remaining failure.

## Runner and dependencies

The job ran on macOS 26.7.2 build 25G332, Darwin 25.6.0, with three arm64 CPUs and 7 GiB of RAM.
Xcode 26.6 selected Apple Clang 21.0.0 (`clang-2100.1.1.101`) and macOS SDK 26.5.
The driver used C++20 and libc++ with `-O0 -g0 -Wall -Wextra -Wshadow -Wconversion -Werror` for first-party
sources. QuickJS-ng's two vendor sources compiled as GNU C11.

```bash
qual_root=/private/tmp/cppgw-macos-requal-fu1-20261118
mkdir -m 700 "$qual_root"
mkdir -p "$qual_root"/{brew-home,brew-tmp,brew-cache,prep-home,prep-tmp,logs}
env -i PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin HOME="$qual_root/brew-home" \
  TMPDIR="$qual_root/brew-tmp" HOMEBREW_CACHE="$qual_root/brew-cache" \
  HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ANALYTICS=1 \
  brew install boost openssl@3 sqlite zlib icu4c@78 brotli zstd cmake ninja python pkgconf
```

Homebrew 7.0.11 supplied Boost 1.92.0_1, OpenSSL 3.5.9, zlib 1.4, ICU 78.3_1, Brotli 1.2.1 and Zstd 1.5.7_1.
CMake 4.5.0, Ninja 1.13.2, Python 3.15.2 and pkgconf 3.0.11 were available. The fixture explicitly
selected the prepared SQLite 3.54.0 dylib. Homebrew SQLite 3.54.1 and ICU 79 stayed installed.
[Preparation recipe](../MACOS.md#prepare-a-clean-development-machine)

```bash
env -i PATH=/opt/homebrew/bin:/usr/bin:/bin HOME="$qual_root/prep-home" TMPDIR="$qual_root/prep-tmp" \
  QUAL_ROOT="$qual_root" python3 -B - <<'PY'
from pathlib import Path
import hashlib
import os
import subprocess
import zipfile

root = Path(os.environ["QUAL_ROOT"])
archive = root / "sqlite.zip"
subprocess.run(["curl", "--fail", "--silent", "--show-error", "--location", "--max-time", "120",
                "https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip", "-o", archive], check=True)
assert hashlib.sha256(archive.read_bytes()).hexdigest() == "68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d"
prefix = root / "sqlite-3.54.0"
(prefix / "include").mkdir(parents=True)
(prefix / "lib").mkdir()
source = root / "sqlite3.c"
with zipfile.ZipFile(archive) as data:
    for target in (source, prefix / "include/sqlite3.h"):
        target.write_bytes(data.read("sqlite-amalgamation-3540000/" + target.name))
dylib = prefix / "lib/libsqlite3.dylib"
subprocess.run(["xcrun", "clang", "-O2", "-dynamiclib", "-DSQLITE_THREADSAFE=1", "-DSQLITE_DQS=3",
                "-DSQLITE_ENABLE_COLUMN_METADATA=1", "-Wl,-install_name," + str(dylib), source, "-o", dylib],
               check=True)
subprocess.run(["file", dylib], check=True)
subprocess.run(["shasum", "-a", "256", dylib], check=True)
PY
```

The SQLite preparation took 23.29 s and produced an arm64 Mach-O dylib with SHA-256
`5a18cb8c2c6cd33d495432e149fa867d34445a1d0eb5ee3de1840db9d665ea29`.

## Native suite and packaging commands

The runtime, full suite and release build ran in that order from clean commit
`53e8ef88cdccc698e29fc079bd7bd79f9dc61405`, tree `02514762c0a7cf9f0b140534e62efd5e31754b82`.
The only change from the supplied branch at that point was this report's work contract.

```bash
set -eo pipefail
mkdir "$qual_root"/{runtime-home,runtime-tmp,full-home,full-tmp,package-home,package-tmp,release}
gate() {
  local phase="$1"
  shift
  local status=0
  /usr/bin/time -l env -i PATH=/opt/homebrew/bin:/usr/bin:/bin HOME="$qual_root/$phase-home" TMPDIR="$qual_root/$phase-tmp" \
    "$@" 2>&1 | tee "$qual_root/logs/$phase-1.log" || status=$?
  printf '%s\t%s\n' "$phase" "$status" | tee -a "$qual_root/logs/gates.tsv"
  return "$status"
}
test -z "$(git status --porcelain)"
git rev-parse HEAD 'HEAD^{tree}' > "$qual_root/logs/qualified-tree.txt"
gate runtime bash cpp/gateway/tools/macos-build.sh \
  --build-root "$qual_root/native" --sqlite-prefix "$qual_root/sqlite-3.54.0" --jobs 3 --suite rt --require-clean
gate full bash cpp/gateway/tools/macos-build.sh \
  --build-root "$qual_root/native" --sqlite-prefix "$qual_root/sqlite-3.54.0" --jobs 3 --suite all --require-clean
gate package python3 -B cpp/gateway/packaging/macos/build.py "$qual_root/release/uos-gateway-macos-arm64.tar.gz"
```

Each fixture used the driver's Seatbelt boundary and its own empty home, temporary directory and working directory.
The preflight logged `loopback accepted; external network denied`. All inference fixtures used local mocks.

## Native results

| Gate                           | Binaries passed | Cases passed | Wall time (s) | Maximum RSS (bytes) |
| ------------------------------ | --------------: | -----------: | ------------: | ------------------: |
| Cold standalone runtime        |           37/37 |      485/485 |        498.25 |       1,310,523,392 |
| Full suite with cached objects |         450/450 |  5,119/5,119 |      1,472.22 |       1,201,324,032 |
| Application phase              |         413/413 |  4,634/4,634 |      Included |            Included |

The cold run compiled all 373 production translation units, including 44 runtime objects, shared startup and the
three entry points. The application archive had 369 members. The driver built 13 vendor objects across six archives
and recorded all six archives on each of the three successful arm64 production links. The full gate reused all 373
production and 37 runtime test objects, and compiled all 435 application test and support objects.

The date fixture passed all seven cases and the QuickJS-ng regex fixture passed all 20. The full suite also passed
the native startup lookup (19 cases), native addon differential (4 case groups), static assets (38 cases),
static differential (1), legacy codec resource lookup (4), Codex quota differential (3) and Responses HTTP
conformance (9). The integrated source, shell driver, release packager and test expectations passed as supplied.

| Receipt under `$qual_root/native/runs` | SHA-256                                                            |
| -------------------------------------- | ------------------------------------------------------------------ |
| `1795039815099629000/provenance.json`  | `5574fed8b0944983534747155b1469a14c25ee9a734dd1c666c248930a0bc2f8` |
| `1795039815099629000/results.json`     | `fb6f1863f56c433839c57bdfa869ac120e0f5eeb4633518d76e05ca8b7be0cf9` |
| `1795040313462494000/provenance.json`  | `3c886d0cec1e9ad91d2c6505b7d7e6caa98c521ae26b3188e1ce91996303120a` |
| `1795040313462494000/results.json`     | `9e0d1d9a9fc2a24701502718dd15de45f65f1f61b1962b7b2771ffaad31288a0` |

The full receipt records QuickJS-ng's `libuos_vendor_quickjs-libregexp.a` SHA-256 as
`5519f5d6ebe9d38cb0e2c6cd1b3bc26784a418f5a8b6c52d7da185dce92f89e7`.

## Release archive

The packager took 748.99 s with a maximum RSS of 1,315,078,144 bytes. It prepared and checked its own SQLite
3.54.0, then repeated the runtime pass with 37/37 binaries and 485/485 cases. Its three executables passed the
arm64, system-only dylib and strict code-signature checks. The packager runtime receipt was captured before
temporary work cleanup.

| Receipt under `$qual_root/package-evidence` | SHA-256                                                            |
| ------------------------------------------- | ------------------------------------------------------------------ |
| `provenance.json`                           | `48f4bf6810ea0cb0439075e96d27d73ad74f3b2b73cc005fba941d7b4db085a4` |
| `results.json`                              | `b325443a592b76d8a70489c491db9c50c261c0967067e840df60c771a04a7d41` |

```bash
archive="$qual_root/release/uos-gateway-macos-arm64.tar.gz"
(cd "$qual_root/release" && shasum -a 256 uos-gateway-macos-arm64.tar.gz > SHA256SUMS && shasum -a 256 -c SHA256SUMS)
mkdir "$qual_root"/{unpacked,version-home,version-tmp}
tar --no-same-owner --no-same-permissions -xzf "$archive" -C "$qual_root/unpacked"
expected="uos-gateway $(git rev-parse HEAD)"
version=$(cd "$qual_root/unpacked" && env -i PATH=/usr/bin:/bin HOME="$qual_root/version-home" TMPDIR="$qual_root/version-tmp" \
  /usr/bin/sandbox-exec -p '(version 1)(allow default)(deny network*)' ./bin/uos-gateway --version)
test "$version" = "$expected"
printf '%s\n' "$version" | tee "$qual_root/logs/package-version.log"
```

The archive is 142,557,927 bytes with SHA-256
`a18fdb7f9ceb2e0aaea52c829a43d6a8675a177dd7fbc638eb60dc0f1dd62e6f`.
`VERSION` records the verified commit and tree, SDK 26.5, minimum macOS 26.0 for all three executables and
QuickJS-ng libregexp 0.18.0. The packaged QuickJS-ng license matched the vendored original.
The extracted executables passed `xcrun lipo -archs` and `/usr/bin/codesign --verify --strict` again.
`xcrun nm -gU bin/uos-gateway` found the linked `_lre_compile` and `_lre_exec` symbols.
The extracted gateway exited 0 and printed `uos-gateway 53e8ef88cdccc698e29fc079bd7bd79f9dc61405`.

An independent check matched the results to their registered selections and verified the per-binary exit status
and case counts, unchanged source identity, all 1,475 source input hashes and the six archive names on every
native production link. The runtime, full and package receipts all matched.

```bash
env -i PATH=/opt/homebrew/bin:/usr/bin:/bin HOME="$qual_root/evidence-home" TMPDIR="$qual_root/evidence-tmp" \
  python3 -B "$qual_root/verify-receipts.py" "$qual_root"
```

## Changed files and handoff

The only changed paths are `cpp/gateway/docs/MACOS.md` and this report. Their scoped
`bun --no-env-file --no-install run format:check` passed in scratch with Bun 1.4.0 and both frozen lockfiles.
`git diff --check` passed. `bash -n cpp/gateway/tools/macos-build.sh` passed, and
`python3 -B cpp/gateway/tools/links.py --check` and `bash cpp/gateway/tools/ci-build.sh --list` each covered all
450 registered binaries. Build output, the archive and receipts are under `$qual_root`.

`docs/RUNTIME.md` still lists Apple Silicon packaging as pending in its opening paragraph. The updated
Mac build guide carries the package result. Mac serving still requires separate startup and confinement
qualification. [Mac platform boundaries](../MACOS.md#proven-darwin-behavior-and-remaining-boundaries)

GitHub CLI had no active authentication. The push preflight exited 128 with
`fatal: could not read Username for 'https://github.com': terminal prompts disabled`.
The fetched `origin/development` at `5ddac3dd154d74b1049197fb7cf5f53551609072` has no `cpp/gateway` tree.
The checkout is clean on `agent-job`. The thin bundle carries the changes atop the supplied branch tip.

```bash
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/usr/bin/false git push --dry-run --porcelain origin \
  HEAD:refs/heads/agent-jobs/cppgw-macos-requal-fu1-20261118
git bundle create "$qual_root/cppgw-macos-requal-fu1-20261118.bundle" agent-job ^824687e6b5e2f561ebc588f3456f83e33e1b3228
git bundle verify "$qual_root/cppgw-macos-requal-fu1-20261118.bundle"
git fetch --no-tags origin refs/heads/development:refs/remotes/origin/development
```

## Summary

- Apple Silicon passed 37/37 runtime binaries and 485/485 runtime cases. The full suite passed 450/450 binaries and 5,119/5,119 cases.
- The macOS arm64 package built successfully. Its extracted gateway returned `uos-gateway 53e8ef88cdccc698e29fc079bd7bd79f9dc61405` from `--version`.
- GitHub write authentication blocked publication and the `development` handoff. The documentation commits are preserved on `agent-job` and in a verified Git bundle.
