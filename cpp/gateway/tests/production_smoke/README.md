# Same-binary synthetic acceptance

The harness runs the primary release executable from the verified package, using its ordinary runtime manifest, native durable store, application handlers, admin authentication and delivery receipts. Its Bun comparison imports the real source application and uses `#http`, pinned `ws`, and a genuine prebuilt native-KV artifact. No replacement HTTP gateway, memory store or authentication bypass is used.

Run the independent OS-boundary check with `python3 -I cpp/gateway/tools/smoke.py --preflight --output /tmp/claude-1002/prod/smoke_perf/<new-preflight-directory> --timeout 10`. `--fixture-self-test` exercises only the new scripted HTTP/SSE/WebSocket client/upstream and cannot qualify the gateway.

After the build/runtime owners publish the exact primary package and receipts, run the full acceptance from the worktree root:

```sh
scratch=$(mktemp -d)
mkdir "$scratch/home" "$scratch/tmp"
bash cpp/gateway/conformance/oracle.sh "$scratch/oracle"
test -f "$scratch/oracle/bun.lock" && test -f "$scratch/oracle/tools/lint/bun.lock"
(
  cd "$scratch/oracle"
  env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$scratch/home" TMPDIR="$scratch/tmp" \
    /usr/local/bin/bun --no-env-file install --frozen-lockfile --ignore-scripts
)
python3 -I cpp/gateway/tools/smoke.py \
  --gateway /absolute/worktree/.data/package/bin/uos-gateway \
  --gateway-sha256 <accepted-primary-binary-sha256> \
  --build-receipt /absolute/worktree/.data/primary-build-receipt.json \
  --fixture-flag=--runtime-manifest \
  --runtime-template /tmp/claude-1002/prod/smoke_perf/verified-template.json \
  --bun /usr/local/bin/bun \
  --oracle "$scratch/oracle" \
  --bun-binding /tmp/claude-1002/prod/smoke_perf/verified-bun-binding.json \
  --output /tmp/claude-1002/prod/smoke_perf/<new-run-directory> \
  --benchmark --timeout 180
```

The runtime template is the `uos-gateway-launch-v1` document with the accepted package/provenance/manifest binding. The harness replaces only `stateRoot`, `hostname`, `port` and `fixture.syntheticOrigin`, creates fresh private state, and retains the exact manifest hash. Each output directory must be new. The Bun binding names genuine frozen artifact/provenance/input files; missing dependencies or artifacts fail explicitly. The comparison uses `--config=path`: the pinned Bun CLI was observed to exit without executing an inline script when the config path was passed as a separate argument.

The runtime workload is finite: four ordinary inference cases, an authenticated live call and WebSocket relay, one SIGTERM-in-flight Responses request, durable usage readback after restart, and optional fifteen-sample serial workloads plus sixteen streamed load requests at concurrency eight. Native and Bun order is interleaved over two trials when benchmarking. The provider script caps dispatches at 4096. Each runtime has a fifteen-second readiness/shutdown deadline, a 2 GiB RSS bound and a 2 GiB host-memory reserve. The outer timeout kills only the owned namespace/process group and retains incomplete evidence.

The result binds exact binary, primary build receipt, runtime template, source/oracle/input hashes, raw client chunks/events, upstream dispatches, process logs and status. Idle and concurrent memory come from the post-exec gateway child's current `/proc/<pid>/status` RSS, sampled every 20 ms for this bounded measurement. Parent/inherited high-water RSS is never used. Comparisons retain event/usage/status/schema/field order; only generated opaque IDs and fresh clocks are normalized. Any difference is retained and fails acceptance until its cause is explained.

JSON measurements use `--json-benchmark /absolute/worktree/.data/<native-release>/uos-json-bench --json-sha256 <accepted-binary-hash> --build-receipt <native-target-result.json> --bun /usr/local/bin/bun --output <new-task-directory> --timeout 180`. The actual production `rt` parser/stringifier and Bun receive identical hashed 1 KiB, 16 KiB and 1 MiB class inputs. Three interleaved runtime pairs each report the first actual parse/stringify call, a separate first timed batch and eight warm batches, median, p95, throughput, checksums and process wall time. Binary hash, native profile owner, compiler hash and unchanged-input receipt are checked before execution. This target does not require gateway credentials or state and can run independently after its qualified native target is built.
