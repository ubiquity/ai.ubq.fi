// Differential cases for src/kv-conversion/legacy-codec.ts. Its TS test file
// (tests/kv-runtime-conversion.test.ts) uses fileDigest only inside cases that need unported
// modules; these cases instead drive openLegacyCodec against fake `deno` shell scripts (the
// child's environment is only DENO_DIR, so they use shell builtins). The same scripts were run
// by the TypeScript module under Bun 1.4.0, which recorded every expected value below.
#include "uos/kv_conversion/legacy_codec.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include <algorithm>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::kv_conversion::legacy_codec;
  using uos::kv_conversion::typed_value::encodeTypedValue;

  const char* const SCENARIOS = R"JSON({
  "happy": {"identity": "2.9.7\n", "identityErr": "", "identityExit": 0, "decode": "{\"root\":{\"type\":\"string\",\"value\":\"decoded\"},\"nodes\":[]}", "encode": "\"AAEC\"", "finalExit": 0},
  "identity mismatch": {"identity": "2.9.6\n", "identityErr": "", "identityExit": 0, "decode": "", "encode": "", "finalExit": 0},
  "identity stderr": {"identity": "2.9.7\n", "identityErr": "warning", "identityExit": 0, "decode": "", "encode": "", "finalExit": 0},
  "identity exit": {"identity": "2.9.7\n", "identityErr": "", "identityExit": 1, "decode": "", "encode": "", "finalExit": 0},
  "identity spaced": {"identity": "  2.9.7  \n\n", "identityErr": "", "identityExit": 0, "decode": "", "encode": "\"AAEC\"\n\"AAED\"", "finalExit": 0},
  "exit nonzero": {"identity": "2.9.7\n", "identityErr": "", "identityExit": 0, "decode": "", "encode": "", "finalExit": 3},
  "codec dies": {"identity": "2.9.7\n", "identityErr": "", "identityExit": 0, "decode": "DIE", "encode": "", "finalExit": 0},
  "silent": {"identity": "2.9.7\n", "identityErr": "", "identityExit": 0, "decode": "", "encode": "", "finalExit": 0},
  "odd responses": {"identity": "2.9.7\n", "identityErr": "", "identityExit": 0, "decode": "not json", "encode": "[256,-1,\"3\"]", "finalExit": 0},
  "number response": {"identity": "2.9.7\n", "identityErr": "", "identityExit": 0, "decode": "", "encode": "5", "finalExit": 0}
}
)JSON";
  const char* const RECORDED = R"JSON({"LEGACY_DENO_VERSION": "2.9.7", "happy": {"steps": [["open", {"ok": {"version": "2.9.7", "binary": "<dir>/fake-deno", "sha256": true}}], ["decode", {"ok": {"root": {"type": "string", "value": "decoded"}, "nodes": []}}], ["encode", {"ok": [0, 1, 2]}], ["close", {}], ["close again", {}], ["abort after close", {}]], "requests": "{\"operation\":\"decode\",\"bytes\":\"AQID\"}\n{\"operation\":\"encode\",\"value\":{\"root\":{\"type\":\"string\",\"value\":\"x\"},\"nodes\":[]}}\n", "cache": false}, "identity mismatch": {"steps": [["open", {"error": {"name": "Error", "message": "Unsupported pinned legacy Deno version"}}]], "requests": null, "cache": false}, "identity stderr": {"steps": [["open", {"error": {"name": "Error", "message": "Unsupported pinned legacy Deno version"}}]], "requests": null, "cache": false}, "identity exit": {"steps": [["open", {"error": {"name": "Error", "message": "Unsupported pinned legacy Deno version"}}]], "requests": null, "cache": false}, "identity spaced": {"steps": [["open", {"ok": {"version": "2.9.7", "binary": "<dir>/fake-deno", "sha256": true}}], ["encode", {"ok": [0, 1, 2]}], ["encode buffered", {"ok": [0, 1, 3]}], ["abort", {}], ["close after abort", {}]], "requests": "{\"operation\":\"encode\",\"value\":{\"root\":{\"type\":\"number\",\"value\":1},\"nodes\":[]}}\n{\"operation\":\"encode\",\"value\":{\"root\":{\"type\":\"number\",\"value\":2},\"nodes\":[]}}\n", "cache": false}, "exit nonzero": {"steps": [["open", {"ok": {"version": "2.9.7", "binary": "<dir>/fake-deno", "sha256": true}}], ["close", {"error": {"name": "Error", "message": "Legacy codec exited 3: bye\n"}}]], "requests": null, "cache": false}, "codec dies": {"steps": [["open", {"ok": {"version": "2.9.7", "binary": "<dir>/fake-deno", "sha256": true}}], ["decode", {"error": {"name": "Error", "message": "Legacy codec failed: fatal\n"}}], ["abort", {}]], "requests": "{\"operation\":\"decode\",\"bytes\":\"CQ==\"}\n", "cache": false}, "silent": {"steps": [["open", {"ok": {"version": "2.9.7", "binary": "<dir>/fake-deno", "sha256": true}}], ["decode during abort", {"error": {"name": "Error", "message": "Legacy codec failed: "}}], ["abort", {}]], "requests": "{\"operation\":\"decode\",\"bytes\":\"\"}\n", "cache": false}, "odd responses": {"steps": [["open", {"ok": {"version": "2.9.7", "binary": "<dir>/fake-deno", "sha256": true}}], ["decode", {"error": {"name": "SyntaxError", "message": "JSON Parse error: Unexpected identifier \"not\""}}], ["encode", {"error": {"name": "SyntaxError", "message": "JSON Parse error: Unexpected identifier \"not\""}}], ["close", {}]], "requests": "{\"operation\":\"decode\",\"bytes\":\"AP8=\"}\n{\"operation\":\"encode\",\"value\":{\"root\":{\"type\":\"null\",\"value\":null},\"nodes\":[]}}\n", "cache": false}, "number response": {"steps": [["open", {"ok": {"version": "2.9.7", "binary": "<dir>/fake-deno", "sha256": true}}], ["encode", {"error": {"name": "TypeError", "message": "The first argument must be of type string or an instance of Buffer, ArrayBuffer, or Array or an Array-like Object. Received type number (5)", "code": "ERR_INVALID_ARG_TYPE"}}], ["close", {}]], "requests": "{\"operation\":\"encode\",\"value\":{\"root\":{\"type\":\"undefined\"},\"nodes\":[]}}\n", "cache": false}, "pins": [["zeros", "Pinned legacy Deno binary does not match its SHA-256: expected <expected>, observed <digest>"], ["uppercase", "Pinned legacy Deno binary does not match its SHA-256: expected <expected>, observed <digest>"], ["short", "Pinned legacy Deno binary does not match its SHA-256: expected <expected>, observed <digest>"], ["missing", {"name": "Error", "message": "ENOENT: no such file or directory, open '/tmp/claude-1002/wave/w01c7/scratch/codec/run-jcQWoh/s-iadOP1/missing-deno'", "code": "ENOENT", "syscall": "open", "path": "<dir>/missing-deno"}]], "fileDigest": [["empty", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"], ["small", 6, "35a9838ae94607ca89c07791a9682ba4a7a4ac9a30f262439c067eeb64ae2ad1"], ["chunked", 614407, "f79145de0cd133899c4e4a6cc3e7f610568829a0859d76742769cd8b94591412"]]})JSON";

  std::string quote(std::string_view text) { return "'" + rt::replaceAll(text, "'", "'\\''") + "'"; }

  // script(s) of the recording script, byte for byte.
  std::string script(const json::object& s) {
    const auto text = [&s](std::string_view key) { return std::string(s.at(key).as_string()); };
    const auto response = [](const std::string& reply) {
      if (reply.empty()) return std::string(":");
      if (reply == "DIE") return std::string("printf 'fatal\\n' >&2; exit 4");
      return "printf '%s\\n' " + quote(reply);
    };
    const std::vector<std::string> lines{
        "#!/bin/sh",
        R"(last="")",
        R"(for arg in "$@"; do last="$arg"; done)",
        R"(if [ "$last" = "--identity" ]; then)",
        "  printf '%s' " + quote(text("identity")),
        text("identityErr").empty() ? std::string("  :") : "  printf '%s' " + quote(text("identityErr")) + " >&2",
        "  exit " + std::to_string(s.at("identityExit").as_int64()),
        "fi",
        "while IFS= read -r line; do",
        R"(  printf '%s\n' "$line" >> requests.log)",
        R"(  case "$line" in)",
        R"(    *'"operation":"decode"'*) )" + response(text("decode")) + " ;;",
        R"(    *'"operation":"encode"'*) )" + response(text("encode")) + " ;;",
        "  esac",
        "done",
        R"(printf 'bye\n' >&2)",
        "exit " + std::to_string(s.at("finalExit").as_int64()),
        "",
    };
    std::string out;
    for (std::size_t index = 0; index < lines.size(); ++index) out += (index ? "\n" : "") + lines[index];
    return out;
  }

  json::value describe(const rt::Reason& reason) {
    json::object out;
    try {
      std::rethrow_exception(reason);
    } catch (const rt::fs::ErrnoError& error) {
      out = json::object{{"name", json::string(error.name)}, {"message", json::string(error.message)}, {"code", json::string(error.code)}, {"syscall", json::string(error.syscall)}};
    } catch (const rt::CodedTypeError& error) {
      out = json::object{{"name", json::string(error.name)}, {"message", json::string(error.message)}, {"code", json::string(error.code)}};
    } catch (const rt::JsError& error) {
      out = json::object{{"name", json::string(error.name)}, {"message", json::string(error.message)}};
    }
    return out;
  }

  json::value bytesJson(const std::vector<std::uint8_t>& bytes) {
    json::array out;
    for (const std::uint8_t byte : bytes) out.push_back(byte);
    return out;
  }

  std::string makeBinary(const std::string& dir, const json::object& scenario) {
    const std::string binary = rt::path::join(dir, "fake-deno");
    rt::fs::writeFileSync(binary, script(scenario));
    rt::fs::chmodSync(binary, 0700);
    return binary;
  }

  struct Steps {
    json::array list;
    void ok(std::string label, json::value value) { list.push_back(json::array{json::string(label), json::object{{"ok", std::move(value)}}}); }
    void done(std::string label) { list.push_back(json::array{json::string(label), json::object{}}); }
    void failed(std::string label, const rt::Reason& reason) { list.push_back(json::array{json::string(label), json::object{{"error", describe(reason)}}}); }
  };

  rt::Task<void> attemptVoid(Steps& steps, std::string label, rt::Task<void> task) {
    rt::Reason failure;
    try {
      co_await std::move(task);
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) steps.failed(label, failure);
    else steps.done(label);
  }

  rt::Task<void> attemptBytes(Steps& steps, std::string label, rt::Task<std::vector<std::uint8_t>> task) {
    rt::Reason failure;
    std::vector<std::uint8_t> bytes;
    try {
      bytes = co_await std::move(task);
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) steps.failed(label, failure);
    else steps.ok(label, bytesJson(bytes));
  }

  rt::Task<void> attemptValue(Steps& steps, std::string label, rt::Task<json::value> task) {
    rt::Reason failure;
    json::value value;
    try {
      value = co_await std::move(task);
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) steps.failed(label, failure);
    else steps.ok(label, value);
  }

  // Waits (real time, up to 2 s) until the child logged `lines` requests. The second buffered
  // encode is answered without reading, so abort() races the child reading that request in Bun
  // and here alike; Bun's recording happened to log it, and this makes the C++ run deterministic.
  rt::Task<void> waitForRequests(std::string log, std::size_t lines) {
    for (int attempt = 0; attempt < 200; ++attempt) {
      if (rt::fs::existsSync(log)) {
        const std::string text = rt::fs::readFileSync(log, "utf8");
        if (static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) >= lines) co_return;
      }
      co_await rt::sleep(10);
    }
  }

  // One scenario of the recording script: { steps, requests, cache }.
  rt::Task<json::value> scenario(std::string root, std::string name, json::object settings) {
    const std::string dir = rt::fs::mkdtempSync(rt::path::join(root, "s-"));
    const std::string binary = makeBinary(dir, settings);
    const std::string digest = co_await fileDigest(binary);
    Steps steps;
    const rt::AbortController controller;
    std::optional<LegacyCodec> codec;
    rt::Reason failure;
    try {
      rt::Task<LegacyCodec> opening = name == "silent" ? openLegacyCodec(binary, digest, dir, controller.signal()) : openLegacyCodec(binary, digest, dir);
      LegacyCodec opened = co_await std::move(opening);
      codec.emplace(std::move(opened));
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      steps.failed("open", failure);
    } else {
      steps.ok("open", json::object{{"version", json::string(codec->identity.version)},
                                    {"binary", json::string(rt::replace(codec->identity.binary, dir, "<dir>"))},
                                    {"sha256", codec->identity.sha256 == digest}});
    }
    // Each step's task is built in its own statement: GCC 12 rejects braced lists and must not
    // see temporaries inside a co_await expression (docs/rt/async.md).
    std::vector<rt::Task<void>> plan;
    if (codec) {
      const auto bytes = [](std::initializer_list<std::uint8_t> list) { return std::vector<std::uint8_t>(list); };
      if (name == "happy") {
        plan.push_back(attemptValue(steps, "decode", codec->decode(bytes({1, 2, 3}))));
        plan.push_back(attemptBytes(steps, "encode", codec->encode(encodeTypedValue(rt::KvValue("x")))));
        plan.push_back(attemptVoid(steps, "close", codec->close()));
        plan.push_back(attemptVoid(steps, "close again", codec->close()));
        plan.push_back(attemptVoid(steps, "abort after close", codec->abort()));
      } else if (name == "identity spaced") {
        plan.push_back(attemptBytes(steps, "encode", codec->encode(encodeTypedValue(rt::KvValue(1)))));
        plan.push_back(attemptBytes(steps, "encode buffered", codec->encode(encodeTypedValue(rt::KvValue(2)))));
        plan.push_back(waitForRequests(rt::path::join(dir, "requests.log"), 2));
        plan.push_back(attemptVoid(steps, "abort", codec->abort()));
        plan.push_back(attemptVoid(steps, "close after abort", codec->close()));
      } else if (name == "exit nonzero") {
        plan.push_back(attemptVoid(steps, "close", codec->close()));
      } else if (name == "codec dies") {
        plan.push_back(attemptValue(steps, "decode", codec->decode(bytes({9}))));
        plan.push_back(attemptVoid(steps, "abort", codec->abort()));
      } else if (name == "odd responses") {
        plan.push_back(attemptValue(steps, "decode", codec->decode(bytes({0, 255}))));
        plan.push_back(attemptBytes(steps, "encode", codec->encode(rt::jsonParse(R"({"root":{"type":"null","value":null},"nodes":[]})"))));
        plan.push_back(attemptVoid(steps, "close", codec->close()));
      } else if (name == "number response") {
        plan.push_back(attemptBytes(steps, "encode", codec->encode(rt::jsonParse(R"({"root":{"type":"undefined"},"nodes":[]})"))));
        plan.push_back(attemptVoid(steps, "close", codec->close()));
      }
    }
    // Tasks start when awaited, so the steps still run one after another.
    for (rt::Task<void>& step : plan) co_await std::move(step);
    if (codec && name == "silent") {
      rt::Promise<json::value> pending(codec->decode({}));
      const rt::AbortController aborting = controller;
      rt::setTimeout([aborting] { aborting.abort(); }, 50);
      rt::Task<void> during = attemptValue(steps, "decode during abort", pending.wait());
      co_await std::move(during);
      rt::Task<void> aborted = attemptVoid(steps, "abort", codec->abort());
      co_await std::move(aborted);
    }
    const std::string log = rt::path::join(dir, "requests.log");
    json::object out;
    out["steps"] = std::move(steps.list);
    out["requests"] = rt::fs::existsSync(log) ? json::value(json::string(rt::fs::readFileSync(log, "utf8"))) : json::value(nullptr);
    out["cache"] = rt::fs::existsSync(rt::path::join(dir, ".deno-codec-cache"));
    co_return out;
  }
}

