#!/usr/bin/python3
"""Shared resource and evidence support for the offline gateway build only."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shutil
import signal
import subprocess
import time

GATEWAY = Path(__file__).resolve().parents[1]
WORKSPACE = GATEWAY.parents[1]
GIB = 1024 ** 3
RESERVE = 2 * GIB
DISK_RESERVE = 8 * GIB
SDK_MANIFEST_SHA256 = "9dde9684e06dcbbdc5433fd26c8517cb0ff0b63c2a80e2501f05f6ce27eb52f5"


def digest(path):
    with Path(path).open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + str(os.getpid()) + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def local_inputs(source):
    """Hash a source's actual first-party quoted-include closure, not unrelated modules."""
    queue = [Path(source).resolve()]
    found = {}
    while queue:
        path = queue.pop()
        if GATEWAY not in path.parents or not path.is_file():
            continue
        relative = path.relative_to(GATEWAY).as_posix()
        if relative in found:
            continue
        content = path.read_bytes()
        found[relative] = hashlib.sha256(content).hexdigest()
        for name in re.findall(rb'^\s*#\s*include\s*"([^"\n]+)"', content, re.M):
            for parent in (path.parent, GATEWAY / "include", GATEWAY):
                candidate = (parent / os.fsdecode(name)).resolve()
                if candidate.is_file() and GATEWAY in candidate.parents:
                    queue.append(candidate)
                    break
    return found


def snapshot():
    fields = dict(line.split(":", 1) for line in Path("/proc/meminfo").read_text().splitlines())
    return {"mem_available_bytes": int(fields["MemAvailable"].split()[0]) * 1024,
            "disk_free_bytes": shutil.disk_usage(WORKSPACE).free, "at": time.time()}


def allocated(paths):
    seen = set()
    total = 0
    for root in paths:
        if not root.exists():
            continue
        for directory, _, files in os.walk(root):
            for path in [Path(directory)] + [Path(directory) / name for name in files]:
                try:
                    info = path.lstat()
                except FileNotFoundError:
                    continue
                identity = (info.st_dev, info.st_ino)
                if identity not in seen:
                    seen.add(identity)
                    total += info.st_blocks * 512
    return total


def supervisor(path, expected_notes=None):
    if not path:
        return None
    rows = Path(path).read_text().splitlines()
    if not rows:
        raise RuntimeError("Supervisor resource log is empty")
    row = rows[-1]
    if " gateway=200:" not in row:
        raise RuntimeError("Supervisor gateway health is not 200; build admission refused")
    marker = row.split("supervisor_notes=", 1)[1].split()[0]
    notes = Path(marker.rsplit("@", 1)[0])
    notes_hash = digest(notes)
    pinned = os.environ.get("UOS_GATEWAY_INSTRUCTION_SNAPSHOT")
    evidence = os.environ.get("UOS_GATEWAY_BUILD_EVIDENCE")
    run_id = os.environ.get("UOS_GATEWAY_RUN_ID")
    if not pinned and evidence and run_id:
        candidate = Path(evidence) / ("instructions-" + run_id + ".md")
        if candidate.is_file():
            pinned = str(candidate)
    if pinned and expected_notes:
        snapshot_path = Path(pinned)
        if snapshot_path.is_symlink() or WORKSPACE / ".data" not in snapshot_path.resolve().parents or digest(snapshot_path) != expected_notes:
            raise RuntimeError("Reviewed instruction snapshot is missing, foreign or changed")
        if notes_hash != expected_notes:
            pending = Path(evidence) / ("instructions-pending-review-" + str(run_id) + ".json")
            previous = json.loads(pending.read_text()) if pending.exists() else {}
            if previous.get("live_notes_sha256") != notes_hash:
                atomic_json(pending, {"run_id": run_id, "active_snapshot": pinned, "active_notes_sha256": expected_notes,
                                     "live_notes": str(notes), "live_notes_sha256": notes_hash, "observed": time.time(),
                                     "policy": "review at controller checkpoint; live resource gates remain authoritative"})
                print("uos-build-instructions-pending-review run=" + str(run_id), flush=True)
        return {"row": row, "notes": str(notes), "notes_sha256": expected_notes, "snapshot": pinned,
                "live_notes_sha256": notes_hash, "pending_review": notes_hash != expected_notes}
    if expected_notes and notes_hash != expected_notes:
        raise RuntimeError("Supervisor notes changed; owner must read the new revision before build admission")
    return {"row": row, "notes": str(notes), "notes_sha256": notes_hash}


