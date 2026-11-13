#!/usr/bin/python3
"""Real engine controls and fail-closed wrapper checks; never execute C++ input."""
import importlib.util
import json
from pathlib import Path
import signal
import sys
import tempfile
import unittest
from unittest.mock import patch

WORKSPACE = Path(__file__).resolve().parents[3]
SDK = Path(sys.argv[1]).resolve()
SPEC = importlib.util.spec_from_file_location("pilot_analysis", WORKSPACE / "cpp/tools/analyze.py")
ANALYSIS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYSIS)


def write_policy(path, value):
    path.write_text("\n".join(key + ": " + json.dumps(scalar) for key, scalar in value.items()) + "\n")


class AnalysisChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = WORKSPACE / ".data/cpp-tmp/analysis-tests"
        temporary.mkdir(parents=True, exist_ok=True)
        cls.output = Path(tempfile.mkdtemp(prefix="controls-", dir=temporary))
        cls.config = WORKSPACE / "cpp/.clang-tidy"
        cls.build = WORKSPACE / ".data/cpp-build/pi-release"
        cls.database = cls.build / "compile_commands.json"
        cls.configuration = ANALYSIS.policy(cls.config)
        metadata = cls.output / "metadata"
        metadata.mkdir()
        cls.tool = ANALYSIS.Tool(SDK, WORKSPACE, metadata)
        cls.metadata = cls.tool.metadata(cls.configuration, cls.config)
        cls.catalog = ANALYSIS.check_names((metadata / "catalog.stdout").read_text())

    def control(self, name, source):
        output = self.output / name
        output.mkdir()
        source = Path(source)
        command = [str(SDK / "root/usr/bin/aarch64-linux-gnu-g++-12"), "--sysroot=" + str(SDK / "root"),
                   "-B" + str(SDK / "host-tools/bin") + "/", "-std=c++20", "-O2", "-g0", "-c", str(source),
                   "-o", str(output / "never-produced.o")]
        (output / "compile_commands.json").write_text(json.dumps([{"directory": str(output), "file": str(source), "arguments": command}]))
        result = ANALYSIS.Tool(SDK, WORKSPACE, output).analyze(source, output, self.config, name, 20)
        self.assertFalse((output / "never-produced.o").exists(), "Static controls must never compile or execute a program")
        return result

    def test_complete_actual_compilation_model(self):
        units = ANALYSIS.compilation_units(self.database, WORKSPACE, self.build, SDK)
        self.assertEqual(len(units), 16)
        self.assertIn(WORKSPACE / "cpp/tests/protocol_tests.cpp", units)
        self.assertEqual(sum(source.parent == WORKSPACE / "cpp/tests/analysis" for source in units), 3)

    def test_missing_artifacts(self):
        with self.assertRaises(FileNotFoundError):
            ANALYSIS.Tool(self.output / "missing-sdk", WORKSPACE, self.output)
        with self.assertRaises(FileNotFoundError):
            ANALYSIS.policy(self.output / "missing-policy")
        with self.assertRaises(FileNotFoundError):
            ANALYSIS.compilation_units(self.output / "missing-db", WORKSPACE, self.build, SDK)

    def test_invalid_policy_and_unknown_checker(self):
        path = self.output / "invalid-policy.json"
        value = dict(self.configuration, WarningsAsErrors="")
        write_policy(path, value)
        with self.assertRaises(ValueError):
            ANALYSIS.policy(path)
        unknown = dict(self.configuration, Checks="-*,clang-analyzer-cplusplus.NewDeleteLeaks,uos-not-an-installed-check")
        write_policy(path, unknown)
        output = self.output / "unknown-check"
        output.mkdir()
        tool = ANALYSIS.Tool(SDK, WORKSPACE, output)
        with self.assertRaisesRegex(ValueError, "Unsupported required"):
            tool.metadata(ANALYSIS.policy(path), path)
        self.assertEqual(json.loads((output / "catalog.json").read_text())["exit"], 0,
                         "A metadata command's success must not hide a misspelled required checker")

    def test_compilation_coverage_failures(self):
        original = json.loads(self.database.read_text())
        cases = {"missing": original[:-1], "duplicate": original[:-1] + [original[0]],
                 "foreign-directory": [dict(original[0], directory=str(self.output))] + original[1:],
                 "foreign-source": [dict(original[0], file=str(self.output / "foreign.cpp"))] + original[1:],
                 "wrong-standard": [dict(original[0], command=original[0]["command"].replace("-std=c++20", "-std=c++17"))] + original[1:]}
        for name, entries in cases.items():
            with self.subTest(name=name):
                path = self.output / (name + ".json")
                path.write_text(json.dumps(entries))
                with self.assertRaises(ValueError):
                    ANALYSIS.compilation_units(path, WORKSPACE, self.build, SDK)

    def test_three_real_controls_and_exact_findings(self):
        units = ANALYSIS.compilation_units(self.database, WORKSPACE, self.build, SDK)
        fixtures = [source for source in units if source.parent == WORKSPACE / "cpp/tests/analysis"]
        results = ANALYSIS.controls(self.tool, fixtures, self.build, self.config)
        self.assertEqual(len(results), 3)
        for result in results:
            self.assertEqual(Path(result["consumer_profile"]["file"]).resolve(), Path(result["source"]).resolve())
            if Path(result["source"]).name != "raw_defects.cpp":
                self.assertEqual(result["exit"], 0)
                self.assertEqual(result["diagnostic_ids"], [])
                self.assertIsNone(result["report"])
                self.assertEqual(result["report_status"], "not-emitted-for-clean-input")

    def test_missing_malformed_and_wrong_source_profiles(self):
        directory = self.output / "invalid-profiles"
        directory.mkdir()
        source = WORKSPACE / "cpp/tests/protocol_tests.cpp"
        with self.assertRaises(ValueError):
            ANALYSIS.consumer_profile(directory, source)
        path = directory / "bad.json"
        path.write_text("not-json")
        with self.assertRaises(ValueError):
            ANALYSIS.consumer_profile(directory, source)
        path.write_text(json.dumps({"file": str(self.output / "foreign.cpp"), "timestamp": "test", "profile": {}}))
        with self.assertRaises(ValueError):
            ANALYSIS.consumer_profile(directory, source)

    def test_frontend_failure_is_not_a_finding_pass(self):
        source = self.output / "invalid.cpp"
        source.write_text("int invalid = ;\n")
        result = self.control("parse-error", source)
        self.assertNotEqual(result["exit"], 0)
        self.assertFalse(result["frontend_success"])
        self.assertIn("clang-diagnostic-error", result["diagnostic_ids"])

    def test_timeout_and_crash_preserve_failure(self):
        with self.assertRaises(TimeoutError):
            ANALYSIS.execute([sys.executable, "-c", "import time; time.sleep(5)"], WORKSPACE, self.output, "timeout", 0.05)
        timeout = json.loads((self.output / "timeout.json").read_text())
        self.assertTrue(timeout["timed_out"])
        self.assertNotEqual(timeout["exit"], 0)
        result = ANALYSIS.execute([sys.executable, "-c", "import os,signal; os.kill(os.getpid(),signal.SIGTERM)"],
                                  WORKSPACE, self.output, "crash", 5)
        self.assertEqual(result["exit"], -signal.SIGTERM)

    def test_emitted_report_must_be_complete_and_match_raw_diagnostics(self):
        source = WORKSPACE / "cpp/tests/protocol_tests.cpp"
        header = "---\nMainSourceFile: '" + str(source) + "'\n"
        cases = [("truncated", header + "Diagnostics: [\n", "", True),
                 ("unterminated", header + "Diagnostics: []\n", "", True),
                 ("wrong-type", header + "Diagnostics: {}\n...\n", "", True),
                 ("raw-disagrees", header + "Diagnostics: []\n...\n",
                  str(source) + ":1:1: error: defect [clang-analyzer-unix.Malloc,-warnings-as-errors]\n", True),
                 ("explicit-empty", header + "Diagnostics: []\n...\n", "", False)]
        original_execute = ANALYSIS.execute
        for name, document, raw, must_fail in cases:
            with self.subTest(name=name):
                output = self.output / ("report-" + name)
                output.mkdir()
                def fake_tool(argv, cwd, directory, label, timeout):
                    if not any(str(value).startswith("--export-fixes=") for value in argv):
                        return original_execute(argv, cwd, directory, label, timeout)
                    report = Path(next(value.split("=", 1)[1] for value in argv if value.startswith("--export-fixes=")))
                    report.write_text(document)
                    profiles = Path(next(value.split("=", 1)[1] for value in argv if value.startswith("--store-check-profile=")))
                    (profiles / "consumer.json").write_text(json.dumps({"file": str(source), "timestamp": "synthetic", "profile": {}}))
                    stdout, stderr = directory / (label + ".stdout"), directory / (label + ".stderr")
                    stdout.write_text(raw)
                    stderr.write_text("")
                    return {"exit": 0, "stdout": str(stdout), "stderr": str(stderr)}
                tool = ANALYSIS.Tool(SDK, WORKSPACE, output)
                with patch.object(ANALYSIS, "execute", side_effect=fake_tool):
                    if must_fail:
                        with self.assertRaises(ValueError):
                            tool.analyze(source, self.build, self.config, name, 5)
                    else:
                        result = tool.analyze(source, self.build, self.config, name, 5)
                        self.assertEqual(result["diagnostic_ids"], [])
                        self.assertEqual(result["report_status"], "exported")
                        self.assertIsNotNone(result["report"])

    @classmethod
    def tearDownClass(cls):
        print(json.dumps({"controls": str(cls.output), "metadata": cls.metadata, "cpp_programs_executed": False}))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *sys.argv[2:]], verbosity=2)
