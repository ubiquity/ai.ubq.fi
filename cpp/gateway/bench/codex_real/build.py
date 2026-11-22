#!/usr/bin/env python3
"""Link the benchmark gateway and state writer from a successful CI Release build."""

import argparse
import fcntl
from io import BytesIO
import json
import os
from pathlib import Path
import runpy
import shlex
import subprocess
import sys
import tarfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
GATEWAY = HERE.parents[1]
ROOT = GATEWAY.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    if out.is_relative_to(ROOT):
        parser.error("Output must be outside the checkout")
    out.mkdir(mode=0o700)
    build = args.receipt.resolve(strict=True).parent
    lock = (build / "run.lock").open("a")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    receipt = json.loads(args.receipt.read_text())
    if (receipt["config"] != "release" or receipt["compile_exit"] or not receipt["inputs_unchanged"]
            or any(receipt["statuses"][name] for name in ("FAIL", "BUILD_FAIL", "TIMEOUT"))):
        raise RuntimeError("Expected a successful Release build receipt")
    sha = receipt["git_commit"]
    source = out / "source"
    source.mkdir()
    archive = subprocess.check_output(["git", "archive", sha], cwd=ROOT)
    with tarfile.open(fileobj=BytesIO(archive)) as files:
        files.extractall(source, filter="data")
    (source / ".data").mkdir()
    snapshot = source / "cpp/gateway"
    driver = runpy.run_path(str(snapshot / "cmake/ci/driver.py"))
    selected = driver["vendor_sources"].selected(snapshot)
    vendors = [library for library in selected if library.archive in receipt["vendor_archives"]]
    if driver["input_digest"](vendors) != receipt["source_sha256"]:
        raise RuntimeError("The Release inputs differ from the archived commit")
    production = driver["cpp_inventory"]("src", recursive=True)
    objects = [build / "obj" / (path + ".o") for path in production]
    if len(objects) != receipt["production_sources"]:
        raise RuntimeError("Production object inventory differs from the Release receipt")
    bins = out / "binaries"
    bins.mkdir()
    env = {name: os.environ[name] for name in ("PATH", "LC_ALL") if name in os.environ}
    commands = []

    def run(argv):
        commands.append(shlex.join(map(str, argv)))
        subprocess.run(argv, cwd=ROOT, env=env, check=True, timeout=90)

    def compile(path, *flags):
        obj = bins / (path.stem + ".o")
        run([*receipt["compiler"], *receipt["flags"], *flags, "-c", path, "-o", obj])
        return obj

    libraries = receipt["libraries"]
    # The package uses its explicit verified system closure. sqlite.sh projects
    # the pinned suite library to the canonical distribution location.
    rpath = libraries.index("-Wl,--disable-new-dtags")
    package_libs = ["-lsqlite3" if item.endswith("/libsqlite3.so") else item
                    for item in libraries[:rpath] + libraries[rpath + 5:]]
    archives = [build / archive for archive in sorted(receipt["vendor_archives"])]
    startup = compile(snapshot / "runtime/startup.cpp")
    entry = compile(snapshot / "entrypoints/gateway.cpp")
    release = compile(snapshot / "src/release.cpp", f'-DUOS_PACKAGE_GIT_COMMIT="{sha}"')
    app = [obj for obj in objects if obj.name != "release.cpp.o"]
    run([*receipt["compiler"], "-pthread", startup, entry, release, *app, *archives,
         *package_libs, "-o", bins / "uos_gateway"])
    fixture = compile(HERE / "seed.cpp")
    run([*receipt["compiler"], "-pthread", fixture, *objects, *archives,
         *package_libs, "-o", out / "seed-state"])
    profile = bins / "profile-owner.json"
    profile.write_text(json.dumps({
        "schema": "uos-gateway-profile-v1", "profile": "clang-release" if "clang" in receipt["compiler_version"] else "release",
        "sanitizer": None, "source_root": str(snapshot), "workspace_root": str(source),
        "compiler": receipt["compiler_version"].splitlines()[0], "flags": receipt["flags"],
        "git_commit": sha, "git_tree": receipt["git_tree"],
    }, indent=2) + "\n")
    run([sys.executable, "-B", HERE.parent / "request_path/package.py", "--source", source,
         "--build-dir", bins, "--profile-manifest", profile, "--output", source / ".data/package"])
    (source / ".data/package").rename(out / "package")
    (out / "build-commands.txt").write_text("\n".join(commands) + "\n")
    print(out / "package")


if __name__ == "__main__":
    main()
