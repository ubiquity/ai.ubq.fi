# Gemini subscription integration

## Starting point

- The dispatched checkout branch is `agent-job`, at input-limits commit `74062589fbb209d376a39d119cc00b5a4140d694`, tree `19eaea54ab1158f096ecfd22cfb413259625e0c7`. The starting checkout was clean.
- `git fetch origin agent-jobs/cppgw-conformance-pin-20261119-44467964951-1 codex/antigravity-gemini-subscription-20261118` fetched both inputs. The conformance head is `2075bae6fffe7fb9bafa367ea65fb9e8c9521a88`. The Gemini head matches the requested `8110276d0f374ac64cc4edff32f5a1f02cfe3706`.
- `uname -m`, `cat /etc/os-release`, `nproc`, `clang++ --version` and `bun --version` identified Ubuntu 24.04.5 ARM64, four CPUs, system Clang 18 and Bun 1.4.0. GCC 14 is installed. The qualified build dependencies are being prepared outside the checkout.
- The scratch root for dependencies, commands and receipts is `/var/tmp/cppgw-gemini-merge-20261119.eiM1DZcH`.

## Conformance merge

`git merge --no-ff --no-commit origin/agent-jobs/cppgw-conformance-pin-20261119-44467964951-1` merged cleanly. The staged tree passed `python3 -B cpp/gateway/tools/links.py --check` with 451 registrations and `git diff --cached --check`. Merge `3b3cf2fa7147999f45fe9548dc13ac3267b40a96` retains production's DeepSeek compaction at 75% of usable context and the paired production oracle pin.

## Status

Both merges and the security audit are complete. Eight Linux ARM64 shard receipts and the sealed paired comparison identify candidate `b42191e8cf7dab62094d355bffe73c71b77dfc6d`, tree `c486abfce9574fd4ceea50b06b2aaf41ab7bd498`. This receipt commit updates Markdown only.

## Gemini merge

`git merge --no-ff --no-commit origin/codex/antigravity-gemini-subscription-20261118` recorded the pinned Gemini head as the second parent of `853907bed0b8de276c42870f4db0b43d6db5a43a`. Resolved the DeepSeek conflict with the first parent's header and kept the retired manifest generator and four legacy files deleted. `git ls-files` found zero forbidden root paths, and `comm -3` on the sorted TypeScript and Rust path lists before and after the merge gave no difference. Compared the Gemini changes against the common ancestor under `tools/native-kv`, `ops`, and `**/permissions-native.c`. There were no changes to transplant. The operator prerequisites carried into `docs/RUNTIME.md`.

`python3 -B cpp/gateway/tools/cmake-sources.py "$scratch/sources.cmake"` validated 325 historical keys plus two native Gemini modules, 44 runtime files and 371 total sources. `python3 -B cpp/gateway/tools/links.py --check`, `bash cpp/gateway/tools/ci-build.sh --list` and `git diff --cached --check` passed with 452 test registrations.

The portable Docker source stage and CMake dependency tracking now include `PORTED.txt`, which the merged inventory checker reads. Build input hashing includes the same file. The native API-key vector header now identifies the Gemini rows as native additions. The provider-selection vector header already described its Gemini extensions correctly. Item **3h FIXED**.

## Confinement audit

- **3a VERIFIED.** The legacy confinement fixture is byte-identical to the starting branch. The new test compares read, write, immutable and execute grants with the pre-merge profile for five disabled settings and an installed CLI fixture. It compares the entire 408-byte ARM64 seccomp program with SHA-256 `6536d38a3e616f3fe229a8cef632c6cda5c15b3aff6caf75de2d7c527b1dd552`. The digest was extracted from `git show 74062589:cpp/gateway/src/runtime/permissions_linux.cpp` by compiling its original `installProcessFilter` with `clang++-21 -std=c++20` and replacing its syscall sink with a write of the filter instructions. The new binary passed all three cases in Release and sanitizers, including refusal of execute grants without opt-in.
- **3b RESIDUAL.** The pinned branch notes list glibc shared dependencies, so the CLI is treated as dynamically linked. The gateway seccomp exception is process-wide. Direct execution is limited to canonical `agy` and its loader. A dedicated spawning thread adds a stricter inheritable Landlock ruleset and closes inherited descriptors beyond standard I/O. The CLI sees individual runtime libraries, private CLI inputs and a per-turn workspace. The added ruleset denies access to gateway data and the general library tree. The loader can still read compatible ELF files from the gateway's broader read set when called by the gateway and from the CLI's own readable paths when called by the CLI. The new fixture tests direct spawn denial and loader verification of an approved ELF and controlled gateway-state ELF. The runner used a dynamic ARM64 stand-in. Live `agy` qualification remains pending.

