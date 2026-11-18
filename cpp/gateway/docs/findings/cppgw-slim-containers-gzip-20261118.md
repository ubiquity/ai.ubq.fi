# Ordered containers and whole-buffer gzip

## Work contract v1

Baseline `6393434b110e2d1afe93e6c7005848219336d9b9`, tree
`89b42863ac7dd6a07610e2d15aa320efc42547af`. This ARM64 job applies the two
briefs in order and commits each change. All application wire formats,
status codes, durable data, authentication, validation and accounting
decisions are invariants. The assigned source regions, component fixtures,
deviation appendix and this report are the write scope. Deployment,
credentials, provider traffic, and edits to routing, SQLite, HTTP tracking,
durable KV, CI or TypeScript are outside the contract.

| Brief              | Library facility                                        | Compatibility surface                                                                                                                         |
| ------------------ | ------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| Ordered containers | `std::unordered_set`, `std::list`, `std::unordered_map` | Catalog duplicate policies, ordered snapshots, live traversal, copy independence, SameValueZero and transparent lookup                        |
| Whole-buffer gzip  | Existing zlib streaming adapter                         | Member and tail handling, checksum/truncation, output cap and scrubbing, catalog storage, authenticated Sentinel replay and deferred ordering |

Run the named release tests first, measure ARM64 ordered churn and report
production lines per brief. Every binary links `src/rt/gzip.cpp`, so the
final portable-driver release and ASan+UBSan selections cover the whole
registry. The three known integration cases from the small-helpers job
are tracked separately. Each binary runs in a private loopback-only
namespace with empty home, fresh temporary storage and a credential-free
environment. Gate receipts will identify the source tree and actual
test and case counts. Dependency installation, architecture constraints
and Git delivery access will be checked on this runner.

## Progress

The runner is Ubuntu 24.04 ARM64 with four CPUs. The portable driver
lists 441 test binaries. GCC 14.2.0 is installed for release. The LLVM
Noble apt repository supplied Clang 21.1.8 and its sanitizer runtime.
The official Boost 1.92.0 archive supplied staged static JSON and
Container libraries. SQLite 3.54.0 was built with `SQLITE_DQS=3` per
the CI guidance. ICU 78.3 matches the existing IDNA fixtures.

## Ordered containers

The named local sets use `std::unordered_set<std::string>` for
membership. The OpenRouter payload keeps the first valid row per ID.
The existing-list checks in the catalog continue to permit repeated
incoming IDs absent from that snapshot. `uniqueStrings` still deduplicates
in insertion order.

The ordered table uses list nodes for stable storage and a hash index
for live keys. With a live iterator, deletion retires the node and
clears lookup immediately. The last iterator releases retired nodes.
Ordinary erasure unlinks directly, and clean iterator releases skip
reclamation. A C++ iterator already at the list end stays there. The
new lifetime case exercises cleared nodes, independent copies, hash
growth, tail reinsertion and release of the last cursor.

| First-party production file        | Before | After | Net removed |
| ---------------------------------- | -----: | ----: | ----------: |
| `include/uos/rt/ordered.hpp`       |    384 |   329 |          55 |
| `src/models/catalog.cpp`           |    272 |   273 |          −1 |
| `src/models/openrouter_models.cpp` |    360 |   360 |           0 |
| Total                              |  1,016 |   962 |          54 |

### ARM64 churn

The scratch probe compiled the original header from `6393434b` and the
list header from `9da7eff6` with GCC 14.2.0 `-std=c++20 -O2` and the
same fatal warnings as the release driver. It ran both binaries on
CPU 3 in alternating order inside the driver's loopback-only fixture
boundary. Each of seven samples per variant measured process CPU time
for 200 repetitions of each workload. All samples produced checksum
`10021908781350352984`.

| Workload                                                      | Original median |    List median |
| ------------------------------------------------------------- | --------------: | -------------: |
| 20,000 seeded operations, snapshots and probes every 97 steps |  25.767 ns/step | 22.313 ns/step |
| 256 live sweeps, deleting and appending 16 keys per sweep     | 11.072 ns/visit | 5.645 ns/visit |

The first workload follows the existing 64-key test's seed and operation
distribution. The second visits 272 entries per sweep, including appended
entries. These are container CPU measurements.

