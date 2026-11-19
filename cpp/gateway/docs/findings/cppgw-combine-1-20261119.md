# Native gateway follow-up integration

## Work contract v1

Integrate the seven reviewed branches onto the F2 gateway in the supplied order. Each source tip must remain in the
combined branch's ancestry. The owned paths are this report, the native gateway files needed to resolve integration
issues, and the README and Bun-deviation guide totals. The job covers the combined branch and Linux ARM64 test fixtures.

| Area                                             | Acceptance evidence                                                                                                                                                                                 |
| ------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Cleanup and packaging                            | Inspect active build, test, and packaging references against the paths removed by the cleanup.                                                                                                      |
| Standard library and Apple Silicon               | Carry forward the helper changes, vendor build coverage, and the recorded Apple Silicon qualification.                                                                                              |
| Runtime, server, provider, and handler lifetimes | Compile the shared runtime with the four fixes and verify each new regression test is registered exactly once.                                                                                      |
| Integrated suite                                 | Run `python3 cpp/gateway/tools/links.py --check`, every `bash cpp/gateway/tools/ci-build.sh --config <release or asan> --shard N/4` with real fixture isolation, and `actionlint` on the workflows. |

The starting commit was `aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`. The gate required all seven merge commits,
both complete suites, and a record of verification and outstanding work. Build and test outputs live outside
the checkout, with clean test environments and local mocks.

## Merge record

Each branch was fetched with `git fetch origin "$branch"` and merged with
`git merge --no-ff --no-edit -m "Merge $branch" FETCH_HEAD`. All seven merges were clean. After each one,
`python3 -B cpp/gateway/tools/links.py --check` resolved the listed inventory.

| Source branch            | Tip                                        | Merge commit                               | Binaries | Change                                                                                                                    |
| ------------------------ | ------------------------------------------ | ------------------------------------------ | -------: | ------------------------------------------------------------------------------------------------------------------------- |
| `cppgw-cleanup-removal`  | `fc4f9ac29d91998eb30dcba70e9ae5505bf882e6` | `b5ed3f62d330aac11e3c7dd56c5fa4566d75484d` |      450 | Retired the TypeScript and Rust gateways and the C++ pilot. Relocated native build and package inputs.                    |
| `cppgw-followups-1`      | `824687e6b5e2f561ebc588f3456f83e33e1b3228` | `86d3310176030de6763e09559fdafde2b3882f01` |      450 | Moved date and string helpers to the C++ standard library and extended the Apple Silicon build to all vendored libraries. |
| `cppgw-macos-requal-fu1` | `2d7ba4880f1e66be6c94ab2714334b8682ba5f7a` | `752378643d360df87f0cad36b50e90beb7b70334` |      450 | Recorded the Apple Silicon run of follow-ups 1, with 450 binaries and 5,119 cases.                                        |
| `cppgw-lifetime-fix-a`   | `113823bbc0675751cfb16d96d25d9c9f5ee79261` | `e1fafb8c23acda149d845db55d643091b5687e86` |      450 | Fixed runtime network, coroutine and callback lifetime issues.                                                            |
| `cppgw-lifetime-fix-b`   | `83b4a34bba1ac3d2357c526b729c15246ddace4f` | `3e171b2abc44442d09b362bfe3c78c6c34d2f32b` |      450 | Fixed server, worker, quota and storage lifetimes, including the HTTP write buffer.                                       |
| `cppgw-lifetime-fix-c`   | `f2656dfb36b8197f2bae0d033857102cb190fece` | `b8c33c8ffb740f7e4ebd43eea49d26031b3eded7` |      451 | Fixed provider and Codex request lifetimes and added the KV fan-out test binary.                                          |
| `cppgw-lifetime-fix-d`   | `b161db8ad081398b7dea3d6fd54b3ae2893a1ce7` | `57efff745ba8eec831ad1142ed9eb71dfc0cb458` |      451 | Fixed handler, authentication and administration lifetimes and startup cleanup.                                           |

