// src/provider/anthropic-response-wire.ts has no direct bun test: differential scenarios recorded
// from the TS under Bun 1.4.0 (tests/anthropic_response_wire_vectors.inc) and replayed here.
#include "uos/provider/anthropic_response_wire.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <vector>

#include "anthropic_response_wire_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
namespace wire = uos::provider::anthropic_response_wire;

namespace {
  json::array toArray(const std::vector<json::object>& events) {
    json::array array;
    for (const json::object& event : events) array.push_back(event);
    return array;
  }

  std::optional<json::value> member(const json::object& object, std::string_view key) {
    const json::value* value = rt::get(object, key);
    return value ? std::optional<json::value>(*value) : std::nullopt;
  }

  wire::WriterOptions optionsFrom(const json::object& source) {
    wire::WriterOptions options;
    options.model = std::string(*rt::getString(source, "model"));
    options.id = std::string(*rt::getString(source, "id"));
    options.created = *rt::getNumber(source, "created");
    if (const json::array* names = rt::getArray(source, "toolNames")) {
      rt::OrderedMap<std::string, wire::ClaudeToolName> map;
      for (const json::value& entry : *names) {
        const json::array& pair = entry.get_array();
        const json::object& tool = pair[1].get_object();
        const std::optional<std::string_view> space = rt::getString(tool, "namespace");
        map.set(std::string(pair[0].get_string()), wire::ClaudeToolName{std::string(*rt::getString(tool, "name")), space ? std::optional<std::string>(*space) : std::nullopt});
      }
      options.toolNames = std::move(map);
    }
    if (const json::array* custom = rt::getArray(source, "customToolNames")) {
      rt::OrderedSet<std::string> set;
      for (const json::value& name : *custom) set.add(std::string(name.get_string()));
      options.customToolNames = std::move(set);
    }
    return options;
  }
}

TEST("differential: createClaudeResponsesWriter scenarios") {
  const json::value scenarios = rt::jsonParse(kWireScenarios);
  ASSERT_EQ(scenarios.get_array().size(), std::size_t{8});
  for (const json::value& entry : scenarios.get_array()) {
    const json::object& scenario = *rt::getObject(entry, "scenario");
    const wire::ClaudeResponsesWriter writer = wire::createClaudeResponsesWriter(optionsFrom(*rt::getObject(scenario, "options")));
    json::array results;
    for (const json::value& opValue : *rt::getArray(scenario, "ops")) {
      const json::object& op = opValue.get_object();
      const std::string kind(*rt::getString(op, "op"));
      // Each result is serialized as soon as the operation returns, as the oracle recorded it.
      if (kind == "open") {
        results.push_back(json::value(rt::jsonStringify(toArray(writer.open()))));
      } else if (kind == "push") {
        results.push_back(json::value(rt::jsonStringify(toArray(writer.push(*rt::getObject(op, "delta"))))));
      } else if (kind == "finish") {
        std::vector<json::object> calls;
        for (const json::value& call : *rt::getArray(op, "calls")) calls.push_back(call.get_object());
        results.push_back(json::value(rt::jsonStringify(toArray(writer.finish(calls, member(op, "reason"), member(op, "usage"))))));
      } else if (kind == "observedUsage") {
        results.push_back(json::value(rt::jsonStringify(writer.observedUsage(member(op, "usage")))));
      } else {
        const std::optional<json::object> response = writer.response();
        results.push_back(json::value(rt::jsonStringify(response ? json::value(*response) : json::value(nullptr))));
      }
    }
    EXPECT_EQ(rt::jsonStringify(results), std::string(*rt::getString(entry, "results")));
  }
}

TEST("differential: claudeResponsesEnvelope echo handling") {
  const json::value expected = rt::jsonParse(kWireEnvelopes);
  std::vector<wire::ClaudeResponseEcho> echoes(4);
  echoes[1] = {"be brief", rt::jsonParse(R"([{"type":"function"}])"), rt::jsonParse(R"({"type":"auto"})"), json::value(false)};
  echoes[2] = {"", json::value(json::object{}), json::value(nullptr), json::value("yes")};
  echoes[3] = {std::nullopt, json::value(nullptr), json::value(0), json::value(true)};
  for (std::size_t index = 0; index < echoes.size(); ++index) {
    const json::object envelope = wire::claudeResponsesEnvelope("id1", "model1", 17.25, "queued", echoes[index]);
    EXPECT_EQ(rt::jsonStringify(envelope), std::string(expected.get_array()[index].get_string()));
  }
}

TEST("differential: encodeClaudeResponsesEvent") {
  const json::value expected = rt::jsonParse(kWireEvents);
  const std::vector<std::string> events = {R"({"type":"response.created","x":1})", "{\"a\":\" \"}", R"({"type":null})", R"({"type":[1,2]})", R"({"type":1.5})"};
  for (std::size_t index = 0; index < events.size(); ++index) {
    const json::value event = rt::jsonParse(events[index]);
    EXPECT_EQ(wire::encodeClaudeResponsesEvent(event.get_object()), std::string(expected.get_array()[index].get_string()));
  }
}
