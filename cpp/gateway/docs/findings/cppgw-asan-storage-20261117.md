# Storage, Sentinel, cache and conversion ASan/UBSan bring-up

## Work contract v1

Objective: build and execute all 62 top-level test binaries whose names begin
with `sentinel_`, `storage_`, `kv_`, `prompt_`, `catalog_`, `sqlite_`, `cache_`,
or `embeddings_`, first without sanitizers and then with AddressSanitizer and
UndefinedBehaviorSanitizer. Preserve the TypeScript/Bun 1.4.0 oracle, including
ordering, values, error text, and coroutine suspension semantics.

Owned writes: the assigned Sentinel, storage, KV conversion, cache, catalog,
embeddings sources and matching headers, `src/kv.cpp`, matching shard tests and
vector includes, assigned counting/native-KV/Sentinel helpers, an optional
`tests/lsan-storage.supp`, and this report, all under `cpp/gateway/`.
Runtime, build tools, other tests, TypeScript, `.github/`, and other C++ trees
are read-only. No provider, credential, deployment, live database, service, or
external Sentinel operation is in scope.

| Surface           | Required behavior / acceptance                                                                                        |
| ----------------- | --------------------------------------------------------------------------------------------------------------------- |
| Normal profile    | Clang C++20, fatal port warnings, no sanitizer; every shard test runs                                                 |
| Sanitizer profile | `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined`                         |
| Execution         | Serial binaries, 600-second process-group timeout, fresh empty HOME/TMPDIR/cwd, credential-free environment allowlist |
| ASan / UBSan      | `detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1` / `print_stacktrace=1`                        |
| Dependencies      | Build every required source once per profile; reuse objects across Links closures                                     |
| Fixes             | Root cause, unchanged Bun-visible semantics; new regression when existing tests do not cover the defect               |
| Suppressions      | Only a narrow documented JavaScript-model limit; none presumed                                                        |
| Evidence          | Exact commands, original diagnostics, per-binary/case counts, consumed-input hashes and final commit/tree             |

Initial unknowns: host-specific assumptions in the shard, native dependency
versions, and sanitizer failures. Resolve behavior questions with isolated Bun
observations before editing expectations. Report defects outside ownership with
the command, first diagnostic and diagnosis instead of editing them.

Stop condition: both complete profiles pass, or record every remaining failure
and the exact ownership/external blocker. Keep the branch committable and attempt
publication; never claim unverified development integration or hosted evidence.

Long-operation budget: four parallel compilers, one test process at a time;
expect 10–30 minutes per initial profile, with progress in retained build logs.
Investigate the first compile error rather than retrying unchanged inputs.
Retain incremental objects and complete diagnostics outside the checkout under
`/tmp/cppgw-asan-storage-20261117`; each runtime process has a 600-second limit.
User updates report state changes at least once per minute during active work.

## Discovery and progress

- Base commit: `141cb4d482a3b0d8624ce0411cbd3b93e45873e6`.
- Base tree: `c57b738432c3c364af03b057a93502cc92fbb6bc`.
- Runner: disposable Linux x86_64, four CPUs, approximately 14 GiB available RAM.
- Clang 21.1.8, Bun 1.4.0, CMake 4.4.5, Ninja 1.13.2.
- Read the porting, port notes, testing/build documents, and prior runtime
  sanitizer and libc++ portability reports. Host-production restrictions are
  superseded by the explicit disposable-runner instructions for this task.
- Selected 62 binaries: Sentinel 20, KV 15, prompt 11, embeddings 5, cache 4,
  catalog 3, storage 3, SQLite 1.
- Installed Boost.JSON/Container 1.90 and Brotli 1.2.0 development packages.
- Prepared SQLite 3.54.0 from the hash-verified official amalgamation, matching
  the prior reports; no versioned expected value was changed.
- External CMake/Ninja driver resolves all 62 Links closures. Their union is
  44 runtime and 278 additional source objects, plus 62 test objects (384
  compilations and 62 links per initial profile). Both profiles completed.
