# Provider sanitizer bring-up — 2026-11-17

Normal validation passes **99 binaries / 1156 cases**. ASan/UBSan passes every case assertion,
but **six outside-owned LSan failures leave 93/99 successful binary exits**. Owned callback
and fixture cycles are fixed; three lifetime regressions pass. Starting commit: `141cb4d482a3b0d8624ce0411cbd3b93e45873e6`.

## Contract v1

Objective: build and run the 99 top-level provider-shard test binaries first
without sanitizers, then with Clang AddressSanitizer and UndefinedBehaviorSanitizer.
Preserve the Bun 1.4.0 behavioral oracle, all existing expected values, source
layout, coroutine ordering, and fatal warning policy. Fix root causes locally.

The behavior matrix consists of the existing Claude/Anthropic, Lithos, Cerebras,
DeepSeek, Surplus, OpenRouter, OpenAI, provider-capacity/selection, metering,
chat/responses, input, usage, and lifecycle fixtures selected by the task's exact
filename prefixes. These are offline fixture contracts; no upstream provider
request, credential mutation, service activation, or deployment is authorized.

Owned production paths are `src/provider/**`, `src/deepseek/**`, `src/chat/**`,
`src/openai*.cpp`, `src/responses_*.cpp` except `src/responses_stream.cpp`,
`src/upstream_wire.cpp`, `src/metered_quota.cpp`, `src/request_policy.cpp`,
`src/input_normalization.cpp`, and their matching `include/uos/` headers, relative
to `cpp/gateway/`. Only selected shard tests/their vector includes,
`tests/lsan-providers.supp`, and this report may also change. Shared runtime,
tools, build configuration, other tests, TypeScript, and `.github/` are read-only.
Outside-owned defects will be recorded with reproduction and diagnostics.

Acceptance: every selected binary builds warning-free and runs with a fresh empty
HOME, TMPDIR, and working directory, a credential-free environment allowlist,
and a 600-second process-group timeout. Sanitized builds use `-O1 -g
-fsanitize=address,undefined -fno-omit-frame-pointer
-fno-sanitize-recover=undefined`; execution sets
`ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1`
and `UBSAN_OPTIONS=print_stacktrace=1`. Normal builds use `-O1 -g`.
Every runtime source links into every test; each module compiles once per profile,
and each test uses its resolved Links closure, preserving test-local stand-ins.

Unknowns: dependency availability, host-specific fixture assumptions, complete
link closures, sanitizer defects, and publication authentication. No provider
behavior change will be inferred from an unobserved assumption. Stop with both
full-profile receipts and every finding fixed or precisely reported, or with a
committable partial report if the runner's execution budget expires.

Long-operation budget: four compiler workers, expected 15–35 minutes per cold
profile, progress in external build logs, incremental recovery after diagnosis.
Tests are serial with a 600-second timeout each; expected seconds per binary.
All downloads, build drivers, objects, binaries, scratch, and full logs stay under
`/tmp/cppgw-asan-providers-20261117`, outside the checkout. Report only compact
phase changes and failing diagnostics. No hosted workflow will be dispatched.

## Findings

| Test or phase | Kind        | Cause                                                               | Fix or disposition                                                                                                                          |
| ------------- | ----------- | ------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| Discovery     | Inventory   | Exact filename-prefix selection contains 99 test binaries           | Normal: 99 binaries / 1156 cases pass; sanitizer: 93 binaries exit 0, six outside-owned LSan failures; all 1156 cases pass their assertions |
| Dependencies  | Preparation | Boost.JSON, Boost.Container, and Brotli development packages absent | Installed distribution Boost 1.90 and Brotli 1.2.0; Clang 21.1.8 and Bun 1.4.0 already present                                              |

## Commands and receipts

Dependency preparation:

```sh
sudo apt-get update -qq
sudo apt-get install -y -qq libboost-json-dev libboost-container-dev libbrotli-dev cmake ninja-build
```

Host dependencies: Clang 21.1.8 with libstdc++, Boost 1.90, OpenSSL 3.5.5,
SQLite 3.46.1, zlib 1.3.1, ICU 78.2, Brotli 1.2.0, Zstd 1.5.7,
CMake 4.2.3, Ninja 1.13.2. Bun 1.4.0 is available for oracle checks.
Unlike the runtime shard, these tests have no observed SQLite-version assertion;
the installed library exceeds the requested SQLite 3.40 minimum.

The external driver resolves the existing Links registry once, preserving each
ordered closure and test-local stand-ins. The inventory has 44 runtime sources,
263 other production sources, 11 helper sources, and 99 test sources: 417 object
compilations and 99 links per profile. Every runtime object is linked directly
into every binary. There are no registry skips or environment overrides.

Commands (repository root, UID/GID 1001):

```sh
python3 /tmp/cppgw-asan-providers-20261117/checks.py inventory
python3 /tmp/cppgw-asan-providers-20261117/checks.py build --profile plain --jobs 4
sudo -n unshare --net -- /usr/bin/python3 /tmp/cppgw-asan-providers-20261117/checks.py run --profile plain --uid 1001 --gid 1001
python3 /tmp/cppgw-asan-providers-20261117/checks.py build --profile asan --jobs 4
sudo -n unshare --net -- /usr/bin/python3 /tmp/cppgw-asan-providers-20261117/checks.py run --profile asan --uid 1001 --gid 1001
```

The runner enables only loopback inside a private network namespace and requires
`192.0.2.1:443` to fail with `ENETUNREACH` before dropping all supplementary groups
and switching to UID/GID 1001. Each test receives only PATH, LANG, LC_ALL, TZ,
fresh HOME/TMPDIR, and the sanitizer options. Its cwd is a third fresh directory
under the same short `/tmp/pv-*` root. No environment credential is inherited.
Build processes also use an environment allowlist. CMake/Ninja files, expanded
compile commands, build logs, per-run logs/counts, full consumed-tree hashes,
and source identity receipts are retained under the external profile directories.
Dirty checkout receipts have null accepted commit/tree fields.

Both configurations use the required warning flags and native dependency bindings:

