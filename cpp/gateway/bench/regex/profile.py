#!/usr/bin/python3
"""Sample only a task-owned benchmark inferior, without rebuilding its code."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from build_support import admission, measured


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--supervisor-log", type=Path)
    args = parser.parse_args()
    workspace = Path(__file__).resolve().parents[4]
    binary, manifest, output = args.binary.resolve(), args.manifest.resolve(), args.output.resolve()
    if workspace / ".data" not in binary.parents or not binary.is_file():
        raise RuntimeError("Profile binary must exist inside this worktree's .data")
    if not (Path("/tmp/claude-1002/prod/idna_regex") in output.parents or workspace / ".data" in output.parents):
        raise RuntimeError("Profile output must be task/worktree scoped")
    if not 1 <= args.iterations <= 10000:
        raise RuntimeError("Profile iterations outside finite bounds")
    admitted = admission("driver", args.supervisor_log)
    output.mkdir(parents=True, exist_ok=False)
    for name in ("h", "t", "w"):
        (output / name).mkdir()
    profile = output / "profile.json"
    binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
    manifest_hash = hashlib.sha256(manifest.read_bytes()).hexdigest()
    sampler = output / "sampler.py"
    sampler.write_text("import os,signal,sys,time\npid=int(sys.argv[1])\nfor number in range(400):\n time.sleep(.01)\n try:os.kill(pid,signal.SIGINT)\n except ProcessLookupError:break\n")
    script = output / "sample.gdb"
    script.write_text("""set pagination off
set confirm off
set startup-with-shell off
set disable-randomization off
set debuginfod enabled off
set auto-load safe-path /nonexistent/uos-profile-noautoload
handle SIGINT stop print nopass
python
import gdb, json, subprocess
samples = []
terminal = []
gdb.events.exited.connect(lambda event: terminal.append(getattr(event, 'exit_code', None)))
gdb.execute('start')
inferior = gdb.selected_inferior()
owned_pid = inferior.pid
sampler = subprocess.Popen(['/usr/bin/python3', """ + repr(str(sampler)) + """, str(owned_pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for iteration in range(400):
        gdb.execute('continue', to_string=True)
        if inferior.pid == 0:
            break
        frame = gdb.newest_frame()
        stack = []
        while frame is not None and len(stack) < 8:
            stack.append({'name':frame.name(), 'pc':int(frame.pc())})
            frame = frame.older()
        samples.append(stack)
finally:
    if sampler.poll() is None:
        sampler.terminate()
    try:
        sampler.wait(timeout=1)
    except subprocess.TimeoutExpired:
        sampler.kill()
        sampler.wait()
if inferior.pid:
    gdb.execute('continue', to_string=True)
with open(""" + repr(str(profile)) + """, 'w') as file:
    json.dump({'interval_ms':10,'samples':samples,'owned_pid':owned_pid,'settled':inferior.pid==0,'terminal':terminal,'sampler_settled':sampler.returncode is not None},file,indent=2)
end
quit
""")
    env = {"PATH": "/usr/bin:/bin", "HOME": str(output / "h"), "TMPDIR": str(output / "t"), "LANG": "C.UTF-8", "TZ": "UTC"}
    command = ["/usr/bin/unshare", "--user", "--map-root-user", "--net", "--mount", "--pid", "--fork", "--mount-proc",
               "--propagation", "private", "--kill-child", "--", "/usr/bin/gdb", "-nx", "-nh", "--batch",
               "-iex", "set auto-load off", "-iex", "set debuginfod enabled off", "-x", str(script), "--args",
               str(binary), str(manifest), args.case, "1", "0", str(args.iterations)]
    started = time.monotonic()
    with (output / "stdout.log").open("w") as stdout:
        code, process_receipt = measured(command, output / "w", env, output / "execution", "gdb-profile", timeout=180, output=stdout)
    receipt = {"command": command, "cwd": str(output / "w"), "code": code, "duration_s": time.monotonic() - started,
               "binary_sha256": binary_hash, "manifest_sha256": manifest_hash,
               "script_sha256": hashlib.sha256(script.read_bytes()).hexdigest(), "sampler_sha256": hashlib.sha256(sampler.read_bytes()).hexdigest(),
               "environment_names": list(env), "admission": admitted, "process_receipt": str(process_receipt),
               "measurement_helper_sha256": hashlib.sha256((workspace / "cpp/gateway/tools/build_support.py").read_bytes()).hexdigest(),
               "capture": "stdout and stderr combined in stdout.log"}
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt), flush=True)
    result = json.loads(profile.read_text()) if profile.is_file() else {}
    unchanged = hashlib.sha256(binary.read_bytes()).hexdigest() == binary_hash and hashlib.sha256(manifest.read_bytes()).hexdigest() == manifest_hash
    if code or not result.get("settled") or result.get("terminal") != [0] or not result.get("samples") or not result.get("sampler_settled") or not unchanged:
        raise RuntimeError("Profiling failed; retained exact logs and receipt")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