- Normal discovery profile: **62/62 binaries, 814/814 cases passed**, zero
  failures/timeouts, 13.317 seconds total runtime. Consumed inputs stayed unchanged.
- Normal run: `plain/runs/1794915453515505207` under the external build root.
  This discovery run records a dirty documentation worktree and is not the final
  committed-tree receipt. No C++ change was needed to obtain the normal baseline.
- ASan/UBSan discovery profile: **62/62 binaries, 814/814 cases passed**,
  zero failures, timeouts, ASan/UBSan/LSan reports or suppressions; 73.082 seconds
  total runtime. Consumed inputs and checkout identity stayed unchanged.
- Sanitizer run: `asan/runs/1794916518229898341` under the external build root.
- No C++ sources, headers, tests, vectors or expectations required edits.
  The only repository change is this report, including the external runner.
- Added an exclusive external-driver lock after discovery to reject concurrent
  writers before they can damage Ninja metadata. The embedded driver matches
  the executed script, parses successfully and rejects a concurrent invocation.
  Final clean-commit sweeps pass all 62 binaries in both profiles.

The first normal build's compilation reached all source/test objects without a
C++ diagnostic. A malformed documentation-command heredoc accidentally started
a second Ninja writer; it was stopped. The interrupted duplicate removed two
objects and damaged Ninja dependency metadata. The first link reported
`clang++: error: no such file or directory` for the `codex/banked_reset.cpp.o`
and `codex/banked_reset_provider.cpp.o` outputs. This is a runner incident,
not a product defect. A single-writer rebuild regenerated the missing objects and linked all
binaries, then passed all tests. The old plain Ninja dependency log remained
non-authoritative and tried to rebuild unchanged objects again at the final
checkpoint. That redundant build was stopped; final execution reused the
complete, fingerprinted binaries after checking every C++ input against the
original compilation/run manifests. The ASan build was a clean single-writer
build. Fresh reproduction directories avoid the damaged disposable cache. Original diagnostics remain in
`plain/build.log`; no runtime pass is claimed for that interrupted build.

Compiler executable: `/usr/lib/llvm-21/bin/clang`, SHA-256
`78e9dd2ccc9e88859b3a29109a44bfab00e3d6cb70f00f3daae063eed3d05251`.
`toolchain.json` under the external build root records exact package versions.

## Reproduction commands and dependency identity

Run from the repository root. All generated files and downloaded dependencies
stay outside the checkout. System development packages already present:
OpenSSL 3.5.5, SQLite 3.46.1, zlib 1.3.1, ICU 78.2, Zstd 1.5.7.
The profile links its independent SQLite 3.54.0 explicitly with an RPATH;
the system SQLite is not the selected library.

```sh
sudo apt-get update -qq
sudo apt-get install -y -qq libboost-json-dev libboost-container-dev libbrotli-dev
sudo ln -s /usr/bin/llvm-symbolizer-21 /usr/local/bin/llvm-symbolizer
mkdir -p /tmp/cppgw-asan-storage-20261117
curl --fail --silent --show-error --location \
  https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip \
  --output /tmp/sqlite-amalgamation-3540000.zip
python3 - <<'PY'
from pathlib import Path
import hashlib, shutil, zipfile
archive = Path('/tmp/sqlite-amalgamation-3540000.zip')
assert hashlib.sha256(archive.read_bytes()).hexdigest() == '68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d'
with zipfile.ZipFile(archive) as source:
    source.extractall('/tmp/cppgw-sqlite-src')
prefix = Path('/tmp/cppgw-sqlite-3.54.0')
for directory in ['include', 'lib']:
    (prefix / directory).mkdir(parents=True, exist_ok=True)
shutil.copy('/tmp/cppgw-sqlite-src/sqlite-amalgamation-3540000/sqlite3.h', prefix / 'include/sqlite3.h')
PY
clang -O2 -fPIC -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_COLUMN_METADATA=1 \
  -DSQLITE_DQS=3 -shared -Wl,-soname,libsqlite3.so \
  -o /tmp/cppgw-sqlite-3.54.0/lib/libsqlite3.so \
  /tmp/cppgw-sqlite-src/sqlite-amalgamation-3540000/sqlite3.c -ldl -pthread -lm
```