```text
-std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror
-DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB
-DUOS_GATEWAY_NATIVE_LIBRARIES -pthread -O1 -g
-I <checkout>/cpp/gateway/include -I <checkout>/cpp/gateway
```

ASan adds the sanitizer flags in the contract. Every link includes
`boost_json boost_container ssl crypto pthread dl sqlite3 z brotlidec zstd icuuc icui18n`;
ASan links also use `-fsanitize=address,undefined`. The driver generates one CMake
OBJECT target per source and one executable per test with that test's exact
object list. Link concurrency is one; total build concurrency is four.

## Initial unsuppressed sanitizer checkpoint

All 417 objects and 99 binaries built warning-free. The initial complete run
finished all 99 binaries: **90 exited successfully, 9 failed LeakSanitizer**.
There was no ASan invalid-access or UBSan diagnostic and no timeout. Captured
harness summaries total 1100 run / 1100 passed / 0 failed; seven leaking
processes exited during leak checking before buffered stdout was flushed, so
those 53 normal-profile cases are not counted as observed sanitizer passes yet.

Run: `/tmp/cppgw-asan-providers-20261117/asan/runs/1794916767039882095`.
No source input changed. Investigation is ongoing; no suppression is active.

| Test                              | Kind | Initial leak bytes / allocations | Current diagnosis                                                                          |
| --------------------------------- | ---- | -------------------------------: | ------------------------------------------------------------------------------------------ |
| `claude_header_meter_test`        | LSan |                      10770 / 104 | Shared runtime HTTP shutdown cycle; outside owned paths                                    |
| `lithos_tier_windows_test`        | LSan |                      30132 / 299 | Provider cycle fixed; remaining 26692 / 267 is outside-owned HTTP/fixture retention        |
| `lithos_wiring_test`              | LSan |                        7200 / 75 | Fixed: preserve late aborts, release pending-cancellation callback cycles at loop teardown |
| `openai_compat_part11_test`       | LSan |                        3072 / 17 | Fixed: weak capture in the slow-health fixture abort listener                              |
| `oss_gateway_http_test`           | LSan |                      25184 / 225 | Shared runtime HTTP shutdown cycle; outside owned paths                                    |
| `oss_stream_lifecycle_test`       | LSan |                       11014 / 66 | Fixed: release the fixture-owned iterator after its disposal checks                        |
| `responses_conformance_http_test` | LSan |                  2847334 / 13884 | Shared runtime HTTP shutdown cycle; outside owned paths                                    |
| `responses_native_headers_test`   | LSan |                      84533 / 139 | Shared runtime HTTP shutdown cycle; outside owned paths                                    |
| `responses_stream_http_test`      | LSan |                        3231 / 35 | Shared runtime HTTP shutdown cycle; outside owned paths                                    |

## Owned fixes and focused evidence

- `src/provider/stream_relay.cpp`: the downstream listener retains
  `PendingCancellation`, whose callback retains the Lithos cancellation controller.
  That controller pins the dependent signal containing the same listener. Keep
  the source's late-abort callback live, and weakly register callback owners with
  an Asio loop service that releases callbacks without invoking them at loop
  shutdown. No await, stream terminal, cancellation count, or accounting order changes.
- `tests/oss_stream_lifecycle_test.cpp`: the fixture stores an iterator whose
  wrappers and generator capture that same fixture state. Drop the fixture's
  iterator copy after its existing disposal assertions and return handling.
  Add two lifetime regressions for silent loop teardown and exactly-one late
  cancellation after completion. All 20 lifecycle cases pass both normal and
  unsuppressed ASan/UBSan profiles.
- `tests/openai_compat_part11_test.cpp`: its slow-health fixture's stored
  abort listener captures its own state strongly. Capture that state weakly,
  preserving every assertion and error propagation while the fixture is alive.

The real `src/provider/stream-relay.ts` chat relay was run under Bun 1.4.0,
with its telemetry/translator/wire helper imports replaced by inert test seams,
empty HOME/TMPDIR, no dotenv/automatic installs, and a private network namespace.
Its observed result is:

```json
{ "bun": "1.4.0", "output": "data: [DONE]\n\n", "before": 0, "after": 1 }
```

The regression's C++ weak-owner assertions concern C++ resource release and
have no JavaScript expected-value substitution. The complete rerun confirms these three owned leak fixes: the lifecycle, Lithos
wiring, and OpenAI Part11 binaries now exit 0 without suppressions.

## Oracle reproduction

The following script is the exact isolated dependency-seam oracle used above.
Save this block as `/tmp/cppgw-asan-providers-20261117/relay-oracle.ts` and run:

```sh
mkdir -p /tmp/cppgw-asan-providers-20261117/oracle/h /tmp/cppgw-asan-providers-20261117/oracle/t
sudo -n unshare --net -- setpriv --reuid=1001 --regid=1001 --clear-groups env -i PATH=/home/runner/.bun/bin:/usr/bin:/bin HOME=/tmp/cppgw-asan-providers-20261117/oracle/h TMPDIR=/tmp/cppgw-asan-providers-20261117/oracle/t bun --no-env-file --no-install /tmp/cppgw-asan-providers-20261117/relay-oracle.ts "$PWD/src/provider/stream-relay.ts"
```

Script SHA-256: `d31f8b5d279191c877acecf9fd3c092522aea986092215103494d8f277c10e3e`.

