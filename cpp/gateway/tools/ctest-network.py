#!/usr/bin/python3
"""Prove a new loopback-only namespace, then exec the exact registered test command."""
import errno
import json
import os
import socket
import sys
from pathlib import Path


def main():
    parent_namespace = sys.argv[1]
    actual = os.readlink("/proc/self/ns/net")
    if actual == parent_namespace:
        raise RuntimeError("Fixture network namespace did not change")
    temporary = os.stat("/tmp")
    identity = str(temporary.st_dev) + ":" + str(temporary.st_ino)
    if identity != sys.argv[2] or os.getcwd() != "/tmp/w":
        raise RuntimeError("Private fixture /tmp mount or working directory identity mismatch")
    mounts = [line.split() for line in Path("/proc/self/mountinfo").read_text().splitlines()]
    selected_mounts = [fields for fields in mounts if fields[4] in ("/", "/tmp")]
    if not selected_mounts or any(any(field.startswith(("shared:", "master:")) for field in fields[6:fields.index("-")])
                                  for fields in selected_mounts):
        raise RuntimeError("Fixture root or /tmp mount propagation is not private")
    with socket.socket() as listener, socket.socket() as client:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        client.settimeout(1)
        client.connect(("127.0.0.1", port))
        with listener.accept()[0] as peer:
            client.sendall(b"uos")
            positive = peer.recv(3) == b"uos"
    with socket.socket() as outside:
        outside.settimeout(1)
        negative = outside.connect_ex(("192.0.2.1", 443))
    if not positive or negative not in (errno.ENETUNREACH, errno.EHOSTUNREACH):
        raise RuntimeError("Fixture network boundary control failed: " + repr((positive, negative)))
    print("UOS_NETWORK_RECEIPT " + json.dumps({"network_namespace_isolated": True, "loopback_positive": positive,
          "outside_route_errno": negative, "os_assigned_loopback_port": port,
          "private_tmp_verified": True, "mount_propagation_private": True,
          "cwd": os.getcwd(), "tmpdir": os.environ.get("TMPDIR")}), flush=True)
    os.execv(sys.argv[3], sys.argv[3:])


if __name__ == "__main__":
    main()