The following external driver is retained inline because creating a new build-tool
file is outside this shard's allowed paths. Extract it with:

````sh
python3 - <<'PY'
from pathlib import Path
report = Path('cpp/gateway/docs/findings/cppgw-asan-storage-20261117.md').read_text()
program = report.split('```python storage-shard-driver\n', 1)[1].split('\n```', 1)[0]
Path('/tmp/cppgw-asan-storage-20261117/run_checks.py').write_text(program + '\n')
PY
python3 /tmp/cppgw-asan-storage-20261117/run_checks.py --profile plain --jobs 4
python3 /tmp/cppgw-asan-storage-20261117/run_checks.py --profile asan --jobs 4
````

Both profiles use the fatal port warnings, native-library bindings and every
runtime object in each binary. Plain uses `-O1 -g`; ASan adds the required
sanitizer flags. The runner passes only PATH, locale, HOME and TMPDIR (and the
explicit sanitizer options), removing all inherited provider/credential variables
by construction. Every binary gets its own process group and 600-second timeout.
`--require-clean` binds final acceptance to committed sources; dirty discovery
runs explicitly record null accepted commit/tree identities. Each run retains
complete logs, counts, source hashes, and before/after identity checks.

```python storage-shard-driver
#!/usr/bin/env python3
"""External build driver for the storage sanitizer shard; no repo-generated files."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import tempfile
import time

REPO = Path.cwd().resolve()
GATEWAY = REPO / 'cpp/gateway'
ROOT = Path('/tmp/cppgw-asan-storage-20261117')
PREFIXES = ('sentinel_', 'storage_', 'kv_', 'prompt_', 'catalog_', 'sqlite_', 'cache_', 'embeddings_')
ENV = {'PATH': '/usr/local/bin:/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'PYTHONDONTWRITEBYTECODE': '1'}
BASE = ['-std=c++20', '-Wall', '-Wextra', '-Wshadow', '-Wconversion', '-Werror',
        '-DBOOST_JSON_NO_LIB', '-DBOOST_CONTAINER_NO_LIB', '-DBOOST_SYSTEM_NO_LIB',
        '-DUOS_GATEWAY_NATIVE_LIBRARIES', '-pthread']
SAN = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=undefined']

def identity():
    revisions = subprocess.check_output(['git', 'rev-parse', 'HEAD', 'HEAD^{tree}'], text=True, env=ENV).splitlines()
    status = subprocess.check_output(['git', 'status', '--porcelain'], text=True, env=ENV)
    return {'head_commit': revisions[0], 'head_tree': revisions[1], 'worktree_clean': not status,
            'commit': None if status else revisions[0], 'tree': None if status else revisions[1]}

def inputs():
    # Deliberately over-record all gateway code, including transitive headers/vector includes.
    files = sorted(p for d in ['src', 'include', 'tests'] for p in (GATEWAY / d).rglob('*')
                   if p.is_file() and p.suffix in ['.cpp', '.hpp', '.h', '.inc'])
    files += [GATEWAY / 'tools/links.py', GATEWAY / 'tests/LINKS.txt', Path(__file__).resolve()]
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}

def command(argv, log):
    print(shlex.join(map(str, argv)), flush=True)
    with log.open('a') as output:
        output.write('\n$ ' + shlex.join(map(str, argv)) + '\n'); output.flush()
        result = subprocess.run(argv, env=ENV, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        print(log.read_text(errors='replace')[-14000:], flush=True)
        raise SystemExit(result.returncode)

def quote(path):
    return '"' + str(path).replace('\\', '/').replace('"', '\\"') + '"'

def build(args, profile, tests):
    root = ROOT / profile
    driver = root / 'driver'
    driver.mkdir(parents=True, exist_ok=True)
    runtime = sorted((GATEWAY / 'src/rt').glob('*.cpp'))
    closures = {}
    for test in tests:
        output = subprocess.check_output(['python3', str(GATEWAY / 'tools/links.py'), str(test)], env=ENV, text=True)
        closures[test.stem] = [GATEWAY / source for source in shlex.split(output)]
    sources = sorted({p for closure in closures.values() for p in closure})
    names = {p: 'obj_' + str(p.relative_to(GATEWAY)).replace('/', '_').replace('.', '_') for p in sources}
    flags = BASE + (SAN if profile == 'asan' else ['-O1', '-g'])
    lines = ['cmake_minimum_required(VERSION 3.25)', 'project(uos_storage_checks LANGUAGES CXX)',
             'set(CMAKE_EXPORT_COMPILE_COMMANDS ON)', 'add_compile_options(' + ' '.join(flags) + ')',
             'include_directories(' + quote(GATEWAY / 'include') + ' ' + quote(GATEWAY) + ' ' + quote(args.sqlite_prefix / 'include') + ')',
             'add_library(runtime OBJECT ' + ' '.join(map(quote, runtime)) + ')']
    if profile == 'asan':
        lines += ['add_link_options(-fsanitize=address,undefined)']
    for source in sources:
        lines += [f'add_library({names[source]} OBJECT {quote(source)})']
    for test in tests:
        objects = ' '.join('$<TARGET_OBJECTS:' + names[p] + '>' for p in closures[test.stem])
        lines += [f'add_executable({test.stem} {quote(test)} $<TARGET_OBJECTS:runtime> {objects})',
                  f'target_link_libraries({test.stem} PRIVATE boost_json boost_container ssl crypto pthread dl '
                  + quote(args.sqlite_prefix / 'lib/libsqlite3.so') + ' z brotlidec zstd icuuc)']
    content = '\n'.join(lines) + '\n'
    cmake_file = driver / 'CMakeLists.txt'
    if not cmake_file.exists() or cmake_file.read_text() != content:
        cmake_file.write_text(content)
    (root / 'closures.json').write_text(json.dumps({k: list(map(str, v)) for k, v in closures.items()}, indent=2) + '\n')
    command(['cmake', '-S', str(driver), '-B', str(root), '-G', 'Ninja', '-DCMAKE_CXX_COMPILER=clang++'], root / 'build.log')
    targets = [test.stem for test in tests if not args.filter or any(f in test.stem for f in args.filter)]
    if not targets:
        raise SystemExit('No selected targets')
    print(f'{profile}: {len(runtime)} runtime sources, {len(sources)} additional sources, {len(targets)} selected tests', flush=True)
    if not args.run_only:
        command(['cmake', '--build', str(root), '-j', str(args.jobs), '--target', *targets], root / 'build.log')
    return root, targets

def run(args, profile, root, targets, before, source_identity):
    rows = []
    run_root = root / 'runs' / str(time.time_ns())
    logs = run_root / 'logs'; logs.mkdir(parents=True)
    provenance = source_identity | {'profile': profile, 'inputs': before}
    receipt = run_root / 'provenance.json'
    receipt.write_text(json.dumps(provenance, indent=2) + '\n')
    for target in targets:
        started = time.monotonic()
        with tempfile.TemporaryDirectory(prefix='sg-') as scratch:
            scratch = Path(scratch)
            for name in ['home', 'tmp', 'work']:
                (scratch / name).mkdir()
            env = ENV | {'HOME': str(scratch / 'home'), 'TMPDIR': str(scratch / 'tmp')}
            if profile == 'asan':
                env |= {'ASAN_OPTIONS': 'detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1',
                        'UBSAN_OPTIONS': 'print_stacktrace=1'}
                if args.suppressions:
                    env['LSAN_OPTIONS'] = 'suppressions=' + str(args.suppressions.resolve())
            log = logs / (target + '.log')
            with log.open('w') as output:
                argv = [str(root / target)] + ([args.case] if args.case else [])
                process = subprocess.Popen(argv, env=env, cwd=scratch / 'work', stdout=output,
                                           stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    code = process.wait(timeout=600)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL); process.wait(); code = 124
            output = log.read_text(errors='replace')
            counts = re.findall(r'(\d+) run, (\d+) passed, (\d+) failed', output)
            diagnostics = bool(re.search(r'ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:|SUMMARY: (AddressSanitizer|UndefinedBehaviorSanitizer)', output))
            row = {'test': target, 'exit': code, 'counts': list(map(int, counts[-1])) if counts else None,
                   'sanitizer_diagnostic': diagnostics, 'seconds': round(time.monotonic() - started, 3), 'log': str(log)}
            rows.append(row)
            print(json.dumps(row), flush=True)
            result_text = json.dumps(rows, indent=2) + '\n'
            (run_root / 'results.json').write_text(result_text); (root / 'results.json').write_text(result_text)
    provenance['inputs_unchanged'] = before == inputs()
    provenance['identity_unchanged'] = source_identity == identity()
    receipt.write_text(json.dumps(provenance, indent=2) + '\n')
    return provenance['inputs_unchanged'] and provenance['identity_unchanged'] and all(
        r['exit'] == 0 and r['counts'] and r['counts'][0] > 0 and r['counts'][2] == 0 and not r['sanitizer_diagnostic'] for r in rows)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--profile', choices=['plain', 'asan', 'both'], default='plain')
parser.add_argument('--sqlite-prefix', type=Path, default=Path('/tmp/cppgw-sqlite-3.54.0'))
parser.add_argument('--jobs', type=int, default=4)
parser.add_argument('--filter', nargs='*')
parser.add_argument('--case')
parser.add_argument('--build-only', action='store_true')
parser.add_argument('--run-only', action='store_true')
parser.add_argument('--require-clean', action='store_true')
parser.add_argument('--suppressions', type=Path)
args = parser.parse_args()
tests = sorted(p for p in (GATEWAY / 'tests').glob('*_test.cpp') if p.name.startswith(PREFIXES))
# One writer across both profiles: concurrent Ninja writers corrupt dependency metadata.
lock = (ROOT / 'driver.lock').open('a')
try:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    raise SystemExit('Another storage shard driver owns this build root')
ok = True
for profile in ['plain', 'asan'] if args.profile == 'both' else [args.profile]:
    before, source_identity = inputs(), identity()
    if args.require_clean and not source_identity['worktree_clean']:
        parser.error('Acceptance requires a clean committed checkout')
    root, targets = build(args, profile, tests)
    if not args.build_only:
        ok = run(args, profile, root, targets, before, source_identity) and ok
raise SystemExit(0 if ok else 1)
```