```ts
import { plugin } from "bun";
const relayPath = process.argv[2]!;
const noops = [
  "recordCompletionUsage",
  "recordErrorUsage",
  "recordFirstSemanticCommitment",
  "recordFirstUpstreamSseEvent",
  "recordStreamTerminal",
  "recordStreamTerminalType",
  "recordTerminalUsage",
];
plugin({
  name: "isolated-relay-dependencies",
  setup(build) {
    build.onResolve({ filter: /^\.\.\// }, (args) =>
      args.importer === relayPath ? { path: args.path, namespace: "relay-oracle" } : undefined
    );
    build.onLoad({ filter: /.*/, namespace: "relay-oracle" }, ({ path }) => {
      let contents: string;
      if (path === "../openai-telemetry.ts")
        contents =
          noops.map((name) => `export const ${name} = () => {};`).join("\n") +
          "\nexport const extractChatUsageTokens = () => null;";
      else if (path === "../deepseek/responses-stream.ts")
        contents =
          "export const createDeepSeekResponsesStreamTranslator = () => {}; export const encodeResponsesEvent = () => {};";
      else if (path === "../chat/stream-translation.ts")
        contents =
          'export const EMPTY_UPSTREAM_COMPLETION_MESSAGE = "empty"; export const markChatSemanticOutput = () => {};';
      else if (path === "../upstream-wire.ts")
        contents =
          "export const chatChunkHasAnswerBearingOutput = () => false; export const isAnswerBearingCompletion = () => false; export const isGatewayDeadlineResponse = () => false;";
      else if (path === "../utils.ts")
        contents =
          'export const getString = value => typeof value === "string" ? value : undefined; export const isRecord = value => value !== null && typeof value === "object";';
      else throw new Error("Unexpected runtime dependency: " + path);
      return { contents, loader: "js" };
    });
  },
});
const { relayChatCompletionStream } = await import(relayPath);
const cancellation = new AbortController();
const signal = AbortSignal.any([cancellation.signal]);
let calls = 0;
const adapter = {
  responseHeaders: () => ({}),
  frames: async function* () {
    yield { kind: "done" };
  },
  recordResponseHealth() {},
  recordProviderError() {},
  recordCancellation() {},
  recordIncompleteResponse() {},
  recordFinishFailureKind() {},
  recordTransportFailure() {},
  terminalTypeForError: () => "error",
  streamErrorCode: "fixture_error",
};
const response = relayChatCompletionStream(
  adapter,
  new Response(),
  null,
  undefined,
  signal,
  signal,
  "deepseek-flash",
  (reason) => {
    calls++;
    cancellation.abort(reason);
  }
);
const output = await response.text();
const before = calls;
cancellation.abort("late downstream abort");
cancellation.abort("second downstream abort");
console.log(JSON.stringify({ bun: Bun.version, output, before, after: calls }));
```

## Normal-profile checkpoint

The initial normal build completed all **417 object compilations and 99 links**
with the fatal warning policy unchanged. The serial isolated run passed
**99 binaries, 1153 run / 1153 passed / 0 failed**, zero timeouts and skips.
No portability fix or expected-value change was required.

Run directory: `/tmp/cppgw-asan-providers-20261117/plain/runs/1794915165866951138`.
All source hashes and checkout identity stayed unchanged across build and run.
This is an exploratory dirty-report receipt; commit/tree fields are null until
final committed-source verification. The normal result gates the sanitizer phase.

## Initial behavior matrix and normal counts

The matrix was selected once by the exact task prefixes. All existing oracle
fixtures remain unchanged. Counts include differential, stream, quota, lifecycle,
and real loopback ingress cases within each selected group.

| Prefix group  | Binaries | Normal cases passed |
| ------------- | -------: | ------------------: |
| `anthropic_`  |        5 |                  73 |
| `capacity_`   |        2 |                  21 |
| `cerebras_`   |        4 |                  33 |
| `chat_`       |        1 |                  43 |
| `claude_`     |       17 |                 153 |
| `deepseek_`   |        9 |                 217 |
| `input_`      |        1 |                   5 |
| `lithos_`     |       10 |                 106 |
| `metered_`    |        4 |                 105 |
| `openai_`     |        7 |                  38 |
| `openrouter_` |        5 |                  55 |
| `oss_`        |        6 |                  91 |
| `provider_`   |       14 |                  84 |
| `removed_`    |        2 |                   2 |
| `responses_`  |        7 |                  91 |
| `surplus_`    |        3 |                  31 |
| `temporary_`  |        1 |                   4 |
| `usage_`      |        1 |                   1 |
| **Total**     |   **99** |            **1153** |

## Intermediate full rerun after initial owned fixes

Both full rebuilds succeed with strict warnings and unchanged expected values.
The two new regressions raise the case total from 1153 to 1155, with 99 binaries.
Three changed objects were rebuilt per profile; unaffected objects were reused.

| Profile                  | Binaries run | Exit 0 | Failed exits | Cases run | Cases passed | Assertion failures | Timeouts |
| ------------------------ | -----------: | -----: | -----------: | --------: | -----------: | -----------------: | -------: |
| Normal `-O1 -g`          |           99 |     99 |            0 |      1155 |         1155 |                  0 |        0 |
| ASan/UBSan, unsuppressed |           99 |     93 |            6 |      1155 |         1155 |                  0 |        0 |

The six sanitizer exits are LSan failures, so the sanitizer gate remains red.
There are no invalid-access ASan reports or UBSan reports. No suppression file
was added. All source hashes and identities remain unchanged during each run.

Full working-source run directories:

- `plain`: `/tmp/cppgw-asan-providers-20261117/plain/runs/1794917741272992583`.
  `results.json` SHA-256: `73ae96d937568f08cb414d404e0a74cd6ec540f3e5d7ccf136e40f3670038291`.
  `provenance.json` SHA-256: `b4cfd53ae0329b8160239c012df69e953012e40a71b20bc5dc2b3cbe7c097926`.
- `asan`: `/tmp/cppgw-asan-providers-20261117/asan/runs/1794917560288522551`.
  `results.json` SHA-256: `ce601524f1eb31c10ea1bd6856f5127ce81385ef252a8853ec5fc85c0764925c`.
  `provenance.json` SHA-256: `f16898953b1b153a3731da7defb1cad1955084a1874d05c74c9b77f30a5c0ad4`.

The clean-commit receipt uses these same commands with `--require-clean`:

```sh
python3 /tmp/cppgw-asan-providers-20261117/checks.py build --profile plain --jobs 4
sudo -n unshare --net -- /usr/bin/python3 /tmp/cppgw-asan-providers-20261117/checks.py run --profile plain --tty-output --require-clean --uid 1001 --gid 1001
python3 /tmp/cppgw-asan-providers-20261117/checks.py build --profile asan --jobs 4
sudo -n unshare --net -- /usr/bin/python3 /tmp/cppgw-asan-providers-20261117/checks.py run --profile asan --tty-output --require-clean --uid 1001 --gid 1001
```

