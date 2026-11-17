#!/usr/bin/env python3
"""Disposable Linux core-shard checks. Requires sudo network namespaces.

Build dependencies: Clang/LLD 21, CMake, Ninja, Boost.JSON/Container, OpenSSL, zlib,
Brotli, Zstd, ICU, and a matching SQLite library/header under --build-root.
Generated sources, objects, executables, isolated homes and logs stay outside
the checkout. Use --require-clean for committed-source acceptance receipts.
"""
import argparse
import hashlib
import json
import os
import re
import runpy
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# This branch runs only after sudo/unshare created a private network namespace.
# Pass only the parent's explicit test allowlist, never sudo's environment.
if len(sys.argv) > 1 and sys.argv[1] == '--isolated-child':
    uid, gid = (int(sys.argv[2]), int(sys.argv[3]))
    env = json.loads(sys.argv[4])
    argv = sys.argv[5:]
    subprocess.run(['/usr/sbin/ip', 'link', 'set', 'lo', 'up'], check=True, env={'PATH': '/usr/sbin:/usr/bin:/bin'})
    with socket.socket() as probe:
        if probe.connect_ex(('192.0.2.1', 443)) != 101:
            raise SystemExit('Private network must deny external routes')
    os.setgroups([])
    os.setgid(gid)
    os.setuid(uid)
    if 'ASAN_OPTIONS' in env:
        env['ASAN_SYMBOLIZER_PATH'] = '/usr/bin/llvm-symbolizer-21'
    # LSan can terminate before libc flushes stdout; retain each real case count.
    os.execve('/usr/bin/stdbuf', ['/usr/bin/stdbuf', '-oL', *argv], env)
P = argparse.ArgumentParser()
P.add_argument('profile', choices=['plain', 'asan'])
P.add_argument('--repo', type=Path, default=Path.cwd())
P.add_argument('--filter', default='')
P.add_argument('--case', default='')
P.add_argument('--jobs', type=int, default=4)
P.add_argument('--build-root', type=Path, default=Path('/tmp/cppgw-asan-core-20261117'))
P.add_argument('--run-only', action='store_true')
P.add_argument('--compile-only', action='store_true')
P.add_argument('--require-clean', action='store_true')
a = P.parse_args()
repo = a.repo.resolve()
g = repo / 'cpp/gateway'
base = a.build_root.resolve()
out = base / a.profile
if base == repo or repo in base.parents:
    P.error('Build root must be outside the checkout')
if not (base / 'libsqlite3.so').is_file() or not (base / 'sqlite-amalgamation-3540000/sqlite3.h').is_file():
    P.error('Prepare the SQLite 3.54.0 header/library under --build-root first; see the findings report')
out.mkdir(parents=True, exist_ok=True)
clean = {'PATH': '/usr/local/bin:/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TZ': 'UTC'}
skip = tuple('codex_ banked_reset cluster_ claude_ lithos_ cerebras_ deepseek_ anthropic_ surplus_ metered_ provider_ openrouter_ openai_ responses_ chat_ oss_ removed_ capacity_ temporary_ usage_ upstream_ input_ native_ authority_ kernel_ paid_ api_ quota_ worker_ sentinel_ storage_ kv_ prompt_ catalog_ sqlite_ cache_ embeddings_'.split())
tests = [p for p in sorted((g / 'tests').glob('*_test.cpp')) if not p.name.startswith(skip)]
m = runpy.run_path(str(g / 'tools/links.py'))
registry = m['read_registry']()
closures = {p.stem: m['resolve']('tests/' + p.name, registry) for p in tests}
rt = sorted((str(p.relative_to(g)) for p in (g / 'src/rt').glob('*.cpp')))
sources = sorted(set(rt + [str(p.relative_to(g)) for p in tests] + [s for c in closures.values() for s in c.sources]))
inputs = {'tests/helpers/core_sanitizers.py'}
if (g / 'tests/lsan-core.supp').is_file():
    inputs.add('tests/lsan-core.supp')
for p in tests:
    inputs.update(m['input_paths']('tests/' + p.name, closures[p.stem].sources))

