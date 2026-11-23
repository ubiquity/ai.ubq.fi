"""Event-driven child readiness and bounded, post-exec physical-memory sampling."""

import json
import os
import queue
import re
import signal
import statistics
import subprocess
import threading
import time
from pathlib import Path

from network import resources, settle, sparse_environment, write_json


class GatewayProcess:
    def __init__(self, command, directory, environment, label="gateway"):
        self.command = command
        self.directory = Path(directory)
        self.environment = sparse_environment(self.directory, environment)
        self.label = label
        self.lines = []
        self.samples = []
        self.phase = "startup"
        self.failure = None
        self.shutdown_signalled = False
        self.ready = queue.Queue()
        self.stop_monitor = threading.Event()
        self.started_ns = time.perf_counter_ns()
        self.stdout = (self.directory / (label + ".stdout")).open("w", encoding="utf-8")
        self.stderr = (self.directory / (label + ".stderr")).open("w", encoding="utf-8")
        self.child = subprocess.Popen(command, cwd=self.directory, env=self.environment, stdin=subprocess.DEVNULL,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1, start_new_session=True)
        try:
            self.exec_path = os.readlink(f"/proc/{self.child.pid}/exe")
        except FileNotFoundError:
            self.exec_path = None
        self.readers = [threading.Thread(target=self._read, args=(self.child.stdout, self.stdout, "stdout"), daemon=False),
                        threading.Thread(target=self._read, args=(self.child.stderr, self.stderr, "stderr"), daemon=False)]
        for reader in self.readers:
            reader.start()
        self.monitor = threading.Thread(target=self._sample, daemon=False)
        self.monitor.start()
        try:
            self.origin = self.ready.get(timeout=15)
        except queue.Empty:
            self.close(force=True)
            raise RuntimeError("Gateway did not publish a loopback listen event within 15 seconds")
        if isinstance(self.origin, Exception):
            self.close(force=True)
            raise self.origin
        self.runtime_cwd = os.readlink(f"/proc/{self.child.pid}/cwd")
        if self.runtime_cwd != str(self.directory.resolve()):
            self.close(force=True)
            raise RuntimeError("Gateway's observed working directory escaped its private state root")
        self.ready_us = (time.perf_counter_ns() - self.started_ns) / 1000

    def _read(self, source, destination, channel):
        try:
            for line in source:
                destination.write(line)
                destination.flush()
                self.lines.append({"channel": channel, "at_ns": time.perf_counter_ns(), "line": line.rstrip("\n")})
                match = re.search(r"Listening on (http://127\.0\.0\.1:(\d+))/", line)
                if match:
                    port = int(match.group(2))
                    self.ready.put(RuntimeError("Prohibited listener port") if port in (7999, 8000) else match.group(1))
        finally:
            source.close()

    def _sample(self):
        path = Path(f"/proc/{self.child.pid}/status")
        while not self.stop_monitor.is_set():
            try:
                fields = dict(line.split(":", 1) for line in path.read_text().splitlines() if ":" in line)
                rss = int(fields.get("VmRSS", "0 kB").split()[0]) * 1024
                self.samples.append({"at_ns": time.perf_counter_ns(), "phase": self.phase, "rss_bytes": rss})
                if rss > 2 * 1024 * 1024 * 1024 or resources()["available_bytes"] < 2 * 1024 * 1024 * 1024:
                    self.failure = "Owned gateway exceeded its 2 GiB RSS bound or host reserve fell below 2 GiB"
                    os.killpg(self.child.pid, signal.SIGKILL)
                    break
            except (OSError, ProcessLookupError):
                break
            self.stop_monitor.wait(0.02)

    def idle(self, label):
        first_sample = len(self.samples)
        self.phase = label
        self.stop_monitor.wait(0.66)
        values = [item["rss_bytes"] for item in self.samples[first_sample:] if item["phase"] == label and item["rss_bytes"]]
        if len(values) < 20:
            raise RuntimeError("Insufficient post-exec idle RSS samples")
        return {"sample_count": len(values), "median_rss_bytes": statistics.median(values), "peak_rss_bytes": max(values)}

    def signal(self, selected):
        if selected in (signal.SIGTERM, signal.SIGINT):
            self.shutdown_signalled = True
        self.child.send_signal(selected)

    def close(self, force=False):
        if force:
            settle(self.child)
        else:
            if self.child.poll() is None and not self.shutdown_signalled:
                self.signal(signal.SIGTERM)
            try:
                self.child.wait(timeout=15)
            except subprocess.TimeoutExpired:
                settle(self.child)
                self.failure = self.failure or "Gateway failed natural shutdown deadline"
        self.stop_monitor.set()
        self.monitor.join(timeout=3)
        for reader in self.readers:
            reader.join(timeout=3)
        self.stdout.close()
        self.stderr.close()
        if self.monitor.is_alive() or any(reader.is_alive() for reader in self.readers):
            raise RuntimeError("Gateway observer threads failed to settle")
        phases = {}
        for item in self.samples:
            if item["rss_bytes"]:
                phases.setdefault(item["phase"], []).append(item["rss_bytes"])
        result = {"command": self.command, "cwd": str(self.directory), "environment_names": sorted(self.environment),
                  "pid": self.child.pid, "post_exec_executable": self.exec_path, "exit_code": self.child.returncode,
                  "observed_runtime_cwd": getattr(self, "runtime_cwd", None),
                  "failure": self.failure, "samples": self.samples, "events": self.lines,
                  "memory": {name: {"sample_count": len(values), "median_rss_bytes": statistics.median(values),
                                    "peak_rss_bytes": max(values)} for name, values in phases.items()},
                  "settled": self.child.poll() is not None, "forced": force}
        write_json(self.directory / (self.label + "-process-receipt.json"), result)
        return result
