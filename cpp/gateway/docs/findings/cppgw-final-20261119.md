# Native gateway final qualification

## Starting point

- Integration base `13502438fb2f1a986e61af3456d84611a12eed9e`, tree
  `cb6e0ccf3859245492c5bdff8107157a0dc93350`, confirmed with
  `git log -1 --format='%H %T'`. The runner checked it out as `agent-job`.
- `git fetch origin agent-jobs/cppgw-residuals-1-20261119-44477130790-1` succeeded.
  `git log -6 --oneline FETCH_HEAD` showed the residual branch headed by `a5b0e14a`.
- `uname -sm`, `cat /etc/os-release` and `nproc` showed Linux aarch64,
  Ubuntu 24.04.5 LTS and four CPUs. `sudo -n unshare --net -- true` passed.
- The checkout had no Git author configured. New commits use the existing
  branch's `Codex <codex@openai.com>` identity in local Git configuration.
- Scratch files and dependency builds go outside the checkout. Tests use the
  repository's loopback isolation and credential-free environments.

## Progress

Merged the fetched residual branch with `git merge --no-ff --no-commit FETCH_HEAD`
and ordinary two-parent merge commit `984f15ee`. It brings in `b58afd3d1` (request bounds),
`f90d511bf` (SSE cancellation), and `d35a8a8b` (allowlists). Applied `git revert --no-commit d35a8a8bbc9a986bded5bba2d9c0f0f1a11c4ea1`.
The only conflict was in the earlier findings file, which retains its historical
results with a note pointing here. The code retains the model length checks in
both inference entry points. The two conformance cases again have the same HTTP
400 and zero-dispatch assertions on both sides. The allowlist tests were removed
by the revert. The two retained fixes and all other test registrations are present.

The runtime documentation now describes the portable and Arch build profiles,
effective bounds, the explicit systemd environment, and the build and service
environment variables. The cutover checklist uses the existing shadow unit and
deployment lock. It labels every live-data and Arch host step as unverified.
The storage runbooks now reflect the sparse and cyclic graph codec already in
the branch. Historic synthetic storage receipts remain scoped to their inputs.

The gateway guide, release, architecture, security and platform pages now
point to the current test inventory and identify prior platform results as
historical. The paired instructions distinguish the production and legacy
pins. The root `README.md` was inspected. Its paired-suite paragraph still names
the historical `2b23fb6837e2b5c0dc9ffa50771f52d322dd413a` revision as the
paired reference. The suite now reads the production pin in
`cpp/gateway/conformance/oracle-revision.txt`. The root file is outside the
authorized edit paths.

## Runner and qualification setup

The scratch root is `/tmp/cppgw-final-20261119`. The Linux ARM64 host kernel
is `7.0.0-1017-azure` from `uname -r`. Querying
`python3 -c 'import ctypes; print(ctypes.CDLL(None).syscall(444, 0, 0, 1))'`
returned Landlock ABI 8. `docker buildx version` showed v0.39.0 and
`docker version --format '{{.Server.Version}}'` showed 28.0.4.

The signed Ubuntu Noble LLVM 21 repository was checked against key fingerprint
`6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`. The installed tools were
`clang-21 llvm-21 libclang-rt-21-dev libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev mold`.
Clang reported 21.1.8 with `clang++-21 --version`, and the Boost build log showed the system GCC 13.3.0 toolset. The ICU build
used GCC 14.2.0. Downloaded Boost, ICU and SQLite archive SHA-256 values passed
`sha256sum --check` against the three pins in
`cpp/gateway/packaging/portable/dependencies.sh` before extraction.

```sh
# From each extracted source directory in scratch, respectively.
./bootstrap.sh --with-libraries=json,container --with-toolset=gcc
./b2 --ignore-site-config --user-config=/dev/null -j2 toolset=gcc --with-json --with-container \
  variant=release link=static runtime-link=shared cxxstd=20 cxxflags=-march=armv8-a stage
sudo install -m644 stage/lib/libboost_{json,container}.a /usr/local/lib/
sudo cp -a boost /usr/local/include/

./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1 && sudo make install && sudo ldconfig

CC=gcc-14 CXX=g++-14 ./configure --prefix=/usr/local --disable-tests --disable-samples --disable-extras
make -j2 && sudo make install && sudo ldconfig

pkg-config --modversion openssl zlib icu-uc libbrotlidec libzstd sqlite3
```