## CLI and request audit

- **3c VERIFIED.** The fake `agy` prints the four explicitly supplied environment variables. The fixture also places synthetic markers in the actual parent OS environment to exercise the `posix_spawn` environment boundary.
- **3d FIXED.** Added the gateway's 120-second timer, concurrent bounded readers and stdin delivery, forceful process termination, pipe cancellation and awaited reaping. Stdout uses the central 16 MiB JSON budget, with 4 MiB per event and 128 KiB for stderr. New fake CLI cases exercise downstream abort, deadline, full pipes, each output limit and `waitpid` reaping.
- **3e VERIFIED.** Both streaming response formats check optional usage before conversion. Retained the Responses regression and added a Chat Completions regression with `include_usage` and missing counts.
- **3f FIXED.** The API uses fixed messages for spawn and quota refresh failures and never forwards CLI diagnostics or denied-action details. Logging uses the redacted upstream diagnostic. Both 413 and local CLI startup or configuration 503 responses avoid provider-health failure observations. The fixtures cover log redaction and the unchanged healthy state after both local refusals.
- **3g FIXED.** Restored `terminal_route.cpp` to the conformance parent's response return. Terminal logging still receives the original provider response through `telemetryResponse`. The Gemini route tests check the actual terminal log for the provider and measured usage, and the separate API-key test checks a single settled request.
- **3i FIXED.** CLI pipe tasks own their readers, cancellation removes its listener on every exit and drains the process before returning. Quota reads and writes bind their owning tasks before awaiting. Fixture callbacks that borrow test state now finish that work before returning an owning task.

The Gemini merge initially added disabled Gemini rows to provider health and the model and provider catalogs. Those public rows now require a validated CLI configuration. The provider picker rejects new Gemini selections when it is unavailable and retains saved selection policy. The two inference routes use the Gemini path only on explicit opt-in. The paired suite and its comparator are unchanged.

## Paired Chat body refusal

The first sealed run used commit `e30529f292f50f993c045f15016b83ad9adcc361`, tree `12e58de8cd58d8eb7a3d7cfeb9c884b4e4e89247`. `bash cpp/gateway/conformance/run.sh "$scratch/paired" "$scratch/paired/seal-receipt.json"` completed 186 cases in 9.577 s. It recorded 183 matches, two known shared failures and one functional difference. The 33,554,433-byte `chat-body-too-large` request exercised the starting branch's earlier transport-level 413, while the pinned Bun oracle returned a structured 400 with a request ID and policy headers. Neither dispatched upstream.

The native gateway now passes the refused fixed-length Chat head to the existing authenticated rejection path. The same 16 MiB ceiling still prevents reading the body and the transport suppresses `100 Continue`. The connection closes after the response. The ordinary transport 413 remains for other routes and chunked wire overflow. A loopback test sends only the oversized head and checks the existing 400 and 401 responses for authenticated and unauthenticated clients. The loopback case passed in both native profiles. The sealed rerun matched the pinned Chat status, headers and error body.

The first Release shard 2 observed a picker test failure. `admin_codex_coverage_test` compared the disabled roster with the full internal list of selectable IDs. The test now checks the exact ordered public IDs for its disabled fixture. The configured roster test still compares the full list and verifies Gemini's three models. The first shard 2 receipt was also stale because the HTTP fix changed inputs during the run.

The first Release shard 3 exposed two further fixture assumptions while the candidate was changing. The PA2 include-order test still expected nine presentation records. It now checks ten and asserts the Gemini label. The static differential fixture still expected the pre-merge admin page. It keeps the Bun-recorded status and headers and checks the updated HTML's exact length and SHA-256 from `wc -c static/admin.html` and `sha256sum static/admin.html`. The earlier Bun recording is identified separately in the fixture header.

## Runner and reproducible commands

Ubuntu 24.04.5 ARM64, four CPUs, Landlock ABI 8, Clang 21.1.8, OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0, Zstd 1.5.5 and Bun 1.4.0. The isolated SQLite probe reported 3.54.0. Boost JSON and Container are static 1.92.0 archives built with GCC 14.2.0. ICU is 78.3. The checkouts, downloads, dependencies, compiled objects and evidence outside the primary checkout are under the scratch root stated above.

| Verified archive                                                                           | SHA-256                                                            |
| ------------------------------------------------------------------------------------------ | ------------------------------------------------------------------ |
| `https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2`                     | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` |
| `https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz` | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |
| `https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz`                               | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |

Downloaded the archives with `curl -q -fLsS --retry 3 <url> -o <archive>`, checked each with `sha256sum -c` and extracted with `tar -xf`. The signed Noble LLVM 21 apt source used the verified key fingerprint `6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`.

```bash
sudo apt-get -o Acquire::Retries=2 -o Acquire::https::Timeout=20 update -qq
sudo apt-get install -y -qq clang-21 libclang-rt-21-dev mold libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev util-linux iproute2 pkg-config

