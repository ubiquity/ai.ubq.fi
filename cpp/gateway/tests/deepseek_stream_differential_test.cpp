// Links: tests/deepseek_stream_differential_test.cpp src/deepseek/stream.cpp src/deepseek/index.cpp
//        src/inference_deadline.cpp src/config.cpp src/utils.cpp src/release.cpp
// Credential-free Bun 1.4.0 raw-SSE traces, independent of provider/stream-relay and the shared
// Responses translator suite. No sockets, credentials or provider dispatch are used.
#include "uos/config.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/deepseek/stream.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace stream = uos::deepseek::stream;
namespace {
#include "tests/deepseek_stream_differential_vectors.inc"
  const json::value& vectors() {
    static const json::value value = [] {
      const auto compressed = rt::fromBase64(STREAM_VECTORS_GZIP_BASE64); const auto bytes = rt::gzip::gunzipSync(compressed);
      return rt::jsonParse(rt::decodeUtf8(bytes));
    }(); return value;
  }
  struct CredentialScope {
    rt::env::ScopedEnvironment environment{{{"HOME", "/nonexistent/w05c8-isolated"}}};
    std::string saved = uos::config::config.codexAuthJsonB64;
    CredentialScope() { uos::config::config.codexAuthJsonB64.clear(); }
    ~CredentialScope() { uos::config::config.codexAuthJsonB64 = saved; }
  };
  json::value frameJson(const stream::DeepSeekStreamFrame& frame) {
    return std::visit([](const auto& value) -> json::value {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, stream::DeepSeekStreamFrameComment>) return json::object{{"kind", "comment"}, {"text", value.text}};
      else if constexpr (std::is_same_v<T, stream::DeepSeekStreamFrameChunk>) return json::object{{"kind", "chunk"}, {"value", value.value}};
      else return json::object{{"kind", "done"}};
    }, frame);
  }
  json::value describe(const rt::Reason& reason) {
    json::object value{{"name", rt::errorName(reason).value_or("Error")}, {"message", rt::errorMessage(reason).value_or(rt::reasonToString(reason))}};
    try { std::rethrow_exception(reason); } catch (const stream::DeepSeekStreamError& error) { value["kind"] = error.kind; } catch (...) {}
    const auto cause = rt::errorCause(reason);
    if (cause) {
      if (rt::isError(*cause)) value["cause"] = json::object{{"name", rt::errorName(*cause).value_or("Error")}, {"message", rt::errorMessage(*cause).value_or("")}};
      else if (const auto thrown = rt::thrownValue(*cause)) value["cause"] = *thrown;
    }
    return value;
  }
  struct Fixture {
    std::string mode;
    std::vector<rt::ByteChunk> chunks;
    std::size_t index = 0;
    int cancels = 0;
    bool hangs = false;
  };
  std::size_t currentTrace = 0;
  rt::Task<void> runTrace() {
    CredentialScope isolation;
    const auto& row = vectors().as_array()[currentTrace]; const std::string mode(*rt::getString(row, "mode")); const std::string name(*rt::getString(row, "name"));
    auto fixture = std::make_shared<Fixture>(); fixture->mode = mode;
    for (const auto& bytes : row.at("chunks").as_array()) fixture->chunks.push_back(rt::fromBase64(std::string_view(bytes.as_string())));
    if (mode == "oversize") fixture->chunks.push_back(rt::encodeUtf8(std::string(static_cast<std::size_t>(uos::deepseek::index::MAX_DEEPSEEK_SSE_FRAME_BYTES) + 1, 'x')));
    fixture->hangs = mode == "first_timeout" || mode == "inactivity_timeout" || mode == "abort_primitive" || mode == "abort_error" || mode == "abort_timeout";
    const rt::AbortController cancellation;
    if (mode == "abort_primitive") cancellation.abort(rt::thrown(json::value(7)));
    if (mode == "abort_error") cancellation.abort(rt::makeReason(rt::DOMException("fixture abort", "AbortError")));
    if (mode == "abort_timeout") cancellation.abort(rt::makeReason(rt::DOMException("fixture timeout", "TimeoutError")));
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [fixture](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
      if (fixture->mode == "read_error") { controller.error(rt::makeReason(rt::JsError("fixture read failed"))); return; }
      if (fixture->index < fixture->chunks.size()) { controller.enqueue(fixture->chunks[fixture->index++]); return; }
      if (!fixture->hangs) controller.close();
    };
    source.cancel = [fixture](rt::Reason) { ++fixture->cancels; };
    const rt::ByteStream body(source, {.highWaterMark = 0});
    const rt::Response response = mode == "no_body" ? rt::Response(nullptr) : rt::Response(rt::BodyInit(body));
    const stream::DeepSeekStreamOptions options{.signal = cancellation.signal(), .firstEventTimeoutMs = 20, .inactivityTimeoutMs = 5};
    const std::string requestedModel("deepseek-flash"); auto iterator = stream::iterateDeepSeekChatCompletionStream(response, requestedModel, options);
    json::array frames; json::value error;
    try {
      for (;;) {
        const auto step = co_await iterator.next(); if (step.done) break;
        frames.push_back(frameJson(*step.value));
        if (mode == "return_early") { co_await iterator.return_(); break; }
      }
    } catch (...) { error = describe(std::current_exception()); }
    const json::value actual = json::object{{"frames", frames}, {"error", error}, {"cancels", fixture->cancels}, {"locked", body.locked()}};
    json::value expected = row.at("expected");
    // Keep the recorded trace intact and project the C++ parser's diagnostic.
    if (name == "malformed JSON retains cause" || name == "data without colon is empty malformed JSON") {
      expected.as_object().at("error").as_object().at("cause").as_object().at("message") = "Invalid JSON";
    }
    if (!rt::deepEqual(actual, expected)) rt::test::report(__FILE__, __LINE__, name + "\nactual: " + rt::jsonStringify(actual) + "\nexpected: " + rt::jsonStringify(expected), false);
  }
  const bool registeredTraces = [] {
    const auto& traces = vectors().as_array();
    for (std::size_t index = 0; index < traces.size(); ++index) {
      const std::string name = "DeepSeek raw SSE Bun differential: " + std::string(*rt::getString(traces[index], "name"));
      rt::test::registry().push_back({name, [index] { currentTrace = index; rt::test::runAsync(runTrace); }});
    }
    return true;
  }();
}
