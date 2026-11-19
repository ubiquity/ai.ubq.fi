# Native gateway production drift

The starting C++ commit is `e97ed985ef6c39377783e0263b10d15767de1432`, the tip of
`agent-jobs/cppgw-combine-1-20261119-44443689630-1`. The production target is
`9353aa208a336da2c1516d604e0be16dc8af0ade` on `development`. The native application at
`e509992d2f2c377e897fe4dd3d870d34107ae612` matches that production revision for the DeepSeek and Passport scope.

```bash
git fetch origin development
git update-ref refs/remotes/origin/development FETCH_HEAD
git log --oneline HEAD..origin/development -- src serve.ts lib static
```

## Production commits

- `a26c80947bf2aaecec5253d4e8fc36017fbdd046` `agent job agy-admin-meters-20261118a`.
  SKIP. `codex/antigravity-gemini-subscription-20261118` owns the Gemini quota route, snapshot, admin assets and their tests.
- `105264f3ff90dbec4e395d9c12ea4a01eff64a46` `fix(admin): preserve Antigravity sign-in and reset evidence`.
  SKIP. The Antigravity branch owns the sign-in and quota reset evidence changes and their tests.
  The test-only loader follow-up `6b2433614d9feb8e94362a7d15c2ceb8cdf4d794` belongs to that branch too.
- `f114395740de98c951d29d92f41a7183fe722354` `fix(deepseek): compact at 75% of usable context`.
  PORT in `854f18df1be9d5542e8a7292c9e2590fabb3ab6a`. `include/uos/deepseek/waterfall.hpp` derives the original 747,110-token threshold for the Codex
  record and configured model capabilities. `tests/deepseek_waterfall_test.cpp` checks 75%, 747,110
  tokens and the projected limit in the two existing cases. This intermediate policy is superseded by
  the next production commit.
- `0d66be90a50b8cf78a1697a1fb94eba4afb4d0c6` `fix(deepseek): use one global compaction rule`.
  PORT in `f69df650b09550fcb05fdb7789657a6bda4244dd`. `src/recent_model_context.cpp` and `include/uos/recent_model_context.hpp` compute the earlier of
  90% or a 50,000-token reserve and clamp declared limits to that ceiling. `src/catalog/store.cpp` uses
  the shared clamp on stored rows. For the gateway waterfall, `include/uos/deepseek/waterfall.hpp`,
  `src/catalog/models.cpp` and `src/input_normalization.cpp` publish 800,000 tokens with compaction at
  720,000. `include/uos/models/metadata.hpp` documents the global rule.
  `tests/recent_model_context_test.cpp`, `tests/deepseek_waterfall_test.cpp`,
  `tests/codex_catalog_test.cpp`, `tests/model_metadata_test.cpp` and `tests/openrouter_catalog_test.cpp`
  port the new boundary, the changed enrichment and cached catalog values, and the preserved early limit.
  `tests/admin_models_catalog_test.cpp` checks the new admin window and ports the two OpenAI capability cases,
  including 900,000 tokens for the official DeepSeek aliases. The catalog store, metadata and input normalization
  vectors (`catalog_store_differential_vectors.inc`, `models_metadata_differential_vectors.inc`,
  `input_normalization_models_vectors.inc`) and the expectations in `recent_model_context_differential_test.cpp`
  were regenerated from the production target.
- `f2768b2a38761d4d42312c0d49fe7874b0560b03` `fix(catalog): keep gateway DeepSeek window across metadata refresh`.
  PORT in `689b828f685e4786c3dd894aa1cb5d780729c899`. `src/catalog/models.cpp` replaces the upstream same-ID record at its original position.
  `src/models/catalog.cpp` disables OpenRouter enrichment for the gateway-owned row.
  `tests/deepseek_waterfall_test.cpp` adds the upstream row replacement case. `tests/admin_models_catalog_test.cpp`
  seeds the 1,048,576-token OpenRouter alias and checks both the enriched and paid-hop fixtures for 800,000-token
  windows and a 720,000-token compaction limit.
- `9353aa208a336da2c1516d604e0be16dc8af0ade` `Sentinel repair: issue 1281`.
  PORT in `e509992d2f2c377e897fe4dd3d870d34107ae612`. `src/auth/passport.cpp` cancels the exchange body on HTTP 4xx and returns HTTP 401 `invalid_api_key`.
  `tests/passport_test.cpp` ports the five 4xx and four 5xx inputs through the callback. Each case checks
  one exchange, exact clearing headers for state and verifier cookies, and the 401 or 502 status and error code.

