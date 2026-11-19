#!/usr/bin/env python3
"""Build the real Linux ARM64 gateway and packaging entrypoints in a scratch Ninja directory."""

import argparse
import json
import os
from pathlib import Path
import platform
import re
import runpy
import shlex
import subprocess
import sys


def ninja_path(path):
    return str(path).replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def ninja_args(args):
    return shlex.join(map(str, args)).replace("$", "$$")


def output(command):
    return subprocess.check_output(command, text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="Root of a scratch source snapshot")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--boost", type=Path, required=True, help="Boost source root with staged static libraries")
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--ar", default="ar")
    parser.add_argument("--linker", default="mold", help="Linker for -fuse-ld, or 'default'")
    parser.add_argument("--jobs", type=int, default=len(os.sched_getaffinity(0)))
    args = parser.parse_args()
    if (platform.system(), platform.machine()) != ("Linux", "aarch64") or args.jobs < 1:
        parser.error("The packaged fixture requires Linux ARM64 and at least one build job")
    root, out, boost = (item.resolve() for item in (args.source, args.out, args.boost))
    checkout = Path(__file__).resolve().parents[4]
    if out.is_relative_to(checkout):
        parser.error("Build output must be outside the benchmark checkout")
    out.mkdir(parents=True, exist_ok=True)
    gateway = root / "cpp/gateway"
    boost_archives = [boost / "stage/lib" / f"libboost_{name}.a" for name in ("json", "container")]
    if '#define BOOST_LIB_VERSION "1_92"' not in (boost / "boost/version.hpp").read_text():
        parser.error("Expected Boost 1.92 headers")
    inventory = out / "sources.cmake"
    counts = json.loads(output([sys.executable, "-I", gateway / "tools/cmake-sources.py", inventory]))
    sources = re.findall(r'^  "([^"]+)"$', inventory.read_text().split("set(UOS_RT_SOURCES")[0], re.M)
    if len(sources) != counts["source_count"]:
        raise ValueError("Unexpected gateway source inventory")
    tools = runpy.run_path(str(gateway / "tools/vendor_sources.py"))
    vendors = tools["required"](tools["selected"](gateway), sources)
    simdjson = tools["simdjson_consumers"](vendors)

    version = output([args.cxx, "--version"]).splitlines()[0]
    c_version = output([args.cc, "--version"]).splitlines()[0]
    options = ["-O2", "-g1", "-DNDEBUG", "-fno-omit-frame-pointer", "-mno-omit-leaf-frame-pointer"]
    warnings = ["-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror"]
    definitions = ["BOOST_JSON_NO_LIB", "BOOST_CONTAINER_NO_LIB", "BOOST_SYSTEM_NO_LIB",
                   "BOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING", "UOS_GATEWAY_NATIVE_LIBRARIES=1"]
    flags = [*warnings, *["-D" + item for item in definitions], "-I", gateway / "include", "-I", gateway,
             "-isystem", boost]
    linker = [] if args.linker == "default" else ["-fuse-ld=" + args.linker]
    libraries = ["ssl", "crypto", "sqlite3", "z", "brotlidec", "zstd", "icuuc", "icui18n", "dl"]
    lines = [
        "rule cxx",
        "  command = " + ninja_args([args.cxx, "-std=c++20", "-pthread", *options]) + " $flags -MMD -MF $out.d -c $in -o $out",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = CXX $in",
        "rule cc",
        "  command = " + ninja_args([args.cc, "-std=c11", *options]) + " $flags -MMD -MF $out.d -c $in -o $out",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = CC $in",
        "rule archive",
        "  command = rm -f $out && " + ninja_args([args.ar, "rcs"]) + " $out $in",
        "rule link",
        "  command = " + ninja_args([args.cxx, *linker, "-pthread"]) + " -o $out $in " + ninja_args(["-l" + lib for lib in libraries]),
        "  description = LINK $out",
        "  pool = link_pool",
        "pool link_pool",
        "  depth = 1",
    ]

    def object_file(source, source_flags):
        path = Path("obj") / (source + ".o")
        (out / path.parent).mkdir(parents=True, exist_ok=True)
        rule = "cc" if source.endswith(".c") else "cxx"
        lines.append(f"build {ninja_path(path)}: {rule} {ninja_path(gateway / source)}")
        if source_flags:
            lines.append("  flags = " + ninja_args(source_flags))
        return path

    include_flags = tools.get("include_flags", lambda _library, _gateway: [])
    c_dialect_flags = tools.get("c_dialect_flags", lambda _source: [])
    app = [object_file(source, flags + (tools["simdjson_flags"](gateway) if source in simdjson else []) +
                       [flag for lib in tools["required"](vendors, [source]) for flag in include_flags(lib, gateway)])
           for source in sources]
    archives = []
    for library in vendors:
        # Benchmark snapshots predating multi-file vendors still use `source`.
        members = library.sources if hasattr(library, "sources") else (library.source,)
        objects = [ninja_path(object_file(source, include_flags(library, gateway) + c_dialect_flags(source)))
                   for source in members]
        lines.append(f"build {library.archive}: archive " + " ".join(objects))
        archives.append(library.archive)
    startup = object_file("runtime/startup.cpp", flags)
    targets = []
    for name in ("gateway",):
        entry = object_file(f"entrypoints/{name}.cpp", flags)
        binary = "uos_" + name.replace("-", "_")
        # Explicit object linkage retains every module registrar, as does the CMake whole-archive link.
        lines.append(f"build {binary}: link " + " ".join(map(ninja_path, [entry, startup, *app, *archives, *boost_archives])))
        targets.append(binary)
    lines.append("default " + " ".join(targets))
    (out / "build.ninja").write_text("\n".join(lines) + "\n")
    (out / "profile-owner.json").write_text(json.dumps({
        "schema": "uos-gateway-profile-v1", "profile": "clang-release" if "clang" in version else "release",
        "sanitizer": None, "source_root": str(gateway), "workspace_root": str(root), "compiler": version,
        "c_compiler": c_version,
        "flags": list(map(str, [*options, *flags])), "entrypoints": targets, "inventory": counts,
    }, indent=2) + "\n")
    print(f"Building {len(sources)} production sources plus {len(vendors)} libraries, startup and entrypoints", flush=True)
    subprocess.run(["ninja", "-C", out, "-j", str(args.jobs)], check=True)


if __name__ == "__main__":
    main()