The independently built SQLite library SHA-256 is
`43fe49e65bc25cfc1cad9084c88ce77a884057087aafc117a42028f6f1aa2d28`,
matching the prior runtime report.

## Per-binary counts

| Binary                                                 | Normal run / passed / failed | ASan/UBSan run / passed / failed |
| ------------------------------------------------------ | ---------------------------: | -------------------------------- |
| `cache_kv_migration_differential_test`                 |                  31 / 31 / 0 | 31 / 31 / 0                      |
| `cache_kv_migration_usage_coverage_test`               |                  43 / 43 / 0 | 43 / 43 / 0                      |
| `cache_scope_experiment_model_coverage_test`           |                  19 / 19 / 0 | 19 / 19 / 0                      |
| `cache_usage_normalization_test`                       |                    4 / 4 / 0 | 4 / 4 / 0                        |
| `catalog_promotion_test`                               |                  13 / 13 / 0 | 13 / 13 / 0                      |
| `catalog_store_differential_test`                      |                  10 / 10 / 0 | 10 / 10 / 0                      |
| `catalog_types_differential_test`                      |                    3 / 3 / 0 | 3 / 3 / 0                        |
| `embeddings_coverage_test`                             |                  23 / 23 / 0 | 23 / 23 / 0                      |
| `embeddings_differential_test`                         |                  32 / 32 / 0 | 32 / 32 / 0                      |
| `embeddings_ledger_differential_test`                  |                    7 / 7 / 0 | 7 / 7 / 0                        |
| `embeddings_part1_test`                                |                  30 / 30 / 0 | 30 / 30 / 0                      |
| `embeddings_part2_test`                                |                  18 / 18 / 0 | 18 / 18 / 0                      |
| `kv_budget_part1_test`                                 |                  24 / 24 / 0 | 24 / 24 / 0                      |
| `kv_budget_part2_test`                                 |                  24 / 24 / 0 | 24 / 24 / 0                      |
| `kv_consistent_snapshot_test`                          |                    3 / 3 / 0 | 3 / 3 / 0                        |
| `kv_conversion_cli_differential_test`                  |                  13 / 13 / 0 | 13 / 13 / 0                      |
| `kv_conversion_convert_differential_test`              |                  13 / 13 / 0 | 13 / 13 / 0                      |
| `kv_conversion_cursors_differential_test`              |                    7 / 7 / 0 | 7 / 7 / 0                        |
| `kv_conversion_legacy_codec_differential_test`         |                    3 / 3 / 0 | 3 / 3 / 0                        |
| `kv_conversion_native_schema_differential_test`        |                    4 / 4 / 0 | 4 / 4 / 0                        |
| `kv_conversion_profile_json_differential_test`         |                    5 / 5 / 0 | 5 / 5 / 0                        |
| `kv_conversion_proof_differential_test`                |                    8 / 8 / 0 | 8 / 8 / 0                        |
| `kv_conversion_typed_value_differential_test`          |                    7 / 7 / 0 | 7 / 7 / 0                        |
| `kv_durable_interop_test`                              |                    7 / 7 / 0 | 7 / 7 / 0                        |
| `kv_durable_suite_test`                                |                  35 / 35 / 0 | 35 / 35 / 0                      |
| `kv_migration_test`                                    |                  41 / 41 / 0 | 41 / 41 / 0                      |
| `kv_runtime_conversion_test`                           |                    8 / 8 / 0 | 8 / 8 / 0                        |
| `prompt_analytics_differential_test`                   |                    3 / 3 / 0 | 3 / 3 / 0                        |
| `prompt_cache_analytics_admin_test`                    |                    3 / 3 / 0 | 3 / 3 / 0                        |
| `prompt_cache_analytics_test`                          |                  10 / 10 / 0 | 10 / 10 / 0                      |
| `prompt_cache_capabilities_test`                       |                    5 / 5 / 0 | 5 / 5 / 0                        |
| `prompt_cache_scope_experiment_failure_test`           |                  10 / 10 / 0 | 10 / 10 / 0                      |
| `prompt_cache_scope_experiment_part1_test`             |                  25 / 25 / 0 | 25 / 25 / 0                      |
| `prompt_cache_scope_experiment_part2_test`             |                    5 / 5 / 0 | 5 / 5 / 0                        |
| `prompt_cache_scope_experiment_targets_test`           |                  23 / 23 / 0 | 23 / 23 / 0                      |
| `prompt_cache_scope_targets_test`                      |                  13 / 13 / 0 | 13 / 13 / 0                      |
| `prompt_cache_telemetry_gate_differential_test`        |                    3 / 3 / 0 | 3 / 3 / 0                        |
| `prompt_cache_telemetry_gate_test`                     |                    7 / 7 / 0 | 7 / 7 / 0                        |
| `sentinel_incident_index_test`                         |                  14 / 14 / 0 | 14 / 14 / 0                      |
| `sentinel_incident_outbox_differential_test`           |                    4 / 4 / 0 | 4 / 4 / 0                        |
| `sentinel_replay_bootstrap_retry_test`                 |                  25 / 25 / 0 | 25 / 25 / 0                      |
| `sentinel_replay_coverage_test`                        |                  20 / 20 / 0 | 20 / 20 / 0                      |
| `sentinel_replay_deletion_budget_test`                 |                    4 / 4 / 0 | 4 / 4 / 0                        |
| `sentinel_replay_differential_test`                    |                  12 / 12 / 0 | 12 / 12 / 0                      |
| `sentinel_replay_envelope_coverage_test`               |                  13 / 13 / 0 | 13 / 13 / 0                      |
| `sentinel_replay_export_test`                          |                  12 / 12 / 0 | 12 / 12 / 0                      |
| `sentinel_replay_limits_differential_test`             |                    3 / 3 / 0 | 3 / 3 / 0                        |
| `sentinel_replay_limits_test`                          |                    3 / 3 / 0 | 3 / 3 / 0                        |
| `sentinel_replay_read_coverage_test`                   |                  13 / 13 / 0 | 13 / 13 / 0                      |
| `sentinel_replay_read_differential_test`               |                    1 / 1 / 0 | 1 / 1 / 0                        |
| `sentinel_replay_retention_metadata_differential_test` |                    1 / 1 / 0 | 1 / 1 / 0                        |
| `sentinel_replay_retention_metadata_test`              |                  10 / 10 / 0 | 10 / 10 / 0                      |
| `sentinel_replay_retention_test`                       |                  31 / 31 / 0 | 31 / 31 / 0                      |
| `sentinel_replay_staging_test`                         |                    8 / 8 / 0 | 8 / 8 / 0                        |
| `sentinel_request_capture_test`                        |                    5 / 5 / 0 | 5 / 5 / 0                        |
| `sentinel_upstream_capture_differential_test`          |                  15 / 15 / 0 | 15 / 15 / 0                      |
| `sentinel_upstream_capture_test`                       |                  15 / 15 / 0 | 15 / 15 / 0                      |
| `sentinel_upstream_providers_test`                     |                  19 / 19 / 0 | 19 / 19 / 0                      |
| `sqlite_readonly_test`                                 |                    5 / 5 / 0 | 5 / 5 / 0                        |
| `storage_bun_kv_test`                                  |                  32 / 32 / 0 | 32 / 32 / 0                      |
| `storage_native_kv_differential_test`                  |                    3 / 3 / 0 | 3 / 3 / 0                        |
| `storage_value_compatibility_test`                     |                    4 / 4 / 0 | 4 / 4 / 0                        |

