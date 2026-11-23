"""A fresh Linux network/PID namespace, never a JavaScript network facade."""

import errno
import hashlib
import json
import os
import signal
import socket
import subprocess
import time
from pathlib import Path

WORKTREE = Path(__file__).resolve().parents[4]
TASK = Path("/tmp/claude-1002/prod/smoke_perf")
FORBIDDEN_PORTS = {7999, 8000}


def sha256(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=False)
        stream.write("\n")
    temporary.chmod(0o600)
    temporary.replace(path)


def source_inputs():
    paths = list((WORKTREE / "cpp/gateway/tests/production_smoke").glob("*.py"))
    paths.extend((WORKTREE / "cpp/gateway/bench/paths").glob("*"))
    paths.append(WORKTREE / "cpp/gateway/tools/smoke.py")
    hashes = {str(path.relative_to(WORKTREE)): sha256(path) for path in sorted(paths) if path.is_file()}
    return hashes


def require_task_path(path):
    resolved = Path(path).resolve()
    if not (resolved.is_relative_to(WORKTREE / ".data") or resolved.is_relative_to(TASK)):
        raise RuntimeError("Harness outputs must stay in worktree .data or assigned task evidence")
    return resolved


def sparse_environment(directory, additions=None):
    directory = Path(directory)
    for name in ("home", "tmp"):
        (directory / name).mkdir(parents=True, exist_ok=True, mode=0o700)
    result = {
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
        "HOME": str(directory / "home"),
        "TMPDIR": str(directory / "tmp"),
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
    }
    if additions:
        result.update(additions)
    return result


def resources():
    memory = {}
    for line in Path("/proc/meminfo").read_text().splitlines():
        key, value = line.split(":", 1)
        memory[key] = int(value.strip().split()[0]) * 1024
    disk = os.statvfs(WORKTREE)
    return {"available_bytes": memory["MemAvailable"], "disk_free_bytes": disk.f_bavail * disk.f_frsize}


def admit():
    measured = resources()
    if measured["available_bytes"] < 2500 * 1024 * 1024:
        raise RuntimeError("Smoke admission requires at least 2500 MiB MemAvailable")
    if measured["disk_free_bytes"] < 8 * 1024 * 1024 * 1024:
        raise RuntimeError("Smoke admission requires at least 8 GiB free disk")
    row = Path("/tmp/claude-1002/codex/resources.log").read_text().splitlines()[-1]
    if " gateway=200:" not in row:
        raise RuntimeError("Supervisor gateway health is not 200; fixture launch refused")
    measured["supervisor_row"] = row
    marker = row.split("supervisor_notes=", 1)[1].split()[0]
    measured["supervisor_notes_sha256"] = sha256(marker.rsplit("@", 1)[0])
    return measured


def netns_argv(command):
    return [
        "/usr/bin/unshare", "--user", "--map-root-user", "--net", "--mount", "--pid", "--fork",
        "--mount-proc", "--kill-child=SIGKILL", "--", *command,
    ]


def settle(process, timeout=3):
    """Terminate only a process group created by this harness, then reap its leader."""
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=timeout)
    return process.returncode


def run_namespace(command, directory, timeout):
    directory = require_task_path(directory)
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    receipt = {"command": netns_argv(command), "cwd": str(directory), "started_ns": time.time_ns(), "resources": admit()}
    with (directory / "namespace.stdout").open("wb") as output, (directory / "namespace.stderr").open("wb") as errors:
        child = subprocess.Popen(receipt["command"], cwd=directory, env=sparse_environment(directory),
                                 stdin=subprocess.DEVNULL, stdout=output, stderr=errors, start_new_session=True)
        receipt["pid"] = child.pid
        try:
            receipt["exit_code"] = child.wait(timeout=timeout)
            receipt["timed_out"] = False
        except subprocess.TimeoutExpired:
            receipt["timed_out"] = True
            receipt["exit_code"] = settle(child)
        finally:
            if child.poll() is None:
                settle(child)
    receipt["finished_ns"] = time.time_ns()
    receipt["settled"] = child.poll() is not None
    write_json(directory / "namespace-receipt.json", receipt)
    return receipt


def qualify(parent_namespace):
    current = os.readlink("/proc/self/ns/net")
    if current == parent_namespace:
        raise RuntimeError("Network namespace equals the parent's namespace")
    subprocess.run(["/usr/bin/ip", "link", "set", "dev", "lo", "up"], check=True, timeout=3)
    addresses = json.loads(subprocess.check_output(["/usr/bin/ip", "-j", "address"], timeout=3))
    routes = json.loads(subprocess.check_output(["/usr/bin/ip", "-j", "route", "show", "table", "all"], timeout=3))
    if any(item["ifname"] != "lo" for item in addresses):
        raise RuntimeError("Unexpected non-loopback interface in smoke namespace")
    if any(item.get("dev") != "lo" for item in routes):
        raise RuntimeError("Unexpected outside route in smoke namespace")
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        address = listener.getsockname()
        if address[1] in FORBIDDEN_PORTS:
            raise RuntimeError("OS assigned a prohibited fixture port")
        with socket.create_connection(address, timeout=1) as sender:
            receiver, _ = listener.accept()
            with receiver:
                sender.sendall(b"namespace-loopback-positive")
                if receiver.recv(64) != b"namespace-loopback-positive":
                    raise RuntimeError("Loopback positive failed")
    with socket.socket() as outside:
        outside.settimeout(1)
        refusal = outside.connect_ex(("192.0.2.1", 443))
    if refusal not in (errno.ENETUNREACH, errno.EHOSTUNREACH, errno.EPERM, errno.EACCES):
        raise RuntimeError("Outside-route negative did not fail closed: " + str(refusal))
    return {"parent_namespace": parent_namespace, "namespace": current, "interfaces": addresses, "routes": routes,
            "loopback_positive": True, "outside_destination": "192.0.2.1:443", "outside_errno": refusal}
