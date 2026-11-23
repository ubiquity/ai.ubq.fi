# Integrated native gateway on Apple Silicon

## Work contract v1

Qualify the integrated F1 tree with Apple Clang and libc++ on arm64 macOS. Build the five registry-selected vendors,
every production translation unit and the three native executables. Run the runtime tests first, then the complete
native suite. Build the macOS release archive with the workflow entry point and run its packaged `--version`.

| Surface          | Acceptance                                                                                                                                                |
| ---------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Native build     | The Darwin driver verifies vendor hashes, uses C11 for C and C++20/libc++ for C++, and links each test closure.                                           |
| Gateway behavior | Preserve functional and security decisions. Use Darwin-specific expectations for observed OS quirks.                                                      |
| Test isolation   | Keep the real Seatbelt network boundary, empty private `HOME`, fresh `TMPDIR` and credential-free test processes. All inference fixtures use local mocks. |
| Release package  | Build through `python3 -B cpp/gateway/packaging/macos/build.py` and verify the arm64 artifact and build identity.                                         |

Owned paths are `tools/macos-build.sh`, `docs/MACOS.md`, this report, Darwin paths in `include`, `src` and `tests`,
and standard-library portability fixes with unchanged Linux semantics. Shared build and packaging code, vendors,
workflows, TypeScript, live services, provider credentials and production state are outside this unit.

Starting commit is `2b23fb68` on the runner's clean `agent-job` checkout. The native driver already consumes the
registry. The first runs will establish whether compiler flags, test assumptions and package dependencies still fit
the integrated tree. Changed C++ is checked by the native compile and affected fixture, then the runtime and full
suites. Script syntax, changed Markdown formatting and `git diff --check` cover the other owned paths.

Budget 20 minutes for the cold runtime qualification and 30 minutes each for the full suite and release package,
using external scratch and one native driver at a time. Follow compiler counts and per-binary receipts, retain the
first diagnostics and reuse verified objects after bounded repairs. The local stop condition is the package version
receipt and a clean full-suite receipt or an exact record of each remaining failure.

## Runner and dependencies

The runner has macOS 26.7.2 build 25G332, Darwin 25.6.0, five CPUs and 14 GiB of RAM. Xcode 26.6 selects
Apple Clang 21.0.0 (`clang-2100.1.1.101`) and macOS SDK 26.5. The driver uses C++20 with libc++,
`-O0 -g0 -Wall -Wextra -Wshadow -Wconversion -Werror` and direct native-library bindings. The vendor C sources
use the Apple Clang C frontend in C11 mode.

```bash
HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ANALYTICS=1 \
  brew install boost openssl@3 sqlite zlib icu4c@78 brotli zstd cmake ninja python pkgconf
```