`--tty-output` attaches only harness stdout to a PTY, making libc line-buffer it.
Sanitizer stderr is still written directly to the per-test log. No preload,
extra sanitizer option, test selection, or process-group change is involved.
The initial reports remain preserved; no failed result is overwritten in its
original per-run directory.

## Outside-owned defects: HTTP and conformance fixture retention

`src/runtime/http.cpp:2002` stores `ServeState::abort` as a listener capturing
`shutdown`; that closure at line 1996 captures the same ServeState strongly.
The finished callback at lines 2007–2012 removes the listener from an optional
external signal but never clears the state's own listener. This forms
`ServeState -> abort listener -> shutdown closure -> ServeState`, even when no
external signal was supplied and the server has already finished shutdown.
The simple one-case `responses_stream_http_test` reproduces it independently
of the provider pending-dispatch logic.

`tests/helpers/responses_conformance_fixture.cpp:178–179` has a second retained
backreference: the fixture stores its gateway handle, and that gateway's
ServeState handler captures the fixture. `stop()` closes and drains the servers
but does not release this handle/handler relationship. The runtime retains its
handler after close (unlike the node server implementation, which clears its
request listener at `src/runtime/http.cpp:1176`). This retains the conformance
journal, captured request bodies, closed KV wrapper, and associated telemetry.
The helper and runtime paths are both prohibited; they were not edited.
No independent defect is attributed to the telemetry allocations retained by
these owner graphs, or to `src/responses_stream.cpp`.

Reproduce any failed binary in the same isolated environment, for example:

```sh
sudo -n unshare --net -- /usr/bin/python3 /tmp/cppgw-asan-providers-20261117/checks.py run --profile asan --tty-output --filter responses_stream_http_test --uid 1001 --gid 1001
```

Replace the filter with each exact binary name below. Every command exits 1;
all its case assertions pass. The first report lines in each log are:

```text
=================================================================
==<pid>==ERROR: LeakSanitizer: detected memory leaks
```

| Test                              | Cases passed | Final leaked bytes | Allocations | First relevant evidence                                                                |
| --------------------------------- | -----------: | -----------------: | ----------: | -------------------------------------------------------------------------------------- |
| `claude_header_meter_test`        |            4 |              10770 |         104 | `src/runtime/http.cpp:1914,1999,2002`                                                  |
| `lithos_tier_windows_test`        |            9 |              26692 |         267 | `src/runtime/http.cpp:1914,1999,2002`                                                  |
| `oss_gateway_http_test`           |            5 |              25184 |         225 | `src/runtime/http.cpp:1914,1999,2002`                                                  |
| `responses_conformance_http_test` |            7 |            2847334 |       13884 | `tests/helpers/responses_conformance_fixture.cpp:141,178–179`; shared HTTP allocations |
| `responses_native_headers_test`   |            9 |              84533 |         139 | `src/runtime/http.cpp:1914,1999,2002`                                                  |
| `responses_stream_http_test`      |            1 |               3231 |          35 | `src/runtime/http.cpp:1914,1999,2002`                                                  |

The shared runtime/fixture owners must fix these cycles and rerun the six
commands above. Their unresolved leaks are not documented model limits, so they
are not eligible for a provider-shard suppression.

## Delivery preflight

`gh auth status` reports no authenticated GitHub host. The credential-free
publication capability check:

```sh
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false git push --dry-run origin HEAD:refs/heads/test/cppgw-asan-providers-20261117
```

exits 128 with:

```text
error: unable to read askpass response from '/bin/false'
fatal: could not read Username for 'https://github.com': terminal prompts disabled
```

The enclosing runner promises publication when this job stops. No independent
push, pull request, merge, or development integration is claimed yet. The task
branch will remain committable for that authenticated publisher. Pre-existing
production-build commits are not accepted as completed work by this task.

## Reproducible external driver

The exact driver is embedded below so reproduction does not depend on retained
runner files. Extract it outside the checkout before using the commands above:

````sh
python3 - <<'PYTHON'
from pathlib import Path
report = Path('cpp/gateway/docs/findings/cppgw-asan-providers-20261117.md').read_text()
block = report.rsplit('<!-- BEGIN PROVIDER DRIVER -->', 1)[1].split('<!-- END PROVIDER DRIVER -->', 1)[0].strip()
code = block.split('\n', 1)[1].rsplit('\n```', 1)[0]
output = Path('/tmp/cppgw-asan-providers-20261117/checks.py')
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(code + '\n')
PYTHON
````

Driver SHA-256: `634cbd7612d47014b46b36e25626f37d52cc2b0cfbc42d3163318bf31cac5314`.
The initial file-output driver hash was `7631ddb0a99bf7044b127783c2ee96c778018e95edb15a05ba6b493a05fa211e`;
`--tty-output` was added to preserve harness summaries before LSan exits. It changes
only stdout capture; sanitizer stderr, isolation, flags, process groups, and timeout remain identical.

<!-- BEGIN PROVIDER DRIVER -->

