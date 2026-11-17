// Differential cases for src/codex/supervisor-log.ts recorded under Bun 1.4.0
// (tests/supervisor_log_vectors.inc). The bun test that targets it, tests/codex-supervisor-brief.test.ts,
// is ported in tests/codex_supervisor_brief_test.cpp.
#include "uos/codex/supervisor_log.hpp"

#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <unistd.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "supervisor_log_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::codex::supervisor_log;

namespace {
  const std::string thread = "01a0c796-caac-7dc0-9446-bf13098203e7";

  json::value textPart(std::string_view type, std::string text) { return json::object{{"type", json::string(type)}, {"text", json::string(text)}}; }

  std::string item(std::string timestamp, json::object payload) {
    json::object line;
    rt::objectDefine(line, "timestamp", json::value(timestamp));
    rt::objectDefine(line, "type", json::value("response_item"));
    rt::objectDefine(line, "payload", json::value(std::move(payload)));
    return rt::jsonStringify(json::value(std::move(line)));
  }

  // The oracle script's lineFor(i).
  std::string lineFor(int i) {
    const std::string n = std::to_string(i);
    if (i % 17 == 0) return "{\"broken\": " + n;
    if (i % 13 == 0) return item("2026-09-22T08:00:00.000Z", json::object{{"type", "message"}, {"role", "developer"}, {"content", json::array{textPart("input_text", "dev " + n)}}});
    if (i % 11 == 0) {
      const std::string seconds = rt::padStart(std::to_string(i % 60), 2, "0");
      return item("2026-09-22T08:00:" + seconds + ".000Z", json::object{{"type", "function_call_output"}, {"output", json::string("  out " + n + "  ")}});
    }
    if (i % 7 == 0) {
      json::object payload;
      rt::objectDefine(payload, "type", json::value("function_call"));
      rt::objectDefine(payload, "name", json::value(i % 2 ? "exec" : " "));
      rt::objectDefine(payload, "status", json::value(i % 3 ? "completed" : ""));
      return item("2026-09-22T08:00:00.000Z", std::move(payload));
    }
    json::object payload;
    rt::objectDefine(payload, "type", json::value("message"));
    rt::objectDefine(payload, "role", json::value(i % 5 ? "assistant" : "user"));
    rt::objectDefine(payload, "content",
                     json::array{textPart("output_text", "event " + n + " é漢\U0001F600"), textPart("text", ""), textPart("input_text", "second")});
    return item("2026-09-22T08:00:00.123Z", std::move(payload));
  }

  void symlinkOrThrow(const std::string& target, const std::string& path) {
    if (::symlink(target.c_str(), path.c_str()) != 0) throw std::runtime_error("symlink failed: " + path);
  }

  json::value eventsJson(const std::vector<SupervisorLogEvent>& events) {
    json::array array;
    for (const SupervisorLogEvent& event : events) {
      json::object object;
      rt::objectDefine(object, "atMs", rt::makeNumber(event.atMs));
      rt::objectDefine(object, "kind", json::value(event.kind));
      rt::objectDefine(object, "text", json::value(event.text));
      array.push_back(std::move(object));
    }
    return array;
  }
}