Homebrew 7.0.11 supplied Boost 1.92.0_1, OpenSSL 3.5.9, zlib 1.3.2, ICU 78.3_1, Brotli 1.2.1, Zstd 1.5.7_1
and Python 3.15.2. The driver selected the versioned SQLite 3.54.0 build below for the SQL fixtures. Homebrew's
installed SQLite version was 3.54.1. [Dependency recipe](../MACOS.md#prepare-a-clean-development-machine)

```bash
qual_root=/private/tmp/cppgw-macos-requal-f1-20261118
mkdir -m 700 "$qual_root"
mkdir -p "$qual_root"/{prep-home,prep-tmp,logs}
env -i PATH=/opt/homebrew/bin:/usr/bin:/bin HOME="$qual_root/prep-home" TMPDIR="$qual_root/prep-tmp" \
  QUAL_ROOT="$qual_root" python3 -B - <<'PY'
from pathlib import Path
import hashlib
import os
import subprocess
import zipfile
root = Path(os.environ['QUAL_ROOT'])
archive = root / 'sqlite.zip'
subprocess.run(['curl', '--fail', '--silent', '--show-error', '--location', '--max-time', '120',
                'https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip', '-o', archive], check=True)
assert hashlib.sha256(archive.read_bytes()).hexdigest() == '68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d'
prefix = root / 'sqlite-3.54.0'
(prefix / 'include').mkdir(parents=True)
(prefix / 'lib').mkdir()
source = root / 'sqlite3.c'
with zipfile.ZipFile(archive) as data:
    for target in (source, prefix / 'include/sqlite3.h'):
        target.write_bytes(data.read('sqlite-amalgamation-3540000/' + target.name))
dylib = prefix / 'lib/libsqlite3.dylib'
subprocess.run(['xcrun', 'clang', '-O2', '-dynamiclib', '-DSQLITE_THREADSAFE=1', '-DSQLITE_DQS=3',
                '-DSQLITE_ENABLE_COLUMN_METADATA=1', '-Wl,-install_name,' + str(dylib), source, '-o', dylib],
               check=True)
subprocess.run(['file', dylib], check=True)
PY
```

The prepared dylib is arm64 Mach-O with SHA-256
`4661036cc1d35d942eab62795480685ebdfed6d32450596126b117d3d008de22`.

## Native build and suite

Commit `b044a5125c5823ab3dabdad75127a6d6a0b4b67c` makes the Darwin driver archive every registry-selected
vendor, including a vendor used only by tests. Production executables and test binaries link the archives
selected by their respective source closures. The driver records the archive names and SHA-256s. On F1,
the three production links consume all five archives.

```bash
set -o pipefail
/usr/bin/time -l env -i PATH=/opt/homebrew/bin:/usr/bin:/bin HOME="$qual_root/prep-home" TMPDIR="$qual_root/prep-tmp" \
  bash cpp/gateway/tools/macos-build.sh --build-root "$qual_root/native" --sqlite-prefix "$qual_root/sqlite-3.54.0" \
  --jobs 5 --suite rt --require-clean 2>&1 | tee "$qual_root/logs/runtime-1.log"
mkdir -p "$qual_root/all-home" "$qual_root/all-tmp"
/usr/bin/time -l env -i PATH=/opt/homebrew/bin:/usr/bin:/bin HOME="$qual_root/all-home" TMPDIR="$qual_root/all-tmp" \
  bash cpp/gateway/tools/macos-build.sh --build-root "$qual_root/native" --sqlite-prefix "$qual_root/sqlite-3.54.0" \
  --jobs 5 --suite all --require-clean 2>&1 | tee "$qual_root/logs/full-1.log"
```

The cold runtime receipt and the first full receipt bind clean commit
`b044a5125c5823ab3dabdad75127a6d6a0b4b67c` and tree `5004fab69bbc17dca84ff7573730273b731d811f`.
Seatbelt preflight returned `loopback accepted; external network denied`. Each fixture ran with a private
empty home, a fresh temporary directory and a credential-free environment.

### Darwin native KV fixture repair

The first full run exited 1 in 757.34 s. It passed 449/450 binaries and 5,105/5,107 cases. The sole failed
binary was `tests/native_addon_differential_test.cpp`, with 2/4 case groups passed. Its first diagnostic
came from `native/runs/1795030574903312000/native_addon_differential_test.log`.

```text
native_addon_differential_test.cpp:145: verify inputs not json
  actual:   {"error":{"name":"SyntaxError","message":"Invalid JSON"}}
  expected: {"error":{"name":"SyntaxError","message":"JSON Parse error: Property name must be a string literal"}}
```

The original integration updated the inline Linux ARM64 vectors. Darwin's separately included vectors
still expected the recorded Bun parser text for two malformed files and two BOM-prefixed files. All
four reached the expected `SyntaxError`. Commit `aab86c74fab172eca13298c4da5ca21eb5463d69` updates those
four messages in the Darwin-only include. The test still checks the exact outcome of all 50 verification inputs and 28 bootstrap inputs. [Common parser diagnostic](../DEVIATIONS-FROM-BUN.md#json)

Source inspection found three Bun-only malformed-JSON messages in
`tests/native_addon_linux_x64_vectors.inc`, which belongs to the legacy comparison fixtures.

```bash
mkdir "$qual_root/focus-home" "$qual_root/focus-tmp" "$qual_root/rerun-home" "$qual_root/rerun-tmp"
/usr/bin/time -l env -i PATH=/opt/homebrew/bin:/usr/bin:/bin HOME="$qual_root/focus-home" TMPDIR="$qual_root/focus-tmp" \
  bash cpp/gateway/tools/macos-build.sh --build-root "$qual_root/native" --sqlite-prefix "$qual_root/sqlite-3.54.0" \
  --jobs 5 --suite all --filter native_addon_differential_test --require-clean 2>&1 | tee "$qual_root/logs/native-addon-focus.log"
/usr/bin/time -l env -i PATH=/opt/homebrew/bin:/usr/bin:/bin HOME="$qual_root/rerun-home" TMPDIR="$qual_root/rerun-tmp" \
  bash cpp/gateway/tools/macos-build.sh --build-root "$qual_root/native" --sqlite-prefix "$qual_root/sqlite-3.54.0" \
  --jobs 5 --suite all --require-clean 2>&1 | tee "$qual_root/logs/full-2.log"
```

The focused and final full receipts bind clean commit `aab86c74fab172eca13298c4da5ca21eb5463d69` and
tree `3a1e3c79d3eed21128d1708d10f7d2f8b6f83c21`. Both recorded unchanged build identity, inputs and
the Seatbelt preflight result. The Darwin vector file was the only build input changed since the
first full run.

| Gate                                                       | Result                              | Wall time                | Maximum RSS              |
| ---------------------------------------------------------- | ----------------------------------- | ------------------------ | ------------------------ |
| Cold runtime command                                       | 37/37 binaries, 478/478 cases       | 265.98 s                 | 1,396,899,840 bytes      |
| Focused fixture rerun                                      | 1/1 binary, 4/4 case groups         | 10.09 s                  | 1,396,965,376 bytes      |
| Final full command with cached production and test objects | 450/450 binaries, 5,107/5,107 cases | 509.92 s                 | 1,396,899,840 bytes      |
| Application phase of the full command                      | 413/413 binaries, 4,629/4,629 cases | Included in full command | Included in full command |

Apple Clang compiled 373/373 production translation units, including 44 runtime objects, 325 gateway
module objects, shared startup and three entry points. The application archive has 369 members. All
three arm64 executables linked successfully. The full gate accepted all 472 test and support compilations, reusing checked objects where inputs matched.
Runtime scope included 33 fetch transport, 17 WebSocket, 21 number and 3 IDNA cases. The IDNA binary
ran its exhaustive dataset in 26.075 s. The 450-binary registry and Links closures both validated.

| Vendored library | Compiled sources | Final full-run archive SHA-256                                     |
| ---------------- | ---------------- | ------------------------------------------------------------------ |
| Ada 4.0.0        | 1 C++            | `516f796df8c82ab52473a228dea5a8b0ea9da848ce21d0e0eaf8e7a736a2aea6` |
| llhttp 9.4.4     | 3 C              | `62fba429f1c851045c95c99c7ad91cf1725c8a1611a9a65588d71e882605571a` |
| QCBOR 1.6.1      | 5 C              | `5468a89249270894daa7195bac1ac173768a020febb4ba807e03aa296cc8866b` |
| simdjson 5.2.0   | 1 C++            | `1be4390c32101f8fa3852a986a51b0dadc57e43e63b1c2348fc0ecac42d6a833` |
| simdutf 9.4.0    | 1 C++            | `a476ed3a6e506413c3e3e04892ee60c78eda6f78ef235c4f4c6bc41d170aac80` |

Paths are relative to `$qual_root`. Native binaries and logs stay in that external scratch root.

| Receipt path                                      | SHA-256                                                            |
| ------------------------------------------------- | ------------------------------------------------------------------ |
| `native/runs/1795030276393774000/provenance.json` | `65cff73db0399c0568e38bf852075e96a843f465195fe234c5aba1de25543dce` |
| `native/runs/1795030276393774000/results.json`    | `83e2520f89b89f116250eb8737baffba76e0d0be628633d0db1a16ec7542daf6` |
| `native/runs/1795030574903312000/provenance.json` | `d853017b58bc1c3ed06c3ba407a2c7fd83f4c3fc4673d2d82692238f11bb7fb2` |
| `native/runs/1795031384001937000/provenance.json` | `565a92414611c6c249d6c2be2886c501b2bca3dcddd8ac397b678133626fc713` |
| `native/runs/1795030574903312000/results.json`    | `6f28808edca8d9aa3526017c0046a70f87db6d680b9c200705ff440b8d5bd72e` |
| `native/runs/1795031384001937000/results.json`    | `26f01dfcb29fabf1beefc64dd8fb8a31548102b7263a4628741e30798bb522cf` |

The focused pass is recorded in `native/runs/1795031373893300000/results.json`.

## Release package

The workflow's macOS build ran from the final full run's clean commit, using its default three workers and its own
checksum-verified SQLite. Its runtime gate passed 37/37 binaries and 478/478 cases.
The command took 399.86 s with maximum RSS 1,397,096,448 bytes.

```bash
mkdir "$qual_root/release" "$qual_root/package-home" "$qual_root/package-tmp" \
  "$qual_root/unpacked" "$qual_root/version-home" "$qual_root/version-tmp"
archive="$qual_root/release/uos-gateway-macos-arm64.tar.gz"
/usr/bin/time -l env -i PATH=/opt/homebrew/bin:/usr/bin:/bin HOME="$qual_root/package-home" TMPDIR="$qual_root/package-tmp" \
  python3 -B cpp/gateway/packaging/macos/build.py "$archive" 2>&1 | tee "$qual_root/logs/package-1.log"
(cd "$qual_root/release" && shasum -a 256 uos-gateway-macos-arm64.tar.gz > SHA256SUMS && shasum -a 256 -c SHA256SUMS)
tar --no-same-owner --no-same-permissions -xzf "$archive" -C "$qual_root/unpacked"
expected="uos-gateway $(git rev-parse HEAD)"
version=$(cd "$qual_root/unpacked" && env -i PATH=/usr/bin:/bin HOME="$qual_root/version-home" TMPDIR="$qual_root/version-tmp" \
  /usr/bin/sandbox-exec -p '(version 1)(allow default)(deny network*)' ./bin/uos-gateway --version)
test "$version" = "$expected"
printf '%s\n' "$version" | tee "$qual_root/logs/package-version.log"
```

The archive is 142,729,792 bytes with SHA-256
`284495ba43bde10c18666be0046e8854c498b6aae08a1eb9e2bab87535cccaed`. The three executables passed the packager's arm64 architecture,
system-only dynamic-link and strict code-signature checks. `VERSION` records tree
`3a1e3c79d3eed21128d1708d10f7d2f8b6f83c21`, macOS SDK 26.5 and minimum macOS 26.0.
The offline invocation printed `uos-gateway aab86c74fab172eca13298c4da5ca21eb5463d69` and exited 0.

The runtime receipt files were copied from the packager's temporary build directory before cleanup.
`package-evidence/provenance.json` has SHA-256 `212a3ca52e4462436249d3e85ff674c6e7c821e8d6258a779d112b77db656406`.
`package-evidence/results.json` has SHA-256 `2c4463a05e21121744da0ce4df9a7704e455b80b7d28aa66a0f49a6f175aed4a`.

## Changed-file checks and handoff

`bash -n cpp/gateway/tools/macos-build.sh`, the embedded Python syntax check,
`bash cpp/gateway/tools/ci-build.sh --list`, `python3 -B cpp/gateway/tools/links.py --check` and
`git diff --check` passed. `bun --no-env-file --no-install run format:check` passed on both changed Markdown
files in external scratch using Bun 1.4.0 and both frozen locks. The documentation commit follows the qualified
build and package commit in the branch history. Its diff contains the guide and report.

The package qualifies offline version reporting. macOS serving still requires Darwin startup and confinement. The earlier F1 paired
conformance findings remain with the integration report. [Mac package scope](../../packaging/macos/INSTALL.md) ·
[F1 conformance](cppgw-integrate-f1-20261118.md#paired-conformance)

The fetched `origin/development` tip was `a3a96bc5fe96ba383936746fae23e64daa69562f` and lacks the C++ gateway
tree. The job-branch push preflight exited 128. Git reported
`fatal: could not read Username for 'https://github.com': terminal prompts disabled`.
The clean `agent-job` checkout and verified thin Git bundle preserve the commits for the integration owner.
The job used local gates and loopback mocks. Local model review would require a provider connection, so review
remains with the integration owner.

```bash
git fetch --no-tags origin refs/heads/development:refs/remotes/origin/development
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/usr/bin/false git push --dry-run --porcelain origin \
  HEAD:refs/heads/agent-jobs/cppgw-macos-requal-f1-20261118
git bundle create "$qual_root/cppgw-macos-requal-f1-20261118.bundle" agent-job ^2b23fb6837e2b5c0dc9ffa50771f52d322dd413a
git bundle verify "$qual_root/cppgw-macos-requal-f1-20261118.bundle"
```

## Summary

- Apple Silicon compiled 373/373 production sources, archived all five vendors and linked all three executables. The runtime suite passed 37/37 binaries and 478/478 cases. After correcting four JSON error expectations in the Darwin KV fixture, the full suite passed 450/450 binaries and 5,107/5,107 cases.
- The macOS release archive built successfully. Its packaged `uos-gateway --version` returned the qualified commit `aab86c74`.
- Source inspection found three stale parser-message expectations in the legacy Linux x64 comparison vectors.
- GitHub write authentication blocked publication and the `development` handoff. The commits are preserved on `agent-job` and in a verified Git bundle.
