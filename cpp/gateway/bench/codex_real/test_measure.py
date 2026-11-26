#!/usr/bin/env python3
"""Deterministic checks for tailing a gateway log while it is being written."""

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import measure


class TerminalTests(unittest.TestCase):
    def terminal(self, **overrides):
        record = {"request_id": "fixture", "provider": "chatgpt_codex", "status": 200,
                  "delivery_outcome": "delivered", "stream_terminal_type": "response.completed",
                  "total_tokens": 16, "detail": "x" * 2400, **overrides}
        return measure.TERMINAL_PREFIX + json.dumps(record) + "\n"

    def read_chunks(self, chunks):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "gateway.log"
            log.write_text(chunks[0])
            remaining = iter(chunks[1:])

            def append(_):
                with log.open("a") as writer:
                    writer.write(next(remaining))

            pending = set()
            with log.open() as reader, patch.object(measure.time, "sleep", side_effect=append) as sleep, \
                    patch.object(measure.time, "monotonic", return_value=0):
                measure.terminals(log, reader, 1, pending)
            self.assertEqual(pending, {"fixture"})
            self.assertEqual(sleep.call_count, len(chunks) - 1)

    def test_complete_terminal(self):
        self.read_chunks(["startup\n" + self.terminal()])

    def test_partial_terminal_waits_for_newline(self):
        line = self.terminal()
        for split in (0, 7, len(measure.TERMINAL_PREFIX), 1968, len(line) - 1):
            with self.subTest(split=split):
                self.read_chunks([line[:split], line[split:]])

    def test_multiple_fragments_after_other_log_lines(self):
        line = self.terminal()
        self.read_chunks(["startup\n" + line[:7], line[7:1968], line[1968:-1], "\n"])

    def test_incomplete_log_still_times_out(self):
        for fragment in ("", measure.TERMINAL_PREFIX + '{"request_id":"fixture', self.terminal()[:-1]):
            with self.subTest(fragment=fragment[:40]), tempfile.TemporaryDirectory() as directory:
                log = Path(directory) / "gateway.log"
                log.write_text(fragment)
                with log.open() as reader, patch.object(measure.time, "monotonic", side_effect=(0, 11)):
                    with self.assertRaisesRegex(RuntimeError, "Timed out waiting for 1 delivered terminals"):
                        measure.terminals(log, reader, 1, set())

    def test_malformed_complete_terminal_is_not_ignored(self):
        with self.assertRaises(json.JSONDecodeError):
            self.read_chunks([measure.TERMINAL_PREFIX + '{"request_id":"fixture\n'])

    def test_complete_terminal_keeps_delivery_checks(self):
        for changes in ({"delivery_outcome": "failed"}, {"provider": "wrong"}, {"status": 503},
                        {"stream_terminal_type": "response.failed"}, {"total_tokens": 0}):
            with self.subTest(changes=changes), self.assertRaises(RuntimeError):
                self.read_chunks([self.terminal(**changes)])


if __name__ == "__main__":
    unittest.main()
