# Building and running the cpp/gateway tests

Every test is one binary: `tests/<name>_test.cpp` (or `tests/rt/<name>_test.cpp`) plus the module
sources it calls, built and run by `tools/unit.sh`, which links every `src/rt/*.cpp` by itself. The
host serves production: build and run only through `tools/unit.sh` (directly or through the two
scripts below), never `g++`/`make` by hand, and run one test at a time. Paths are relative to
`cpp/gateway`.

## One test

```sh
tools/links.py tests/foo_test.cpp                 # the sources that follow the test on the unit.sh line
tools/unit.sh foo tests/foo_test.cpp $(tools/links.py tests/foo_test.cpp)
tools/unit.sh foo tests/foo_test.cpp $(tools/links.py tests/foo_test.cpp) -- "case name substring"
tools/unit.sh --link-only foo tests/foo_test.cpp $(tools/links.py tests/foo_test.cpp) # build/link without executing cases
tools/run-tests.sh foo_test                       # the same, isolated as below, with a log and a result row
```

A test that can reach codex auth, native auth, dispatch or a provider key must not see this host's
credentials (`~/.codex/auth.json`, provider keys in the environment); `run-tests.sh` isolates every
test it runs. When running `unit.sh` by hand, give the test the same environment, for example:

```sh
scratch=$(mktemp -d /tmp/claude-1002/<your dir>/XXXXXX); mkdir -p "$scratch/t" "$scratch/h"
env -u CODEX_HOME -u CODEX_AUTH_JSON_B64 -u OPENAI_API_KEY ... HOME="$scratch/h" TMPDIR="$scratch/t" \
  tools/unit.sh foo tests/foo_test.cpp $(tools/links.py tests/foo_test.cpp)
```

The harness prints `ok  <case>` / `FAIL <case>` per case and ends with `<n> run, <p> passed, <f> failed`;
the exit status is 0 only when every selected case passed and at least one ran.

`--link-only` measures real complete link closures without consuming a runtime acceptance run. The qualified BFD linker uses `--no-keep-memory`, `--reduce-memory-overheads` and a 64 MiB input cache cap. Compiler processes retain the 2 GiB address-space limit and two-slot policy. A link takes both slots in order, uses a 3 GiB address-space limit and starts only with at least 6 GiB MemAvailable after acquisition; insufficient memory returns a clear admission deferral and releases the slots. There is no automatic limit escalation or linker substitution.

`rt::console::Capture::setObserver(std::function<void(const Call&)>)` optionally observes calls synchronously after storage and before passthrough. Empty clears the observer. Mutable callback state persists; reentrant logging and observer replacement/clear are supported, and the supplied Call remains stable during the callback even if stored calls are cleared. Copy it to retain it. Callback exceptions propagate after recording and prevent forwarding. Keep Capture lifetimes and the capture stack unchanged during a callback. Observer storage is external to Capture, preserving its existing size, alignment and default behavior.

## Link lists: `tools/links.py`

`tools/links.py <test>` prints the sources that go after the test file on the `unit.sh` command line
(deduplicated, brace groups expanded, `src/rt/*` dropped because `unit.sh` adds it, every file checked
to exist). It resolves, in this order:

1. an entry in `tests/LINKS.txt` that gives a source list (the registry overrides the comment);
2. the declaration in the test's leading comment (first 40 lines). Forms understood:
   - `// Links: <test> <sources...>` continued on following comment lines that begin with a path
     (any indentation); the test file itself may be listed or not; text from `(` on is prose, so
     `(plus src/rt/*.cpp, which unit.sh adds)` and `(unit.sh links src/rt).` are ignored;
   - prose: `... Links src/catalog/{store,types}.cpp, src/models/x.cpp,` / `// src/y.cpp and src/z.cpp.`
     (commas, `and`, a final period; a list that trails off into prose after `and` is rejected as
     incomplete);
   - `// Links the sources of tests/<other>_test.cpp.` and
     `// Links the sources of tests/<other>_test.cpp but <x>.cpp and <y>.cpp.` (exclusions match by
     path suffix);
   - `// Links: tests/<name>_test.cpp` alone: no extra sources;
3. for `tests/rt/*_test.cpp` without a declaration: no extra sources.

A test that matches none of these, or whose declaration is incomplete, fails to resolve: add an entry to
`tests/LINKS.txt`. Other modes:

