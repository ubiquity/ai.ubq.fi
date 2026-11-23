#!/usr/bin/env python3
"""Exercise the actual compiler/linker, result parsing and OS test boundary."""
import concurrent.futures
import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("ci_driver", Path(__file__).with_name("driver.py"))
driver = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(driver)


class DriverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="uos-driver-tests-", dir="/tmp")
        cls.build = Path(cls.temporary.name)
        (cls.build / "bin").mkdir()
        cls.compiler = shlex.split(os.environ.get("CXX", "g++"))
        cls.compiler[0] = shutil.which(cls.compiler[0])
        cls.flags = driver.CONFIGS[os.environ.get("UOS_CI_SELFTEST_CONFIG", "debug")]
        cls.environment = driver.clean_environment()
        cls.boundary = driver.find_boundary(cls.build, cls.environment)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def fixture(self, name, body, *, timeout=5):
        selected = SimpleNamespace(test="tests/" + name + "_test.cpp", sources=[], env=[])
        target = driver.object_path(self.build, selected.test)
        target.parent.mkdir(parents=True, exist_ok=True)
        code = '#include <cstdio>\n#include <cstdlib>\n#include <filesystem>\n#include <cstring>\n#include <unistd.h>\nint main() {\n' + body + '\n}\n'
        subprocess.run(self.compiler + self.flags + ["-std=c++20", "-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror", "-x", "c++", "-c", "-o", str(target), "-"],
                       input=code, text=True, check=True, capture_output=True)
        return driver.run_test(selected, [], self.build, self.compiler, self.flags, [], self.environment, timeout, self.boundary)

    def test_fresh_environment_parallel_and_no_credentials(self):
        body = r'''
        if (std::strcmp(std::getenv("HOME"), "/tmp/h") || std::strcmp(std::getenv("TMPDIR"), "/tmp/t")) return 2;
        if (!std::filesystem::is_empty("/tmp/h") || !std::filesystem::is_empty("/tmp/t")) return 3;
        if (std::getenv("OPENAI_API_KEY") || std::getenv("UNRECOGNIZED_PROVIDER_TOKEN") || std::getenv("CODEX_HOME")) return 4;
        if (std::filesystem::current_path() != "/tmp/w") return 5;
        std::puts("2 run, 2 passed, 0 failed"); return 0;
        '''
        old = os.environ.get("UNRECOGNIZED_PROVIDER_TOKEN")
        os.environ["UNRECOGNIZED_PROVIDER_TOKEN"] = "synthetic-control"
        try:
            self.environment = driver.clean_environment()
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                rows = list(pool.map(lambda name: self.fixture(name, body), ["isolation_a", "isolation_b"]))
            for row in rows:
                self.assertEqual((row["status"], row["run"], row["passed"], row["failed"]), ("PASS", 2, 2, 0))
        finally:
            if old is None:
                os.environ.pop("UNRECOGNIZED_PROVIDER_TOKEN")
            else:
                os.environ["UNRECOGNIZED_PROVIDER_TOKEN"] = old

    def test_failure_counts(self):
        row = self.fixture("failure", 'std::puts("3 run, 2 passed, 1 failed"); return 1;')
        self.assertEqual((row["status"], row["run"], row["passed"], row["failed"]), ("FAIL", 3, 2, 1))

    def test_no_summary_is_failure(self):
        row = self.fixture("no_summary", 'std::puts("no harness summary"); return 0;')
        self.assertEqual((row["status"], row["run"]), ("FAIL", 0))

    def test_timeout_kills_namespace_descendants(self):
        row = self.fixture("timeout", 'if (::fork() == 0) { for (;;) ::pause(); } for (;;) ::pause();', timeout=1)
        self.assertEqual(row["status"], "TIMEOUT")

    def test_link_error_is_build_failure(self):
        row = self.fixture("bad_link", 'extern void missing(); missing(); return 0;')
        self.assertEqual((row["status"], row["run"]), ("BUILD_FAIL", 0))

    def test_shards_cover_selection_once(self):
        command = ["bash", str(driver.GATEWAY / "tools/ci-build.sh"), "--list"]
        all_tests = subprocess.check_output(command, text=True, cwd=self.build).splitlines()
        shards = [subprocess.check_output(command + ["--shard", str(index) + "/3"], text=True).splitlines()
                  for index in range(1, 4)]
        self.assertEqual(sorted(test for shard in shards for test in shard), all_tests)
        self.assertFalse(set(shards[0]) & set(shards[1]))
        self.assertEqual(subprocess.run(command + ["--shard", "0/2"], capture_output=True).returncode, 2)

    def test_inventory_excludes_unpublished_staging_sources(self):
        with tempfile.TemporaryDirectory(dir="/tmp", prefix="uos-inventory-") as directory:
            root = Path(directory)
            for name in ("src/main.cpp", "src/rt/ready.cpp", "src/rt/.unfinished.cpp", "src/.staging/hidden.cpp"):
                source = root / name
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text("// inventory fixture\n")
            with patch.object(driver, "GATEWAY", root):
                self.assertEqual(driver.cpp_inventory("src/rt"), ["src/rt/ready.cpp"])
                self.assertEqual(driver.cpp_inventory("src", recursive=True), ["src/main.cpp", "src/rt/ready.cpp"])


if __name__ == "__main__":
    unittest.main()