```sh
job=/tmp/cppgw-slim.hjXkVGQs
for variant in baseline candidate; do
  env -i PATH=/usr/bin:/bin g++-14 -std=c++20 -O2 -Wall -Wextra -Wshadow -Wconversion -Werror \
    -I"$job/benchmark/$variant" -isystem "$job/boost-1.92.0" "$job/benchmark/churn.cpp" -o "$job/benchmark/bin/$variant"
done
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 TZ=UTC taskset -c 3 python3 -B "$job/churn-run.py"
```

## Whole-buffer gzip

The adapter drains the existing inflater in 64 KiB input slices and
checks its final status. Whole-buffer compression and decompression use
one scrubbed buffer. Sentinel checks its existing plaintext limit before
copying each piece and uses that limit for accumulator growth. Superseded
allocations and abandoned inputs or outputs are scrubbed.

Catalog and Sentinel compression directly return the runtime task.
Uncapped decode failures surface during the await without scheduling a
turn. Capped decode schedules both outcomes by one microtask, preserving
Sentinel's existing completion order and limit error.

The original gzip recordings still exercise the streaming partitions and
exact pieces. Buffer cases compare joined plaintext or errors with bounded
callbacks, including concatenated members across both former Blob boundaries,
CRC and truncation failures, and exact-limit success and overflow.

| First-party production file        | Before | After | Net removed |
| ---------------------------------- | -----: | ----: | ----------: |
| `include/uos/rt/gzip.hpp`          |    154 |   141 |          13 |
| `src/rt/gzip.cpp`                  |    455 |   447 |           8 |
| `src/catalog/store.cpp`            |    817 |   813 |           4 |
| `src/sentinel/replay_envelope.cpp` |    561 |   538 |          23 |
| Total                              |  1,987 | 1,939 |          48 |

The separate `gzip-probe.py` linked the old and new gzip objects against
the same runtime and zlib. A 2,114,295-byte two-member buffer decoded to
identical 2,179,072-byte plaintext in both. The old adapter used four
callbacks with a maximum piece of 2,096,512 bytes. The new adapter used
34 callbacks with a maximum of 65,536 bytes. Each run also checked the
corrupt CRC and truncated-trailer classifications and exited 0.

## Verification setup

The portable driver is invoked with `bash cpp/gateway/tools/ci-build.sh`.
The scratch wrapper at `/tmp/cppgw-slim.hjXkVGQs/gate` starts from an
empty environment and sets `CXX=g++-14` for `release` or `CXX=clang++-21`
for `asan`. Both use `UOS_CI_JOBS=4` and an external `UOS_CI_BUILD_DIR`.
The ASan+UBSan profile adds `-gline-tables-only -fno-sanitize-recover=all`.
Boost 1.92.0 headers are supplied with `-isystem` and its staged static
libraries with `-L`. `PKG_CONFIG_PATH` resolves SQLite 3.54.0 and ICU
78.3 under `/mnt/cppgw-slim`. Their shared libraries use a scratch linker
RPATH so fixture subprocesses can keep their sparse environment.
Other linked versions are OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0 and
Zstd 1.5.5. The driver's namespace probe proved loopback and external
route denial for each run.

The verified Boost, SQLite and ICU archive SHA-256 values were
`ea7b982002cc9dfbe59b0b217b206f470dc75f3de0bb2973d844118934d82411`,
`134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` and
`3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0`.
Dependency commands ran outside the checkout.

```sh
sudo -n apt-get update -qq
sudo -n apt-get install -y -qq libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev mold
sudo -n apt-get -o Acquire::Retries=2 -o Acquire::https::Timeout=30 update -qq
sudo -n apt-get install -y -qq clang-21 libclang-rt-21-dev
./bootstrap.sh --with-toolset=gcc --with-libraries=json,container
./b2 -j2 toolset=gcc variant=release link=static threading=multi --with-json --with-container stage
./configure --prefix=/mnt/cppgw-slim/sqlite --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1 install
../icu/source/configure --prefix=/mnt/cppgw-slim/icu --disable-samples --disable-tests --disable-extras
make -j2
make install
```

