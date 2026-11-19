"""Controls for the public contract comparison."""

import unittest
from io import BytesIO

from compare import compare, expectations, normalize, redact
from wire import read_sse


def observation(**fields):
    return {"status": 200, "headers": {"content-type": ["application/json"]}, "body": {},
            "dispatches": [], "mock_errors": [], "benchmark_failures": 0, **fields}


class CompareTest(unittest.TestCase):
    def differences(self, bun, native, **spec):
        return [(item["classification"], item["path"])
                for item in compare(spec, bun, native, {"bun": {}, "native": {}})]

    def test_ids_keep_references_and_catalog_slugs(self):
        response = "resp_" + "a" * 32
        item = "msg_" + response + "_0"
        value = {"id": response, "model": "gpt-smoke-fixture", "created_at": 1780000001,
                 "output": [{"id": item}], "item_id": item}
        normalized = normalize(value, {}, {})
        self.assertEqual(normalized["item_id"], normalized["output"][0]["id"])
        self.assertEqual(normalized["model"], value["model"])
        self.assertNotEqual(normalized["id"], value["id"])
        self.assertEqual(normalized["created_at"], "<timestamp-s>")

    def test_absent_body_field_and_numeric_type_are_functional(self):
        bun = observation(body={"output": None, "active": True})
        native = observation(body={"active": 1})
        self.assertEqual(self.differences(bun, native), [("FUNCTIONAL", "/body/active"), ("FUNCTIONAL", "/body/output")])

    def test_models_order_can_be_cosmetic(self):
        models = [{"id": "alpha", "object": "model"}, {"id": "beta", "object": "model"}]
        bun = observation(body={"data": models})
        native = observation(body={"data": models[::-1]})
        self.assertEqual(self.differences(bun, native, unordered={"/body/data": "id"}), [("COSMETIC", "/body/data/order")])

    def test_sse_terminal_and_sequence_order_are_functional(self):
        bun = observation(events=[{"event": "response.created", "data": {"type": "response.created"}},
                                  {"event": "response.completed", "data": {"type": "response.completed"}}], terminal="response.completed")
        native = observation(events=bun["events"][:1], terminal="response.created")
        self.assertEqual(self.differences(bun, native), [("FUNCTIONAL", "/events/length"), ("FUNCTIONAL", "/terminal")])

    def test_quota_counts_and_retry_presence_survive_clock_tolerance(self):
        bun = observation(headers={"ratelimit-remaining": ["0"], "ratelimit-reset": ["29"], "retry-after": ["29"]})
        native = observation(headers={"ratelimit-remaining": ["1"], "ratelimit-reset": ["30"]})
        self.assertEqual(self.differences(bun, native), [("FUNCTIONAL", "/headers/ratelimit-remaining"), ("FUNCTIONAL", "/headers/retry-after")])

    def test_error_wording_keeps_error_identity(self):
        bun = observation(body={"error": {"code": "bad_request", "message": "No model"}})
        native = observation(body={"error": {"code": "unknown_model", "message": "Model not supplied"}})
        self.assertEqual(self.differences(bun, native), [("FUNCTIONAL", "/body/error/code"), ("COSMETIC", "/body/error/message")])

    def test_shared_contract_failure_cannot_pass_by_comparison(self):
        spec = {"expect": {"status": 400, "routes": []}, "request_id": False}
        result = observation(dispatches=[{"route": "deepseek.success"}])
        self.assertEqual(self.differences(result, result), [])
        self.assertEqual([item["path"] for item in expectations(spec, result)], ["/status", "/routes"])

    def test_credential_redaction(self):
        self.assertEqual(redact({"echo": ["prefix private-fixture suffix"]}, ("private-fixture",)),
                         {"echo": ["prefix <synthetic credential> suffix"]})

    def test_malformed_sse_metadata_can_be_reported_as_a_wire_error(self):
        for body in (b'{"type":[]}', b'{"choices":[null]}',
                     b'{"choices":[{"finish_reason":1}]}', b'{"choices":[{"delta":null}]}'):
            with self.subTest(body=body), self.assertRaisesRegex(ValueError, "SSE"):
                read_sse(BytesIO(b"data: " + body + b"\n\n"), {}, None)


if __name__ == "__main__":
    unittest.main()