def hashes():
    return {s: hashlib.sha256((g / s).read_bytes()).hexdigest() for s in sorted(inputs)}

def identity():
    ids = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD', 'HEAD^{tree}'], env=clean, text=True).splitlines()
    dirty = bool(subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain'], env=clean, text=True))
    return {'head_commit': ids[0], 'head_tree': ids[1], 'commit': None if dirty else ids[0], 'tree': None if dirty else ids[1], 'worktree_clean': not dirty}

def call(argv, log):
    with open(log, 'a') as f:
        f.write('\n$ ' + shlex.join(map(str, argv)) + '\n')
        f.flush()
        return subprocess.call(list(map(str, argv)), stdout=f, stderr=subprocess.STDOUT, env=clean)

def q(p):
    return '"' + str(p) + '"'

def obj(s):
    return 'o_' + hashlib.sha256(s.encode()).hexdigest()[:16]
flags = ['-std=c++20', '-Wall', '-Wextra', '-Wshadow', '-Wconversion', '-Werror', '-DBOOST_JSON_NO_LIB', '-DBOOST_CONTAINER_NO_LIB', '-DBOOST_SYSTEM_NO_LIB', '-DUOS_GATEWAY_NATIVE_LIBRARIES', '-pthread', '-O1', '-g']
if a.profile == 'asan':
    flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=undefined']
lines = ['cmake_minimum_required(VERSION 3.25)', 'project(core_checks LANGUAGES CXX)', 'set(CMAKE_EXPORT_COMPILE_COMMANDS ON)', 'add_compile_options(' + ' '.join(flags) + ')', 'add_link_options(-fuse-ld=/usr/bin/ld.lld-21)', 'include_directories(' + q(g / 'include') + ' ' + q(g) + ')']
if a.profile == 'asan':
    lines += ['add_link_options(-fsanitize=address,undefined)']
for s in sources:
    lines += ['add_library(' + obj(s) + ' OBJECT ' + q(g / s) + ')']
lines += ['target_include_directories(' + obj('src/rt/sqlite.cpp') + ' BEFORE PRIVATE ' + q(base / 'sqlite-amalgamation-3540000') + ')', 'add_custom_target(all_objects DEPENDS ' + ' '.join((obj(s) for s in sources)) + ')']
for t in tests:
    ss = ['tests/' + t.name] + rt + closures[t.stem].sources
    lines += ['add_executable(' + t.stem + ' ' + ' '.join(('$<TARGET_OBJECTS:' + obj(s) + '>' for s in ss)) + ')', 'target_link_libraries(' + t.stem + ' PRIVATE boost_json boost_container ssl crypto pthread dl ' + q(base / 'libsqlite3.so') + ' z brotlidec zstd icuuc)']
content = '\n'.join(lines) + '\n'
driver = out / 'driver'
driver.mkdir(exist_ok=True)
cm = driver / 'CMakeLists.txt'
if not cm.exists() or cm.read_text() != content:
    cm.write_text(content)
(out / 'inventory.json').write_text(json.dumps({'tests': [t.name for t in tests], 'runtime': rt, 'sources': sources, 'closures': {n: {'sources': c.sources, 'env': c.env, 'skip': c.skip} for n, c in closures.items()}}, indent=2) + '\n')
selected = [t for t in tests if not a.filter or any((x in t.stem for x in a.filter.split(',')))]
if not selected:
    raise SystemExit('No selected tests')
before = hashes()
ident = identity()
if a.require_clean and (not ident['worktree_clean']):
    raise SystemExit('Clean commit required')
runroot = out / 'runs' / str(time.time_ns())
runroot.mkdir(parents=True)
provenance = ident | {'inputs': before, 'profile': a.profile, 'flags': flags, 'timeout': 600, 'compiler': subprocess.check_output(['clang++', '--version'], env=clean, text=True), 'linker': subprocess.check_output(['/usr/bin/ld.lld-21', '--version'], env=clean, text=True), 'driver_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
(runroot / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
print(json.dumps({'phase': 'inventory', 'profile': a.profile, 'tests': len(tests), 'runtime': len(rt), 'distinct_units': len(sources), 'selected': len(selected), 'run': str(runroot)}), flush=True)
rc = call(['cmake', '-S', driver, '-B', out, '-G', 'Ninja', '-DCMAKE_CXX_COMPILER=clang++'], out / 'build.log')
if rc:
    raise SystemExit('Configure failed; see ' + str(out / 'build.log'))
if not a.run_only:
    rc = call(['cmake', '--build', out, '-j', a.jobs, '--target', 'all_objects', '--', '-k', '0'], out / 'build.log')
    print(json.dumps({'phase': 'compile', 'profile': a.profile, 'exit': rc, 'log': str(out / 'build.log')}), flush=True)
    if rc:
        raise SystemExit(rc)
if a.compile_only:
    raise SystemExit(0)
rows = []
for t in selected:
    start = time.monotonic()
    log = runroot / (t.stem + '.log')
    rc = call(['cmake', '--build', out, '-j', a.jobs, '--target', t.stem], log)
    if rc:
        row = {'test': t.stem, 'status': 'BUILD_FAIL', 'exit': rc, 'counts': None, 'seconds': round(time.monotonic() - start, 3), 'log': str(log)}
    else:
        with tempfile.TemporaryDirectory(prefix='co-', dir='/tmp') as raw:
            scratch = Path(raw)
            for n in ['h', 't', 'w']:
                (scratch / n).mkdir()
            env = clean | {'HOME': str(scratch / 'h'), 'TMPDIR': str(scratch / 't')}
            for assignment in closures[t.stem].env:
                k, v = assignment.split('=', 1)
                env[k] = v.replace('${SCRATCH}', str(scratch))
            if a.profile == 'asan':
                env |= {'ASAN_OPTIONS': 'detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1', 'UBSAN_OPTIONS': 'print_stacktrace=1'}
                if (g / 'tests/lsan-core.supp').exists():
                    env['LSAN_OPTIONS'] = 'suppressions=' + str(g / 'tests/lsan-core.supp')
            argv = ['sudo', '-n', 'unshare', '--net', '--', '/usr/bin/python3', str(Path(__file__).resolve()), '--isolated-child', str(os.getuid()), str(os.getgid()), json.dumps(env), str(out / t.stem)] + ([a.case] if a.case else [])
            with log.open('a') as f:
                f.write('\n$ ' + shlex.join(argv) + '\n')
                f.flush()
                proc = subprocess.Popen(argv, cwd=scratch / 'w', env=env, stdout=f, stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    rc = proc.wait(timeout=600)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
                    rc = 124
                finally:
                    # A completed test must not leave child processes in its group.
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
        txt = log.read_text(errors='replace')
        cnt = re.findall('(\\d+) run, (\\d+) passed, (\\d+) failed', txt)
        cnt = list(map(int, cnt[-1])) if cnt else None
        status = 'PASS' if rc == 0 and cnt and (cnt[0] > 0) and (cnt[2] == 0) else 'TIMEOUT' if rc == 124 else 'FAIL'
        row = {'test': t.stem, 'status': status, 'exit': rc, 'counts': cnt, 'seconds': round(time.monotonic() - start, 3), 'log': str(log)}
        (out / t.stem).unlink(missing_ok=True)
    rows.append(row)
    text = json.dumps(rows, indent=2) + '\n'
    (runroot / 'results.json').write_text(text)
    (out / 'results.json').write_text(text)
    print(json.dumps(row), flush=True)
provenance |= {'inputs_unchanged': before == hashes(), 'identity_unchanged': ident == identity()}
(runroot / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
print(json.dumps({'phase': 'terminal', 'profile': a.profile, 'pass': sum((r['status'] == 'PASS' for r in rows)), 'fail': sum((r['status'] != 'PASS' for r in rows)), 'cases': [sum(((r['counts'] or [0, 0, 0])[i] for r in rows)) for i in range(3)], 'inputs_unchanged': provenance['inputs_unchanged']}), flush=True)
raise SystemExit(int(any((r['status'] != 'PASS' for r in rows)) or not provenance['inputs_unchanged'] or (not provenance['identity_unchanged'])))
