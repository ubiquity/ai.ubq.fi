#!/usr/bin/env python3
"""Run the existing package producer with the benchmark host's ARM64 multiarch directory."""

import argparse
from pathlib import Path
import runpy
import sys
import sysconfig


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    parser.add_argument("--source", type=Path, required=True)
    args, package_args = parser.parse_known_args()
    package = args.source.resolve(strict=True) / "cpp/gateway/tools/package.py"
    sys.argv = [str(package), *package_args]
    sys.path.insert(0, str(package.parent))
    packager = runpy.run_path(str(package))["main"]
    runtime = packager.__globals__["runtime"]
    multiarch = sysconfig.get_config_var("MULTIARCH")
    if multiarch == "aarch64-linux-gnu":
        runtime.LIBRARY_ROOTS += tuple(root / multiarch for root in runtime.LIBRARY_ROOTS if (root / multiarch).is_dir())
    packager()