```python
#!/usr/bin/env python3
import argparse
import errno
import errno
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import selectors
import signal
import socket
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent
GATEWAY = Path.cwd() / 'cpp/gateway'
PREFIXES = tuple('claude_ lithos_ cerebras_ deepseek_ anthropic_ surplus_ metered_ provider_ openrouter_ openai_ responses_ chat_ oss_ removed_ capacity_ temporary_ usage_ upstream_ input_'.split())
ENV = {'PATH': '/usr/local/bin:/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TZ': 'UTC'}
FLAGS = '-std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror -DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB -DUOS_GATEWAY_NATIVE_LIBRARIES -pthread -O1 -g'.split()
SAN = '-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined'.split()

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def inputs():
    paths = sorted(p for d in ['src', 'include', 'tests'] for p in (GATEWAY / d).rglob('*') if p.is_file())
    paths += [GATEWAY / 'tools/links.py']
    return {str(p.relative_to(GATEWAY)): sha(p) for p in paths}

def identity():
    values = subprocess.check_output(['git', 'rev-parse', 'HEAD', 'HEAD^{tree}'], env=ENV, text=True).splitlines()
    clean = not subprocess.check_output(['git', 'status', '--porcelain'], env=ENV, text=True)
    return {'head_commit': values[0], 'head_tree': values[1], 'clean': clean,
            'commit': values[0] if clean else None, 'tree': values[1] if clean else None}

def inventory():
    spec = importlib.util.spec_from_file_location('links', GATEWAY / 'tools/links.py')
    links = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(links)
    registry = links.read_registry()
    rows = []
    for test in sorted((GATEWAY / 'tests').glob('*_test.cpp')):
        if not test.name.startswith(PREFIXES):
            continue
        rel = str(test.relative_to(GATEWAY))
        result = links.resolve(rel, registry)
        if result.skip:
            raise SystemExit('Registry skip prohibited for selected test: ' + rel)
        rows.append({'test': rel, 'sources': result.sources, 'env': result.env, 'origin': result.origin})
    return rows

def command(argv, log):
    print('$ ' + shlex.join(map(str, argv)), flush=True)
    with log.open('a') as output:
        output.write('\n$ ' + shlex.join(map(str, argv)) + '\n')
        output.flush()
        code = subprocess.call(argv, stdout=output, stderr=subprocess.STDOUT, env=ENV)
    if code:
        print(log.read_text()[-14000:], flush=True)
        raise SystemExit(code)

def build(args, rows):
    root = ROOT / args.profile
    driver = root / 'driver'
    driver.mkdir(parents=True, exist_ok=True)
    (root / 'inventory.json').write_text(json.dumps(rows, indent=2) + '\n')
    rt = [str(p.relative_to(GATEWAY)) for p in sorted((GATEWAY / 'src/rt').glob('*.cpp'))]
    sources = sorted(set(rt + [s for r in rows for s in r['sources']]))
    def quote(p):
        return '"' + str(p).replace('"', '\\"') + '"'
    def target(p):
        return 'obj_' + hashlib.sha256(p.encode()).hexdigest()[:16]
    lines = ['cmake_minimum_required(VERSION 3.25)', 'project(provider_checks LANGUAGES CXX)',
             'set(CMAKE_EXPORT_COMPILE_COMMANDS ON)', 'set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")',
             'set_property(GLOBAL PROPERTY JOB_POOLS links=1)', 'set(CMAKE_JOB_POOL_LINK links)',
             'add_compile_options(' + ' '.join(FLAGS + (SAN if args.profile == 'asan' else [])) + ')',
             'include_directories(' + quote(GATEWAY / 'include') + ' ' + quote(GATEWAY) + ')']
    if args.profile == 'asan':
        lines += ['add_link_options(-fsanitize=address,undefined)']
    for source in sources:
        lines += [f'add_library({target(source)} OBJECT {quote(GATEWAY / source)})']
    for row in rows:
        name = Path(row['test']).stem
        objects = ' '.join('$<TARGET_OBJECTS:' + target(p) + '>' for p in rt + row['sources'])
        lines += [f'add_executable({name} {quote(GATEWAY / row["test"])} {objects})',
                  f'target_link_libraries({name} PRIVATE boost_json boost_container ssl crypto pthread dl sqlite3 z brotlidec zstd icuuc icui18n)']
    cmake = driver / 'CMakeLists.txt'
    text = '\n'.join(lines) + '\n'
    if not cmake.exists() or cmake.read_text() != text:
        cmake.write_text(text)
    receipt = {'identity': identity(), 'inputs': inputs(), 'driver_sha256': sha(Path(__file__)), 'profile': args.profile,
               'runtime_units': len(rt), 'module_support_units': len(sources) - len(rt), 'tests': len(rows)}
    (root / 'build-start.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: receipt[k] for k in ['profile', 'runtime_units', 'module_support_units', 'tests']}), flush=True)
    command(['cmake', '-S', str(driver), '-B', str(root), '-G', 'Ninja', '-DCMAKE_CXX_COMPILER=clang++'], root / 'build.log')
    targets = [Path(r['test']).stem for r in rows if args.filter in r['test']]
    command(['cmake', '--build', str(root), '-j', str(args.jobs), '--target', *targets], root / 'build.log')
    receipt['inputs_unchanged'] = receipt['inputs'] == inputs()
    receipt['identity_unchanged'] = receipt['identity'] == identity()
    (root / 'build-end.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if not receipt['inputs_unchanged'] or not receipt['identity_unchanged']:
        raise SystemExit('Inputs or checkout identity changed during build')

def wait_with_output(process, terminal, output, timeout):
    if terminal is None:
        return process.wait(timeout=timeout)
    deadline = time.monotonic() + timeout
    with selectors.DefaultSelector() as selector:
        selector.register(terminal, selectors.EVENT_READ)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not selector.select(remaining):
                raise subprocess.TimeoutExpired(process.args, timeout)
            try:
                chunk = os.read(terminal, 65536)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                chunk = b''
            if not chunk:
                break
            os.write(output.fileno(), chunk)
    return process.wait(timeout=max(deadline - time.monotonic(), 0.001))

def run(args, rows):
    if os.geteuid() != 0 or args.uid is None or args.gid is None:
        raise SystemExit('Use sudo unshare --net with --uid and --gid for isolated execution')
    subprocess.run(['ip', 'link', 'set', 'lo', 'up'], env=ENV, check=True)
    with socket.socket() as probe:
        code = probe.connect_ex(('192.0.2.1', 443))
        if code != errno.ENETUNREACH:
            raise SystemExit('Network isolation probe failed: ' + str(code))
    os.setgroups([])
    os.setgid(args.gid)
    os.setuid(args.uid)
    root = ROOT / args.profile
    run_root = root / 'runs' / str(time.time_ns())
    run_root.mkdir(parents=True)
    before = inputs()
    original_identity = identity()
    if args.require_clean and not original_identity['clean']:
        raise SystemExit('Clean committed tree required')
    receipt = {'identity': original_identity, 'inputs': before, 'profile': args.profile,
               'driver_sha256': sha(Path(__file__)), 'network_probe_errno': code, 'timeout': args.timeout, 'tty_output': args.tty_output}
    (run_root / 'provenance.json').write_text(json.dumps(receipt, indent=2) + '\n')
    results = []
    for row in rows:
        if args.filter not in row['test']:
            continue
        name = Path(row['test']).stem
        start = time.monotonic()
        log = run_root / (name + '.log')
        with tempfile.TemporaryDirectory(prefix='pv-', dir='/tmp') as scratch:
            scratch = Path(scratch)
            for d in ['h', 't', 'w']:
                (scratch / d).mkdir()
            env = ENV | {'HOME': str(scratch / 'h'), 'TMPDIR': str(scratch / 't')}
            for assignment in row['env']:
                key, value = assignment.split('=', 1)
                if re.search(r'(KEY|TOKEN|SECRET|PASSWORD|CREDENTIAL|AUTH)', key):
                    raise SystemExit('Unexpected credential registry variable: ' + key)
                env[key] = value.replace('${SCRATCH}', str(scratch))
            if args.profile == 'asan':
                env |= {'ASAN_OPTIONS': 'detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1',
                        'UBSAN_OPTIONS': 'print_stacktrace=1'}
                if args.suppressions:
                    env['LSAN_OPTIONS'] = 'suppressions=' + str(args.suppressions.resolve())
            argv = [str(root / 'bin' / name)] + ([args.case] if args.case else [])
            with log.open('w') as output:
                output.write('$ ' + shlex.join(argv) + '\n')
                output.flush()
                if not Path(argv[0]).is_file():
                    status = 127
                else:
                    terminal, child_output = os.openpty() if args.tty_output else (None, None)
                    process = subprocess.Popen(argv, env=env, cwd=scratch / 'w',
                                               stdout=child_output if child_output is not None else output,
                                               stderr=output, start_new_session=True)
                    if child_output is not None:
                        os.close(child_output)
                    try:
                        status = wait_with_output(process, terminal, output, args.timeout)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
                        status = 124
                    finally:
                        if terminal is not None:
                            os.close(terminal)
            text = log.read_text(errors='replace')
            summaries = re.findall(r'(\d+) run, (\d+) passed, (\d+) failed', text)
            counts = list(map(int, summaries[-1])) if summaries else None
            result = {'test': name, 'exit': status, 'counts': counts, 'seconds': round(time.monotonic() - start, 3), 'log': str(log)}
            results.append(result)
            print(json.dumps(result), flush=True)
            (run_root / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            (root / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    receipt['inputs_unchanged'] = before == inputs()
    receipt['identity_unchanged'] = original_identity == identity()
    (run_root / 'provenance.json').write_text(json.dumps(receipt, indent=2) + '\n')
    ok = receipt['inputs_unchanged'] and receipt['identity_unchanged'] and all(r['exit'] == 0 and r['counts'] and r['counts'][0] > 0 and r['counts'][2] == 0 for r in results)
    print(json.dumps({'profile': args.profile, 'binaries': len(results), 'passing_binaries': sum(r['exit'] == 0 and r['counts'] and r['counts'][2] == 0 for r in results), 'counts': [sum(r['counts'][i] for r in results if r['counts']) for i in range(3)], 'ok': ok, 'run': str(run_root)}), flush=True)
    return 0 if ok else 1

parser = argparse.ArgumentParser()
parser.add_argument('action', choices=['build', 'run', 'inventory'])
parser.add_argument('--profile', choices=['plain', 'asan'], default='plain')
parser.add_argument('--jobs', type=int, default=4)
parser.add_argument('--timeout', type=int, default=600)
parser.add_argument('--filter', default='')
parser.add_argument('--case', default='')
parser.add_argument('--uid', type=int)
parser.add_argument('--gid', type=int)
parser.add_argument('--require-clean', action='store_true')
parser.add_argument('--tty-output', action='store_true', help='Line-buffer harness stdout through a PTY; sanitizer stderr remains a file')
parser.add_argument('--suppressions', type=Path)
args = parser.parse_args()
rows = inventory()
if args.action == 'inventory':
    (ROOT / 'inventory.json').write_text(json.dumps(rows, indent=2) + '\n')
    print(json.dumps({'tests': len(rows), 'environment_overrides': [r for r in rows if r['env']]}))
elif args.action == 'build':
    build(args, rows)
else:
    raise SystemExit(run(args, rows))
```

