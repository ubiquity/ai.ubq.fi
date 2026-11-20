#!/usr/bin/env python3
"""Build, test and package the gateway on Apple Silicon."""

import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

GATEWAY = Path(__file__).resolve().parents[2]
ROOT = GATEWAY.parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(GATEWAY / "tools"))
import compiler_cache
import vendor_sources

SQLITE_VERSION = "3.54.0"
SQLITE_SOURCE = "sqlite-amalgamation-3540000"
SQLITE_SHA256 = "68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d"
LIBRARIES = {
    "boost": ("libboost_json.a", "libboost_container.a"),
    "openssl@3": ("libssl.a", "libcrypto.a"),
    "icu4c@78": ("libicui18n.a", "libicuuc.a", "libicudata.a"),
    "brotli": ("libbrotlidec.a", "libbrotlicommon.a"),
    "zstd": ("libzstd.a",),
    "zlib": ("libz.a",),
}
EXECUTABLES = ("uos-gateway",)


def capture(*command, env=None):
    return subprocess.check_output(command, cwd=ROOT, env=env, text=True).strip()


def run(*command, env, cwd=ROOT):
    subprocess.run(command, cwd=cwd, env=env, check=True)


def source_identity(env):
    if capture("git", "status", "--porcelain", env=env):
        raise ValueError("A committed, clean source checkout is required")
    return capture("git", "show", "-s", "--format=%H%n%T%n%ct", "HEAD", env=env).splitlines()


def prepare_sqlite(work, env, launcher, build_env):
    archive = work / "sqlite.zip"
    run("curl", "-fsSL", "--retry", "3", "--max-time", "120",
        f"https://www.sqlite.org/2026/{SQLITE_SOURCE}.zip", "-o", archive, env=env)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SQLITE_SHA256:
        raise ValueError("SQLite archive checksum mismatch")
    prefix = work / "sqlite"
    (prefix / "include").mkdir(parents=True)
    (prefix / "lib").mkdir()
    source = work / "sqlite3.c"
    with zipfile.ZipFile(archive) as files:
        for target in (source, prefix / "include/sqlite3.h"):
            target.write_bytes(files.read(f"{SQLITE_SOURCE}/{target.name}"))
    obj = work / "sqlite3.o"
    cc = capture("xcrun", "--find", "clang", env=env)
    run(*launcher, cc, "-O2", "-g0", "-fPIC", f"-ffile-prefix-map={work}=.", "-DSQLITE_THREADSAFE=1",
        "-DSQLITE_DQS=3", "-DSQLITE_ENABLE_COLUMN_METADATA=1", "-c", source.name, "-o", obj, env=build_env, cwd=work)
    dylib = prefix / "lib/libsqlite3.dylib"
    run("xcrun", "clang", "-dynamiclib", "-Xlinker", "-install_name", "-Xlinker", dylib, obj, "-o", dylib, env=env)
    return prefix, obj


def copy_license_files(prefix, destination):
    names = ("LICEN[CS]E*", "COPYING*", "COPYRIGHT*", "NOTICE*")
    files = sorted({p for glob in names for p in prefix.glob(glob) if p.is_file()})
    if not files:
        raise ValueError(f"No installed license files in {prefix}")
    destination.mkdir(parents=True)
    for path in files:
        shutil.copyfile(path, destination / path.name)


def archive_tree(stage, output, epoch):
    with output.open("xb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w|", format=tarfile.USTAR_FORMAT) as tar:
            for source in sorted(stage.rglob("*"), key=lambda path: path.relative_to(stage).as_posix()):
                entry = tar.gettarinfo(str(source), arcname=f"./{source.relative_to(stage).as_posix()}")
                entry.uid = entry.gid = 0
                entry.uname = entry.gname = ""
                entry.mtime = epoch
                if entry.isdir():
                    entry.mode = 0o755
                    tar.addfile(entry)
                elif entry.isfile():
                    entry.mode = 0o755 if entry.mode & 0o111 else 0o644
                    with source.open("rb") as data:
                        tar.addfile(entry, data)
                else:
                    raise ValueError(f"Unexpected archive entry: {source}")


