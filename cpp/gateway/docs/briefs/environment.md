# Retire the unused in-process dotenv parser

Plan row 53. Target 120–150 net first-party lines. Risk 1. Coordinate with the env helper owner on the branch that includes its changes.

Own `src/rt/env.cpp:64–201,218–265`, the corresponding declarations and fixture scopes in `include/uos/rt/env.hpp`, and `tests/rt/env_test.cpp`. Keep `get` and the existing override interface used by the compaction smokes and runtime tests. The operator environment is described in [RUNTIME.md](../RUNTIME.md#launch-and-offline-tools).

1. Trace `loadEnvFile` and `parseEnvFile` across source and test closures. Verify the packaged entrypoints receive explicit environment values and that deployments use their existing launcher policy. Remove the in-process file loader and parser when all references are fixtures.
2. Keep a C++ view of the first-use process snapshot, copying entries from the OS. Use the existing snapshot/override seam, with ordinary container lookup and RAII where simpler. Keep missing distinct from explicit empty, preserve overwrite and nested restoration, and ensure a change to the view never mutates libc or a spawned child's inherited environment.
3. Reuse the synthetic file corpus in the env fixture to prove files in cwd/home have no effect on the runtime view, and use conflicting explicit values to prove the input that wins. Preserve enumeration and override cases. Keep TLS reads and the subprocess environment build outside the patch. Use only fixture data.

```bash
for test in tests/rt/env_test.cpp tests/rt/process_test.cpp \
  tests/runtime_config_differential_test.cpp tests/jev_compaction_smoke_test.cpp; do
  bash cpp/gateway/tools/ci-build.sh --config release "$test" || exit 1
done
```

Accept when the native source exposes no dotenv entrypoint, an isolated file cannot supply credentials or configuration, and the parent view, nested overrides and child environment checks all pass. [Env parser](../../src/rt/env.cpp#L82) · [Overrides](../../src/rt/env.cpp#L241) · [Test driver](../CI.md)
