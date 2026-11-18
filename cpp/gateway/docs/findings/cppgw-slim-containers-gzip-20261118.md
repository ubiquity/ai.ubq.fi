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

| Brief | Library facility | Compatibility surface |
| ----- | ---------------- | --------------------- |
| Ordered containers | `std::unordered_set`, `std::list`, `std::unordered_map` | Catalog duplicate policies, ordered snapshots, live traversal, copy independence, SameValueZero and transparent lookup |
| Whole-buffer gzip | Existing zlib streaming adapter | Member and tail handling, checksum/truncation, output cap and scrubbing, catalog storage, authenticated Sentinel replay and deferred ordering |

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

| First-party production file | Before | After | Net removed |
| --------------------------- | -----: | ----: | ----------: |
| `include/uos/rt/ordered.hpp` | 384 | 329 | 55 |
| `src/models/catalog.cpp` | 272 | 273 | −1 |
| `src/models/openrouter_models.cpp` | 360 | 360 | 0 |
| Total | 1,016 | 962 | 54 |

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

| First-party production file | Before | After | Net removed |
| --------------------------- | -----: | ----: | ----------: |
| `include/uos/rt/gzip.hpp` | 154 | 141 | 13 |
| `src/rt/gzip.cpp` | 455 | 447 | 8 |
| `src/catalog/store.cpp` | 817 | 813 | 4 |
| `src/sentinel/replay_envelope.cpp` | 561 | 538 | 23 |
| Total | 1,987 | 1,939 | 48 |