ASYNC_TEST("differential: readSupervisorRolloutTail over a fixture Codex home") {
  const json::value vectors = rt::jsonParse(kSupervisorLogVectors);
  std::string bigText;
  for (int i = 0; i < 2000; ++i) bigText += lineFor(i) + "\n";
  std::string smallText;
  const std::vector<std::string> smallLines = {lineFor(1), lineFor(7), lineFor(11), lineFor(13), lineFor(17), "", "   ", lineFor(22)};
  for (std::size_t index = 0; index < smallLines.size(); ++index) smallText += (index ? "\n" : "") + smallLines[index];

  const std::string root = rt::fs::mkdtempSync(rt::os::tmpdir() + "/uos-supervisor-log-");
  const std::string home = root + "/codex";
  rt::fs::mkdirSync(home + "/sessions/2026", {.recursive = true});
  rt::fs::mkdirSync(home + "/archived_sessions", {.recursive = true});
  rt::fs::mkdirSync(root + "/outside", {.recursive = true});
  rt::fs::writeFileSync(home + "/sessions/2026/rollout-big-" + thread + ".jsonl", bigText);
  rt::fs::writeFileSync(home + "/archived_sessions/rollout-small-" + thread + ".jsonl", smallText);
  rt::fs::writeFileSync(home + "/sessions/empty-" + thread + ".jsonl", std::string_view());
  rt::fs::writeFileSync(home + "/sessions/oneline-" + thread + ".jsonl", std::string(300 * 1024, 'x'));
  rt::fs::writeFileSync(root + "/outside/escape-" + thread + ".jsonl", smallText);
  symlinkOrThrow(root + "/outside/escape-" + thread + ".jsonl", home + "/sessions/link-" + thread + ".jsonl");
  symlinkOrThrow(home + "/archived_sessions/rollout-small-" + thread + ".jsonl", home + "/sessions/inner-" + thread + ".jsonl");
  symlinkOrThrow(home + "/archived_sessions/rollout-small-" + thread + ".jsonl", home + "/sessions/renamed.jsonl");
  rt::fs::mkdirSync(home + "/sessions/dir-" + thread + ".jsonl");

  const std::vector<ReadSupervisorRolloutTailInput> inputs = {
    {std::nullopt, thread, json::value(home + "/sessions/2026/rollout-big-" + thread + ".jsonl")},
    {"", thread, json::value(home + "/sessions/2026/rollout-big-" + thread + ".jsonl")},
    {home, thread, json::value(home + "/sessions/2026/rollout-big-" + thread + ".jsonl")},
    {home + "//", thread, json::value("  " + home + "/archived_sessions/rollout-small-" + thread + ".jsonl\n")},
    {home, thread, json::value(home + "/sessions/empty-" + thread + ".jsonl")},
    {home, thread, json::value(home + "/sessions/oneline-" + thread + ".jsonl")},
    {home, thread, json::value(home + "/sessions/missing-" + thread + ".jsonl")},
    {home, thread, json::value(home + "/sessions/link-" + thread + ".jsonl")},
    {home, thread, json::value(home + "/sessions/inner-" + thread + ".jsonl")},
    {home, "renamed", json::value(home + "/sessions/renamed.jsonl")},
    {home, thread, json::value(home + "/sessions/dir-" + thread + ".jsonl")},
    {home, thread, json::value(5)},
    {home, thread, json::value(root + "/outside/escape-" + thread + ".jsonl")},
  };
  const json::array& expected = *rt::getArray(vectors, "results");
  EXPECT_EQ(inputs.size(), expected.size());
  for (std::size_t index = 0; index < inputs.size() && index < expected.size(); ++index) {
    const ReadSupervisorRolloutTailInput input = inputs[index];
    const std::vector<SupervisorLogEvent> events = co_await readSupervisorRolloutTail(input);
    const json::array& entry = expected[index].get_array();
    if (!rt::deepEqual(eventsJson(events), entry[1])) {
      EXPECT_EQ(rt::jsonStringify(eventsJson(events)), rt::jsonStringify(entry[1]));
      EXPECT_EQ(std::string(entry[0].get_string()), "case passed");
    }
  }
  rt::fs::rmSync(root, {.recursive = true, .force = true});
}

TEST("differential: resolveSupervisorRolloutPath") {
  const json::value vectors = rt::jsonParse(kSupervisorLogVectors);
  for (const json::value& entry : *rt::getArray(vectors, "resolves")) {
    const json::array& row = entry.get_array();
    const std::optional<std::string> actual = resolveSupervisorRolloutPath(std::string_view(row[0].get_string()), std::string_view(row[1].get_string()), &row[2]);
    const std::optional<std::string> expected = row[3].is_null() ? std::nullopt : std::optional<std::string>(std::string(row[3].get_string()));
    EXPECT_EQ(actual, expected);
    if (row[2].is_string()) EXPECT_EQ(resolveSupervisorRolloutPath(std::string_view(row[0].get_string()), std::string_view(row[1].get_string()), std::string_view(row[2].get_string())), expected);
  }
  EXPECT_EQ(resolveSupervisorRolloutPath("/h", "t", static_cast<const json::value*>(nullptr)), std::nullopt);
}

TEST("differential: parseSupervisorRolloutTail") {
  const json::value vectors = rt::jsonParse(kSupervisorLogVectors);
  for (const json::value& entry : *rt::getArray(vectors, "parsed")) {
    const json::array& row = entry.get_array();
    EXPECT_EQ(rt::jsonStringify(eventsJson(parseSupervisorRolloutTail(std::string_view(row[0].get_string())))), rt::jsonStringify(row[1]));
  }
}