def build(work, output, jobs):
    (work / "home").mkdir()
    (work / "tmp").mkdir()
    env = {"PATH": "/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin", "LC_ALL": "C", "TZ": "UTC",
           "HOME": str(work / "home"), "TMPDIR": str(work / "tmp"),
           "HOMEBREW_NO_AUTO_UPDATE": "1", "HOMEBREW_NO_ANALYTICS": "1"}
    commit, tree, epoch = source_identity(env)
    launcher = compiler_cache.launcher()
    build_env = compiler_cache.environment(env, ROOT, "macos") if launcher else env.copy()
    if compiler_cache.is_ccache(launcher) and not build_env.get("CCACHE_DIR"):
        build_env["CCACHE_DIR"] = str(Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "ccache")
    build_env["SDKROOT"] = capture("xcrun", "--show-sdk-path", env=env)
    sqlite, sqlite_obj = prepare_sqlite(work, env, launcher, build_env)
    build_root = work / "build"
    run("/bin/bash", GATEWAY / "tools/macos-build.sh", "--build-root", build_root, "--sqlite-prefix", sqlite,
        "--jobs", str(jobs), "--suite", "rt", "--require-clean", "--release-commit", commit,
        env=dict(build_env, UOS_CI_COMPILER_LAUNCHER=shlex.join(launcher)))
    (receipt,) = (build_root / "runs").glob("*/provenance.json")
    profile = json.loads(receipt.read_text())
    identity = profile["source_identity"]
    if (identity["commit"], identity["tree"]) != (commit, tree) or not (profile["inputs_unchanged"] and profile["identity_unchanged"]):
        raise ValueError("macOS test receipt does not match the source inputs")
    vendors = tuple(vendor_sources.selected())
    if profile["vendor_archives"].keys() != {library.archive for library in vendors}:
        raise ValueError("macOS build receipt does not cover every registered vendor archive")

    stage = work / "stage"
    (stage / "bin").mkdir(parents=True)
    licenses = stage / "licenses"
    static_libraries = [sqlite_obj]
    versions = {"SQLite": SQLITE_VERSION}
    for formula, archives in LIBRARIES.items():
        prefix = Path(capture("brew", "--prefix", formula, env=env)).resolve(strict=True)
        versions[formula] = prefix.name
        static_libraries.extend(prefix / "lib" / archive for archive in archives)
        if formula != "boost":
            copy_license_files(prefix, licenses / formula)
    for library in ("boost", "compiler-rt"):
        destination = licenses / library
        destination.mkdir(parents=True)
        shutil.copyfile(GATEWAY / "packaging/macos" / f"{library}.LICENSE", destination / "LICENSE")
    for library in vendors:
        directory = GATEWAY / "third_party" / library.name
        versions[library.name] = library.version
        static_libraries.append(build_root / library.archive)
        destination = licenses / library.name
        copy_license_files(directory, destination)
        notice = GATEWAY / "docs/licenses" / library.name / "NOTICE"
        if library.name in ("simdjson", "simdutf") or notice.is_file():
            shutil.copyfile(notice, destination / "NOTICE")
    for library in ("sqlite", "denokv"):
        (licenses / library).mkdir()
        shutil.copyfile(GATEWAY / "packaging" / f"{library.upper()}-LICENSE", licenses / library / "LICENSE")

    env["SDKROOT"] = profile["sdk"]
    build_env["SDKROOT"] = profile["sdk"]
    flags = [*profile["flags"], f"-ffile-prefix-map={work}=."]
    compile_flags = [*launcher, *flags, f"-fdebug-prefix-map={ROOT}=."]
    minimum_os = {}
    for name in EXECUTABLES:
        source = GATEWAY / "entrypoints" / (name.removeprefix("uos-") + ".cpp")
        entry = work / (name + ".o")
        objects = [entry]
        definitions = []
        if name == "uos-gateway":
            gateway_entry = work / "gateway.o"
            run(*compile_flags, "-Dmain=uosGatewayMain", "-c", source, "-o", gateway_entry, env=build_env)
            source = GATEWAY / "packaging/macos/gateway-version.cpp"
            definitions = [f'-DUOS_PACKAGE_GIT_COMMIT="{commit}"']
            objects.append(gateway_entry)
        run(*compile_flags, *definitions, "-c", source, "-o", entry, env=build_env)
        startup = work / "startup.o"
        run(*compile_flags, "-c", GATEWAY / "runtime/startup.cpp", "-o", startup, env=build_env)
        target = stage / "bin" / name
        run(*flags, *objects, startup,
            "-Xlinker", "-force_load", "-Xlinker", build_root / "libuos_gateway.a", *static_libraries,
            "-Wl,-reproducible", "-Wl,-adhoc_codesign", "-o", target, env=env)
        if capture("xcrun", "lipo", "-archs", target, env=env) != "arm64":
            raise ValueError(f"Unsupported Mach-O architecture for {name}")
        linked = capture("xcrun", "otool", "-L", target, env=env).splitlines()[1:]
        if not linked or any(not line.lstrip().startswith(("/usr/lib/", "/System/Library/")) for line in linked):
            raise ValueError(f"{name} has a non-system dylib dependency: {linked}")
        minimum_os[name], = (line.split()[1] for line in capture("xcrun", "vtool", "-show-build", target, env=env).splitlines()
                             if line.split()[:1] == ["minos"])
        run("/usr/bin/codesign", "--verify", "--strict", target, env=env)
        reported = capture(str(target), "--version", env=env)
        if reported != f"uos-gateway {commit}":
            raise ValueError(f"{name} does not report the packaged Git commit: {reported}")

    assets = capture("git", "ls-files", "-z", "--", "static/", "favicon.png", "favicon-32.png", env=env)
    for relative in assets.rstrip("\0").split("\0"):
        source = ROOT / relative
        if source.is_symlink() or not source.is_file():
            raise ValueError(f"Invalid static asset: {source}")
        target = stage / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    shutil.copyfile(GATEWAY / "packaging/macos/INSTALL.md", stage / "INSTALL.md")
    version = {"gitCommit": commit, "gitTree": tree, "target": "arm64-apple-darwin", "minimumMacOS": minimum_os,
               "buildDate": datetime.fromtimestamp(int(epoch), timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
               "compiler": profile["compiler"], "sdk": capture("xcrun", "--show-sdk-version", env=env), "static": versions}
    (stage / "VERSION").write_text(json.dumps(version, sort_keys=True, indent=2) + "\n")
    if source_identity(env) != [commit, tree, epoch]:
        raise ValueError("Source identity changed during packaging")
    with tempfile.TemporaryDirectory(prefix=".uos-gateway.", dir=output.parent) as scratch:
        archive = Path(scratch) / output.name
        archive_tree(stage, archive, int(epoch))
        os.link(archive, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path, help="New archive path outside the source checkout")
    parser.add_argument("--jobs", type=int, default=3)
    args = parser.parse_args()
    if (platform.system(), platform.machine()) != ("Darwin", "arm64") or args.jobs < 1:
        parser.error("Requires Apple Silicon macOS and a positive job count")
    output = args.archive.parent.resolve(strict=True) / args.archive.name
    scratch = Path(tempfile.gettempdir()).resolve()
    if output.exists() or output.is_symlink() or any(p.is_relative_to(ROOT) for p in (output, scratch)):
        parser.error("Use a new archive path and a temporary directory outside the source checkout")
    with tempfile.TemporaryDirectory(prefix="uos-gateway.", dir=scratch) as work:
        build(Path(work), output, args.jobs)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f"macOS package failed: {error}")
