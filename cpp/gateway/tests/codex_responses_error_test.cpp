// Port of tests/codex-responses-error.test.ts.
// Links: src/codex/responses_error.cpp src/responses_failover_stream.cpp src/responses_stream.cpp src/utils.cpp
//        src/inference_deadline.cpp
#include "uos/codex/responses_error.hpp"
#include "uos/responses_failover_stream.hpp"
#include "uos/responses_stream.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using uos::responses_failover_stream::createOwnedResponsesStream;
using uos::responses_failover_stream::OwnedResponsesStreamOptions;
using uos::responses_failover_stream::responseEventFromValue;
using uos::responses_stream::ResponsesStreamEvent;
using uos::responses_stream::ResponsesStreamIterator;

namespace {
  json::object obj(std::string_view text) { return rt::jsonParse(text).as_object(); }

  const json::object& fixtureError() {
    static const json::object error = obj(R"({"code":"context_length_exceeded","message":"ISOLATED_FIXTURE_CONTEXT_OVERFLOW","param":"input"})");
    return error;
  }

  std::vector<json::object> values(const std::string& text) {
    static const rt::Regex DATA("^data: (.+)$", "gm");
    std::vector<json::object> parsed;
    for (const rt::RegexMatch& match : rt::matchAll(text, DATA)) parsed.push_back(rt::jsonParse(*match[1]).as_object());
    return parsed;
  }

  ResponsesStreamIterator iterator(std::vector<ResponsesStreamEvent> events) {
    return ResponsesStreamIterator([events](rt::Yield<ResponsesStreamEvent> yield) -> rt::Task<void> {
      rt::UnderlyingSource<ResponsesStreamEvent> source;
      source.start = [events](rt::ReadableStreamDefaultController<ResponsesStreamEvent> controller) {
        for (const ResponsesStreamEvent& item : events) controller.enqueue(item);
        controller.close();
      };
      const rt::ReadableStream<ResponsesStreamEvent> stream(std::move(source));
      const rt::AsyncGenerator<ResponsesStreamEvent> drained = stream.values();
      co_await yield.from(drained);
    });
  }

  rt::Task<std::string> textOf(rt::ByteStream stream) {
    const rt::Response response(rt::BodyInit{stream});
    std::string text = co_await response.text();
    co_return text;
  }

  std::optional<std::string> stringAt(const json::object& value, std::string_view key) {
    const std::optional<std::string_view> found = rt::getString(value, key);
    return found ? std::optional<std::string>(std::string(*found)) : std::nullopt;
  }

