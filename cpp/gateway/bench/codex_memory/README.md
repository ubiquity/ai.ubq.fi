# Codex session memory

Run against a sealed Linux ARM64 Release package. Use an output directory outside the checkout with space for about 2 GiB of temporary state. The benchmark uses the request-path namespace launcher, empty homes and a credential-free environment with synthetic auth fixtures.

```sh
bash cpp/gateway/bench/request_path/isolated.sh --codex-memory \
  --package "$sealed_package" --out "$scratch/memory"
```

The setup process seeds an exact-version catalog, a Codex auth pool, eight API keys and settled inference usage through the gateway's APIs. After shutdown, the driver populates the same marked SQLite database to 540,000 rows, then starts a fresh gateway on that state. The client fetches the versioned catalog and streams a small completion for a roughly 60 kB Codex-shaped request. The mock checks the request and the client checks status, catalog shape, SSE text and usage. Natural shutdown and the log establish one delivered terminal per completion.

`results.json` records startup memory and the idle readings following each request at sessions 1, 10, 100 and 1,000. It also records the result of `malloc_trim(0)` at the last checkpoint. The trim probe is compiled into the benchmark output directory and loaded only in the measured child.

Use `--traffic catalog`, `--traffic completion` or `--traffic idle` with `--requests 1` to isolate the first allocations. Add `--heaptrack` to capture this trial and stop the fixture with live allocations for attribution. Use `--no-trim` to measure the process without any memory probe.

[Request-path launcher](../request_path/README.md) · [Package instructions](../../docs/RUNTIME.md) · [Measurements](../../docs/findings/cppgw-memory-20261121.md)
