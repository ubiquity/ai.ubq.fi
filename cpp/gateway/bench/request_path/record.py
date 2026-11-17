"""Gate perf recording to the measured batch using perf's FIFO control channel."""

from contextlib import contextmanager, ExitStack
import os
import select
import subprocess


@contextmanager
def record(pid, root, use_sudo):
    control = root / "perf.control"
    acknowledgement = root / "perf.ack"
    log = root / "perf.log"
    for fifo in (control, acknowledgement):
        os.mkfifo(fifo, 0o600)
    command = ["sudo", "-n"] if use_sudo else []
    command += ["perf", "record", "-e", "cycles:u", "-F", "997", "--call-graph", "fp", "--no-buildid-cache", "--delay=-1",
                f"--control=fifo:{control},{acknowledgement}", "--pid", str(pid), "-o", "-"]
    with ExitStack() as resources:
        command_fd = os.open(control, os.O_RDWR | os.O_NONBLOCK)
        resources.callback(os.close, command_fd)
        ack_fd = os.open(acknowledgement, os.O_RDWR | os.O_NONBLOCK)
        resources.callback(os.close, ack_fd)
        data = resources.enter_context((root / "perf.data").open("wb"))
        errors = resources.enter_context(log.open("wb"))
        profiler = subprocess.Popen(command, stdout=data, stderr=errors, stdin=subprocess.DEVNULL)

        def send(message):
            os.write(command_fd, message + b"\n")
            ready, _, _ = select.select([ack_fd], [], [], 10)
            reply = os.read(ack_fd, 32) if ready else b""
            if reply.rstrip(b"\0\r\n") != b"ack":
                raise RuntimeError(f"perf replied {reply!r} to {message.decode()}, see {log}")

        try:
            send(b"enable")
            yield
            send(b"disable")
        finally:
            if profiler.poll() is None:
                os.write(command_fd, b"stop\n")
                try:
                    profiler.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    profiler.terminate()
                    profiler.wait(timeout=10)
            if profiler.returncode:
                raise RuntimeError(f"perf exited {profiler.returncode}, see {log}")
    for fifo in (control, acknowledgement):
        fifo.unlink()