The first ordered qualification checkout was under `/tmp`. The driver
mounts a private fixture filesystem there, so the admin catalog's
`__FILE__` asset lookups saw missing files. The ordered, SQLite and
OpenRouter binaries passed. The admin catalog reported 20/33 cases.
The ordered gate restarted after moving the scratch checkout to `/mnt`.
The fixture boundary and the repository test files stayed as checked in.

`env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 TZ=UTC python3 -B /tmp/cppgw-slim.hjXkVGQs/gzip-quick.py`
also passed all 12 gzip adapter cases. This early release run rebuilt the
adapter and its test, linked cached runtime objects and vendored archives,
and ran through the portable driver's fixture boundary.

### Portable driver commands

The named ordered release checks use commit
`9da7eff6b74c629838876a66be32c4395c4c5da2`, tree
`510b939898b0c25020e230f3ccac83a3a074fe09`. The gzip release checks and
both complete registry runs use commit
`a3930a341ddf4373b718d40129332b0547555aaa`, tree
`e6dbe37ae53bc16cc2485934b3a2dd0fde5b7c09`.

```sh
job=/tmp/cppgw-slim.hjXkVGQs
for test in tests/rt/ordered_test.cpp tests/rt/sqlite_test.cpp tests/openrouter_models_test.cpp \
  tests/admin_models_catalog_test.cpp tests/codex_catalog_reasoning_order_test.cpp \
  tests/runtime_http_lifetime_test.cpp tests/serve_delivery_http_test.cpp; do
  "$job/gate" release "$test" || exit 1
done
```

On the gzip commit, the release pipeline ran the six named checks and
the whole registry from the `/mnt` checkout. The ASan+UBSan driver ran
the whole registry from the repository-root checkout.

```sh
job=/tmp/cppgw-slim.hjXkVGQs
for test in tests/rt/gzip_test.cpp tests/rt/fetch_transport_test.cpp \
  tests/catalog_store_differential_test.cpp tests/sentinel_replay_envelope_coverage_test.cpp \
  tests/sentinel_replay_differential_test.cpp tests/sentinel_replay_limits_test.cpp; do
  "$job/gate" release "$test" || exit 1
done
"$job/gate" release
CPPGW_GATE_JOBS=4 "$job/gate" asan
```

### Named release results

Every named command exited 0. The ordered group passed 7/7 binaries and
94/94 cases. The gzip group passed 6/6 binaries and 83/83 cases.

| Group   | Fixture                                  | Cases passed | Driver seconds |
| ------- | ---------------------------------------- | -----------: | -------------: |
| Ordered | `rt/ordered_test`                        |        10/10 |       1057.872 |
| Ordered | `rt/sqlite_test`                         |        29/29 |         25.174 |
| Ordered | `openrouter_models_test`                 |          7/7 |          5.945 |
| Ordered | `admin_models_catalog_test`              |        33/33 |         20.867 |
| Ordered | `codex_catalog_reasoning_order_test`     |          4/4 |          8.671 |
| Ordered | `runtime_http_lifetime_test`             |          4/4 |          9.377 |
| Ordered | `serve_delivery_http_test`               |          7/7 |         18.839 |
| Gzip    | `rt/gzip_test`                           |        12/12 |         17.698 |
| Gzip    | `rt/fetch_transport_test`                |        33/33 |         38.978 |
| Gzip    | `catalog_store_differential_test`        |        10/10 |         13.676 |
| Gzip    | `sentinel_replay_envelope_coverage_test` |        13/13 |         16.678 |
| Gzip    | `sentinel_replay_differential_test`      |        12/12 |         11.530 |
| Gzip    | `sentinel_replay_limits_test`            |          3/3 |         12.217 |

The first ordered run rebuilt all 369 production translation units
after relocation. Subsequent named checks reused the resulting objects.

### Sanitizer driver adjustment

The unmodified ASan+UBSan driver hit Clang `-Werror,-Wsign-conversion`
diagnostics in vendored `third_party/simdutf/simdutf.h`, included by
`src/rt/base64.cpp`. That source, the vendor files, and the driver match
the baseline byte for byte. The original diagnostic is preserved in
`full-asan-initial.log` under the scratch root. The initial run was stopped
with SIGINT and returned 130 before attempting the registry.