## Integration fixes

- `d93cc0a8` updated the new startup regression in `tests/serve_test.cpp` to use the relocated
  `packaging/native-kv` manifest on both platforms.
- `2529815a` added the new provider KV fan-out binary to `tests/LINKS.txt` once, retaining the link closure
  declared in the test. The combined Links check resolved all 451 binaries.

- `e675dd83` formatted three imported Area A audit files so they pass the combined CI formatting gate.

## Reference and registry checks

```bash
git grep -nE '(^|[^[:alnum:]_])(src/|scripts/|tools/native-kv|rust/|ops/)' \
  -- cpp/gateway .github/workflows .githooks ':(exclude,glob)**/*.md' ':(exclude)cpp/gateway/third_party/**'
git diff --name-only --no-renames --diff-filter=D aae39d0e b5ed3f62
git ls-files -- src scripts tools/native-kv rust ops
git diff --name-only --diff-filter=D aae39d0e HEAD -- 'cpp/gateway/tests/*_test.cpp' 'cpp/gateway/tests/**/*_test.cpp'
python3 cpp/gateway/tools/links.py --check
bash cpp/gateway/tools/ci-build.sh --list
```

The remaining legacy path matches in active files refer to the pinned Bun oracle, the installed codec payload, the
scratch Rust toolchain, or recorded fixture text. The native package and test inputs resolve in the current checkout.
The tracked legacy directories are absent. All 450 original native test binaries are present.

I ran `bash cpp/gateway/tools/ci-build.sh --list --config "$config" --shard "$shard/4"` for both profiles and all
four shards. Each set contains the complete inventory once, including all 38 binaries touched by the four lifetime
branches. The only new test binary, `tests/provider_kv_fanout_lifetime_test.cpp`, has one `LINKS.txt` entry.

## Runner and commands

Ubuntu 24.04.5 ARM64, kernel `7.0.0-1017-azure`, four CPUs, Clang 21.1.8 from apt.llvm.org, Ninja 1.13.2 and
mold 2.30.0. System libraries were OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5. Build outputs and all
logs live in `/var/tmp/cppgw-combine-1-20261119.FjNwtHT3`.

| Upstream archive | SHA-256                                                            |
| ---------------- | ------------------------------------------------------------------ |
| Boost 1.92.0     | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` |
| ICU4C 78.3       | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |
| SQLite 3.54.0    | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |

The upstream archives were verified with `sha256sum --check`. The apt.llvm.org Noble repository supplied
`clang-21` and `libclang-rt-21-dev`. The Boost user config selected `/usr/bin/g++-14`. The SQLite version
probe inside every shard's namespace loaded 3.54.0. [Archive URLs and preparation](../CONFORMANCE.md#run-on-linux-arm64)

```bash
sudo apt-get -o Acquire::Retries=2 update -qq
sudo apt-get install -y -qq clang-21 libclang-rt-21-dev mold libssl-dev zlib1g-dev libicu-dev \
  libbrotli-dev libzstd-dev util-linux iproute2 pkg-config

# In $scratch/boost_1_92_0, with an empty HOME.
./bootstrap.sh --with-libraries=json,container
./b2 -j2 --user-config="$scratch/boost-user-config.jam" --with-json --with-container \
  variant=release link=static threading=multi toolset=gcc-14 stage

# In $scratch/icu-build.
../icu/source/configure --prefix="$scratch/icu78" --disable-tests --disable-samples --disable-extras --disable-static
make -j2
make install

