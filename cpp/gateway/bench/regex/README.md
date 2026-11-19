# Regex and supervisor redaction measurements

The fixtures are synthetic and derived from the production pattern inventory in `docs/rt/regex.md` and the six existing supervisor redaction stress inputs. The native benchmark calls the actual `rt::Regex` and `redactSupervisorSecrets` implementations. No provider, credentials, service, or database is used.

Run from the repository root after the shared build owner has admitted the selected profile and target:

```sh
python3 cpp/gateway/tools/build.py native release checkpoint --incremental --target uos_bench_regex --tests --jobs 1
python3 cpp/gateway/bench/regex/make_fixtures.py .data/regex-fixtures.json
python3 cpp/gateway/bench/regex/run.py \
  --binary .data/cpp-gateway-native-release/uos_bench_regex --manifest .data/regex-fixtures.json --output .data/regex-before
```

Each output directory must be new. Runs execute serially in fresh HOME, TMPDIR, and working directories inside a Linux user/network namespace, with an explicit credential-empty environment. Coordinate the measurement window with other performance work; record remaining compiler activity or host load as noise.

The default is three warmup batches and seven measured batches, with fixture-specific iteration counts. Result normalization and serialization happen after timing. Exact results are retained in the individual logs. The Unicode sticky case starts inside an emoji surrogate pair. Existing correctness suites remain required.

`results.json` retains every sample, median, nearest-rank p95, operations and input bytes per second, and whole-child maximum resident set size from the existing `build_support.measured` wait4 helper. With seven samples the reported p95 is the maximum. RSS includes startup, manifest loading, and runtime state. Each child has a combined stdout/stderr log and command/exit/resource receipts with process-group settlement and the shared host reserve. `identity.json` binds the executable, consumed local sources/headers, measurement helper, qualified profile owner, and all nine compile commands; changed inputs fail the run.

If a material slowdown needs diagnosis, `profile.py` samples only its own unmodified benchmark inferior through the installed GDB, in a private PID/network namespace. Sampling is bounded at 400 interruptions spaced 10 ms apart and the workload exits normally; a timeout settles the owned process group/namespace. This diagnostic is separate from timing acceptance:

```sh
python3 cpp/gateway/bench/regex/profile.py --binary .data/cpp-gateway-native-release/uos_bench_regex --manifest .data/regex-fixtures.json --case redact-word --iterations 100 --output .data/regex-profile-word
```

Keep raw before/after results and profile evidence. Production regex changes require an exact source claim and a measured root cause; never replace source semantics or weaken existing assertions to improve a number.