## Findings

| Test                 | Sanitizer / diagnostic  | Cause                                                                              | Fix or disposition                                                               |
| -------------------- | ----------------------- | ---------------------------------------------------------------------------------- | -------------------------------------------------------------------------------- |
| All 62 binaries      | ASan / UBSan / LSan     | No reports in 814 executed cases                                                   | No C++ fix or suppression needed                                                 |
| Initial normal build | Linker / Ninja metadata | Accidental duplicate writer removed two objects and interrupted dependency records | Single-writer rebuild passed; retained driver now rejects concurrent invocations |

## Verification and remaining handoff

Changed path: this Markdown findings file. Selected document gates are the
locked Prettier formatter and `git diff --check`. The C++ sweeps above are the
explicitly requested qualification work; no TypeScript, unrelated repository,
native deployment or full-verify aggregate is selected.

```sh
mkdir -p /tmp/cppgw-asan-storage-20261117/lint
cp tools/lint/package.json tools/lint/bun.lock /tmp/cppgw-asan-storage-20261117/lint/
bun --no-env-file install --frozen-lockfile --ignore-scripts --cwd /tmp/cppgw-asan-storage-20261117/lint
bun --no-env-file --no-install /tmp/cppgw-asan-storage-20261117/lint/node_modules/prettier/bin/prettier.cjs --check cpp/gateway/docs/findings/cppgw-asan-storage-20261117.md
git diff --check
```