# From $scratch/boost_1_92_0.
./bootstrap.sh --with-libraries=json,container
./b2 -j2 --user-config="$scratch/boost-user-config.jam" --with-json --with-container variant=release link=static threading=multi toolset=gcc-14 stage

# From $scratch/icu-build.
../icu/source/configure --prefix="$scratch/icu78" --disable-tests --disable-samples --disable-extras --disable-static
make -j2
make install

# From $scratch/sqlite-autoconf-3540000.
./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1
make install
```

The Boost user config contains `using gcc : 14 : /usr/bin/g++-14 ;`. `run-gate.sh` creates a fresh `session` directory with empty home and temp for each invocation. Its actual driver command follows. The driver gives each binary its own Linux mount, network and PID namespaces with loopback and a fresh tmpfs. The sanitizer profile uses `instrument='-gline-tables-only -fno-sanitize-recover=all'`. Release uses an empty value.

```bash
env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/lib/llvm-21/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
  HOME="$session/home" TMPDIR="$session/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS="$workers" \
  UOS_CI_BUILD_DIR="$scratch/ci" UOS_CI_SQLITE_PREFIX="$scratch/sqlite" \
  PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
  UOS_CI_CFLAGS="$instrument" UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0 $instrument" \
  UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
  bash cpp/gateway/tools/ci-build.sh --config "$profile" "$@"
