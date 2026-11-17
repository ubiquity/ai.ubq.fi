#pragma once
// Port of src/provider/anthropic-response-wire.ts.
// Pure Responses envelopes and lifecycles: no provider routing imports.
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::provider::anthropic_response_wire {
  struct ClaudeToolName {
    std::string name = {};
    std::optional<std::string> namespace_ = std::nullopt;  // `namespace`
  };

  struct ClaudeResponseEcho {
    std::optional<std::string> instructions = std::nullopt;
    // `unknown` members: std::nullopt is undefined.
    std::optional<json::value> tools = std::nullopt;
    std::optional<json::value> tool_choice = std::nullopt;
    std::optional<json::value> parallel_tool_calls = std::nullopt;
  };

  std::string encodeClaudeResponsesEvent(const json::object& event);

  json::object claudeResponsesEnvelope(std::string_view id, std::string_view model, double created, std::string_view status, const ClaudeResponseEcho& echo);

  // The options of createClaudeResponsesWriter (`WriterOptions`).
  struct WriterOptions {
    std::string model = {};
    std::string id = {};
    double created = 0;
    std::optional<rt::OrderedMap<std::string, ClaudeToolName>> toolNames = std::nullopt;
    std::optional<rt::OrderedSet<std::string>> customToolNames = std::nullopt;
  };

  /** One active output item, with tools published after their arguments are complete. */
  // The object createClaudeResponsesWriter returns. Copies share the writer state.
  class ClaudeResponsesWriter {
  public:
    struct State;
    explicit ClaudeResponsesWriter(std::shared_ptr<State> state) : state_(std::move(state)) {}

    std::vector<json::object> open() const;
    std::vector<json::object> push(const json::object& delta) const;
    // `reason` and `usage` are unknown values: std::nullopt is undefined.
    std::vector<json::object> finish(const std::vector<json::object>& calls, const std::optional<json::value>& reason, const std::optional<json::value>& usage) const;
    json::object observedUsage(const std::optional<json::value>& usage) const;
    std::optional<json::object> response() const;

  private:
    std::shared_ptr<State> state_;
  };

  ClaudeResponsesWriter createClaudeResponsesWriter(WriterOptions options);
}
