#!/usr/bin/python3
"""Pinned native profile identities and task-owned executable/cache views."""
import json
import os
from pathlib import Path
import shlex

from build_support import GATEWAY, WORKSPACE, atomic_json, digest

TOOL_ROOT = Path("/home/codex/toolchains/uos-cpp-2026.11")
PROFILES = ("release", "clang-release", "asan-ubsan-clang", "asan-ubsan-gcc", "tsan-threaded", "fuzz-clang", "clang-libcxx")


def executable(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        temporary = path.with_name(path.name + ".tmp")
        temporary.write_text(text)
        temporary.chmod(0o755)
        temporary.replace(path)


def native(profile, cache_enabled, lane=None):
    if profile not in PROFILES or not (TOOL_ROOT / "READY").is_file():
        raise RuntimeError("Qualified native profile/toolchain READY is unavailable")
    clang = profile not in ("release", "asan-ubsan-gcc")
    compiler = TOOL_ROOT / "bin/clang++" if clang else Path("/usr/bin/g++")
    c_compiler = TOOL_ROOT / "bin/clang" if clang else Path("/usr/bin/gcc")
    name = profile + ("-" + lane if lane else "")
    build = WORKSPACE / (".data/cpp-gateway-native-" + name)
    view = WORKSPACE / ".data/cpp-gateway-tool-view" / name
    temporary = WORKSPACE / ".data/cpp-gateway-tmp" / name
    evidence = WORKSPACE / ".data/cpp-gateway-evidence" / name
    cache = WORKSPACE / ".data/cpp-gateway-ccache" / profile
    sanitizer = "address,undefined" if "asan-ubsan" in profile or profile == "fuzz-clang" else "thread" if profile == "tsan-threaded" else None
    libraries = [Path("/usr/lib") / name for name in ("libboost_json.a", "libboost_container.a", "libssl.so", "libcrypto.so",
                 "libsqlite3.so", "libz.so", "libbrotlidec.so", "libzstd.so", "libicuuc.so", "libicui18n.so")]
    tools = [compiler, c_compiler, TOOL_ROOT / "bin/cmake", TOOL_ROOT / "bin/ninja", TOOL_ROOT / "bin/mold", TOOL_ROOT / "bin/ccache",
             Path("/usr/bin/ar"), Path("/usr/bin/ranlib"), TOOL_ROOT / "boost-1.92.0/boost/version.hpp"] + libraries
    for path in tools:
        if not path.is_file():
            raise RuntimeError("Missing installed native artifact: " + str(path))
    identity = {"schema": "uos-gateway-profile-v1", "source_root": str(GATEWAY), "workspace_root": str(WORKSPACE), "profile": profile,
                "compiler": str(compiler), "compiler_sha256": digest(compiler), "sanitizer": sanitizer,
                "stdlib": "libc++" if profile == "clang-libcxx" else "libstdc++", "runtime_prefix": [],
                "toolchain_manifest_sha256": digest(TOOL_ROOT / "SHA256SUMS"),
                "artifacts_sha256": {str(path): digest(path) for path in tools}}
    if lane:
        identity["lane"] = lane
    owner = build / "profile-owner.json"
    if owner.exists():
        if json.loads(owner.read_text()) != identity:
            raise RuntimeError("Native build profile identity changed; preserve the existing owned artifacts: " + str(build))
    elif build.exists() and any(build.iterdir()):
        raise RuntimeError("Refusing nonempty native output without matching profile ownership: " + str(build))
    else:
        build.mkdir(parents=True, exist_ok=True)
        atomic_json(owner, identity)
    for path in (temporary / "h", temporary / "t", evidence, cache):
        path.mkdir(parents=True, exist_ok=True)
    for tool in ("cmake", "ctest", "cpack", "ninja"):
        seal = 'UOS_GATEWAY_BUILD_DIR=' + shlex.quote(str(build)) + '\nexport UOS_GATEWAY_BUILD_DIR\n'
        executable(view / "bin" / tool, '#!/bin/sh\nset -eu\n' + seal + 'exec ' + shlex.quote(str(TOOL_ROOT / "bin" / tool)) + ' "$@"\n')
    for tool in ("ar", "ranlib"):
        command = ["/usr/bin/python3", str(GATEWAY / "tools/compiler-limit.py"), "--archive", "/usr/bin/" + tool]
        executable(view / "bin" / ("uos-" + tool), '#!/bin/sh\nset -eu\nexec ' + shlex.join(command) + ' "$@"\n')
    for wrapper, driver in (("uos-cc", c_compiler), ("uos-cxx", compiler)):
        command = ["/usr/bin/python3", str(GATEWAY / "tools/compiler-limit.py"), str(driver)]
        executable(view / "bin" / wrapper, '#!/bin/sh\nset -eu\nexec ' + shlex.join(command) + ' "$@"\n')
    config = cache / "ccache.conf"
    (cache / "tmp").mkdir(exist_ok=True)
    cache_config = "cache_dir = " + str(cache) + "\ntemporary_dir = " + str(cache / "tmp") + \
                   "\nmax_size = 512M\ncompiler_check = content\ncompression = true\nnamespace = " + identity["compiler_sha256"] + "-" + profile + "\n"
    if not config.exists() or config.read_text() != cache_config:
        candidate = config.with_suffix(".tmp")
        candidate.write_text(cache_config)
        candidate.replace(config)
    env = {"PATH": str(TOOL_ROOT / "bin") + ":/usr/bin:/bin", "HOME": str(temporary / "h"), "TMPDIR": str(temporary / "t"),
           "TZ": "UTC", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8", "UOS_GATEWAY_TOOL_ROOT": str(TOOL_ROOT),
           "UOS_GATEWAY_TOOL_VIEW": str(view), "UOS_GATEWAY_BUILD_PROFILE": profile, "UOS_GATEWAY_BUILD_EVIDENCE": str(evidence),
           "UOS_GATEWAY_BUILD_DIR": str(build),
           "PYTHONPYCACHEPREFIX": str(WORKSPACE / ".data/cpp-gateway-pycache"), "CCACHE_DIR": str(cache),
           "CCACHE_CONFIGPATH": str(config), "CCACHE_TEMPDIR": str(temporary / "t"),
           "UOS_GATEWAY_CCACHE": str(TOOL_ROOT / "bin/ccache") if cache_enabled else ""}
    return {"identity": identity, "build": build, "view": view, "temporary": temporary, "evidence": evidence, "cache": cache, "env": env}