The scratch command `/tmp/cppgw-slim.hjXkVGQs/asan-system-vendor-gate`
reuses the checked-in driver and cached object directory. Its
`asan-system-vendor.py` gives the generated Ninja rule for
`src/rt/base64.cpp` alone the Clang option
`--system-header-prefix=third_party/simdutf/`. The rule retains fatal
first-party warnings and sanitizer instrumentation. The adjustment
changes header diagnostics for that vendor subtree and makes no
repository edits. A strict `clang++-21 -fsyntax-only` probe of the same
translation unit with the prefix exited 0 before the restart.

### Full ARM64 registry

Both runs selected all 441 binaries and compiled 369/369 production
sources. The ASan+UBSan object phase reused objects from the initial
attempt. Both runners reported zero build failures and zero timeouts.

| Profile             | Binaries passed | Cases passed | Object seconds | Link and test seconds | Elapsed seconds | Exit |
| ------------------- | --------------: | -----------: | -------------: | --------------------: | --------------: | ---: |
| GCC 14 release      |         438/441 |  5,027/5,030 |      2,035.269 |               104.655 |       2,140.735 |    1 |
| Clang 21 ASan+UBSan |         438/441 |  5,027/5,030 |      2,351.149 |               180.877 |       2,532.961 |    1 |

The release receipt SHA-256 is
`dc6acd948471ba50cd63a5b962108bfbf29952400da61530533f62183ddab6b6`.
The sanitizer receipt SHA-256 is
`15a654e4d140f69e45a2fba757501c3bc4ac240ded9e6f0da9b1b2576de62f2b`.
Both receipts bind the same commit, tree and unchanged input digest
`9473f4e1f0a595761b8fda0656b88711a0236dccd8fadb3cbd123f821461ca34`.
All 441 sanitizer run logs were checked. They reported no ASan, UBSan
or LSan diagnostics.

| Existing failure                        | Cases passed | Observed difference                                                                                                                     |
| --------------------------------------- | -----------: | --------------------------------------------------------------------------------------------------------------------------------------- |
| `claude_code_usage_schema_test`         |          2/3 | A legacy raw `resets_at` string of `"0"` leaves the normalized deadline null.                                                           |
| `codex_external_auth_differential_test` |          3/4 | The legacy `last_refresh` forms `"Nov 16 2026"`, `" 2026-11-16 "` and `"1"` still reject.                                               |
| `rt/http_test`                          |          5/6 | Invalid redirects still throw `TypeError`. The snapshot expects the former `Location` spelling and rejected value in the error message. |

All three failures belong to the integration job.
[Small-helpers findings](cppgw-std-small-helpers-20261117.md)

### Checkout and formatting

`git diff --check 6393434b` passed. The final scope contains the eleven
assigned paths. The original deviation document prefix is byte-identical.
`bun --no-env-file run format:check` with pinned Bun 1.4.0 and frozen
dependencies passed for the report and the extracted deviation appendix.
The full deviation document has a formatting failure in its prior text.
The dependencies and all C++ artifacts were prepared outside the checkout.

`GIT_TERMINAL_PROMPT=0 git push --dry-run origin HEAD:refs/heads/agent-jobs/cppgw-slim-containers-gzip-20261118`
returned 128 because GitHub username authentication was unavailable.
`gh auth status --hostname github.com` also found no logged-in account.
The refreshed `origin/development` was
`2ac5aae69b0f0bac77cf1c6f329995cf1187616c`. The baseline and both
implementation tips are outside its history. The implementation and report
commits remain on the clean local `agent-job` branch. Push and pull request
integration remain blocked by GitHub authentication.

## Summary

- Ordered containers removed 54 production lines. ARM64 median churn fell from 25.77 to 22.31 ns per step, and live mutation fell from 11.07 to 5.65 ns per visit. An iterator that reaches the list end stays there.
- Whole-buffer gzip removed 48 production lines. Fixed 64 KiB slices feed the existing inflater, and Sentinel shares the scrubbed accumulator with its plaintext cap.
- All 13 named release checks passed. The full ARM64 release and ASan+UBSan runs each passed 438/441 binaries and 5,027/5,030 cases. Only the three documented baseline failures reproduced. The sanitizer run needed a local simdutf header flag and reported no sanitizer diagnostics.
- GitHub authentication blocked push and pull request integration. The checkout is clean on `agent-job`.