<!-- END PROVIDER DRIVER -->

## Final gates and handoff checkpoint

Changed paths and smallest relevant gates:

- `src/provider/stream_relay.cpp`: strict C++20 compilation and the task-requested
  full 99-binary shard in both profiles; all its effective source closures are used.
- `tests/oss_stream_lifecycle_test.cpp`: all 20 lifecycle cases in both profiles,
  including the two new lifetime regressions.
- `tests/openai_compat_part11_test.cpp`: all 22 existing compatibility cases in
  both profiles, including the formerly leaking slow-health fixture.
- This report: pinned Prettier 3.9.6, embedded-driver syntax/hash reconciliation,
  and `git diff --check`. All changed C++ files are below the source/test size caps.

Quality dependencies were installed under `/tmp` from the unchanged frozen lint
lock. The repository format wrapper also bootstraps root dependencies and patches
installed Bun types; the equivalent pinned Prettier CLI keeps those generated
artifacts outside this task's owned checkout paths:

```sh
bun --no-env-file --no-install /tmp/cppgw-asan-providers-20261117/lint/node_modules/prettier/bin/prettier.cjs --check cpp/gateway/docs/findings/cppgw-asan-providers-20261117.md
git diff --check
```

No TypeScript application, Rust, unrelated native suite, hosted workflow, or
production/Mac deployment gate was run. Source and test expectations are unchanged
except for the three added lifetime regressions and the documented fixture ownership repairs. No leak suppression
is active. The weak registry preserves callbacks until abort or loop shutdown;
unreachable cyclic signal graphs can remain during a live loop, consistent with
the runtime's existing lifetime boundary and the late-abort contract.