def bind_instruction_snapshot(resource_log, evidence, run_id):
    """Bind a human-reviewed note version; never auto-approve newly changed text."""
    accepted = supervisor(resource_log)
    review_path = WORKSPACE / ".data/cpp-gateway-reviewed-instructions.json"
    review = json.loads(review_path.read_text())
    if review.get("notes_sha256") != accepted["notes_sha256"] or review.get("notes") != accepted["notes"]:
        raise RuntimeError("New phase requires the execution owner to read and acknowledge current supervisor notes")
    target = Path(evidence) / ("instructions-" + str(run_id) + ".md")
    content = Path(accepted["notes"]).read_bytes()
    if hashlib.sha256(content).hexdigest() != accepted["notes_sha256"]:
        raise RuntimeError("Supervisor notes changed during reviewed snapshot binding")
    if not target.exists():
        temporary = target.with_name(target.name + "." + str(os.getpid()) + ".tmp")
        temporary.write_bytes(content)
        temporary.chmod(0o400)
        temporary.replace(target)
    if digest(target) != accepted["notes_sha256"]:
        raise RuntimeError("Existing phase snapshot differs from the reviewed instructions")
    return {**accepted, "snapshot": str(target), "review": review}


def admission(kind, resource_log=None, notes_hash=None, memory_minimum=None):
    # Supervisor v24 supersedes the older measured-link minimum hints.
    minimum = 7 * GIB // 2 if kind == "link" else 3 * GIB if kind in ("compile", "archive") else RESERVE
    started = time.monotonic()
    deadline = started + 20 * 60
    while True:
        current = snapshot()
        if current["disk_free_bytes"] <= DISK_RESERVE:
            raise RuntimeError("Build admission refused: free disk must exceed 8 GiB")
        if current["mem_available_bytes"] < RESERVE:
            raise RuntimeError("Build live reserve exhausted: MemAvailable is below 2 GiB")
        current["supervisor"] = supervisor(resource_log, notes_hash)
        current["admission_policy"] = "supervisor-v24"
        current["admission_minimum_bytes"] = minimum
        current["admission_wait_seconds"] = time.monotonic() - started
        if memory_minimum is not None:
            current["superseded_memory_minimum_bytes"] = memory_minimum
        if current["mem_available_bytes"] >= minimum:
            return current
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError("Build admission deferred after 1200 seconds: %s requires %d MiB MemAvailable" %
                               (kind, minimum // 1024 ** 2))
        print("uos-build-admission-wait kind=%s available_mib=%d minimum_mib=%d retry_s=30 remaining_s=%d" %
              (kind, current["mem_available_bytes"] // 1024 ** 2, minimum // 1024 ** 2, remaining), flush=True)
        time.sleep(min(30, remaining))


def acquire_slots(kind):
    # This is the existing unit.sh/cxx.sh lock directory, never a private copy.
    directory = WORKSPACE / ".data/cpp-gateway/locks"
    directory.mkdir(parents=True, exist_ok=True)
    first = (directory / "compiler-1.lock").open("a+")
    if kind == "link":
        fcntl.flock(first, fcntl.LOCK_EX)
        second = (directory / "compiler-2.lock").open("a+")
        fcntl.flock(second, fcntl.LOCK_EX)
        return [first, second]
    for number, handle in ((1, first), (2, (directory / "compiler-2.lock").open("a+"))):
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if number == 2:
                first.close()
            return [handle]
        except BlockingIOError:
            if number == 2:
                handle.close()
    # Block on the authoritative lock instead of polling for a free slot.
    fcntl.flock(first, fcntl.LOCK_EX)
    return [first]


def start_ticks(pid):
    return Path("/proc/%d/stat" % pid).read_text().rsplit(")", 1)[1].split()[19]


def owned_process_sample(leader):
    queue = [leader]
    rows = []
    while queue:
        pid = queue.pop()
        try:
            stat = Path("/proc/%d/stat" % pid).read_text().rsplit(")", 1)[1].split()
            if int(stat[2]) != leader:
                continue
            fields = dict(line.split(":", 1) for line in Path("/proc/%d/status" % pid).read_text().splitlines() if ":" in line)
            row = {"pid": pid, "name": fields["Name"].strip()}
            for key in ("VmRSS", "VmHWM", "VmSize", "VmPeak"):
                row[key + "_bytes"] = int(fields.get(key, "0 kB").split()[0]) * 1024
            rows.append(row)
            queue.extend(int(value) for value in Path("/proc/%d/task/%d/children" % (pid, pid)).read_text().split())
        except (FileNotFoundError, ProcessLookupError):
            continue
    return rows


def terminate_group(child):
    try:
        os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        child.wait(timeout=3)
    except subprocess.TimeoutExpired:
        os.killpg(child.pid, signal.SIGKILL)
        child.wait(timeout=3)


def measured(argv, cwd, env, evidence, label, timeout, address_limit=None, low_priority=False, output=None, footprint_paths=(), sample_interval_s=1, sample_processes=False):
    """One bounded process group; samples enforce the host reserve, not application behavior."""
    evidence = Path(evidence)
    evidence.mkdir(parents=True, exist_ok=True)
    stamp = "%d-%d" % (time.time_ns(), os.getpid())
    result_path = evidence / (label + "-" + stamp + ".json")
    active_path = evidence / (label + "-" + stamp + ".active.json")
    command = list(map(str, argv))
    if address_limit:
        command = ["/usr/bin/prlimit", "--as=" + str(address_limit), "--"] + command
    if low_priority:
        command = ["/usr/bin/nice", "-n", "19", "/usr/bin/ionice", "-c", "3"] + command
    started = time.monotonic()
    before = snapshot()
    baseline_bytes = allocated(footprint_paths) if footprint_paths else None
    peak_bytes = baseline_bytes
    last_footprint = started
    child = subprocess.Popen(command, cwd=cwd, env=env, start_new_session=True,
                             stdout=output, stderr=subprocess.STDOUT if output else None)
    completion_fd = os.pidfd_open(child.pid)
    active = {"pid": child.pid, "start_ticks": start_ticks(child.pid), "command": command,
              "cwd": str(cwd), "started": time.time(), "run_id": env.get("UOS_GATEWAY_RUN_ID")}
    atomic_json(active_path, active)
    old_handlers = {}

    def interrupted(number, _frame):
        raise InterruptedError("Owned build received signal %d" % number)

    for number in (signal.SIGTERM, signal.SIGINT):
        old_handlers[number] = signal.signal(number, interrupted)
    samples = []
    failure = None
    usage = None
    code = None
    last_size = 0
    last_progress = started
    try:
        while True:
            waited, status, child_usage = os.wait4(child.pid, os.WNOHANG)
            if waited:
                code = os.waitstatus_to_exitcode(status)
                child.returncode = code
                usage = child_usage
                break
            current = snapshot()
            if sample_processes:
                current["owned_processes"] = owned_process_sample(child.pid)
            if footprint_paths and time.monotonic() - last_footprint >= 5:
                current["owned_allocated_bytes"] = allocated(footprint_paths)
                peak_bytes = max(peak_bytes, current["owned_allocated_bytes"])
                last_footprint = time.monotonic()
            samples.append(current)
            if current["mem_available_bytes"] < RESERVE or current["disk_free_bytes"] <= DISK_RESERVE:
                raise RuntimeError("Owned build crossed the 2 GiB host-memory or 8 GiB free-disk reserve")
            if time.monotonic() - started > timeout:
                raise TimeoutError("Owned build exceeded its %ds phase budget" % timeout)
            if output:
                size = os.fstat(output.fileno()).st_size
                if size != last_size:
                    last_progress = time.monotonic()
                    last_size = size
                if size > 64 * 1024 ** 2:
                    raise RuntimeError("Owned build output exceeded its 64 MiB log budget")
                if time.monotonic() - last_progress > 600:
                    raise RuntimeError("Owned build made no log progress for 600s; bounded owner termination")
            # Process completion is authoritative; the timeout exists only for
            # resource/deadline sampling, so cache hits do not pay a 1s poll delay.
            select.select([completion_fd], [], [], sample_interval_s)
    except BaseException as error:
        failure = str(error)
        terminate_group(child)
        code = child.returncode
    finally:
        for number, handler in old_handlers.items():
            signal.signal(number, handler)
        os.close(completion_fd)
        result = {**active, "actual_command": command, "code": code, "failure": failure,
                  "duration_s": round(time.monotonic() - started, 3), "before": before, "after": snapshot(),
                  "max_rss_kib": usage.ru_maxrss if usage else None,
                  "user_s": usage.ru_utime if usage else None, "system_s": usage.ru_stime if usage else None,
                  "measurement": "wait4 child rusage; maxRSS is maximum, not sum of concurrent children",
                  "sample_interval_s": sample_interval_s, "samples": samples, "settled": child.returncode is not None}
        if sample_processes:
            rows = [row for sample in samples for row in sample.get("owned_processes", [])]
            result["sampled_process_peak_rss_bytes_lower_bound"] = max((row["VmHWM_bytes"] for row in rows), default=0)
            result["sampled_process_peak_vm_bytes_lower_bound"] = max((row["VmPeak_bytes"] for row in rows), default=0)
            result["sampled_tree_peak_rss_bytes_lower_bound"] = max((sum(row["VmRSS_bytes"] for row in sample.get("owned_processes", [])) for sample in samples), default=0)
        if footprint_paths:
            final_bytes = allocated(footprint_paths)
            result.update(owned_bytes_baseline=baseline_bytes, owned_bytes_peak_lower_bound=max(peak_bytes, final_bytes),
                          owned_bytes_final=final_bytes, footprint_sample_interval_s=5,
                          footprint_paths=[str(path) for path in footprint_paths])
        atomic_json(result_path, result)
        active_path.unlink(missing_ok=True)
    if failure:
        raise RuntimeError(failure + "; receipt=" + str(result_path))
    return code, result_path