  rt::Task<void> contextErrorCase(bool nested, bool partial) {
    json::object value = obj(R"({"type":"error","sequence_number":2})");
    if (nested) {
      json::object error = rt::objectSpread(fixtureError());
      rt::objectDefine(error, "type", json::string("invalid_request_error"));
      rt::objectDefine(value, "error", std::move(error));
    } else {
      for (const json::key_value_pair* entry : rt::objectEntries(fixtureError())) rt::objectDefine(value, entry->key(), entry->value());
    }
    const ResponsesStreamEvent original = responseEventFromValue(std::move(value));
    auto observed = std::make_shared<std::vector<ResponsesStreamEvent>>();
    const json::object templateValue = obj(R"({"id":"resp_context","object":"response","status":"in_progress","model":"fixture","output":[]})");
    json::object created = obj(R"({"type":"response.created","sequence_number":0})");
    created["response"] = templateValue;
    std::vector<ResponsesStreamEvent> initial{responseEventFromValue(std::move(created))};
    if (partial)
      initial.push_back(
        responseEventFromValue(obj(R"({"type":"response.output_text.delta","sequence_number":1,"item_id":"msg_context","delta":"preserved partial"})")));
    OwnedResponsesStreamOptions options{
      .codexContextErrorCompatibility = true,
      .initial = initial,
      .iterator = iterator({original}),
      .responseId = "resp_context",
    };
    options.onEvent = [observed](ResponsesStreamEvent event) { observed->push_back(std::move(event)); };
    const rt::ByteStream stream = createOwnedResponsesStream(std::move(options));
    const std::string text = co_await textOf(stream);
    const std::vector<json::object> emitted = values(text);
    ASSERT(!emitted.empty());
    const json::object& failed = emitted.back();
    EXPECT_EQ(stringAt(failed, "type"), std::optional<std::string>("response.failed"));
    int failedCount = 0;
    bool forbidden = false;
    for (const json::object& event : emitted) {
      if (stringAt(event, "type") == std::optional<std::string>("response.failed")) failedCount += 1;
      if (stringAt(event, "type") == std::optional<std::string>("error") || stringAt(event, "type") == std::optional<std::string>("response.completed")) forbidden = true;
    }
    EXPECT_EQ(failedCount, 1);
    EXPECT_EQ(forbidden, false);
    const json::object& response = rt::get(failed, "response")->as_object();
    EXPECT_EQ(stringAt(response, "id"), stringAt(templateValue, "id"));
    EXPECT_EQ(stringAt(response, "model"), stringAt(templateValue, "model"));
    EXPECT_EQ(stringAt(response, "status"), std::optional<std::string>("failed"));
    json::object expectedError = rt::objectSpread(fixtureError());
    if (nested) rt::objectDefine(expectedError, "type", json::string("invalid_request_error"));
    EXPECT(rt::deepEqual(*rt::get(response, "error"), json::value(expectedError)));
    EXPECT_EQ(rt::jsonStringify(*rt::get(response, "output")).find("preserved partial") != std::string::npos, partial);
    ASSERT(!observed->empty());
    EXPECT_EQ(observed->back().identity(), original.identity());
    int terminalCount = 0;
    for (const ResponsesStreamEvent& event : *observed)
      if (event.terminal) terminalCount += 1;
    EXPECT_EQ(terminalCount, 1);
  }

  rt::Task<void> unchangedTerminalCase(bool compatibility, std::optional<std::string> responseId, std::string code) {
    json::object value = obj(R"({"type":"error","sequence_number":0})");
    for (const json::key_value_pair* entry : rt::objectEntries(fixtureError())) rt::objectDefine(value, entry->key(), entry->value());
    rt::objectDefine(value, "code", json::string(code));
    const ResponsesStreamEvent original = responseEventFromValue(std::move(value));
    const rt::ByteStream stream = createOwnedResponsesStream(OwnedResponsesStreamOptions{
      .codexContextErrorCompatibility = compatibility,
      .initial = {},
      .iterator = iterator({original}),
      .responseId = responseId,
    });
    const std::string text = co_await textOf(stream);
    EXPECT_EQ(text, original.raw);
  }
}

ASYNC_TEST("Codex context error becomes one failed response: nested=false, partial=false") { co_await contextErrorCase(false, false); }
ASYNC_TEST("Codex context error becomes one failed response: nested=false, partial=true") { co_await contextErrorCase(false, true); }
ASYNC_TEST("Codex context error becomes one failed response: nested=true, partial=false") { co_await contextErrorCase(true, false); }
ASYNC_TEST("Codex context error becomes one failed response: nested=true, partial=true") { co_await contextErrorCase(true, true); }

ASYNC_TEST("Unchanged terminal: compatibility=false, identity=resp_context, code=context_length_exceeded") {
  const std::optional<std::string> responseId = std::string("resp_context");
  const std::string code = std::string("context_length_exceeded");
  co_await unchangedTerminalCase(false, responseId, code);
}
ASYNC_TEST("Unchanged terminal: compatibility=true, identity=null, code=context_length_exceeded") {
  const std::optional<std::string> responseId = std::nullopt;
  const std::string code = std::string("context_length_exceeded");
  co_await unchangedTerminalCase(true, responseId, code);
}
ASYNC_TEST("Unchanged terminal: compatibility=true, identity=resp_context, code=provider_stream_error") {
  const std::optional<std::string> responseId = std::string("resp_context");
  const std::string code = std::string("provider_stream_error");
  co_await unchangedTerminalCase(true, responseId, code);
}