Both repository lockfiles remain unchanged. Document formatting and whitespace
checks pass. The exact clean-commit, review and publication receipts follow.
GitHub CLI has no authenticated host; no connector providing GitHub writes is
available.

No defects were found outside the owned paths. The entire requested shard ran;
there are no remaining C++ test failures or unbuilt shard binaries. Runtime,
other-shard, real legacy-Deno conversion, other-platform and deployment acceptance
remain outside this qualification. No production state, provider credentials or
live inference provider was accessed.

## Committed-source acceptance receipt

- Commit: `833d743a28c7805ee730dc8d338650e5a765e298`.
- Tree: `684d26b4101d1562b7f55f21bccf85f3265b4093`.
- Base: `141cb4d482a3b0d8624ce0411cbd3b93e45873e6`.
- Both final profiles: **62 binaries, 814 run / 814 passed / 0 failed**.
- Zero timeouts, sanitizer reports, skipped binaries or suppressions.
- Both receipts: `worktree_clean`, `inputs_unchanged`, and `identity_unchanged`
  are true. All 1,356 C++ inputs match the original build/run manifests; all
  124 binary hashes were checked before and after the committed-tree runs.
- This documentation follow-up adds receipts only; no compiled C++ input changes
  after the checkpoint. `plain-reuse.json` / `asan-reuse.json` retain the input
  and binary hashes under the external root.

