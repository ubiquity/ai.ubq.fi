# Native gateway memory with Codex CLI

The benchmark uses the sealed Release gateway and a fresh Bun/JSC KV fixture. The
C++ state writer uses the gateway's ordered tuple keys, typed records and atomic
KV writes. A loopback Responses endpoint supplies programmed SSE replies. The
real Codex CLI executes a patch and nine shell calls in a temporary workspace.
The mix also includes three single-turn reply shapes and a resume of the tool
session. The mock verifies the catalog instructions and the returned tool output.
The driver checks Codex's final text, delivery terminals and the active key's
durable usage after shutdown.

## Run on Ubuntu ARM64

Install the [CI dependencies](../../docs/CI.md), pinned SQLite and `patchelf`.
Install Heaptrack separately for profiling. Use a committed checkout and a fresh
scratch directory outside the checkout with about 1 GiB free per concurrent run.
The commands work with either the native Codex binary or the installed Codex
launcher when its adjacent package contains the native Linux ARM64 executable.

```sh
bench=cpp/gateway/bench
scratch=$(mktemp -d)
mkdir "$scratch/home" "$scratch/tmp"
env -i PATH=/usr/local/bin:/usr/bin:/bin LC_ALL=C.UTF-8 HOME="$scratch/home" TMPDIR="$scratch/tmp" \
  CC=clang CXX=clang++ UOS_CI_JOBS=4 UOS_CI_BUILD_DIR="$scratch/build" \
  bash cpp/gateway/tools/ci-build.sh --config release oss_gateway_http_test
receipt=$(find "$scratch/build" -name receipt.json -type f)

bash "$bench/codex_real/sqlite.sh" python3 -B "$bench/codex_real/build.py" \
  --receipt "$receipt" --out "$scratch/artifacts"
bash "$bench/codex_real/sqlite.sh" bash "$bench/request_path/isolated.sh" --codex-real \
  --package "$scratch/artifacts/package" --seed-state "$scratch/artifacts/seed-state" \
  --out "$scratch/memory" --codex "$(command -v codex)" --sessions 500
```

The package builder reuses the successful CI objects and verifies the archived
source before linking the selected Git identity. The private SQLite mount gives
the package's loader the CI library at the canonical distribution path. The
measurements run with loopback-only networking, empty homes, disposable Codex
state and synthetic credentials. Use `--version` to select the catalog's exact
version. The exercised CLI version is 0.171.0.

`results.json` and `samples.jsonl` include process RSS, anonymous RSS, peak
RSS, heap and anonymous mappings, and cumulative inference requests after every
session. `fixture.json` records the physical database inventory. `upstream.json`
records dispatched body sizes and tool counts. Add `--trim` for an end-of-run
`malloc_trim(0)` control. Use `--heaptrack --sessions 5` for one full mix
cycle. Select `--scripted --traffic idle`, `auth`, `catalog` or `replay`
for isolated attribution, using a fresh output directory each time. Omit
`--traffic` to run the complete scripted mix. [Measurements](../../docs/findings/cppgw-memory-2-20261121.md)

## CI memory budget

Run the same launch with `--check` in place of `--sessions 500`. The fixed
profile runs 20 sessions, checks health and terminal memory fields, and compares
ending anonymous RSS and peak RSS to the Release budgets of 17 MiB and 58 MiB.
It preserves the readings in `results.json` when either budget is exceeded.
Release shard 1 runs the check with the installed native CLI when available.
The scripted path uses the same state, reply types, tool history and limits
on images without the CLI. Select it locally by replacing `--codex <path>`
with `--scripted`. [CI workflow](../../../../.github/workflows/cpp-gateway-ci.yml)

For phase totals, pass the recorded `heaptrack.zst` to
`python3 -B cpp/gateway/bench/codex_real/profile.py <trace>`. It uses
`zstdcat` and groups live allocation bytes by the first matching frame on
each stack. Each phase's peak is taken independently across the trace.
