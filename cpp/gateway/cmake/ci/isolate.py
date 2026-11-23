#!/usr/bin/env python3
"""Enter an already-created Linux namespace, prove loopback isolation, then exec."""
import argparse
import errno
import fcntl
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scratch", type=Path)
    parser.add_argument("binary_directory", type=Path)
    parser.add_argument("binary")
    parser.add_argument("checkout", type=Path)
    parser.add_argument("checkout_alias")
    parser.add_argument("parent_namespace")
    parser.add_argument("uid", type=int)
    parser.add_argument("gid", type=int)
    parser.add_argument("environment", help="credential-free environment supplied by the controller")
    options = parser.parse_args()
    actual_namespace = os.readlink("/proc/self/ns/net")
    if actual_namespace == options.parent_namespace:
        raise RuntimeError("network namespace did not change")
    commands = {"PATH": "/usr/bin:/bin:/usr/sbin:/sbin"}
    # tmpfs preserves the full Linux timestamp range used by the Bun filesystem
    # vectors; an ext4-backed runner /tmp clamps those otherwise valid times.
    subprocess.run(["mount", "-t", "tmpfs", "-o", "size=512M,mode=0700,uid=%d,gid=%d" % (options.uid, options.gid),
                    "tmpfs", str(options.scratch)], env=commands, check=True)
    for name in ("h", "t", "w"):
        (options.scratch / name).mkdir()
        os.chown(options.scratch / name, options.uid, options.gid)
    # Keep scratch and binary paths short while hiding all other host /tmp state.
    (options.scratch / "b").mkdir()
    subprocess.run(["mount", "--bind", str(options.binary_directory), str(options.scratch / "b")], env=commands, check=True)
    fixture_source = options.scratch / options.checkout_alias
    fixture_source.mkdir()
    subprocess.run(["mount", "-o", "bind,ro", str(options.checkout), str(fixture_source)], env=commands, check=True)
    subprocess.run(["mount", "--rbind", str(options.scratch), "/tmp"], env=commands, check=True)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as control:
        flags = fcntl.ioctl(control, 0x8913, struct.pack("16sH14x", b"lo", 0))
        current = struct.unpack("16sH14x", flags)[1]
        fcntl.ioctl(control, 0x8914, struct.pack("16sH14x", b"lo", current | 1))
    os.chdir("/tmp/w")
    # Root exists only for namespace setup. The sudo fallback drops back to the
    # invoking host user before the controls and the test execute.
    if Path("/proc/self/setgroups").read_text().strip() != "deny":
        os.setgroups([])
    os.setgid(options.gid)
    os.setuid(options.uid)
    with socket.socket() as listener, socket.socket() as client:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        client.settimeout(1)
        client.connect(listener.getsockname())
        with listener.accept()[0] as peer:
            client.sendall(b"uos")
            positive = peer.recv(3) == b"uos"
    with socket.socket() as outside:
        outside.settimeout(1)
        negative = outside.connect_ex(("192.0.2.1", 443))
    if not positive or negative not in (errno.ENETUNREACH, errno.EHOSTUNREACH):
        raise RuntimeError("fixture network control failed: " + repr((positive, negative)))
    environment = json.loads(options.environment)
    environment.update(HOME="/tmp/h", TMPDIR="/tmp/t")
    print("UOS_CI_NETWORK " + json.dumps({"loopback": positive, "outside_errno": negative,
          "namespace": actual_namespace, "private_tmp": True, "tmpfs_mib": 512, "uid": os.getuid()}), flush=True)
    if options.binary != ":probe:":
        binary = "/tmp/b/" + options.binary
        os.execve(binary, [binary], environment)


if __name__ == "__main__":
    main()
