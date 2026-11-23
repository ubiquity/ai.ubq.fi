# Native C++ gateway final integration

## Work contract v1

Merge the reviewed release integration and then the regex and runtime cleanup as ordinary two-parent merges.
The starting checkout is `45118332b287b7ba9534bb66ab1518f1990a9d9e` on `agent-job`, at the published port branch tip.
Ubuntu 24.04.5 on Linux ARM64 supplies four cores, 15 GiB of RAM, Bun 1.4.0 and passwordless namespace setup.

| Surface                          | Required behavior                                                                                                                                                                                            |
| -------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| JSON, HTTP and WebSocket         | Retain simdjson, the 128-container JSON limit, llhttp and Beast. Preserve request and response formats, statuses, delivery and durable settlement.                                                           |
| Regex and both KV codecs         | Use QuickJS-ng libregexp. Preserve malformed-value rejection, stored bytes, graph bounds and the typed values offered by Bun.                                                                                |
| Filesystem and numbers           | Use standard filesystem paths for trusted resources. Retain request path validation and authorization, numeric formatting, quota and paid accounting.                                                        |
| Vendors, workflows and packaging | Use a single registry for dependencies and a single workflow for Release, sanitizers and robustness. Preserve vendor bytes, tests from both sides, target-specific packaging, and default Linux confinement. |

The integration owns `cpp/gateway/` and the C++ gateway workflows. TypeScript, the other C++ projects, production
services, credentials and live provider access are outside the work. Dependency preparation and outputs use `/var/tmp`.
Tests use fresh homes and temporary directories, an empty environment with allowlisted settings, real fixture isolation
and loopback mocks. The four reported merge conflicts, final suite totals and package and fuzz outcomes are verified below.

Acceptance requires all four `ci-build.sh --config release --shard N/4` selections, a covering `--config asan` selection,
`python3 cpp/gateway/tools/links.py --check`, `actionlint`, two 600-second workflow fuzz campaigns and
packaged native versus Bun agreement on all 186 cases. The stop condition is a clean, committed tree preserving both
incoming merge tips, current guides, exact receipts and a record of any remaining blockers.

## Incoming history

| Input                                          | Fetched tip                                | Prior Linux ARM64 Release result |
| ---------------------------------------------- | ------------------------------------------ | -------------------------------: |
| Release integration and packaging              | `2b23fb6837e2b5c0dc9ffa50771f52d322dd413a` |        450 binaries, 5,104 cases |
| Filesystem, numeric and QuickJS-ng integration | `9acb016ae9900ffac255b7f3cec02c735f6ddeda` |        448 binaries, 5,095 cases |

Both inputs include `af7a0036b823ef2357c26b9b5f03f2677796b775`. The first input descends from the starting tip.

| Merge                                                   | Commit                                     |
| ------------------------------------------------------- | ------------------------------------------ |
| Release integration into the port branch                | `fbbfef06689d0b1bd82abecae638062a8c19d5c8` |
| QuickJS-ng and runtime cleanup into the integrated tree | `ecce29dc92c627b669680b557a5dec3cd7124dea` |

The regex merge keeps both sets of deviation notes, combines the vendor registrations and fuzz targets, and uses the
unified CI workflow's full sanitizer inventory. The retired standalone sanitizer runner stays deleted. Work resumed
from these two committed merges and the package build that was already running against the merged source.

[Release integration findings](cppgw-integrate-f1-20261118.md) · [Regex and runtime integration findings](cppgw-integrate-s3b-20261118.md)

Runner scratch is `/var/tmp/cppgw-integrate-f2.zcCiN79E`. Dependency preparation, compilation and verification use
persistent execution handles. Logs and receipts remain in scratch.

## Merge checks

- The 450 executable test files are the union of both parents. Both edits to
  `kv_conversion_legacy_codec_differential_test.cpp` and `native_addon_differential_test.cpp` survived. They exercise
  paths containing spaces and URL punctuation and retain the JSON rejection expectations.