## Bun recordings and tests

```bash
git worktree add --detach "$scratch/development" origin/development
# In that worktree, with a fresh home and temp and an allowlisted environment.
bun --no-env-file install --frozen-lockfile --ignore-scripts --backend=copyfile
"$scratch/isolated-command" rerecord-compaction
"$scratch/isolated-command" test-development
"$scratch/isolated-command" test-intermediate
```

The wrapper runs `$scratch/rerecord-compaction.mts` using Bun 1.4.0 and the native CI namespace helper.
The recorder repeats the checked-in inputs against the TypeScript exports with OpenRouter and KV mocks.
It recorded 209 metadata resolutions, 69 snapshot projections, 12 warmed-cache resolutions plus the cold
lookup, 20 catalog parses, 10 catalog store/load round trips, 167 input normalization calls and 21 compaction
boundary calls. The catalog vectors retain their 18 historical gzip fixtures. The decoded vector comparison
found changes only to compaction limits, DeepSeek windows and the served catalog body and its hash.
The recorder SHA-256 is `1d81b45019f6bdd2008c45e058b1b1cffbaa49d13551ac23d135d093b519836b`.

The test wrapper ran each file from the detached worktree with a separate empty home and temp in the native
CI namespace. It used `bun --no-env-file --no-install test --no-orphans --max-concurrency=1 --timeout=300000`.
The initial attempt used Bun's 5-second default and the slow Codex refresh lease case timed out. The run with
the repository runner's timeout passed all 152 cases across the eight affected files.

| TypeScript file                | Passing cases |
| ------------------------------ | ------------: |
| `deepseek-waterfall.test.ts`   |            24 |
| `codex-catalog.test.ts`        |            27 |
| `model-metadata.test.ts`       |            17 |
| `openai-compat-part2.test.ts`  |            27 |
| `openrouter-catalog.test.ts`   |             9 |
| `recent-model-context.test.ts` |             3 |
| `admin-models-catalog.test.ts` |            29 |
| `passport.test.ts`             |            16 |

The separate detached worktree at `f114395740de98c951d29d92f41a7183fe722354` passed the 23 cases in
`deepseek-waterfall.test.ts`, including the interim 75% and 747,110-token assertions.

## Runner

Ubuntu 24.04.5 ARM64, Linux `7.0.0-1017-azure`, four CPUs, Clang 21.1.8, Ninja 1.13.2 and mold 2.30.0.
The native test builds used OpenSSL 3.0.13, zlib 1.3, ICU 78.3, Brotli 1.1.0, Zstd 1.5.5 and SQLite 3.54.0.
Builds and receipts are under `/var/tmp/cppgw-drift-port-20261119.Td3tgNyr` (`$scratch` below).
Bun 1.4.0 was already installed.

```bash
curl -fLsS --retry 2 https://apt.llvm.org/llvm-snapshot.gpg.key -o "$scratch/llvm.asc"
gpg --batch --with-colons --show-keys "$scratch/llvm.asc" |
  awk -F: '$1 == "fpr" {print $10}' | head -1 |
  diff - <(printf '%s\n' 6084F3CF814B57C1CF12EFD515CF4D18AF4F7421)
sudo -n install -m 0644 "$scratch/llvm.asc" /usr/share/keyrings/cppgw-drift-llvm.asc
printf '%s\n' 'deb [arch=arm64 signed-by=/usr/share/keyrings/cppgw-drift-llvm.asc] https://apt.llvm.org/noble/ llvm-toolchain-noble-21 main' |
  sudo -n tee /etc/apt/sources.list.d/cppgw-drift-llvm.list >/dev/null
sudo -n apt-get -o Acquire::Retries=2 update -qq
sudo -n apt-get install -y -qq clang-21 libclang-rt-21-dev llvm-21 mold libssl-dev zlib1g-dev libicu-dev \
  libbrotli-dev libzstd-dev util-linux iproute2 pkg-config
```

I verified each archive with `sha256sum -c` before extraction and built each into scratch.

