"""Real exec and resume invocations, plus a matching scripted CI workload."""

from contextlib import closing
import json
from pathlib import Path
import shutil
import subprocess

from driver import connection, require
from fixtures import API_TOKEN, encode
from programmed import MODEL, TEXT, TOOL_FILE, TOOL_ROUNDS
from run import sparse


def native_codex(name):
    command = shutil.which(name)
    if command is None:
        return None
    path = Path(command).resolve(strict=True)
    # The npm-distributed CLI includes the prebuilt native executable.
    # Find it without invoking the JavaScript launcher or a package manager.
    root = path.parent.parent
    vendor = root / "node_modules/@openai/codex-linux-arm64/vendor"
    candidates = [path, *vendor.glob("*/bin/codex")]
    for candidate in candidates:
        with candidate.open("rb") as stream:
            if stream.read(4) == b"\x7fELF":
                return candidate.resolve()
    return None


def codex_version(binary, root):
    result = subprocess.check_output([binary, "--version"], text=True, env=sparse(root), cwd=root, timeout=10).split()
    require(len(result) == 2 and result[0] == "codex-cli", "Expected an installed native Codex CLI")
    return result[1]


class Codex:
    def __init__(self, binary, version, root, port, tool_text):
        self.binary = binary
        self.version = version
        self.root = root
        self.tool_text = tool_text
        self.port = port
        self.resume = None

    def run(self, index, kind):
        if kind == "resume":
            root, env, thread = self.resume
            args = ["resume", thread, "Resume the programmed-reply memory fixture."]
        else:
            root = self.root / f"session-{index:04}"
            root.mkdir(mode=0o700)
            home = root / "codex"
            home.mkdir()
            env = {**sparse(root), "CODEX_HOME": str(home), "BENCH_API_KEY": API_TOKEN}
            (root / TOOL_FILE).write_text(self.tool_text)
            origin = f"http://127.0.0.1:{self.port}/v1"
            (home / "config.toml").write_text(f"""
model = "{MODEL}"
model_provider = "fixture"
model_reasoning_effort = "none"
approval_policy = "never"
sandbox_mode = "workspace-write"
check_for_update_on_startup = false

[features]
api_key_model_discovery = true

[analytics]
enabled = false

[model_providers.fixture]
name = "Loopback memory fixture"
base_url = "{origin}"
model_catalog_url = "{origin}/models"
wire_api = "responses"
env_key = "BENCH_API_KEY"
request_max_retries = 0
stream_max_retries = 0
stream_idle_timeout_ms = 10000
supports_websockets = false
""")
            args = ([] if kind == "tools" else ["--ephemeral"]) + [f"Programmed-reply memory fixture {index}."]
        command = [self.binary, "exec", "--strict-config", "--skip-git-repo-check", "--json", "--color", "never", *args]
        with (root / f"exec-{index:04}.stderr").open("wb") as errors:
            done = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=errors,
                                  cwd=root, env=env, timeout=60, check=False)
        (root / f"exec-{index:04}.jsonl").write_bytes(done.stdout)
        require(done.returncode == 0, f"Codex {kind} session {index} exited {done.returncode}")
        rows = [json.loads(line) for line in done.stdout.splitlines() if line.startswith(b"{")]
        thread_id = next(row["thread_id"] for row in rows if row["type"] == "thread.started")
        items = [row["item"] for row in rows if row["type"] == "item.completed"]
        answers = [item["text"].strip() for item in items if item["type"] == "agent_message"]
        require(answers and answers[-1] == TEXT[kind].strip(), f"Codex {kind} omitted its exact programmed reply")
        require(sum(row["type"] == "turn.completed" for row in rows) == 1, f"Codex {kind} lacked one completed client turn")
        if kind == "tools":
            require((root / "fixture-patch.txt").stat().st_size == 32_001, "Codex apply_patch did not write the fixture file")
            require(sum(item["type"] == "command_execution" and item["exit_code"] == 0 for item in items) == TOOL_ROUNDS - 1,
                    "Codex missed a successful shell tool")
            self.resume = root, env, thread_id
        if kind == "resume":
            require(thread_id == thread, "Codex did not resume the earlier tool session")
        if kind == "reasoning":
            require(any(item["type"] == "reasoning" for item in items), "Codex did not receive the reasoning item")