The first clean-commit receipt is preserved below. Its source hashes became stale
for the working tree when the subsequent live-loop refinement changed Lithos
ownership; final-tree validation is required again. Six outside-owned sanitizer
failures remain unresolved.

## First clean source receipt and local review

The initial implementation was committed as
`621e5e2fb0a2047c28520f190adcb1573d6083e6`, tree
`31b1f17f3ce0333e216dc815ee5d0bfcd4d2fa30`, base/parent
`141cb4d482a3b0d8624ce0411cbd3b93e45873e6`.
Both clean full-profile gates reproduced 1155 passing cases: normal 99/99 binary
exits, sanitizer 93/99 exits and the same six LSan failures. All 1368 input
hashes and commit/tree identities stayed unchanged in those runs. Their sorted
input manifest hash is `35a32823c27e047635c305529324c8070944ec7e9a11bf7142f117f830834e39`.
These are exact receipts for that intermediate commit, not the later refinement.

Actual local Codex review mode (CLI 0.169.0):

```sh
timeout 180s codex --sandbox read-only -c 'review_model="codex-auto-review"' review --commit 621e5e2fb0a2047c28520f190adcb1573d6083e6
```

The command exited 0 after 83.399 seconds, with zero credential redactions.
Transcript: `/tmp/cppgw-asan-providers-20261117/review.log`, SHA-256
`6b1253162cd7714f6fd94d8f27bd162e6098fe0d577c6827c83ba83c599e5139`.
Its banner reports `model: ubiquity/deepseek-flash`, `provider: uos`, reasoning
`none`; it does not verify the requested `codex-auto-review` identity.
**Reject this receipt for official review ingestion.** No finding was ingested
or identity-mismatched review marked accepted. The original final finding text
is preserved here; the CLI printed it twice and the complete transcript retains
both copies:

```text
The new registry defers callback release until loop destruction, but production loops are long-lived or process-wide, so completed requests can retain cancellation graphs indefinitely. The added tests only cover loop teardown and do not expose this production lifetime issue.

Review comment:

- [P1] Release completed cancellation callbacks before loop shutdown — /home/runner/work/_temp/agent-jobs/cppgw-asan-providers-20261117/checkout/cpp/gateway/src/provider/stream_relay.cpp:127-129
  When a long-lived HTTP loop handles a completed stream with a non-empty `cancelPending`, this registry only stores a weak reference and clears the callback in `shutdown()`. The downstream abort listener still strongly owns `PendingCancellation`, so its callback and captured cancellation graph remain reachable for the entire loop lifetime; the process-wide default loop is never destroyed, causing unbounded per-request memory retention. Remove or weaken this ownership when the relay is terminal, while preserving only the required late-abort behavior.
```

## Live-loop ownership refinement

Code inspection confirms that Lithos's production callback can own the same
controller whose dependent downstream signal owns that callback. Teardown cleanup
alone preserves the late-abort contract but does not release that production
graph during a running loop. The contract scope and behavioral requirements are
unchanged; the intermediate source receipts are superseded after these edits.

`src/provider/lithos_handlers.cpp` now keeps the cancellation controller in the
owning deferred coroutine input, and the callback captures it weakly. The owner
survives every refusal wait and transport attempt and ends when deferred dispatch
settles. External downstream abort still propagates into the request signal:
`openai::inferenceSignal` is composed from `downstreamSignalFor`; relay-owned
cancellation still interrupts stream reads. The callback remains callable after
completion, with no additional pending work to cancel. No wait, delivery,
settlement, status, or output ordering is changed.

The matching header adds a scoped, normally empty test observer receiving only
a weak controller reference. The new `lithos_wiring_test` case exercises both
wires through three refused attempts, deferred retry, completed streamed output,
and a live-loop weak-owner check. It preserves all existing expected values.
An initial probe of the per-fetch child signal also passed against the old
implementation; it did not observe the source controller, so it was replaced by
the direct weak-controller observer before acceptance. The stronger negative
control is recorded with its actual result below.

The direct-controller regression passes all 29 Lithos wiring cases in the normal
profile. A controlled variant restores only the strong cancellation capture,
keeping the observer and all production/test inputs otherwise identical. The
same new test then fails twice at `observed.expired()` (one failure for each
wire), before the loop is destroyed:

```text
strong-capture control exit 1
    tests/lithos_wiring_test.cpp:204: expected observed.expired()
    tests/lithos_wiring_test.cpp:204: expected observed.expired()
FAIL lithos wiring: completed deferred dispatch releases its signals while the loop stays live
1 run, 0 passed, 1 failed
```

The strong-capture control source SHA-256 is
`298cb130f3dae7a45f25b5c276bef42d1ba214d6c64cc42fd9158a9e3c6c11b9`.
Its expanded compile/link commands are retained in
`/tmp/cppgw-asan-providers-20261117/negative-build-strong.log`, and its output is
`negative-live-loop-strong.log` in the same directory. It is a temporary test
control, not a checkout source edit. It compiles one controlled owned source
object with the normal flags and replaces only that object in the existing
Lithos wiring link closure. The initial per-fetch probe's passing negative
control remains separately retained in `negative-live-loop.log`.

Final changed production/test paths now also include:

- `src/provider/lithos_handlers.cpp` and `include/uos/provider/lithos_handlers.hpp`:
  the owning coroutine input/weak cancellation callback and an inert, scoped,
  weak-only test observer. Both profiles compile the affected header closures.
- `tests/lithos_wiring_test.cpp`: the live-loop release regression on both wires;
  29 cases, with the strong-capture negative control above.

All 99 binaries rebuilt successfully after this refinement. The final case
count is 1156 (three new cases relative to the original 1153). Full clean-commit
run receipts follow below; no pass count for this refined tree is inferred from
the superseded 621e5e2f receipt.

## Final refined-source receipt

Final implementation commit: `e8ea396a523a9ffd1a95dff7fef5dbd74fbe4b3a`.
Tree: `e3f29a9e891c404a268ed6db16fce15c0ae80275`.
Parent: `621e5e2fb0a2047c28520f190adcb1573d6083e6`.
The following documentation-only receipt update changes no C++ input.