# In $scratch/sqlite-autoconf-3540000.
./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1
make install
```

The wrapper `$scratch/run-suites.sh` ran these commands from the clean repository root. The driver used
fresh Linux network, mount and PID namespaces via passwordless sudo and dropped tests to UID 1001.
Each test had a private empty home, a fresh temp filesystem and only loopback networking. The runner's
outer environment was also built from an allowlist.

```bash
for config in release asan; do
  instrument=
  if [[ $config == asan ]]; then
    instrument='-gline-tables-only -fno-sanitize-recover=all'
  fi
  for shard in 1 2 3 4; do
    session=$(mktemp -d "$scratch/gate.XXXXXXXX")
    mkdir "$session/home" "$session/tmp"
    env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/lib/llvm-21/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
      HOME="$session/home" TMPDIR="$session/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS=4 \
      UOS_CI_BUILD_DIR="$scratch/ci" UOS_CI_SQLITE_PREFIX="$scratch/sqlite" \
      PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
      UOS_CI_CFLAGS="$instrument" UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0 $instrument" \
      UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
      bash cpp/gateway/tools/ci-build.sh --config "$config" --shard "$shard/4"
  done
done
```

## Linux ARM64 results

The test commit is `508683ce9f4ba7a77345a1ec1e2db35bacc56321`, tree
`594318d42716506bae685abd5c93005b688cf138`. All eight driver receipts recorded input SHA-256
`7ce4d4d9eb187a30fcefff0f8d49730f05c899ef2b3442b26075fd0db218d353` before and after execution.
The guides, report, and audit record formatting are the only later edits. The final input digest matches the receipts.

| Profile and shard   | Passing binaries | Passing cases | Compile (s) | Link and test (s) | Driver wall (s) |
| ------------------- | ---------------: | ------------: | ----------: | ----------------: | --------------: |
| Release 1/4         |              113 |         1,189 |     906.690 |            17.030 |         924.337 |
| Release 2/4         |              113 |         1,336 |     263.506 |            34.386 |         298.417 |
| Release 3/4         |              113 |         1,485 |     275.915 |            22.846 |         299.293 |
| Release 4/4         |              112 |         1,176 |     231.111 |            11.287 |         242.922 |
| **Release total**   |          **451** |     **5,186** |             |                   |   **1,764.969** |
| Sanitizers 1/4      |              113 |         1,189 |    1200.273 |            90.301 |        1291.090 |
| Sanitizers 2/4      |              113 |         1,336 |     337.471 |           103.924 |         441.914 |
| Sanitizers 3/4      |              113 |         1,485 |     359.563 |            41.856 |         401.950 |
| Sanitizers 4/4      |              112 |         1,176 |     308.029 |            29.527 |         338.081 |
| **Sanitizer total** |          **451** |     **5,186** |             |                   |   **2,473.035** |

Both profiles built 369 production objects, including 44 shared runtime objects, and six vendor archives.
Later shards reused their profile's production objects. All 902 binary executions and 10,372 case executions
passed. The 451 sanitizer logs each recorded the fixture boundary, with zero ASan, UBSan or LSan diagnostics.
In both profiles the affected startup, provider KV fan-out and HTTP lifetime fixtures passed 6, 9 and 8 cases,
respectively. `python3 -B "$scratch/verify-suites.py" "$scratch/gates-v1"` verified the partition, commit, tree,
stable inputs, build results and counts from the receipts.

Receipt SHA-256 values in `$scratch/gates-v1` follow.

```text
6e050ae62dda0699b9dedf957dff37ec8c3e5e9413c5f7374026166d7534feeb  release-1.json
9a618b85fbf09bd47caaec20203fbebb7a0d37810bf6912550d330ca6317adaa  release-2.json
2b6ab1e47db7d00f30549c3df86a7371c9eda81bd79d99e281237ce3b1332fcd  release-3.json
8a88b7ad24440bd619c004ee28a4f67f26d6dc6e258f44b372bc8ceb68a7c9aa  release-4.json
cab43eb47ca8024574d14bf7ab3977f6867f6d63f7b5527f4c5baf654ccadf90  asan-1.json
79d4353ae6fddbd99ed445f079e0a106aef5abbe1c4d0d27b8eb44997c02369b  asan-2.json
81edcce838e2407af7fd9a495e41490082f1e5e796d14c0e4c38f1cf22dfed2f  asan-3.json
71e973c8ccf70c673d8411954947aec9db2059a0990efd37e69fd2019d43e2f0  asan-4.json
```

## Workflow checks and handoff

Both workflows passed actionlint 1.7.11 and ShellCheck 0.9.0 with `ubuntu-26.04-arm` registered in the scratch
actionlint configuration. The final CI-format selection covered 48 changed paths using Prettier 3.9.6 from
the frozen Bun 1.4.0 lock. The formatted audit JSON decodes to the same contents as its source branch version.

```bash
"$scratch/actionlint/actionlint" -config-file "$scratch/actionlint/github-runner-labels.yml" .github/workflows/*.yml
git diff --check aae39d0e HEAD
# Apply the workflow's extension filter to git diff --diff-filter=ACMR --name-only -z aae39d0e HEAD.
# With its paths in the files array, run the frozen formatter from the repository root.
bun --no-env-file --no-install "$scratch/format/node_modules/prettier/bin/prettier.cjs" \
  --check --ignore-unknown "${files[@]}"
git log --first-parent --merges --reverse aae39d0e..HEAD
```

The stacked base is `agent-jobs/cppgw-integrate-f2-20261118-44374217576-1` and still points to
`aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`. All seven source tips are ancestors of the clean
`agent-job` branch. The direct push preflight exited 128 at the GitHub HTTPS username prompt.
The intended publication ref is `agent-jobs/cppgw-combine-1-20261119-44443689630-1`.
A verified Git bundle in scratch carries the committed branch for handoff.

```bash
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false git push --dry-run --porcelain origin \
  HEAD:refs/heads/agent-jobs/cppgw-combine-1-20261119-44443689630-1
git bundle create "$scratch/cppgw-combine-1-20261119.bundle" agent-job ^aae39d0e
git bundle verify "$scratch/cppgw-combine-1-20261119.bundle"
```

## Existing findings outside this integration

- The older paired run passed 184 of 186 cases. Both gateways dispatched and returned HTTP 200 for
  `chat-invalid-role` and `responses-invalid-tool-type`, where the fixtures expect HTTP 400 before dispatch.
  [Cleanup comparison](cppgw-cleanup-removal-20261118.md#portable-package-and-paired-comparison)
- Pending Promise reaction retention and the SSE header setup failure remain documented in the source lifetime
  reports. They need separate verification and repair. [Area A](cppgw-lifetime-fix-a-20261118.md) · [Area D](cppgw-lifetime-fix-d-20261118.md#out-of-area-observations)
- The runtime guide still lists Apple Silicon package qualification as pending. The earlier follow-up has
  a successful archive and extracted `--version` receipt. [Runtime guide](../RUNTIME.md) · [Apple Silicon findings](cppgw-macos-requal-fu1-20261118.md)

## Summary

This branch combines all seven reviewed follow-ups on F2 in order, with seven merge commits and preserved ancestry.

**Cleanup removal**\
Removes the TypeScript and Rust gateways and the old C++ pilot. Moves native inputs under `cpp/gateway/packaging/`.

**Follow-ups 1**\
Uses standard C++ date and string helpers and builds all vendored libraries on Apple Silicon.

**Apple Silicon requalification**\
Records 450 binaries and 5,119 passing cases on the standard-library follow-up.

**Lifetime fix A**\
Fixes coroutine and callback lifetimes in the runtime network and async code.

**Lifetime fix B**\
Fixes server, worker, quota and storage lifetimes, including the HTTP response write buffer.

**Lifetime fix C**\
Fixes provider and Codex request lifetimes and adds the KV fan-out regression.

**Lifetime fix D**\
Fixes handler, authentication and admin lifetimes and drains startup migration before KV closes.

**Verified**\
All four Release shards and all four sanitizer shards passed on Linux ARM64, with 451 binaries and 5,186 cases per profile. The registry, path audit, workflow lint and formatting checks passed. Integration fixes cover the relocated KV fixture, one test registration and formatting for the combined CI check.

**Remaining**\
The runner lacks GitHub write authentication, so publication and hosted CI are pending. Apple Silicon and package verification on the combined tree remain open.
