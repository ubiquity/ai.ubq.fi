"""Identical synthetic request bytes and response shapes for both gateways."""

from dataclasses import dataclass
import json


MODEL = "deepseek-flash"
COMPLETION = "fixture-wire-ok"
CHAT_TEXT = "Synthetic request path chat."
RESPONSES_TEXT = "Synthetic request path Responses."
STREAM_TEXT = ("fixture-", "wire-", "ok", ".", ".", ".", ".", ".")
ADMIN_TOKEN = "request-path-synthetic-admin"
API_TOKEN = "u_" + "b" * 64
PROVIDER_TOKEN = "request-path-synthetic-deepseek"
CHAT_PATH = "/v1/chat/completions"
USAGE = {"prompt_tokens": 11, "completion_tokens": 5, "total_tokens": 16, "prompt_cache_hit_tokens": 0}
CHAT_ENVELOPE = {"id": "chatcmpl-request-path-fixture", "created": 1780000001, "model": MODEL}


def encode(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode()


def chat(messages, stream=False):
    return encode({"model": MODEL, "messages": messages, "stream": stream, "max_completion_tokens": 64})


SMALL_CHAT = chat([{"role": "user", "content": CHAT_TEXT}])
STREAM_CHAT = chat([{"role": "user", "content": CHAT_TEXT}], stream=True)
LARGE_MESSAGES = [{"role": "user" if index % 2 == 0 else "assistant", "content": "a" * 62_000}
                  for index in range(16)]
LARGE_MESSAGES.append({"role": "user", "content": ""})
LARGE_MESSAGES[-1]["content"] = "z" * (1024 * 1024 - len(chat(LARGE_MESSAGES)))
LARGE_CHAT = chat(LARGE_MESSAGES)
assert len(LARGE_CHAT) == 1024 * 1024


@dataclass(frozen=True)
class Scenario:
    path: str
    body: bytes | None
    requests: int
    warmup: int
    status: int = 200


SCENARIOS = {
    "small": Scenario(CHAT_PATH, SMALL_CHAT, 1500, 200),
    "large": Scenario(CHAT_PATH, LARGE_CHAT, 240, 20),
    "stream": Scenario(CHAT_PATH, STREAM_CHAT, 750, 100),
    "responses": Scenario("/v1/responses", encode({"model": MODEL, "input": RESPONSES_TEXT, "stream": False, "max_output_tokens": 64}), 1500, 200),
    "models": Scenario("/v1/models", None, 3000, 400),
    "reject": Scenario(CHAT_PATH, SMALL_CHAT, 3000, 400, 401),
}
INFERENCE = frozenset(("small", "large", "stream", "responses"))


def sse(value):
    return b"data: " + encode(value) + b"\n\n"


BUFFERED = encode({**CHAT_ENVELOPE, "object": "chat.completion", "choices": [{"index": 0, "message": {
    "role": "assistant", "content": COMPLETION}, "finish_reason": "stop"}], "usage": USAGE})
SSE_TEXT = [sse({**CHAT_ENVELOPE, "object": "chat.completion.chunk", "choices": [{"index": 0,
    "delta": {"role": "assistant", "content": word} if index == 0 else {"content": word}, "finish_reason": None}]})
    for index, word in enumerate(STREAM_TEXT)]
SSE_TERMINAL = (
    sse({**CHAT_ENVELOPE, "object": "chat.completion.chunk", "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}),
    sse({**CHAT_ENVELOPE, "object": "chat.completion.chunk", "choices": [], "usage": USAGE}),
    b"data: [DONE]\n\n",
)