The last command reported 3.0.13, 1.3, 78.3, 1.1.0, 1.5.5 and 3.54.0.
The source archives and build output are outside the repository. The portable
Bookworm toolchain stage was warmed with `docker buildx build --platform linux/arm64 --progress plain --target toolchain --build-arg JOBS=2`
using the archived portable packaging directory. Bun 1.4.0 installed the
pinned formatter using `bun --no-env-file install --frozen-lockfile --ignore-scripts`
in a scratch copy of `package.json` and `bun.lock`.

The test driver resolved 453 tests after the revert with
`bash cpp/gateway/tools/ci-build.sh --list | wc -l`. The initial release and
sanitizer driver runs on the revert commit warmed their object caches. They were
interrupted during compilation and produced no passing test receipt.

## Qualification checkpoint

The resumed qualification started from commit
`2507e1229334f21341eb6444cfdb276eceda6f9e`, tree
`ca65880000600d53c0e1f9d61c0cb5dae0c014e6`. The package, fuzz and paired
lanes use its clean detached checkout. The suite driver uses the same source
paths as the warmup to reuse objects and records its own commit, tree and before
and after input hashes for every shard. The resumed receipts are in
`/tmp/cppgw-final-20261119/final-run.7Q5kEdU9`. Each shard gets a private
outer home and temp directory, and the driver independently uses the supported
Linux namespaces with an empty fixture home. The resumed command uses three
jobs per driver and four shards per configuration.

```bash
scratch=/tmp/cppgw-final-20261119
session=$(mktemp -d "$scratch/suite-env.XXXXXXXX")
mkdir "$session/home" "$session/tmp"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
  HOME="$session/home" TMPDIR="$session/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS=3 \
  UOS_CI_BUILD_DIR="$scratch/suite" UOS_CI_SQLITE_PREFIX=/usr/local \
  bash cpp/gateway/tools/ci-build.sh --config "$config" --shard "$shard/4"
```

- `python3 cpp/gateway/tools/links.py --check` exited 0 in the credential-free
  environment with 453 successful registrations. The integration base and
  candidate have identical test file lists from
  `git ls-tree -r --name-only "$ref" cpp/gateway/tests | rg '_test\.cpp$' | LC_ALL=C sort`.
  `python3 -B cpp/gateway/conformance/compare_test.py` passed 9 comparator controls.
