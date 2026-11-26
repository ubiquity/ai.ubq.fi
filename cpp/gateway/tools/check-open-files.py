#!/usr/bin/env python3
"""Check the native service budget, or exercise its inherited limit in isolation."""
import argparse
import configparser
from contextlib import ExitStack
import json
import os
from pathlib import Path
import re
import resource
import socket
import sys

NOFILE = 4096
ACTIVE = 512
WAITING = 128
IDLE_PER_SCHEME = 64
RUNTIME_RESERVE = 256
STREAMS = 500


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def check_templates():
    gateway = Path(__file__).resolve().parents[1]
    admission = (gateway / "include/uos/inference_admission.hpp").read_text()
    limits = re.search(r"DEFAULT_INFERENCE_ADMISSION_LIMITS\{(\d+),\s*(\d+),", admission)
    require(limits and tuple(map(int, limits.groups())) == (ACTIVE, WAITING),
            "admission defaults changed: revisit the open-file budget and fixture")
    transport = (gateway / "include/uos/rt/fetch_transport.hpp").read_text()
    idle = re.search(r"maxIdleConnections = (\d+);", transport)
    require(idle and int(idle[1]) == IDLE_PER_SCHEME,
            "idle pool changed: revisit the open-file budget and fixture")
    for name in ("uos-gateway-native.service", "uos-gateway-native-no-landlock.service",
                 "side-by-side/uos-gateway-side-by-side.service.in"):
        unit = configparser.ConfigParser(interpolation=None, strict=False)
        unit.optionxform = str
        unit.read(gateway / "ops" / name)
        require(unit.get("Service", "LimitNOFILE", fallback="") == str(NOFILE),
                f"{name}: expected LimitNOFILE={NOFILE} (both soft and hard)")
    budget = ACTIVE * 2 + WAITING + IDLE_PER_SCHEME * 2 + RUNTIME_RESERVE
    require(budget < NOFILE, "service limit must leave headroom above the planned budget")
    print(f"PASS: all three services set soft/hard {NOFILE}; planned budget {budget}")


def socket_fixture(parent_netns, port):
    require(sys.platform == "linux" and parent_netns,
            "fixture requires Linux isolation and --parent-netns from the calling host")
    require(re.fullmatch(r"net:\[\d+\]", parent_netns), "invalid --parent-netns")
    require(os.readlink("/proc/self/ns/net") != parent_netns,
            "refusing socket fixture without a private network namespace")
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    print(json.dumps({"effective_soft": soft, "effective_hard": hard}), flush=True)
    require((soft, hard) == (NOFILE, NOFILE),
            f"expected inherited soft/hard {NOFILE}; fixture never raises its own limit")

    with ExitStack() as owned:
        listener = owned.enter_context(socket.socket(socket.AF_INET, socket.SOCK_STREAM))
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.settimeout(5)
        listener.bind(("127.0.0.1", port))
        listener.listen()

        def connection():
            client = owned.enter_context(socket.socket(socket.AF_INET, socket.SOCK_STREAM))
            client.settimeout(5)
            client.connect(listener.getsockname())
            peer = owned.enter_context(listener.accept()[0])
            peer.settimeout(5)
            client.sendall(b".")
            require(peer.recv(1) == b".", "loopback socket did not deliver its payload")

        # Keep BOTH ends of every client and upstream link in this process:
        # four descriptors per stream, versus two in the actual gateway.
        for _ in range(STREAMS):
            connection()
            connection()
        for _ in range(WAITING + IDLE_PER_SCHEME * 2):
            connection()
        for _ in range(RUNTIME_RESERVE):
            owned.enter_context(open(os.devnull, "rb"))
        # listdir temporarily opens one descriptor of its own.
        held = len(os.listdir("/proc/self/fd")) - 1
        require(1024 < held < soft, "fixture must exceed the old limit and retain headroom")
        print(json.dumps({"result": "PASS", "streams": STREAMS, "queued_clients": WAITING,
                          "idle_upstreams": IDLE_PER_SCHEME * 2,
                          "runtime_reserve": RUNTIME_RESERVE, "open_files": held}), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="check source templates, no sockets")
    mode.add_argument("--fixture", action="store_true", help="run from a disposable isolated unit")
    parser.add_argument("--parent-netns", help="calling host's /proc/self/ns/net symlink target")
    parser.add_argument("--port", type=int, default=0, help="rendered unit's allowed loopback port")
    args = parser.parse_args()
    try:
        if args.check:
            check_templates()
        else:
            socket_fixture(args.parent_netns, args.port)
    except (OSError, RuntimeError, ValueError, configparser.Error) as error:
        parser.exit(1, f"open-file verification failed: {error}\n")


if __name__ == "__main__":
    main()
