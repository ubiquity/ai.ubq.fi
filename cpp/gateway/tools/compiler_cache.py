"""Compiler launcher settings shared by the disposable gateway builds."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess


def launcher(*, path=None):
    requested = os.environ.get("UOS_CI_COMPILER_LAUNCHER")
    if requested is None:
        ccache = shutil.which("ccache", path=path)
        return [os.path.abspath(ccache)] if ccache else []
    command = shlex.split(requested)
    if command:
        executable = shutil.which(command[0], path=path)
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


def stats(command, env, log):
    if not is_ccache(command) or not log.is_file():
        return None
    output = subprocess.check_output([command[0], "--print-log-stats"], env=env, text=True)
    counters = dict(line.split("\t", 1) for line in output.splitlines())
    direct = int(counters["direct_cache_hit"])
    preprocessed = int(counters["preprocessed_cache_hit"])
    misses = int(counters["cache_miss"])
    calls = sum(line.startswith("# ") for line in log.read_text().splitlines())
    return dict(hits=direct + preprocessed, misses=misses, direct_hits=direct,
                preprocessed_hits=preprocessed, uncacheable=calls - direct - preprocessed - misses)