| Archive       | URL                                                                                                         | SHA-256                                                            |
| ------------- | ----------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------ |
| Boost 1.92.0  | [archives.boost.io](https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2)                   | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` |
| ICU 78.3      | [Unicode release](https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz) | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |
| SQLite 3.54.0 | [sqlite.org](https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz)                                    | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |

```bash
# Boost, from $scratch/boost_1_92_0.
printf '%s\n' 'using gcc : 14 : /usr/bin/g++-14 ;' >"$scratch/boost-user-config.jam"
env -i PATH=/usr/bin:/bin HOME="$scratch/boost-home" LC_ALL=C ./bootstrap.sh --with-libraries=json,container
env -i PATH=/usr/bin:/bin HOME="$scratch/boost-home" LC_ALL=C ./b2 -j2 --user-config="$scratch/boost-user-config.jam" \
  --with-json --with-container variant=release link=static threading=multi toolset=gcc-14 stage
# ICU, from $scratch/icu-build.
../icu/source/configure --prefix="$scratch/icu78" --disable-tests --disable-samples --disable-extras --disable-static
make -j2 && make install
# SQLite, from $scratch/sqlite-autoconf-3540000.
./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1 && make install
```

## Native test commands

The driver confirmed SQLite 3.54.0 inside the fixture boundary. Each test ran as UID 1001 in fresh Linux
network, mount and PID namespaces with a private tmpfs, empty home and temp, and a credential-free environment.
The loopback control succeeded and the outside route probe returned `ENETUNREACH`.

```bash
# $scratch/ci-gate CONFIG JOBS LABEL [test substring or --shard N/4]
session=$(mktemp -d "$scratch/ci-gate.XXXXXXXX")
mkdir "$session/home" "$session/tmp"
instrument=''
if [[ $config == asan ]]; then instrument='-gline-tables-only -fno-sanitize-recover=all'; fi
env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/lib/llvm-21/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
  HOME="$session/home" TMPDIR="$session/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS="$jobs" \
  UOS_CI_BUILD_DIR="$scratch/ci" UOS_CI_SQLITE_PREFIX="$scratch/sqlite" \
  PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
  UOS_CI_CFLAGS="$instrument" UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0 $instrument" \
  UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
  bash cpp/gateway/tools/ci-build.sh --config "$config" "$@"
```

```bash
"$scratch/ci-gate" release 2 release-shard-3 --shard 3/4
for filter in catalog model_metadata passport recent_model_context; do
  "$scratch/ci-gate" release 2 "release-$filter" "$filter"
done
for filter in recent_model_context catalog model_metadata models_metadata passport deepseek_waterfall transport_normalization input_normalization; do
  "$scratch/ci-gate" asan 2 "asan-$filter" "$filter"