```sh
tools/links.py --explain tests/foo_test.cpp       # origin of the list, env, skip, notes, warnings
tools/links.py --check                            # resolve every test; nonzero exit if one fails
tools/links.py --closure --diff tests/foo_test.cpp
tools/links.py --list-tests
tools/links.py --hashes tests/foo_test.cpp       # SHA-256 of local build inputs, including runtime
```

`--closure` computes the smallest set of module objects that defines every symbol the test object
needs, reading the symbol tables of the cached objects with the SDK's `nm` (read-only; nothing is
compiled; build the test object once with `tools/unit.sh --check tests/foo_test.cpp` if it has none).
It cannot see modules a test needs only for their static initialisers (self-registering modules), and a
stale cached object can be off by a module, so confirm a computed list by building. `--closure
--includes` maps each reached `include/uos/a/b.hpp` to `src/a/b.cpp` instead; across the import cycles
that over-approximates widely. `--diff` compares either with the resolved list.

New tests: start the file with `// Links: tests/<name>_test.cpp <every source>` (wrap onto `//` lines
that begin with a path) and check it with `tools/links.py --check tests/<name>_test.cpp`.

`--hashes` follows local quoted includes from the test, its listed sources and every runtime source that `unit.sh` links. It includes build wrappers, test helpers and included vector files. The sweep records these hashes before building and verifies them afterwards; a changed input makes the receipt SKIP with the observed result preserved in its note and log.

Input hashes describe the source files, while the shared object cache uses modification times. If an earlier compile overlaps a source replacement, settle those compiles, have the source owner touch the affected file afterwards, then compile it again through `unit.sh --check` before the stable test run. Retain the explicit compile line: atomic object publication alone does not prevent an older compile from publishing after the replacement.

## The registry: `tests/LINKS.txt`

```
# comment
tests/foo_test.cpp: src/a.cpp src/b/{c,d}.cpp
    src/e.cpp                       an indented line without a field name continues the list
    env: KEY=VALUE OTHER=value      extra environment for the test process; ${SCRATCH} is replaced by
                                    the test's scratch directory
    skip: <reason>                  run-tests.sh reports SKIP with this reason and builds nothing
    note: <text>                    why the entry exists
tests/bar_test.cpp: (header)        keep the file's own declaration; only the fields below apply
    env: ...
tests/baz_test.cpp: (none)          no extra sources
```

An entry with a source list replaces the file's own declaration. Use it for tests without a
declaration, for declarations that are stale or incomplete (say so in a `note:`), for environment a
test needs, and for skips. Do not edit other porters' test files to change their headers.

## The sweep: `tools/run-tests.sh`

```sh
tools/run-tests.sh                    # every test, one at a time
tools/run-tests.sh kv_ codex_auth     # only tests whose path contains one of the substrings
tools/run-tests.sh --list [filters]   # selection, binary names and link origins; builds nothing
tools/run-tests.sh --failed           # re-run tests whose last status is FAIL, BUILD_FAIL or TIMEOUT
tools/run-tests.sh --missing          # resume: only tests without a row in results.tsv
tools/run-tests.sh --cluster ...      # include tests/cluster_w04c1/* (SKIP otherwise)
tools/run-tests.sh --timeout 280 --build-timeout 5400 --no-rescan
tools/run-tests.sh --keep-binaries --keep-scratch foo_test   # keep the binary / scratch dirs for a look
tools/run-tests.sh --prune-binaries [hours]   # delete binaries of any name older than 6 h (or `hours`)
```