On this clean commit, all 417 commands per profile were reconciled against the
required warning, C++20, optimization, debug, and sanitizer flags. All 1368
production/header/test/helper inputs and checkout identities stayed unchanged.
Sorted input manifest SHA-256:
`4e135bf0adc340d03d322180c5ecf1d58790cd7ef5a464642e8840ff2f5cd9b5`.
No C++ edit followed these gates.

Exact final commands (two workers per profile, four total):

```sh
python3 /tmp/cppgw-asan-providers-20261117/checks.py build --profile plain --jobs 2
sudo -n unshare --net -- /usr/bin/python3 /tmp/cppgw-asan-providers-20261117/checks.py run --profile plain --tty-output --require-clean --uid 1001 --gid 1001
python3 /tmp/cppgw-asan-providers-20261117/checks.py build --profile asan --jobs 2
sudo -n unshare --net -- /usr/bin/python3 /tmp/cppgw-asan-providers-20261117/checks.py run --profile asan --tty-output --require-clean --uid 1001 --gid 1001
```

| Profile    | Binaries | Successful exits | LSan failed exits | Cases run | Cases passed | Failed assertions | Timeouts |
| ---------- | -------: | ---------------: | ----------------: | --------: | -----------: | ----------------: | -------: |
| Normal     |       99 |               99 |                 0 |      1156 |         1156 |                 0 |        0 |
| ASan/UBSan |       99 |               93 |                 6 |      1156 |         1156 |                 0 |        0 |

There are zero invalid-access ASan reports and zero UBSan reports. The six
remaining LSan byte/allocation counts exactly match the outside-owned table.
No tests or sanitizer reports were skipped or suppressed. The three new cases
cover loop teardown, exactly-one late abort, and release during a live loop;
the live-loop case rejects the controlled strong-capture implementation.

- `plain`: `/tmp/cppgw-asan-providers-20261117/plain/runs/1794921149515566183`.
  `results.json` SHA-256: `806a16b0e610f2fc3131f676710f5f25f9782131dd641fefc7fbed9c32dbec72`.
  `provenance.json` SHA-256: `4570fcd709051bdd3f14ac519d45c97ee9dc7bd298f6f538de123f00e73b9ec5`.
- `asan`: `/tmp/cppgw-asan-providers-20261117/asan/runs/1794921149547640264`.
  `results.json` SHA-256: `5eec6ac3080e1d8a5ffb360d7332f24c8560ea5d999d439324057e700ad630a9`.
  `provenance.json` SHA-256: `bd241df1bb555fe4514fd79ef07643ef3512bc8e99242f34f94e6a4b99f16ca4`.

The refined implementation also ran actual local review mode:

```sh
timeout 180s codex --sandbox read-only -c 'review_model="codex-auto-review"' review --commit e8ea396a523a9ffd1a95dff7fef5dbd74fbe4b3a
```

Reviewed head/base are the implementation and parent SHAs above. It exited 0
after 84.824 seconds with zero credential redactions. Transcript:
`/tmp/cppgw-asan-providers-20261117/review-refined.log`, SHA-256
`addcfefca5040012afd440de6d03e7dd6f1f3b75340dd47ab6dbacda5a2fb835`.
The banner again reports `ubiquity/deepseek-flash` / `uos`, not verified
`codex-auto-review` identity. This receipt is also rejected for official review
ingestion; no finding was ingested and no model mismatch marked accepted.
Its complete final review text (printed twice in the raw log) is:

```text
The change weakens the deferred cancellation callback while retaining controller ownership in the active dispatch coroutine, preserving cancellation behavior and allowing completed dispatches to release the controller during a live loop. The added regression coverage exercises both supported response paths.
```

The first review's concern is addressed by `lithos_handlers.cpp` and the direct
live-loop regression, with the original head/base/command/model discrepancy
and finding text preserved above. Review status was not a merge gate.

## Publication and remaining work

An actual publication attempt, after the first clean source checks:

```sh
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false git push --set-upstream origin test/cppgw-asan-providers-20261117
```

exited 128 with `fatal: could not read Username for 'https://github.com': terminal prompts disabled`.
The final branch is preserved for the enclosing runner's promised publication.
No credential was printed, stored, refreshed, or replaced. No hosted workflow
was manually triggered and no PR merge or development integration is claimed.

This single-branch clone's fetch refspec covers only the pre-existing port branch;
refresh development explicitly with:

```sh
git fetch origin refs/heads/development:refs/remotes/origin/development
```

The refreshed `origin/development` is `002fe15fa926674c97f0169355d24929bf529710`.
Task-created tips are not its ancestors, and there is no local `development`
branch to prove equal. Preserve the clean owned task branch for the enclosing
publisher; do not integrate the unrelated pre-existing production-build work
or switch the checkout to claim a handoff that has not happened.

Remaining owner work is the six shared HTTP/conformance leak failures and
GitHub publication, pull request, ancestry-preserving merge and final development
checkout handoff. Complete sanitizer qualification is red until those leak
owners fix their paths and rerun. The local provider-shard runs, owned fixes,
regressions, negative control, formatting and source receipts are complete.

## Final handoff summary

Built the 99-test provider shard with Clang 21, C++20, strict warnings and the
requested `-O1 -g` ASan/UBSan flags. Normal: **99/99 binaries, 1156/1156 cases**.
Sanitizer: **93/99 binary exits**, all **1156 cases pass their assertions**;
six outside-owned HTTP/conformance LSan failures remain. No invalid-access ASan,
UBSan, timeout or suppression occurred.

Fixed relay callback teardown, active-coroutine ownership of deferred Lithos
controllers, and two test-fixture self-cycles. Added three lifetime regressions;
the live-loop test fails with the strong capture restored. Bun 1.4.0 confirms
exactly-one late abort. Every changed source, reason, reproduction command,
original/final leak count and receipt is recorded above.

Reproduce both profiles using the embedded driver and the four exact final
commands above. Implementation is committed as `e8ea396a`; publication and
PR/development integration remain with the runner publisher because this clone
has no GitHub write authentication. The shared HTTP/fixture owners must resolve
the remaining leaks; this report does not claim a green sanitizer gate.