Exact acceptance command (the complete normal and sanitizer builds are recorded
above; this command reuses those verified binaries):

```sh
python3 /tmp/cppgw-asan-storage-20261117/run_checks.py --profile both --jobs 4 --run-only --require-clean
```

- `plain` run: `/tmp/cppgw-asan-storage-20261117/plain/runs/1794916908492472811`, 12.815 seconds total runtime.
  Results SHA-256: `59a59887844e7323b822320e30b917398467100f19c2810e085ab910178e0663`.
  Provenance SHA-256: `fe99bc95678b0e4cd4d991270ec419b883136dd4353320680e7fb8b33a9b1f9b`.
- `asan` run: `/tmp/cppgw-asan-storage-20261117/asan/runs/1794916924503040428`, 75.338 seconds total runtime.
  Results SHA-256: `c87b67d426780264eaa9c2ddb8163a4a495a4dd90b9f7e432a644650d9da31e1`.
  Provenance SHA-256: `7852f7f83496c137d0273d4f35bc62a35b7a46eaf03640d150d8b59de3bad582`.

## Local review and publication limits

Actual local review mode, Codex CLI 0.169.0:

```sh
codex -c 'review_model="codex-auto-review"' review --commit 833d743a28c7805ee730dc8d338650e5a765e298
```