Test binaries are disposable. Objects stay in the shared cache, so relinking a binary costs seconds,
while the binaries themselves fill the disk (tens of MB each, hundreds of them; `cxx.sh` refuses to
compile below 1 GiB free). The sweep deletes each `sweep_*` binary right after its run; delete your own
when you are done with them, and use `--prune-binaries` for the rest. Pruning spares a binary that some
process is executing (its path is on that process's command line).

For each test it resolves the link list with `links.py` just before building (so edited comments and
registry entries take effect at once), deletes any previous binary, runs `tools/unit.sh sweep_<path>
<test> <sources>` (binary name: the path under `tests/` with `/` replaced by `__`, prefixed `sweep_`, so
the sweep never overwrites a porter's binary), and records the result. Tests run strictly one after
another; `unit.sh`'s make runs one compile at a time (MAKEFLAGS is cleared), and `cxx.sh` keeps every
compile at idle priority. A lock (`$SWEEP_DIR/run.lock`) stops a second sweep from starting. Tests that
appear while the sweep runs are picked up by up to three rescans at the end; a test file that vanishes
is reported as SKIP.

Statuses:

| status     | meaning                                                                                                                                                                    |
| ---------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| PASS       | the binary ran, printed its summary line, every case passed, exit 0                                                                                                        |
| FAIL       | the binary ran but a case failed, it crashed, or it printed no summary line                                                                                                |
| BUILD_FAIL | no binary: a compile or link error, or `links.py` could not resolve the list                                                                                               |
| TIMEOUT    | killed after `--timeout` seconds of running (default 280; `unit.sh` itself stops a test at 300 s, also reported as TIMEOUT), or the build phase exceeded `--build-timeout` |
| SKIP       | `skip:` in the registry, `tests/cluster_w04c1/*` without `--cluster`, the file vanished, or input hashes changed during the run                                            |

The sweep exits 1 when this invocation records any FAIL, BUILD_FAIL or TIMEOUT; otherwise it exits 0. SKIP remains nonfatal and needs its recorded reason checked. Old result rows outside the selected invocation do not affect the exit code. Always inspect the rows and logs: an older runner revision returned 0 after printing its summary even when a test failed.

The run phase starts when the linked binary appears; build and run times are recorded separately.
Non-PASS rows note the first diagnostic and whether the test file or its newest linked source changed in
the last 30 minutes (often a porter mid-edit).

A recent modification time is a diagnostic hint. Defer an actively edited test only with a named owner and path claim in the registry, or classify a confirmed input hash change during its run as SKIP. Remove a temporary registry skip after that owner releases the path and rerun it.

Output, under `$SWEEP_DIR` (default `/tmp/claude-1002/wave/sweep`):

- `results.tsv`: one row per test, merged across runs (a re-run replaces the row): `test, status, run,
passed, failed` (from the harness's last line), `seconds` (run phase), `build_seconds`, `finished`
  (UTC), `note`;
- `history.tsv`: the same columns, one row per run, never rewritten (shows flaky tests);
- `logs/sweep_<path>.log`: the resolved list, the command, compiler and test output, exit status,
  timing and leftovers.

Each log also contains input SHA-256 hashes and confirms whether they stayed unchanged. Before a rerun, the old log moves to `logs/archive/` with a UTC timestamp and process ID, preserving earlier output.

### Isolation applied to every test process

- `HOME` and `TMPDIR` point at fresh empty directories under `$SWEEP_DIR/tmp/<random>/` (`h/`, `t/`) and the
  working directory is a third one (`w/`). Paths stay short (Unix socket names are limited to 107 bytes).
- `CODEX_HOME`, `CODEX_AUTH_JSON_B64`, `CLAUDE_CODE_WORKSPACE_ID`, `SSH_AUTH_SOCK`, and every variable whose name has a credential
  component (`API_KEY`, `KEY`, `KEY_ID`, `TOKEN`, `SECRET`, `PASSWORD`, `CREDENTIAL(S)`, `AUTH`, `OAUTH`,
  `ACCESS_KEY`, ... as an `_`-separated part, which covers every provider key src/ and tests/ read:
  `OPENAI_API_KEY`, `CLAUDE_API_KEY`, `CLAUDE_CODE_OAUTH_TOKEN`, `METERED_API_KEY`, `SURPLUS_API_KEY`,
  `OPENLUX_API_KEY`, `DEEPSEEK_API_KEY`, `CEREBRAS_API_KEY`, `OPENROUTER_API_KEY`, `LITHOSAI_API_KEY`,
  `VOYAGEAI_API_KEY`, `UOS_AI_TOKEN`, `DENO_DEPLOY_TOKEN`, `SENTINEL_REPLAY_KEY`, ...) are removed from
  the environment. The sweep prints the names it removes, never values.
- The test runs in its own session and process group (`setsid`); a timeout kills the whole group.
- Afterwards the binary and the scratch directories are deleted (`--keep-binaries`, `--keep-scratch`). Files a test left
  in its scratch `TMPDIR`, `HOME` or working directory, and new entries it created directly under
  `/tmp`, are listed in the note and the log.

Tests still clear credentials themselves where they assert on their absence (`rt::env::ScopedOverride`,
see PORT-NOTES "Cluster sub-chunk b5"): the sweep's isolation is a second layer, not a substitute.