done
python3 -B cpp/gateway/tools/links.py --check
python3 -B cpp/gateway/conformance/compare_test.py
git diff --check e97ed985 HEAD
```

The registration gate resolved all 451 binaries. The nine conformance comparator controls passed.
The changed Markdown passed Prettier 3.9.6 from the frozen root Bun lockfile. In the scratch directory
containing a copy of the root `package.json` and `bun.lock`, I ran
`bun --no-env-file install --frozen-lockfile --ignore-scripts` and
`bun --no-env-file --no-install run format:check -- "$repo/cpp/gateway/docs/findings/cppgw-drift-port-20261119.md"`.

## Native test results

Release shard 3/4 passed all 113 binaries and 1,486 cases. The release shard and focused filters covered
127 distinct binaries and 1,633 cases after deduplication. The sanitizer filters passed 21 binaries
and 228 cases, each with its namespace proof and zero ASan, UBSan or LSan diagnostics. All 369 production
translation units compiled in both profiles. The cold release object phase took 2,798.739 seconds and
linking and execution took 35.350 seconds. The cold sanitizer object phase took 2,658.520 seconds.
The builds ran with two jobs per profile alongside the portable package build for part of the run.

The release shard and first sanitizer run recorded commit `e509992d2f2c377e897fe4dd3d870d34107ae612`
and tree `16f1432c8ad821cc97eed2829121874dc0166c9b`. The subsequent filters recorded the conformance
report commit `b47b4f90347a3aefa5d581bc5f09086f78eca024` and tree `8c7b6defb5c987d2cb6035c401267058d3e27d1f`.
All receipts recorded the same CI input SHA-256 before and after execution.

```text
e37a74363813bf47ba6d427dfe6541ae75c576e3553d65531614e8e07548224c
```

`python3 -B "$scratch/summarize-ci.py" "$scratch/ci-receipts"` verified the input digests, candidate
identities and case counts. The receipt files use the gate names below.

| Gate                           | Passing binaries | Passing cases | Wall seconds | Receipt SHA-256                                                    |
| ------------------------------ | ---------------: | ------------: | -----------: | ------------------------------------------------------------------ |
| `release-shard-3`              |              113 |         1,486 |    2,834.638 | `e61a91b79fbb108f189ba78eafeabfffdb868b073ea1c43c890cd7531d0c1eca` |
| `release-catalog`              |               11 |           125 |       48.324 | `53f36184621474875f87b7e668664802c61fbe4198e14aaaea51b4e561bf6a28` |
| `release-model_metadata`       |                1 |            17 |        8.992 | `ddb7295f0b122b837ad16323302a33f0808c5304017651ca049494c3891c72ff` |
| `release-passport`             |                3 |            22 |       14.350 | `5596d2b55c71c34bc4d78cec534dd0eaa218d26d139262d30f663c151349478d` |
| `release-recent_model_context` |                2 |             6 |        3.764 | `c8257d18149843e2b3a9bf5203a1bf228dabd3f7e2bc0f469cb67b8f187e66f1` |
| `asan-recent_model_context`    |                2 |             6 |    2,659.750 | `e423ec7b3c718b49c519efe07344162ab3b3811fd735294f142d24159b7b138d` |
| `asan-catalog`                 |               11 |           125 |      103.967 | `dd2fd54b7f1c6348605f36af16cd1b0f398bf8c4bf8150d799efeb0cede5cb72` |
| `asan-model_metadata`          |                1 |            17 |       12.512 | `9fab98a1fbfa5e21645e3b75d636159de695b8b8dcb62bb4b97017be160cdedb` |
| `asan-models_metadata`         |                1 |             4 |        8.453 | `1a03a8df734d70deddb02e27e598c69c5c8a6054fc3515fc4d6e43d12835b0bd` |
| `asan-passport`                |                3 |            22 |       34.243 | `db5d8e1869b2481807b8b4ba102b60ef338e4be6960f366ec826191849f9994b` |
| `asan-deepseek_waterfall`      |                1 |            13 |       14.866 | `3a9cf70a9d44c08b31db7ffef4695baf4dd5bd3f0a3faa96820eeacb95f704c9` |
| `asan-transport_normalization` |                1 |            36 |       15.726 | `c2c4af18bf4aba7844fcf510d87055700a556d9ba84aa0e5acef1265659f87e0` |
| `asan-input_normalization`     |                1 |             5 |       11.393 | `81d427d313fbfbc0daf0547b3fae94050545ec1cba20362af67f0e2dca642fe4` |

## Paired conformance

The sealed package and case matrix tested native commit `e509992d2f2c377e897fe4dd3d870d34107ae612`, tree
`16f1432c8ad821cc97eed2829121874dc0166c9b`. The development application tree is
`e349a209c455dad00d5c82579f8f69934c94d6ec`.

```bash
work="$scratch/portable"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$work/home" TMPDIR="$work/tmp" DOCKER_CONFIG="$work/docker-config" PORTABLE_JOBS=2 \
  /usr/bin/time -o "$work/build.time" -f 'wall_s=%e peak_process_kib=%M exit=%x' \
  bash cpp/gateway/packaging/portable/build.sh "$work/uos-gateway-linux-arm64.tar.gz"