The exact reviewed head and base are the checkpoint and base above. The command
exited 0 in 83.393 seconds. Its banner announced `model: ubiquity/deepseek-flash`,
`provider: uos`, instead of confirming `codex-auto-review`. **Model identity is
unverified; reject this receipt for official review ingestion.** No finding was
ingested and no failed or identity-mismatched review was marked complete. Review
status did not gate the local qualification or publication attempt.

The credential-safe transcript is `/tmp/cppgw-asan-storage-20261117/review.log`,
SHA-256 `ae087a58fb074dfc39ac29f53bf5be8ce47ef5d94aed82d2bc2f3b5a23552f9e`.
No credential appeared in its output. Original final text (printed twice):

> The commit adds only a documented sanitizer qualification report and embedded reproducibility driver. No definite correctness or regression issues were identified.

Publication command:

```sh
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false \
  git push --set-upstream origin test/cppgw-asan-storage-20261117
git fetch origin refs/heads/development:refs/remotes/origin/development
git merge-base --is-ancestor 833d743a28c7805ee730dc8d338650e5a765e298 origin/development
```

Push exited 128: `fatal: could not read Username for 'https://github.com': terminal
prompts disabled`. No credential was printed, persisted, replaced or refreshed.
Fetching the explicit remote ref succeeded; `origin/development` is
`002fe15fa926674c97f0169355d24929bf529710`; the ancestry check exited 1.
This checkout has no local `development` branch and does not have GitHub write
credentials. No push, PR, merge, hosted CI or development handoff is claimed.

The runner publisher / enclosing port owner owns authenticated publication,
bounded PR integration with an ancestry-preserving merge, and the clean
`development` handoff. Preserve the clean owned task branch; do not merge the
base's unknown production-build work merely to satisfy this shard's handoff.
No hosted workflow was dispatched and no production or Mac service was activated.

## Final handoff summary

Clang 21.1.8 builds all 384 unique source/test translation units and links all 62
shard binaries in both normal and ASan/UBSan profiles. Both execute 814/814 passing
cases, with zero failures, timeouts, sanitizer reports or suppressions. Commands,
dependency hashes and the complete isolated runner are above. No C++ sources or
expected values changed; this report is the only repository change. No defects
were found outside ownership. GitHub authentication blocks publication and
development integration; the clean committed task branch is preserved for the
runner publisher. The review receipt is rejected for its unverified model identity.