namespace {
  void removeTree(const std::string& root) {
    rt::fs::RmOptions everything;
    everything.recursive = true;
    everything.force = true;
    rt::fs::rmSync(root, everything);
  }
}

ASYNC_TEST("differential: openLegacyCodec drives fake codec children like Bun") {
  const std::string root = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "uos-legacy-codec-"));
  const json::value settings = rt::jsonParse(SCENARIOS);
  const json::value recorded = rt::jsonParse(RECORDED);
  for (const auto& entry : settings.as_object()) {
    const std::string name(entry.key());
    rt::Task<json::value> running = scenario(root, name, entry.value().as_object());
    const json::value actual = co_await std::move(running);
    const json::value& expected = recorded.as_object().at(entry.key());
    if (!rt::deepEqual(actual, expected)) {
      EXPECT_EQ(actual, expected);
      std::fprintf(stderr, "      scenario: %s\n", name.c_str());
    }
  }
  removeTree(root);
}

ASYNC_TEST("differential: pins and missing binaries are refused like Bun") {
  const std::string root = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "uos-legacy-codec-"));
  const std::string dir = rt::fs::mkdtempSync(rt::path::join(root, "s-"));
  const json::value settings = rt::jsonParse(SCENARIOS);
  const json::value recorded = rt::jsonParse(RECORDED);
  const std::string binary = makeBinary(dir, settings.as_object().at("happy").as_object());
  const std::string digest = co_await fileDigest(binary);
  json::array pins;
  const std::vector<std::pair<std::string, std::string>> cases{{"zeros", std::string(64, '0')}, {"uppercase", rt::toUpperCase(digest)}, {"short", digest.substr(1)}};
  for (const auto& [label, expected] : cases) {
    std::string message = "opened";
    rt::Reason failure;
    try {
      rt::Task<LegacyCodec> opening = openLegacyCodec(binary, expected, dir);
      co_await std::move(opening);
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) message = rt::replace(rt::replace(rt::errorMessage(failure).value_or(""), digest, "<digest>"), expected, "<expected>");
    pins.push_back(json::array{json::string(label), json::string(message)});
  }
  json::object missing;
  try {
    rt::Task<LegacyCodec> opening = openLegacyCodec(rt::path::join(dir, "missing-deno"), std::string(64, '0'), dir);
    co_await std::move(opening);
  } catch (const rt::fs::ErrnoError& error) {
    missing = json::object{{"name", json::string(error.name)},
                           {"message", json::string(rt::replace(error.message, dir, "<dir>"))},
                           {"code", json::string(error.code)},
                           {"syscall", json::string(error.syscall)},
                           {"path", json::string(rt::replace(error.path.value_or(""), dir, "<dir>"))}};
  }
  pins.push_back(json::array{"missing", std::move(missing)});
  // Bun's message names its own temporary directory.
  json::value expected = recorded.as_object().at("pins");
  json::object& recordedMissing = expected.as_array()[3].as_array()[1].as_object();
  const std::string recordedMessage(recordedMissing.at("message").as_string());
  const std::size_t open = recordedMessage.find("open '") + 6;
  recordedMissing["message"] = json::string(recordedMessage.substr(0, open) + "<dir>" + recordedMessage.substr(recordedMessage.rfind("/missing-deno")));
  EXPECT_EQ(json::value(pins), expected);
  removeTree(root);
}

ASYNC_TEST("differential: fileDigest hashes files of any size like Bun") {
  const std::string root = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "uos-legacy-codec-"));
  const json::value recorded = rt::jsonParse(RECORDED);
  for (const json::value& row : recorded.as_object().at("fileDigest").as_array()) {
    const std::string label(row.as_array()[0].as_string());
    const auto size = static_cast<std::size_t>(row.as_array()[1].as_int64());
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t index = 0; index < size; ++index) bytes[index] = static_cast<std::uint8_t>((index * 31 + 7) & 255);
    const std::string path = rt::path::join(root, label);
    rt::fs::writeFileSync(path, std::span<const std::uint8_t>(bytes));
    const std::string digest = co_await fileDigest(path);
    EXPECT_EQ(digest, std::string(row.as_array()[2].as_string()));
  }
  EXPECT_EQ(LEGACY_DENO_VERSION, std::string_view(recorded.as_object().at("LEGACY_DENO_VERSION").as_string()));
  removeTree(root);
}