"$scratch/conformance-gates"
```

The package built in 1,610.95 seconds and the 83,915,399-byte archive passed SHA-256 verification.
The wrapper extracted the archive and ran `python3 -B "$work/bundle/seal.py" "$work/package"`.
It ran the documented `conformance/run.sh` against the development revision by redirecting only the
oracle pin and launcher path in scratch copies of `run.sh` and `oracle.sh`. The worktrees, frozen
installs and package identity checks remained active. The direct run exited 1 before any requests
with `ModuleNotFoundError: No module named 'cases'`. The development tree lacks
`cpp/gateway/tests/production_smoke/`, including the Bun launcher and Python helpers that the suite
loads from the oracle.

The diagnostic run restored the exact historical smoke fixture in that detached development worktree.
It verified that the tracked production source still matched `9353aa20` and that the launcher blob matched
the historical revision. The documented paired runner and loopback isolation then ran the full matrix.

```bash
historical=2b23fb6837e2b5c0dc9ffa50771f52d322dd413a
# $launch is the direct gate's run directory, with its detached oracle and verified KV binding.
source="$launch/oracle"
git archive "$historical" cpp/gateway/tests/production_smoke | tar -x -C "$source"
git -C "$source" diff --quiet "$target" --
timeout -k 10s 180s bash "$launch/isolated.sh" --bench "$repo/cpp/gateway/bench/request_path" \
  --benchmark-revision "$candidate" --source "$source" --source-revision "$target" --source-tree "$source_tree" \
  --suite-revision "$candidate" --suite-tree "$candidate_tree" --bun "$bun" --binding "$launch/oracle-binding.json" \
  --package "$package" --out "$launch/output-with-original-launcher"
bash cpp/gateway/conformance/run.sh "$scratch/conformance/historical" "$work/seal-receipt.json"
```

With that fixture the 186 development cases completed in 7.811 seconds. There were 184 passes,
zero native/Bun wire differences and the same two shared validation failures. Each gateway exited
cleanly on both phases. IPv4 and IPv6 outside-route probes returned `ENETUNREACH`. The launcher
used `#http`, pinned `ws@8.18.3`, the package-bound native KV and newly created state.

At the end of the drift port, the oracle pin was `2b23fb6837e2b5c0dc9ffa50771f52d322dd413a` because
the gate loaded its fixtures there. A pin-only change to `9353aa20` failed before the comparison. The unmodified
historical gate completed 186 cases in 8.089 seconds with 176 matches, the two shared failures and
eight production drift differences. They affect the admin and public catalogs, model capabilities,
and versioned Codex rows and their ETags. The older Bun fixture advertises the previous compaction
limits and DeepSeek window in those replies. The follow-up moved the fixture into the native suite
and pinned production. [Production pin findings](cppgw-conformance-pin-20261119.md)

| Evidence                              | SHA-256                                                            |
| ------------------------------------- | ------------------------------------------------------------------ |
| ARM64 archive                         | `fd0184efe21f9883fec9ce647f95dc8041b52ff6bbc93917afd5ca9c21fa0dee` |
| Sealed manifest                       | `179190805db726a5c54c281eb4d75d7c41bfcf463c3e5f3bdbab34227a79df13` |
| Original Bun launcher                 | `bc021f6abb1dba5ce89546f58999fdbbbbe197857347049539f38929658a3f9a` |
| Development comparison `results.json` | `d41be5c8e0b31efd0d89647a273466a7f11449f94f527535c492eb9c8dd5f648` |
| Historical comparison `results.json`  | `0cb161b16c586e33b74393cadec963391519d88633e8548f271f566925fb279f` |

The out-of-scope validation cases `chat-invalid-role` and `responses-invalid-tool-type`
still dispatch and return HTTP 200 on both gateways. Their independent expectations require HTTP 400
before dispatch. [Earlier validation findings](cppgw-conformance-e2e-20261118.md#functional-findings)

## Committed-diff review

`codex review --base origin/agent-jobs/cppgw-combine-1-20261119-44443689630-1` used model
`gpt-6.2-sol` against base `e97ed985ef6c39377783e0263b10d15767de1432` and head
`e509992d2f2c377e897fe4dd3d870d34107ae612`. I ran it via `"$scratch/isolated-command" committed-review`
with the test network boundary and a 40-second timeout. The review exited 124 after connection retries
and produced no findings. Its log is `$scratch/review.log`.

## Summary

Ported DeepSeek compaction and the Passport callback repair through production commit `9353aa20`.
The DeepSeek waterfall advertises an 800,000-token window and a 720,000-token compaction limit across catalog refreshes.
Passport returns 401 for rejected exchanges and 502 for upstream server failures, clearing temporary login cookies in both cases.
The Antigravity changes belong to their separate port.

ARM64 release shard 3/4 passed all 113 binaries and 1,486 cases. All focused release and sanitizer checks passed.
The paired run against production passed 184 of 186 cases with the original smoke fixture restored in scratch.
The other two were the known shared validation failures. This job kept the historical pin because `development`
lacked the fixture. The isolated model review timed out. [Production pin follow-up](cppgw-conformance-pin-20261119.md)
