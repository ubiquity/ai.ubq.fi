"""Compiler launcher settings shared by the disposable gateway builds."""
from collections import Counter
import os
from pathlib import Path
import shlex
import shutil


def launcher():
    requested = os.environ.get("UOS_CI_COMPILER_LAUNCHER")
    if requested is None:
        ccache = shutil.which("ccache")
        return [os.path.abspath(ccache)] if ccache else []
    command = shlex.split(requested)
    if command:
        executable = shutil.which(command[0])
        if not executable:
            raise ValueError(f"UOS_CI_COMPILER_LAUNCHER executable not found: {command[0]}")
        command[0] = os.path.abspath(executable)
    return command


def settings():
    env = {name: os.environ[name] for name in ("CCACHE_DIR", "CCACHE_MAXSIZE") if name in os.environ}
    if env.get("CCACHE_DIR"):
        env["CCACHE_DIR"] = str(Path(env["CCACHE_DIR"]).resolve())
    return env


def environment(env, root, config):
    return dict(env, **settings(), CCACHE_BASEDIR=str(root), CCACHE_NOHASHDIR="1",
                CCACHE_COMPILERCHECK="content", CCACHE_NAMESPACE="uos-gateway-" + config,
                CCACHE_SLOPPINESS="", CCACHE_IGNOREOPTIONS="", CCACHE_IGNOREHEADERS="")


def is_ccache(command):
    return bool(command and Path(command[0]).name == "ccache")


def stats(log):
    if not log.is_file():
        return None
    # Each compiler call appends a source line and its counter IDs. The log
    # format also works with ccache releases that lack --print-log-stats.
    counters = Counter(log.read_text().splitlines())
    direct = counters["direct_cache_hit"]
    preprocessed = counters["preprocessed_cache_hit"]
    misses = counters["cache_miss"]
    calls = sum(count for line, count in counters.items() if line.startswith("# "))
    return dict(hits=direct + preprocessed, misses=misses, direct_hits=direct,
                preprocessed_hits=preprocessed, uncacheable=calls - direct - preprocessed - misses)
