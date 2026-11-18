#!/usr/bin/env python3
"""Build a scratch-only lifetime probe with a gateway sanitizer receipt."""

import argparse
import json
import os
from pathlib import Path
import sys
import tempfile


GATEWAY = Path(__file__).resolve().parents[3]
sys.dont_write_bytecode = True
sys.path.insert(0, str(GATEWAY / "cmake/ci"))
import driver


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="ASan driver directory with receipt.json")
    parser.add_argument("source", type=Path, help="scratch C++ probe source")
    parser.add_argument("fixture", help="gateway-relative test whose link closure the probe uses")
    args = parser.parse_args()
    build = args.build.resolve()
    source = args.source.resolve()
    for path in (build, source):
        if path.is_relative_to(GATEWAY.parents[1]):
            parser.error("the probe and build directory must be outside the checkout")

    receipt = json.loads((build / "receipt.json").read_text())
    if receipt["config"] != "asan" or not receipt["inputs_unchanged"]:
        parser.error("the build must carry an unchanged sanitizer receipt")

    sources = driver.links.resolve(args.fixture, driver.links.read_registry()).sources
    runtime = sorted(str(path.relative_to(GATEWAY)) for path in (GATEWAY / "src/rt").glob("[!.]*.cpp"))
    objects = [str(driver.object_path(build, name)) for name in dict.fromkeys(sources + runtime)]
    archives = [str(build / name) for name in receipt["vendor_archives"]]
    environment = driver.clean_environment()

    with tempfile.TemporaryDirectory(prefix="lifetime-probe-", dir=source.parent) as directory:
        scratch = Path(directory)
        binaries = scratch / "bin"
        binaries.mkdir()
        (scratch / "home").mkdir()
        (scratch / "tmp").mkdir()
        binary = binaries / source.stem
        command = receipt["compiler"] + receipt["flags"] + [str(source)] + objects + archives + receipt["libraries"] + ["-o", str(binary)]
        compiler_environment = dict(environment, HOME=str(scratch / "home"), TMPDIR=str(scratch / "tmp"))
        log_base = source.parent / source.stem
        compiled, _, seconds = driver.execute(
            command, log_base.with_suffix(".compile.log"), cwd=scratch, environment=compiler_environment, timeout=90
        )
        print(f"Compile exit {compiled} in {seconds:.3f} s", flush=True)
        if compiled:
            return 2

        boundary = driver.find_boundary(scratch, environment)
        fixture_environment = dict(environment, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        with tempfile.TemporaryDirectory(prefix="uos-ci-", dir="/tmp") as isolated:
            case = Path(isolated)
            for name in ("h", "t", "w"):
                (case / name).mkdir()
            command = driver.boundary_command(boundary, case, scratch, binary.name, fixture_environment)
            code, expired, seconds = driver.execute(
                command, log_base.with_suffix(".isolated.log"), cwd=scratch, environment=environment, timeout=20
            )
        print(f"Run exit {code} in {seconds:.3f} s", flush=True)
        if expired:
            return 124
        return code if code >= 0 else 128 - code


if __name__ == "__main__":
    sys.exit(main())