class Scripted:
    def __init__(self, port, version, tool_text):
        self.port, self.version, self.tool_text = port, version, tool_text
        self.previous = None

    def headers(self):
        return {"Authorization": "Bearer " + API_TOKEN, "User-Agent": f"codex_cli_rs/{self.version}", "originator": "codex_cli_rs"}

    def catalog(self):
        with closing(connection(self.port)) as conn:
            conn.request("GET", "/v1/models?client_version=" + self.version, headers=self.headers())
            response = conn.getresponse()
            body = response.read()
            require(response.status == 200 and response.getheader("x-uos-cache") in ("hit", "stale"), "Exact-version catalog did not hit durable state")
            rows = json.loads(body)["models"]
            require(len(rows) == 15 and rows[0]["slug"] == MODEL, "Exact-version catalog shape changed")
            return body

    def request(self, items, instructions, *, failure=False):
        body = encode({"model": MODEL, "instructions": instructions, "input": items, "stream": True,
                       "store": False, "reasoning": {"effort": "none"}, "include": ["reasoning.encrypted_content"]})
        with closing(connection(self.port)) as conn:
            return self.send(conn, body, failure=failure)

    def send(self, conn, body, *, failure=False):
        text, output, terminal, seen = [], [], None, None
        conn.request("POST", "/v1/responses", body, {**self.headers(), "Content-Type": "application/json"})
        response = conn.getresponse()
        require(response.status == 200 and response.getheader("Content-Type", "").startswith("text/event-stream"),
                f"Expected SSE, received {response.status}")
        for line in response:
            if not line.startswith(b"data: ") or line.startswith(b"data: [DONE]"):
                continue
            event = json.loads(line[6:])
            if event["type"] == "response.output_text.delta":
                text.append(event["delta"])
            if event["type"] == "response.output_item.done":
                output.append(event["item"])
            if event["type"] in ("response.completed", "response.failed"):
                require(terminal is None, "Scripted client received an extra terminal")
                seen = event["type"]
                terminal = event["response"]
            status = "response.failed" if failure else "response.completed"
            require(seen == status and terminal is not None, "Scripted client lost its terminal")
            if not failure:
                require(terminal["usage"]["total_tokens"] > 0, "Scripted client lost token usage")
        return "".join(text), output

    def run(self, index, kind):
        models = json.loads(self.catalog())["models"]
        instructions = models[0]["model_messages"]["instructions_template"]
        items = list(self.previous) if kind == "resume" else []
        items.append({"type": "message", "role": "user",
                      "content": [{"type": "input_text", "text": f"Programmed-reply memory fixture {index}."}]})
        count = TOOL_ROUNDS + 1 if kind == "tools" else 1
        for turn in range(count):
            text, output = self.request(items, instructions, failure=kind == "replay")
            items.extend(output)
            if turn + 1 < count:
                call = output[-1]
                items.append({"type": "custom_tool_call_output" if turn == 0 else "function_call_output",
                              "call_id": call["call_id"], "output": "Success: fixture-patch.txt" if turn == 0 else self.tool_text})
        if kind != "replay":
            require(text == TEXT[kind], "Scripted client lost reply text")
        if kind == "tools":
            self.previous = items


def check_auth(port, version):
    # An authenticated miss exercises the key lookup without catalog or settlement.
    with closing(connection(port)) as conn:
        conn.request("GET", "/v1/fixture-auth-miss", headers=Scripted(port, version, "").headers())
        response = conn.getresponse()
        response.read()
        require(response.status == 404, f"Expected authenticated route miss, received {response.status}")
