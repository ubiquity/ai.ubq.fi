#!/usr/bin/python3
"""Process-group and physical-memory bounds for serial gateway test execution."""
import fcntl
import os
from pathlib import Path
import select
import signal
import subprocess
import time
import resource

from build_support import GIB, RESERVE, WORKSPACE, atomic_json, snapshot, start_ticks


def lock(path, owner=None):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = path.open("a+")
    fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    if owner:
        import json
        handle.seek(0)
        handle.truncate()
        handle.write(json.dumps({"owner": owner, "pid": os.getpid(), "started": time.time()}) + "\n")
        handle.flush()
    return handle


def group_rss(pgid):
    """Sum RSS only for our process group; never read another process's environment/argv."""
    total = 0
    members = []
    page_size = os.sysconf("SC_PAGE_SIZE")
    for path in Path("/proc").glob("[0-9]*/stat"):
        try:
            fields = path.read_text().rsplit(")", 1)[1].split()
            if int(fields[2]) != pgid:
                continue
            pid = int(path.parent.name)
            rss = int((path.parent / "statm").read_text().split()[1]) * page_size
            total += rss
            members.append(pid)
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
    return total, members


def stop_group(child, hard=False):
    try:
        os.killpg(child.pid, signal.SIGKILL if hard else signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        child.wait(timeout=3)
    except subprocess.TimeoutExpired:
        os.killpg(child.pid, signal.SIGKILL)
        child.wait(timeout=3)
    # A reaped leader does not prove its descendants settled. Kill our remaining group.
    try:
        os.killpg(child.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def wait_for_group(pgid):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        _, members = group_rss(pgid)
        if not members:
            return []
        time.sleep(0.02)
    return members


def execute(command, cwd, environment, evidence, timeout=300, rss_limit=2 * GIB, address_limit=GIB):
    """Capture exactly once; a sanitizer's shadow mapping never gets an RLIMIT_AS cap."""
    evidence = Path(evidence)
    evidence.mkdir(parents=True, exist_ok=True)
    log = evidence / "run.log"
    active_path = evidence / "active.json"
    actual = list(map(str, command))
    if address_limit is not None:
        actual = ["/usr/bin/prlimit", "--as=" + str(address_limit), "--"] + actual
    elif resource.getrlimit(resource.RLIMIT_AS) != (resource.RLIM_INFINITY, resource.RLIM_INFINITY):
        raise RuntimeError("Sanitizer runtime refused an inherited virtual-address-space limit")
    before = snapshot()
    started = time.monotonic()
    peak_rss = 0
    samples = []
    failure = None
    usage = None
    code = None
    cancellation = None
    old_handlers = {}
    with log.open("w") as output:
        child = subprocess.Popen(actual, cwd=cwd, env=environment, stdin=subprocess.DEVNULL,
                                 stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            ticks = start_ticks(child.pid)
        except FileNotFoundError:
            ticks = None
        atomic_json(active_path, {"pid": child.pid, "start_ticks": ticks, "command": actual,
                                  "cwd": str(cwd), "started": time.time()})

        def interrupted(number, _frame):
            raise InterruptedError("Owned test received signal %d" % number)

        for number in (signal.SIGINT, signal.SIGTERM):
            old_handlers[number] = signal.signal(number, interrupted)
        pidfd = None
        try:
            try:
                pidfd = os.pidfd_open(child.pid)
            except ProcessLookupError:
                pass
            while True:
                waited, status, child_usage = os.wait4(child.pid, os.WNOHANG)
                if waited:
                    code = os.waitstatus_to_exitcode(status)
                    child.returncode = code
                    usage = child_usage
                    break
                current = snapshot()
                rss, members = group_rss(child.pid)
                peak_rss = max(peak_rss, rss)
                current.update(process_group_rss_bytes=rss, process_group_members=members)
                samples.append(current)
                if current["mem_available_bytes"] < RESERVE:
                    raise RuntimeError("Test crossed the 2 GiB host MemAvailable reserve")
                if rss > rss_limit:
                    raise RuntimeError("Test process-group physical RSS exceeded %d bytes" % rss_limit)
                if current["disk_free_bytes"] <= 8 * GIB:
                    raise RuntimeError("Test crossed the 8 GiB free-disk reserve")
                if time.monotonic() - started > timeout:
                    raise TimeoutError("Test exceeded %ds" % timeout)
                if os.fstat(output.fileno()).st_size > 64 * 1024 ** 2:
                    raise RuntimeError("Test output exceeded 64 MiB")
                select.select([pidfd] if pidfd is not None else [], [], [], 0.2)
        except BaseException as error:
            failure = str(error)
            stop_group(child, hard=isinstance(error, RuntimeError))
            code = child.returncode
            if isinstance(error, (InterruptedError, KeyboardInterrupt, SystemExit)):
                cancellation = error
        finally:
            if pidfd is not None:
                os.close(pidfd)
            for number, handler in old_handlers.items():
                signal.signal(number, handler)
            _, remaining = group_rss(child.pid)
            if remaining:
                stop_group(child)
                failure = failure or "Test left descendant processes after its leader exited"
                remaining = wait_for_group(child.pid)
            result = {"actual_command": actual, "cwd": str(cwd), "code": code, "failure": failure,
                      "duration_s": round(time.monotonic() - started, 3), "before": before, "after": snapshot(),
                      "address_space_limit_bytes": address_limit, "process_group_rss_limit_bytes": rss_limit,
                      "rss_enforcement": "0.2s sampled trigger, not a kernel hard cap; observed overshoot is recorded",
                      "process_group_peak_rss_bytes": peak_rss, "max_child_rss_kib": usage.ru_maxrss if usage else None,
                      "user_s": usage.ru_utime if usage else None, "system_s": usage.ru_stime if usage else None,
                      "sample_interval_s": 0.2, "samples": samples, "log": str(log), "settled": not remaining}
            atomic_json(evidence / "process.json", result)
            active_path.unlink(missing_ok=True)
    if cancellation is not None:
        raise cancellation
    return result


def runtime_lock():
    return lock(WORKSPACE / ".data/cpp-gateway/locks/test-runtime.lock")