- The six linked vendors and 25 fuzz targets are the exact, duplicate-free union of both branches. The unified
  workflow selects every test for Release and sanitizers, including the regex and stored-value cases. Its fuzz jobs
  discover both regex harnesses and `kv_jsc` from the registry. The portable driver uses GNU C11 for the QuickJS-ng
  objects in both configurations. The retained regex source filenames hold the adapter. The retired numeric helpers
  have no production, test or build references.
- The QuickJS-ng directory byte-compares with the incoming regex branch. The other vendor directories byte-compare
  with the first merge. Linux package versions and notices, and the macOS package's vendor archive list and licenses,
  resolve from that same registry.

```bash
git merge-base --is-ancestor 2b23fb68 HEAD
git merge-base --is-ancestor 9acb016a HEAD
git diff --exit-code fbbfef06 ecce29dc -- cpp/gateway/third_party \
  ':(exclude)cpp/gateway/third_party/quickjs-libregexp/**'
git diff --exit-code 9acb016a HEAD -- cpp/gateway/third_party/quickjs-libregexp
python3 -B cpp/gateway/tools/vendor_sources.py cmake
python3 -B cpp/gateway/tools/links.py --check
```

Links resolved all 450 binaries. Both surviving C++ workflows passed actionlint 1.7.12 with shellcheck and this
scratch configuration. GitHub lists `ubuntu-26.04-arm` as a hosted label. This actionlint release's built-in list
predates that label, and the first lint invocation reported it at all three uses.
[Runner image list](https://github.com/actions/runner-images#available-images)

```bash
cat > "$scratch/actionlint/github-runner-labels.yml" <<'YML'
self-hosted-runner:
  labels:
    - ubuntu-26.04-arm
YML
"$scratch/actionlint/bin/actionlint" -config-file "$scratch/actionlint/github-runner-labels.yml" \
  .github/workflows/cpp-gateway-ci.yml .github/workflows/cpp-gateway-release.yml
```

## First-party size

Physical line counts come from tracked Git blobs. The baseline is `55afcc8b13b2ddd10747355745da19e5264f7b76`,
tree `5514adb01452ecaa20719fef73b6bf16c7e84b1a`. The integrated source is
`ecce29dc92c627b669680b557a5dec3cd7124dea`, tree `f5de80d2c409570ff2b817a55f910137cef826e9`. C++ totals take
the `.cpp` and `.hpp` files in `src/`, `include/`, `runtime/` and `entrypoints/`, plus
`packaging/macos/gateway-version.cpp`. They exclude vendors, tests, the two runtime assertion headers, benchmarks,
build tooling and the launch schema JSON. The broad directory count from the preceding integration includes the
two assertion headers and two schema files, and goes from 191,526 to 179,638.

| First-party production C++ path |      Before |       After |
| ------------------------------- | ----------: | ----------: |
| `src/`                          |     151,419 |     139,530 |
| `include/`                      |      39,201 |      39,170 |
| `runtime/`                      |         489 |         508 |
| `entrypoints/`                  |          67 |          69 |
| macOS version entry point       |           0 |          11 |
| **Total**                       | **191,176** | **179,288** |

The file groups include the retained policy and adapters. The HTTP measurements include the session code that
shares the parser's file and the outbound connection adapter. [Replacement plan](../OSS-REPLACEMENT-PLAN.md)

| Replaced or slimmed module                  | Library or standard facility                             | Before | After |
| ------------------------------------------- | -------------------------------------------------------- | -----: | ----: |
| ECMAScript regex engine                     | QuickJS-ng libregexp                                     |  6,318 |   901 |
| URL, IDNA and query handling                | Ada with gateway validation                              |  2,018 |   662 |
| Unicode, UTF and Base64                     | ICU and simdutf                                          |  3,261 | 1,701 |
| JSON parser and writer                      | simdjson and Boost.JSON                                  |  1,928 | 1,189 |
| Passkey CBOR and certificate decoding       | QCBOR and OpenSSL                                        |  5,072 | 4,197 |
| HTTP request and Fetch response parsing     | llhttp                                                   |  2,748 | 2,763 |
| WebSocket handshake and framing             | Boost.Beast and llhttp                                   |  2,987 | 2,880 |
| BigInt and numeric helpers                  | Boost.Multiprecision and C++20                           |  1,253 |   884 |
| Dates and lexical paths                     | C++20                                                    |  2,106 | 1,411 |
| Environment, ordered containers and console | POSIX, C++20 and shared Boost.JSON serialization         |  1,497 |   915 |
| Gzip, headers and request bodies            | Shared zlib adapter and C++20 string and byte operations |  2,524 | 2,061 |

```bash
python3 -B "$scratch/production-lines.py" 55afcc8b1 ecce29dc
```

The counter reads each blob from `git ls-tree -rz` using `git cat-file --batch` and counts newlines. Group paths
use the `src/rt/` and `include/uos/rt/` implementation families for regex, URL, string/text_codec/base64,
json/json_simdjson/json_document, websocket and number. The passkeys row is `src/auth/passkeys.cpp`. The HTTP row
counts `src/runtime/http.cpp` and `src/rt/fetch_transport_{http.cpp,internal.hpp}`. The final three rows use files
named for the listed modules under `src/rt/` and `include/uos/rt/`, including `fs`, `env`, `ordered.hpp`, and `http`.

## Runner and dependencies

The runner uses Ubuntu 24.04.5 with kernel `7.0.0-1017-azure`, Clang 21.1.8 from apt.llvm.org, OpenSSL 3.0.13,
zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5. The compiled upstream libraries below stay outside the checkout.

| Upstream archive | SHA-256                                                            |
| ---------------- | ------------------------------------------------------------------ |
| Boost 1.92.0     | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` |
| ICU4C 78.3       | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |
| SQLite 3.54.0    | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |

Boost staged static JSON and Container archives. ICU configured in `$scratch/icu-build` with
`../icu/source/configure --prefix="$scratch/icu78" --disable-tests --disable-samples --disable-extras --disable-static`.
SQLite configured with `./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'`.
The portable driver independently checks the SQLite headers and loaded version in the isolated fixture.
[Preparation guide](../CI.md#ubuntu-prerequisites) · [Upstream Boost and ICU build commands](../CONFORMANCE.md#run-on-linux-arm64)

## Release and sanitizer suites

All four round-robin Release shards linked and passed every binary. The focused sanitizer selection included all
37 `tests/rt/` binaries, fixtures with `passkey`, `kv`, `http`, `websocket`, `regex`, `permissions`, `static` or
`url` in their paths, and all test closures that link `src/runtime/http.cpp`. Each gate built or reused all 369
production objects and six vendor archives. The 161 sanitizer run logs had zero ASan, UBSan or LSan diagnostics.
The separate portable driver controls passed 7/7 in 13.842 seconds.

```bash
for shard in 1 2 3; do
  bash "$scratch/run-gate.sh" release 3 \
    bash cpp/gateway/tools/ci-build.sh --config release --shard "$shard/4"
done
bash "$scratch/run-gate.sh" release 4 bash cpp/gateway/tools/ci-build.sh --config release --shard 4/4
bash "$scratch/run-gate.sh" asan 3 python3 -B "$scratch/asan-focus.py" "$PWD/cpp/gateway" --config asan
python3 -B "$scratch/verify-final-receipts.py" "$scratch/receipts/final"
```

The sanitizer selector substitutes only the test inventory in the standard `ci-build.sh` driver. The wrapper used
the following environment for both configurations. `instrument` was empty for Release and
`-gline-tables-only -fno-sanitize-recover=all` for ASan. Each test in turn ran as UID 1001 in fresh Linux network,
mount and PID namespaces with loopback-only mocks. The three-worker gates overlapped with other verification.

```bash
session=$(mktemp -d "$scratch/gate.XXXXXXXX")
mkdir "$session/home" "$session/tmp"
env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/lib/llvm-21/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
  HOME="$session/home" TMPDIR="$session/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS="$jobs" \
  UOS_CI_BUILD_DIR="$scratch/ci" UOS_CI_SQLITE_PREFIX="$scratch/sqlite" \
  PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
  UOS_CI_CFLAGS="$instrument" UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0 $instrument" \
  UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
  "$@"
```

| Linux ARM64 gate         | Passing binaries |   Passing cases | Compile (s) | Link and test (s) | Driver wall (s) |
| ------------------------ | ---------------: | --------------: | ----------: | ----------------: | --------------: |
| Release 1/4              |          113/113 |     1,333/1,333 |   2,781.981 |            48.707 |       2,831.566 |
| Release 2/4              |          113/113 |     1,236/1,236 |     721.155 |            33.639 |         755.682 |
| Release 3/4              |          112/112 |     1,440/1,440 |     574.069 |            23.893 |         598.515 |
| Release 4/4              |          112/112 |     1,104/1,104 |     306.247 |            18.649 |         325.539 |
| **Release total**        |      **450/450** | **5,113/5,113** |             |                   |                 |
| **ASan, UBSan and LSan** |      **161/161** | **1,989/1,989** |   4,160.668 |           194.733 |       4,357.037 |

The driver recorded source SHA-256 `4cb3e57b1a8692013237acf157e5c766c62c75bae95b247d237c0e715d42b123`
before and after every gate. The verifier checked the shard partition, all pass and case counts, the sanitizer
selection, the absence of diagnostics, the vendor inventory, and the source hash against the current tree.
The interrupted connection left the outer timing files for Release 3 and sanitizers empty. The table uses the
driver's terminal receipts for all five gates.

| Gate receipts           | Git commit                                 | Git tree                                   |
| ----------------------- | ------------------------------------------ | ------------------------------------------ |
| Release 1 and sanitizer | `84bbc3ca9341b1949ec6f0ef48f83b5e8808c9ef` | `1a06ad5ee0ea605c916d51a18bd026be29dbc6d3` |
| Release 2–4             | `a552e1ddf600e8a51e909df007b62a00df91b2dd` | `ec5c2ea060e636e3dc2e046aaafe9cfb23662994` |

| Receipt under `receipts/final/` | SHA-256                                                            |
| ------------------------------- | ------------------------------------------------------------------ |
| `release-1.json`                | `04d7ca20d6925ebef34559c3659a3e8d791fdc495181ed4cedebb1b686399232` |
| `release-2.json`                | `683de066ab43ba6968d353c95f76f164ba7bba2991cb523c59fa8d6c1600a3f6` |
| `release-3.json`                | `219f48a10f669fe2515dbafe62355f61f6be0225aae9c041e23e999a31247f4e` |
| `release-4.json`                | `e4c91d6f2fdf7fd6cd3517657c94971b4c71da8a0119964c846d2b7e215dbbfa` |
| `asan.json`                     | `1c473786f1780cdac40257805420c4b0f16d52bec087a021fae9726fea74c97f` |

## Fuzz campaigns

The two requested workflow harnesses and the additional QuickJS-ng harness each ran for at least 600 seconds
with four workers. Each invocation used a fresh home and temporary directory, an empty environment and a private
network namespace. The scratch compiler wrappers select Clang 21 and the prepared Boost, ICU and SQLite headers.

```bash
bash "$scratch/run-fuzz.sh" regex
bash "$scratch/run-fuzz.sh" kv_jsc
bash "$scratch/run-fuzz.sh" regex_library
```

The wrapper runs the workflow's command for each target with the staged library paths.

```bash
out=$(mktemp -d "$scratch/fuzz-$target.XXXXXXXX")
mkdir "$out/home" "$out/tmp"
sudo -n unshare --net -- setpriv --reuid="$(id -u)" --regid="$(id -g)" --clear-groups --no-new-privs \
  env -i PATH="$scratch/fuzz-compilers:/usr/local/bin:/usr/bin:/bin" LANG=C.UTF-8 HOME="$out/home" TMPDIR="$out/tmp" \
  LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -L$scratch/icu78/lib -L$scratch/sqlite/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
  python3 -B cpp/gateway/fuzz/ci.py "$target" --seconds 600 --jobs 4 --out "$out"
```

| Harness         | Fuzzed seconds | Campaign wall (s) | Final corpus entries | Findings | Workflow exit |
| --------------- | -------------: | ----------------: | -------------------: | -------: | ------------: |
| `regex`         |            605 |           606.082 |                2,713 |        0 |             0 |
| `kv_jsc`        |            611 |           611.942 |                1,965 |        0 |             0 |
| `regex_library` |            605 |           605.864 |                3,233 |        0 |             0 |

The workflow retained three expected slow inputs from `regex` and one empty ARM64 libFuzzer fork-parent leak
artifact per campaign. The underlying libFuzzer commands exited 77 after completing their campaigns. The workflow
driver classified all three as complete, with zero genuine findings. The per-run logs contain the tool diagnostics.

| Receipt under scratch                      | SHA-256                                                            |
| ------------------------------------------ | ------------------------------------------------------------------ |
| `fuzz-regex.q1ViNCYz/summary.json`         | `9afaa38f6d7f6e5805feabc1ccba9477b057b48d752c175c7cd2c180adaa3447` |
| `fuzz-kv_jsc.Wa70FroG/summary.json`        | `14afdc9a148ac8557757ff92d854a96181b9c2973ac89283fcb4f10ad745e1ca` |
| `fuzz-regex_library.jQBtRq9u/summary.json` | `a184a5c85d7c835ebc7a24438eb22d86a1a7959be24c6d9f6de7ea06db25ff64` |

## Package and conformance

The portable build was already running at the merge commit when the first connection dropped. Docker Buildx finished
its 396-edge CMake/Ninja phase in 1,167.4 seconds and exported the 83,740,809-byte ARM64 archive. The original outer time
file is empty. The archive SHA-256 is `c4e2bd76ba5b049b47e2c033a4fff4d146c3c6711d6798eae7591dd6d3aec4e7`.
The version record contains the merge commit and tree, and all six vendor licenses match their tracked originals.

```bash
work="$scratch/portable"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$work/home" TMPDIR="$work/tmp" \
  DOCKER_CONFIG="$scratch/docker-config" PORTABLE_JOBS=2 \
  bash cpp/gateway/packaging/portable/build.sh "$work/uos-gateway-linux-arm64.tar.gz"
gate=$(mktemp -d "$work/gate.XXXXXXXX")
mkdir "$gate/home" "$gate/tmp"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$gate/home" TMPDIR="$gate/tmp" \
  DOCKER_CONFIG="$scratch/docker-config" \
  /usr/bin/time -o "$work/verify.time" -f 'wall_s=%e peak_process_kib=%M exit=%x' \
  timeout -k 20s 900s bash cpp/gateway/packaging/portable/verify.sh \
  "$work/uos-gateway-linux-arm64.tar.gz" "$work/verify"
```

The 80.81-second verifier exited 0 on Debian 12 and 13. All four proofs used two CPUs, default Landlock ABI 8
and two seccomp filters. Each returned 401 for unauthenticated Chat, completed 1,161 authenticated mock requests
with 1,161 matching records after reopening the store, and reported zero OOM events.

| Runtime   | Memory cap | Peak cgroup memory |
| --------- | ---------: | -----------------: |
| Debian 12 |    512 MiB |       42,946,560 B |
| Debian 12 |    256 MiB |       37,806,080 B |
| Debian 13 |    512 MiB |       38,621,184 B |
| Debian 13 |    256 MiB |       38,227,968 B |

The conformance wrapper verified 400 oracle and fixture blobs against the merge commit, sealed the exported archive,
and bound Bun 1.4.0 to its package-verified native KV artifact. The oracle's root and lint dependencies came from the
frozen lockfiles in scratch. All nine comparator controls passed. The driver used the unchanged
`bench/request_path/isolated.sh` and `conformance/run.py`, with fresh state for each gateway and IPv4 and IPv6
outside-route probes returning `ENETUNREACH`.

```bash
gate=$(mktemp -d "$scratch/conformance/gate.XXXXXXXX")
mkdir "$gate/home" "$gate/tmp"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC HOME="$gate/home" TMPDIR="$gate/tmp" \
  /usr/bin/time -o "$scratch/conformance/run-portable.time" -f 'wall_s=%e peak_process_kib=%M exit=%x' \
  bash "$scratch/run-portable-conformance.sh" ecce29dc92c627b669680b557a5dec3cd7124dea
```

The wrapper verifies the release `VERSION`, extracts and seals the bundle with `python3 -B "$run/bundle/seal.py" "$run/package"`,
and runs the following command next to a symlink to the conformance driver. The package binding supplies the independent
provenance hash. The copied isolation wrapper prepares loopback and drops to the runner uid.

```bash
timeout -k 10s 180s bash "$run/isolated.sh" --bench "$bench" --benchmark-revision "$revision" \
  --source "$source" --source-revision "$revision" --suite-revision "$revision" --suite-tree "$tree" \
  --bun /home/runner/.bun/bin/bun --binding "$run/bun-kv-binding.json" --package "$run/package" --out "$run/output"
```

Native and Bun agreed on all 186 cases, 162 before restart and 24 after it. There were 184 passing cases,
zero differential functional or cosmetic differences, and two shared validation failures. Both gateways returned
HTTP 200 and dispatched `chat-invalid-role` and `responses-invalid-tool-type`. Both cases require HTTP 400 without
dispatch. The comparator exited 1 for those same defects present in the preceding integration. The paired phase took
8.426 seconds. The wrapper took 16.72 seconds with 141,660 KiB peak process RSS.
[Validation findings](cppgw-conformance-e2e-20261118.md#functional-findings)

| Evidence in `conformance/run.HbFCxvyB/` | SHA-256                                                            |
| --------------------------------------- | ------------------------------------------------------------------ |
| Native executable                       | `2ab18f43e04268e9ab2d275b5c76ae05cfbe369297bce0204eb525c4d348c7b3` |
| Bun executable                          | `086c4121c8738a8e0f5ed730e8a461bc3973b4444e372ddb77aef9a747fa2ae9` |
| Sealed package manifest                 | `dc0c7d6cd2a69902d0dfeb0ffd73e0da8e5e2d81fb26ab548499f774f0698646` |
| `output/results.json`                   | `18e5bd70f81f5ee6a7f6c101694b17b455dfaddba8787edfeb820a6ca7948893` |
| `output/table.md`                       | `2bea47a8a0cf89edca4f79f7da9b007578c6d65da0bc1e3028399fec1918de8e` |

## Open items and handoff

- The Chat role and Responses tool type checks are shared defects, including in the TypeScript oracle outside
  the integration's paths. Their conformance assertions still require rejection before dispatch.
  [Conformance cases](../CONFORMANCE.md)
- The coroutine lifetime audit and tighter input-limit rollout are in progress. The Linux ARM64 runner verified
  the Linux archive. The macOS packaging receipt is from the earlier Apple Silicon run, and its gateway exposes
  only `--version`. Physical Raspberry Pi acceptance and production cutover remain owner decisions.
  [Coroutine audit](local-coroutine-lifetime-audit.md) · [Runtime limits](../RUNTIME.md#input-limits) ·
  [Platform and cutover stages](../OPEN-STAGES.md)
- The first-party integration diff passed `git diff --check`. The full diff reports whitespace in the imported
  llhttp, QCBOR and QuickJS-ng vendor sources. Their hashes match the supplying branches. Scoped Prettier
  checks use Bun 1.4.0 with the repository's frozen dependencies in scratch.

```bash
git diff --check 45118332..HEAD -- . ':(exclude)cpp/gateway/third_party/**'
git diff --exit-code ecce29dc HEAD -- . ':(exclude,glob)**/*.md'
git fetch --no-tags origin refs/heads/development:refs/remotes/origin/development
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false git push --dry-run --porcelain origin \
  HEAD:refs/heads/agent-jobs/cppgw-integrate-f2-20261118
git bundle create "$scratch/cppgw-integrate-f2-20261118.bundle" agent-job \
  ^45118332 ^2b23fb68 ^9acb016a
git bundle verify "$scratch/cppgw-integrate-f2-20261118.bundle"
```

The dry-run push exited 128 while requesting GitHub authentication. The refreshed `origin/development` is
`a3a96bc5fe96ba383936746fae23e64daa69562f` and precedes the gateway merges. The verified bundle preserves the
local review branch. All commits after the source integration change only Markdown. The final checkout and
tree verification are recorded in `receipts/final/handoff.txt` under scratch.

The completed integration is committed on the runner's clean `agent-job` branch. GitHub write authentication
blocked direct publication. Pull request publication and the `development` handoff remain pending.