```

`python3 -B cpp/gateway/tools/links.py --check` and `bash cpp/gateway/tools/ci-build.sh --list` resolved 453 registrations. `python3 -B cpp/gateway/tools/cmake-sources.py "$scratch/final-sources.cmake"` found 327 modules and 44 runtime files, including both Gemini implementations. The checker refused a scratch manifest with the provider removed even after its two summary counters were adjusted. `git diff --check 74062589 HEAD` and `node --check static/admin.js` passed. Bun 1.4.0 installed the formatter from a scratch copy of `package.json` and `bun.lock` with `bun --no-env-file install --frozen-lockfile --ignore-scripts`. The check used `bun --no-env-file --no-install run --cwd "$scratch/formatter" format:check -- <changed Markdown files> static/admin.html static/admin.js cpp/gateway/PORT-MANIFEST.json` with absolute file paths.

`git merge-base --is-ancestor` confirmed both requested heads. `git ls-files 'src/**' 'tests/**' 'scripts/**' 'rust/**' 'docs/**' 'ops/**' 'lib/**' 'tools/**' '.github/workflows/deno-deploy.yml'` returned no paths. `git ls-files | rg '\.(ts|rs)$'` matched exactly the conformance parent's four TypeScript tooling files.

- `cpp/gateway/bench/paths/json.ts`
- `cpp/gateway/packaging/native-kv/build-native-kv.ts`
- `cpp/gateway/runtime/legacy-codec/typed-value.ts`
- `cpp/gateway/tests/production_smoke/bun.ts`

## Sealed paired gate

Built the portable archive from a clean detached checkout of the tested candidate. Set `pair="$scratch/paired-qualification"` and ran from that checkout with a credential-free environment and fresh home and temp. The Docker build used `PORTABLE_JOBS=4`. The archive name and digest came from the build output.

```bash
bash cpp/gateway/packaging/portable/build.sh "$pair/uos-gateway-linux-arm64.tar.gz"
# In $pair.
sha256sum -c SHA256SUMS
tar --no-same-owner --no-same-permissions -xzf uos-gateway-linux-arm64.tar.gz -C "$pair/bundle"
python3 -B "$pair/bundle/seal.py" "$pair/package" > "$pair/seal-receipt.json"
# From the candidate checkout.
BUN=/home/runner/.bun/bin/bun bash cpp/gateway/conformance/run.sh "$pair" "$pair/seal-receipt.json"
```

- Archive SHA-256 `d6c77d13c3b85df46c877b216b75e041059640c20c9e91196c610add29575b33`
- Sealed manifest SHA-256 `6996cb1a3bf42c87c9642b997ca99d67f40ecf5b94da6cf8a4b6ebc6df3e9ca9`
- Native KV provenance SHA-256 `b27aae214e855f7ebb3f1825b5a57d1f72fc2c6be0fca525ebc25b0b02f1f2e3`
- Native executable SHA-256 `0f35761ee5199d332ed2a1b9df2c6750fa1131bd6fefa5840ecde2fea9764292`

The gate ran 186 cases in 12.487 s against Bun 1.4.0 and `development` at `9353aa208a336da2c1516d604e0be16dc8af0ade` (tree `e349a209c455dad00d5c82579f8f69934c94d6ec`). It recorded 184 matches, zero wire differences and the two known shared failures. On `chat-body-too-large`, both returned HTTP 400 and recorded zero dispatches. The fixture proved outward IPv4 and IPv6 route denial. The receipt and comparison table are in `$pair/run.sRU8NqZr/output`.

## Review and remaining work

`timeout -k 5s 40s codex review --base origin/agent-jobs/cppgw-input-limits-20261119-44456009556-1 -c model=gpt-6.2-sol` used base `74062589fbb209d376a39d119cc00b5a4140d694` and head `b42191e8cf7dab62094d355bffe73c71b77dfc6d`. The isolated command had a fresh home, fresh temp and loopback only. Responses WebSocket connection retries reached the timeout with exit 124, so there was no complete review result. Logs are at `$scratch/review-qualification.log`.

The shared validation defects are `chat-invalid-role` and `responses-invalid-tool-type`. Both applications returned HTTP 200 and dispatched to `deepseek.success` on these cases. The recorded gate expects HTTP 400 and no dispatch. Those validation fixes are outside this merge. A live signed-in Antigravity qualification and the automatic hosted run remain pending.

## Native matrix

All receipts identify commit `b42191e8cf7dab62094d355bffe73c71b77dfc6d`, tree `c486abfce9574fd4ceea50b06b2aaf41ab7bd498` and driver input SHA-256 `f324eab4c81a6a8fd7cb66b413a2837aa659afe693562be21248c58b1af5cfff`. Each shard resolved 371 production objects and reported stable inputs. Release and ASan, UBSan and LSan ran the same 453 distinct binaries and 5,258 cases per profile.

The shard script invoked `bash "$scratch/run-gate.sh" release 4 --shard "$shard/4"` in order 1–4. For sanitizers it invoked `bash "$scratch/run-gate.sh" asan 4 --shard "$shard/4"` in order 1–4 and repeated shard 1 after the fixture commit. The earlier shard 1 passed 114 binaries and 1,393 cases while inputs were changing. Its repeat supplies the matching receipt. The final driver logs, JSON receipts and detailed case counts are in `$scratch/qualification/final`.

| Run            | Binaries passed | Cases passed | Object phase (s) | Link and test (s) | Total (s) |
| -------------- | --------------: | -----------: | ---------------: | ----------------: | --------: |
| Release 1/4    |             114 |        1,393 |          137.951 |            31.947 |   170.971 |
| Release 2/4    |             113 |        1,234 |          142.516 |            25.089 |   168.436 |
| Release 3/4    |             113 |        1,330 |           11.771 |            36.544 |    49.436 |
| Release 4/4    |             113 |        1,301 |            0.017 |            44.750 |    45.812 |
| Sanitizers 1/4 |             114 |        1,393 |            0.008 |            33.202 |    33.743 |
| Sanitizers 2/4 |             113 |        1,234 |          367.771 |            34.911 |   403.217 |
| Sanitizers 3/4 |             113 |        1,330 |          367.322 |           104.342 |   472.192 |
| Sanitizers 4/4 |             113 |        1,301 |          360.162 |           109.049 |   469.733 |

Each profile passed all 27 Gemini subscription cases and all three Gemini confinement cases. The live acceptance registration returned through its disabled fixture guard. The first Release cache-warming run observed a process abort with zero reported cases while sources were changing. The focused stable run `bash "$scratch/run-gate.sh" release 3 gemini` passed 30 cases across two binaries in 98.365 s. The final matrix repeated both binaries in each profile.

## Summary

Both merge commits preserve their ancestry and the C++-only tree. The gateway serves text-only Gemini 3.8 Flash through Antigravity and shows subscription quota in Admin Analytics.

Install the official ARM64 `agy`, sign in as the gateway OS user, set `UOS_GATEWAY_ENABLE_ANTIGRAVITY=1`, and restart. The default sandbox matches the starting policy byte for byte. The opt-in allows process creation across the gateway. Landlock limits direct execution to `agy` and its loader, and a tighter rule restricts the child's file access. The loader can still open compatible binaries from readable paths, including writable gateway state when called by the gateway.

All four Release and four sanitizer shards passed, with 453 binaries and 5,258 cases per profile. Paired Bun conformance reported 184 matches, zero wire differences, and only its two known shared validation failures. An oversized fixed-length Chat upload returns the expected structured error at the 16 MiB cap.

Live `agy` acceptance and hosted CI remain pending. The isolated committed-diff review timed out.