- Stock `actionlint -color=false` 1.7.12 exited 1 on the three uses of
  `ubuntu-26.04-arm` in the existing CI workflow. GitHub's hosted runner
  inventory at `fbb26cb01c3651ea3a0347e54bfa2ef83d740d49` lists that label.
  `actionlint -config-file /tmp/cppgw-final-20261119/qualification/actionlint-platform.yml -color=false`
  exited 0 with only that label declared in the scratch config.
  [GitHub runner inventory](https://github.com/actions/runner-images/blob/fbb26cb01c3651ea3a0347e54bfa2ef83d740d49/README.md)
- The historical side-by-side harness preflight failed before launching because
  its required `/tmp/claude-1002/codex/resources.log` is absent.
  `unshare --user --map-root-user --net --mount --pid --fork --mount-proc --kill-child=SIGKILL -- true`
  independently returned `Operation not permitted` at `uid_map`. The CI driver
  supports passwordless sudo and passes its boundary check. The historical
  smoke fixture uses unprivileged user namespaces.

```bash
out=$(mktemp -d /tmp/claude-1002/prod/smoke_perf/resume.XXXXXXXX)
mkdir "$out/home" "$out/tmp"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$out/home" TMPDIR="$out/tmp" \
  python3 -I cpp/gateway/tools/smoke.py --preflight --output "$out/preflight" --timeout 10
```

The first fuzz attempts for `regex`, `kv_jsc` and `json` could not link ICU:
the headers and compiled symbols were from 78.3 while the default library search
picked the distribution copy. The retry uses the workflow command with the
runner's Clang 21 selected explicitly and the prepared `/usr/local/lib` for
link and runtime lookup.

```bash
sudo -n unshare --net -- setpriv --reuid="$(id -u)" --regid="$(id -g)" --clear-groups --no-new-privs \
  env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$out/home" TMPDIR="$out/tmp" \
  CC=clang-21 CXX=clang++-21 LDFLAGS='-L/usr/local/lib -Wl,-rpath,/usr/local/lib' \
  python3 -B cpp/gateway/fuzz/ci.py "$target" --seconds 300 --jobs 4 --out "$out"
```

The candidate has 2,088 tracked files. Its first-party production C and C++
implementation and headers total 182,156 physical lines in 756 files, including
blank and comment lines. This counts the native runtime, entry points and the
shipping C/C++ platform shims. It excludes tests, benchmarks, fuzzers, third-party
sources, web assets and scripts.

```bash
git ls-files -z | tr -cd '\000' | wc -c
git ls-files -z cpp/gateway/src cpp/gateway/include cpp/gateway/runtime cpp/gateway/entrypoints \
  cpp/gateway/packaging/confinement cpp/gateway/packaging/macos |
  python3 -c 'import pathlib, sys; paths = [pathlib.Path(p.decode()) for p in sys.stdin.buffer.read().split(bytes(1)) if p and pathlib.Path(p.decode()).suffix in {".c", ".h", ".cpp", ".hpp"}]; print(len(paths), sum(len(p.read_bytes().splitlines()) for p in paths))'
```

## Portable package and Debian proof

The package and both Debian proofs passed on the qualification commit above.
The build took 811.74 s with the prepared Buildx cache, including compilation.
The archive contains 84,268,773 bytes by `stat -c '%s' "$archive"` and passed
`sha256sum -c SHA256SUMS`. The archive SHA-256 is
`e901ebcc3aae379a6f36e262a8bb83d8dfb54b8bb530e9921a2d2b57169a95e0`.
The gateway executable SHA-256 is
`7ee30a1f59e9595fb0052c8f8d4519e0f55032aa459e41ec4788a982328bd08f`.
The verifier took 88.38 s. Each container checked ELF dependencies and
relocations, packaged favicons, confinement, unauthorized rejection and 1,161
authenticated Chat requests. It counted 1,161 settlements before and after
SIGTERM and restart, with zero cgroup OOM events. All four runs used two CPUs,
swap disabled, eight concurrent requests and Landlock ABI 8.

| Runtime   | Memory cap (MiB) | Median mock chat requests/s | cgroup peak (bytes) | Gateway peak with eight in flight (bytes) |
| --------- | ---------------: | --------------------------: | ------------------: | ----------------------------------------: |
| Debian 12 |              512 |                      205.73 |          37,277,696 |                                36,147,200 |
| Debian 12 |              256 |                      208.51 |          36,814,848 |                                36,139,008 |
| Debian 13 |              512 |                      193.00 |          37,576,704 |                                37,330,944 |
| Debian 13 |              256 |                      156.48 |          37,322,752 |                                37,339,136 |

The mock throughput is a measurement under concurrent qualification load.
Read the figures from `verify/evidence/debian-{12,13}/{512,256}MiB.json` using
`jq '{cap_mib,completed_chat_requests,reopened_accounted_requests,median_requests_per_second,cgroup_memory_peak_bytes,concurrent_peak_rss_bytes:.eight_in_flight.peak_rss_bytes,ooms:.cgroup_memory_events.oom}' "$proof"`.
The exact package compiler and library versions are in the archive's `VERSION`.

```bash
# Run from the clean detached qualification checkout.
q=/tmp/cppgw-final-20261119/final-run.7Q5kEdU9
archive="$q/package/uos-gateway-linux-arm64.tar.gz"
/usr/bin/time -f '%e' -o "$q/package/build.seconds" \
  env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$q/package/home" TMPDIR="$q/package/tmp" \
  bash cpp/gateway/packaging/portable/build.sh "$archive"
(cd "$q/package" && sha256sum -c SHA256SUMS)
/usr/bin/time -f '%e' -o "$q/verify/verify.seconds" \
  env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$q/verify/home" TMPDIR="$q/verify/tmp" \
  bash cpp/gateway/packaging/portable/verify.sh "$archive" "$q/verify/evidence"
```

## Paired production conformance

The pinned production oracle was `9353aa208a336da2c1516d604e0be16dc8af0ade`,
tree `e349a209c455dad00d5c82579f8f69934c94d6ec`. The seal bound manifest
`6e7eaa90274f1fb18bca10bb70cba799026b3ad88fa136be45c18290a731b9aa` and
native KV provenance
`b27aae214e855f7ebb3f1825b5a57d1f72fc2c6be0fca525ebc25b0b02f1f2e3`.
Bun 1.4.0 ran the exact pinned source through its `#http` and `ws@8.18.3`.
The isolated suite verified IPv4 and IPv6 no-route outcomes. It reported 186
cases, 184 matches, zero wire differences, zero cosmetic differences and two
shared expectation failures in 15.375 s. Both `chat-invalid-role` and
`responses-invalid-tool-type` returned HTTP 200 and one `deepseek.success`
dispatch per gateway. The independent assertions still require HTTP 400 and
zero dispatch, and the command exited 1 as expected for that baseline.

```bash
out="$q/conformance"
tar --no-same-owner --no-same-permissions -xzf "$archive" -C "$out/bundle"
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$out/home" TMPDIR="$out/tmp" \
  python3 -B "$out/bundle/seal.py" "$out/package" > "$out/seal-receipt.json"
/usr/bin/time -f '%e' -o "$out/compare.seconds" \
  env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$out/home" TMPDIR="$out/tmp" BUN=/home/runner/.bun/bin/bun \
  bash cpp/gateway/conformance/run.sh "$out" "$out/seal-receipt.json"
receipt="$out/run.wVgdjtRz/output/results.json"
jq '{source_revision,source_tree,suite_revision,suite_tree,network,wall_s,counts,failures:[.cases[]|select(.outcome=="functional")|{id,expectation_failures}]}' "$receipt"
sha256sum "$receipt"
```

The command took 24.32 s including Bun installation and comparison setup.
The completed comparison receipt SHA-256 is
`9ee029705025a83e1ad4e4a9ce8898d70cd852625964af3a932177fd4932edb2`.

## Isolated side-by-side

The historical supervisor-bound same-binary harness remained blocked at
preflight. The current paired launcher could start both exact runtimes with
separate synthetic stores in its supported sudo-backed isolated namespace.
The runbook's `ops/side-by-side/replay.sh` dry run and `--run` matched
all four GET and HEAD pairs for `/health` and `/openapi.json`. Both gateways
returned HTTP 401 for anonymous `GET /health/providers`. The mock saw zero
upstream calls. The first replay launch refused non-private state because the
sudo boundary reset umask. The retry set umask 077 after the uid change.

```bash
out="$q/replay-final"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$out/parent-home" TMPDIR="$out/parent-tmp" \
  timeout -k 5s 120s bash "$out/launch.sh"
jq '{replay,replay_exit,mock_request_count,bun,native,native_release_matches_build}' "$out/replay.json"
```

The scratch driver used the same fixture network checks and launch code as
`conformance/run.py`. It ran the replay script as documented with the four
public reads, then compared the native health SHA and deployment ID to
`build-profile.json`. The replay command exited 0. The driver exited 1
because the package health body and `x-uos-git-sha` both reported `unknown`
despite `DENO_DEPLOY_BUILD_ID=vps-2507e1229334f21341eb6444cfdb276eceda6f9e`
successfully supplying the matching deployment ID. The portable build records
the source identity in its package metadata and the compiled constant in
`src/release.cpp` remains `unknown`. That artifact cannot pass production
identity acceptance. Arch systemd, live-data rehearsal and ingress are
unverified. [Cutover checklist](../CUTOVER.md)

The rendered shadow unit passed `systemd-analyze verify --recursive-errors=one
"$unit_file"` on Ubuntu systemd 255 using the sealed package path and scratch
state paths. This checks unit syntax. Linux ARM target-host enforcement
requires the live host checks in the checklist.

## Release snapshot replay

Rebuilt the archive after completing the cutover documentation at
`be033a747e12d6d1703d02e6ed42347783e551c8`, tree
`f55aa029a10b08be8bc654f97a4fdc79252bd270`. The output root is
`/tmp/cppgw-final-20261119/release-snapshot.5tylerEa`. The scripts and arguments
above were repeated from the clean detached snapshot with that root as `q`.
`git diff --name-only 2507e122..be033a74` returned only four Markdown paths
and `stat -c '%s' "$q/package/uos-gateway-linux-arm64.tar.gz"` returned 84,268,698.
The cached build took 36.07 s and the verifier took 63.88 s. The gateway
executable hash was identical to the first build. The archive digest was
`de5047dcb654a1c5baa86eb4bf9dd712af58d8d2ce1753b1cc80657f95b39df3` and
the Ubuntu seal's manifest digest was
`b8e9431c0a032f174c190041f8a0fca2164e620032d1d962535202ba1820d824`.

All four Debian proofs again passed 1,161 mock Chat completions and the same
settlement total on reopen, with zero OOMs. The paired comparison again
reported 184 matches, zero wire differences and exactly the two shared
expectation failures. It took 19.337 s in the namespace and 27.59 s end to end.
The paired receipt `conformance/run.tTvKsl20/output/results.json` has SHA-256
`07dae5c581f11244e891a1a42cdebc591e2a4c4ff6350770974fc59bc028e27c`.
All four public read pairs matched on the exact snapshot package, with zero
mock dispatches. The exact production SHA check failed again. Setting
`DENO_DEPLOY_BUILD_ID=vps-be033a747e12d6d1703d02e6ed42347783e551c8`
set the expected deployment ID while the Git SHA remained `unknown`.

## Committed-diff review

Requested review on exact base `13502438fb2f1a986e61af3456d84611a12eed9e`,
tree `cb6e0ccf3859245492c5bdff8107157a0dc93350`, and head
`96a15f5be9326f92a899a135954cbe74d38d99e3`, tree
`c46d8a1b4a4157be6624b573ea6085c7fe2715aa`, using the configured provider and
model `ubiquity/deepseek-flash`. The earlier attempt timed out. The resumed
review and the retry with the exact patch supplied through
`model_instructions_file` returned an incomplete review. The tool failed with
`bwrap: loopback: Failed RTM_NEWADDR: Operation not permitted`. Its original
conclusion was "Review could not be performed: every exec_command invocation
fails before execution with \`bwrap: loopback: Failed RTM_NEWADDR: Operation not
permitted\`, so the diff against
13502438fb2f1a986e61af3456d84611a12eed9e could not be inspected." There are
no ingested review findings.

```bash
timeout -k 10s 360s codex review --base origin/agent-jobs/cppgw-gemini-merge-20261119-44556239421-1 \
  -c model=ubiquity/deepseek-flash -c review_model=ubiquity/deepseek-flash \
  -c sandbox_mode=read-only -c approval_policy=never -c shell_environment_policy.inherit=none
```

## Recovered shard and fuzz results

The saved receipts in `final-run.7Q5kEdU9` completed after the previous
checkpoint. All four Release shards and all four sanitizer shards passed
with the sanitizer shard 1 retry. Each mode passed 453 binaries and 5,264
cases: 114/1,394, 113/1,237, 113/1,331 and 113/1,302 by shard.
Release shard 1 ran at `2507e122`. The other passing receipts, including the
sanitizer shard 1 retry in `asan-retry.qinvIiaN`, ran at `1e8fc45f`,
tree `3c40d7822bb5b320f40895e80da44dd976098aee`. All used source input
digest `3ab20c3a1b83656aa230cb33703ceff243f540672fa80668773045ea2db9194e`
with before and after hashes equal. The initial sanitizer shard 1 completed
1,393/1,394 cases while other jobs ran. Its supervisor secret-redaction case
missed the existing 2,000 ms wall-clock budget. The unchanged shard passed
on a quiet retry in 51.549 s of test time, with 41/41 in that binary.

Five 300-second fuzz campaigns completed using the workflow arguments
above. `jq '{target, outcome, fuzzed_seconds, corpus_count, expected_fork_exit_leaks, findings}' "$out/summary.json"`
reported `regex` 302 s and 2,292 corpus files, `kv_jsc` 308 s and 1,933,
`json` 306 s and 1,336, `http_request` 303 s and 1,807, and `url` 309 s
and 1,942. Each recorded one classified ARM64 libFuzzer fork-parent exit
leak and zero harness findings. The first `http_request` campaign ended at
109 s with the same 32-byte fork-parent leak. Its clean retry in
`fuzz-http-retry.daaFORNp` completed the duration.

## Portable release identity correction

The exact identity failure above is in the authorized packaging and runtime
scope. The portable build now passes its archived source commit into CMake,
which validates it and binds it to the release constant at compile time.
The Debian verifier checks the sealed build's exact Git SHA and
`vps-<full-sha>` in the health JSON and headers, including after reopening.
The standard CI test driver still builds the development constant. The
negative configure check used
`cmake -S cpp/gateway -B "$out/build" -G Ninja -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 -DUOS_GATEWAY_RELEASE_COMMIT=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaz`
with a clean environment and `UOS_GATEWAY_BUILD_EVIDENCE=invalid-release-check`.
CMake refused the invalid SHA before building.

The first stamped build at `3946c4a3227fd8966130010b810de25c689a1498`, tree
`c053a41ed19f107e88c1977e4906313c9c3e2f13`, passed the four Debian proofs
in 52.73 s. Its cached package build took 37.85 s. The manifest bound the
compiled health identity and the verifier checked it through restart.
`sha256sum -c SHA256SUMS` passed for
`f178fa67a797278237294a870d93186c2f1f1cf5b12f3c21cce26128d114ee73`.
The original paired comparator surfaced 185 differences from comparing
each candidate release value to the detached Bun source's `unknown` value.
All differences were the two identity headers, their health JSON fields,
and API-key audit `release` fields. The paired receipt at
`identity-final.THqVkvsc/conformance/run.bfdjL8n7/output/results.json` has
SHA-256 `5a54161fe481ac5a2c0fc3b11c4e0fe9a39e3adee508fd8114d2b789981cf714`.

The comparator now requires each runtime's exact observed release values and
matches the pair only when both are correct. Missing or incorrect values
still fail. Raw identity fields stay in the receipt. The two application
validation assertions are unchanged. `python3 -B cpp/gateway/conformance/compare_test.py`
passed 11 controls in a credential-free network namespace, including the new
wrong-SHA and missing-field checks.

## Final package and paired replay

The commands in [Portable package and Debian proof](#portable-package-and-debian-proof)
and [Paired production conformance](#paired-production-conformance) ran from
the detached checkout of `e4f58d71d79527eac4b7c2db31328f7f8a3961e8`, tree
`12db08c5299338441d86a15058ea14962d3f0955`, in
`/tmp/cppgw-final-20261119/final-paired.hvmNWOvR` as `q`. The cached package
build took 31.69 s and `stat -c '%s' "$archive"` reported 84,259,478 bytes.
`sha256sum -c SHA256SUMS` verified archive
`2d901026330d52cf0eb3ab1e3c359bd8e5e9601921a2f34560195a7204d9d2a5` with
executable SHA-256 `3b3794d0d57ed173c3ea55397ad761e17bbdd43df7095d138772cb20c77754d7`.
Ubuntu sealing recorded manifest
`77209d2d36e34b3404af81aab3a788b6467c9f1829c387dba103c68c8f760e58` and
native KV provenance `b27aae214e855f7ebb3f1825b5a57d1f72fc2c6be0fca525ebc25b0b02f1f2e3`.
`tar -xOzf "$archive" ./VERSION | jq '{compiler,static,debianPackages}'` identified
Debian Clang 21.1.5, the static pins in the release guide and the exact
Bookworm build packages.

The Debian verifier exited 0 in 48.00 s. All four runs matched the exact
health SHA and deployment ID in headers and body, including after reopen.
Each completed and reopened with 1,161 mock Chat settlements and zero OOMs.
The same `jq` command above against the final package proof JSON read these
measurements for two CPUs without swap.

| Runtime   | Memory cap (MiB) | Median mock chat requests/s | cgroup peak (bytes) | Gateway peak with eight in flight (bytes) |
| --------- | ---------------: | --------------------------: | ------------------: | ----------------------------------------: |
| Debian 12 |              512 |                      210.98 |          41,377,792 |                                36,143,104 |
| Debian 12 |              256 |                      213.33 |          37,568,512 |                                36,122,624 |
| Debian 13 |              512 |                      226.77 |          40,583,168 |                                37,380,096 |
| Debian 13 |              256 |                      209.17 |          37,875,712 |                                37,400,576 |

The paired run on the same package took 9.425 s in the isolated namespace
and 12.43 s including preparation. It exited 1 with 184 of 186 cases matching,
zero functional wire differences, zero cosmetic differences and the two
shared expectation failures. Both invalid request cases returned 200 with
exactly one `deepseek.success` call per runtime, against the unchanged
assertions of 400 and zero dispatch. The final receipt
`$q/conformance/run.IUJkqncl/output/results.json` has SHA-256
`d8cad73eec98d9aafbd8e0b272de6f9553a4d6639d0308eae9d1d109d1e475da`.

The side-by-side replay command ran with the final package and the current
paired fixture launcher in the real isolated namespace. All four public
reads matched, both anonymous provider-health reads returned 401 and the mock
saw zero dispatches. The native health body and headers matched the full
build SHA and the explicit `vps-` deployment ID. The command exited 0.
`sha256sum "$q/replay-final/replay.json"` reported
`f558d8ebbbd78ed675cdc1832024548a61ed3fb61467d0a89f7dc701f0120048`.
The earlier standalone smoke preflight and Arch host gates remain open.
[Cutover checklist](../CUTOVER.md)

## Final C++ suites and static checks

With the same suite command above, all eight shards under
`/tmp/cppgw-final-20261119/identity-final.THqVkvsc` exited 0. The Release
shards and first three sanitizer shards used the stamped gateway source
commit `3946c4a3`. Sanitizer shard 4 recorded the paired suite commit
`e4f58d71`. `git diff --name-only 3946c4a3..e4f58d71` listed only paired
suite files and Markdown. Every receipt recorded identical before and after
C++ input digest `077cfbf876b4d087fac505ca0e1c06ace5a63dd23b78592cbab98410f61d6a8a`.
The build steps reused qualified objects. Each mode passed 453 binaries and
5,264 cases, with no failures or timeouts.

| Shard | Binaries | Cases | Release test wall time (s) | Sanitizer test wall time (s) |
| ----: | -------: | ----: | -------------------------: | ---------------------------: |
|     1 |      114 | 1,394 |                     25.468 |                       51.775 |
|     2 |      113 | 1,237 |                     17.411 |                       52.690 |
|     3 |      113 | 1,331 |                     28.004 |                      134.311 |
|     4 |      113 | 1,302 |                     38.547 |                      127.557 |

Read the totals and wall times with
`jq '{statuses,cases,test_wall_seconds,git_commit,git_tree,source_sha256,inputs_unchanged}' "$q/$config/$shard/receipt.json"`
using the suite output root above as `q`. The registration command reported
453 successful resolutions by `rg -c '^ok ' "$out/links.log"`.
Both actionlint results reproduced. Stock 1.7.12 lists the hosted ARM
runner label as unknown, and the runner-label configuration passes.
The tracked-file count is 2,088 and the production C/C++ count is 182,159
physical lines across the same 756 files, using the commands above.
The changed Markdown paths from
`git diff --name-only 13502438 -- 'cpp/gateway/*.md' 'cpp/gateway/**/*.md'`
passed `bun --no-env-file --no-install run format:check -- <absolute paths>`
from the prepared scratch formatter. `git diff --check 13502438..HEAD` passed.

The committed-diff review was requested again from the detached qualified
commit `e4f58d71d79527eac4b7c2db31328f7f8a3961e8` against the fetched
base `13502438fb2f1a986e61af3456d84611a12eed9e`, using the command above with
`--enable use_legacy_landlock`. Its original conclusion was
"Review could not be performed: the execution sandbox fails on every command
with a bubblewrap isolation panic, so the diff and repository files were
never inspected. No verdict on the code change itself is implied."
The incomplete response yielded no review findings.
Installing Ubuntu's `bubblewrap` 0.9.0-1ubuntu0.3 did not recover the sandbox.
The independent probe
`bwrap --die-with-parent --ro-bind / / --unshare-all --proc /proc --dev /dev /bin/true`
exited 1 with `loopback: Failed RTM_NEWADDR: Operation not permitted`.

## Release snapshot with updated guides

Repeated the portable build, checksum, Debian verifier, pinned production
comparison and public replay commands above from
`984861d339379e1147852da6dbd0ed889773b4f3`, tree
`ee80c4eee82e3a7f4180967c26df00b7af490174`, in
`/tmp/cppgw-final-20261119/release-final.h6chV7FP` as `q`. The build took
38.29 s and verified an 84,253,838-byte archive with SHA-256
`50243c6c0f02a5dac7341610c792416d999cad7d4850ca02969e04221a56fca2`.
The gateway SHA-256 was
`d0b3ec6358417fff368a8ad3bc9fd0cf2e312b4029ca24e677ecc7a8c6858ba0`.
The Ubuntu manifest SHA-256 was
`c0d764a3ad76e01c24276ff96ddf01a73288eef856615312c62fd829746a384b`.

The verifier exited 0 in 66.23 s with the exact health identity across
restart, 1,161 matched settlements per proof and zero OOMs across all four
runs. The same proof JSON query reports the latest application RSS peaks
from 37,335,040 to 37,412,864 bytes with eight in flight. This replay ran
with other qualification load. The package proof measures the gateway RSS
and the full container cgroup peak separately.

The paired comparison exited 1 after 14.55 s overall and 11.455 s in the
namespace. All 184 matches and the two shared failures reproduced. A mock
connection reset appeared in the server log. It had no corresponding wire,
dispatch or expectation difference in the completed receipt
`$q/conformance/run.bujtR1zX/output/results.json`, SHA-256
`3457d7becd2ee5456702e42ab4555c543c9c9e9fe4e2957e9babe2ee9bcaace2`.
Public replay matched four pairs with no mock dispatch and the exact native
identity. Its receipt SHA-256 was
`e8f5c11565af41aca416919005ffd568c6ab71711bbb9196646c0665f0e174d0`.
`systemd-analyze verify --recursive-errors=one "$unit_file"` passed again
on systemd 255.4 with the sealed package paths and a rendered scratch unit.
The independent historical smoke preflight again failed on its missing
supervisor evidence file. The target host's enforcement remains unverified.

## Post-fix fuzz replay

The workflow command above ran from the clean checkout of
`3946c4a3227fd8966130010b810de25c689a1498`, tree
`c053a41ed19f107e88c1977e4906313c9c3e2f13`. The receipts are under
`/tmp/cppgw-final-20261119/identity-final.THqVkvsc/fuzz`. All five
campaigns passed with 300 seconds requested, four jobs, one classified
fork-parent leak each and zero harness findings.

| Harness      | Build (s) | Observed fuzzing (s) | Corpus files |
| ------------ | --------: | -------------------: | -----------: |
| regex        |    98.425 |                  305 |        3,053 |
| kv_jsc       |    98.838 |                  304 |        2,286 |
| json         |    98.097 |                  303 |        1,810 |
| http_request |   114.783 |                  303 |        1,989 |
| url          |    99.844 |                  304 |        2,316 |

Read the result for each target with
`jq '{target,seconds,jobs,build_seconds,fuzzed_seconds,corpus_count,outcome,expected_fork_exit_leaks,findings}' "$out/summary.json"`.
The first `http_request` replay ended after 73 seconds with the classified
fork-parent leak and exited 3. Its retry at
`/tmp/cppgw-final-20261119/fuzz-http-final-retry.1j9CmzH7` ran the full
duration on the same commit and exited 0, as did the other four campaigns.
