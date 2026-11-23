// Links: tests/cerebras_differential_test.cpp src/provider/cerebras.cpp src/sentinel/upstream_capture.cpp src/utils.cpp
// src/provider/cerebras.ts beyond tests/cerebras-usage.test.ts (its other bun tests drive the
// Responses adapter and the HTTP handlers): differential vectors recorded from the TS under
// Bun 1.4.0 (tests/cerebras_differential_vectors.inc) for every pure export, then the transport's
// dispatch lifecycle and failure classification, mirrored from the LithosAI transport tests.
//
// Known divergence, not recorded here: a tool schema whose own `__proto__` member holds an object
// makes the projected copy inherit from it in JS (`projected.__proto__ = child` is the prototype
// setter), and the root-union collapse then reads `type`/`anyOf` through that prototype; rt models
// the setter (no own member) but not inherited reads (docs/rt/values.md).
#include "uos/provider/cerebras.hpp"

#include "uos/api_key_policy.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "cerebras_differential_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
namespace cerebras = uos::provider::cerebras;
namespace api_key_policy = uos::api_key_policy;
namespace upstream_capture = uos::sentinel::upstream_capture;

namespace {
  const json::array& vectors() {
    static const json::array all = [] {
      const std::vector<std::uint8_t> packed = rt::fromBase64(kCerebrasVectors);
      return rt::jsonParse(rt::decodeUtf8(rt::gzip::gunzipSync(packed))).as_array();
    }();
    return all;
  }

  // One recorded argument: a JSON text, or null for undefined.
  std::optional<json::value> argument(const json::array& args, std::size_t index) {
    const json::value& text = args.at(index);
    if (text.is_null()) return std::nullopt;
    return rt::jsonParse(text.get_string());
  }
  const json::value* pointer(const std::optional<json::value>& value) { return value ? &*value : nullptr; }

  std::string optionalString(const std::optional<std::string>& value) { return value ? rt::jsonQuote(*value) : "null"; }
  std::string optionalNumber(const std::optional<double>& value) { return value ? rt::jsonStringify(rt::makeNumber(*value)) : "null"; }
  std::string unknownResult(const std::optional<json::value>& value) { return value ? rt::jsonStringify(*value) : "$undefined"; }
  std::string result(const cerebras::NormalizationResult<json::object>& value) {
    json::object out;
    if (const auto* ok = std::get_if<cerebras::NormalizationResultOk<json::object>>(&value)) {
      out["ok"] = true;
      out["value"] = ok->value;
    } else {
      out["ok"] = false;
      out["message"] = std::get<cerebras::NormalizationResultNotOk>(value).message;
    }
    return rt::jsonStringify(json::value(std::move(out)));
  }
  // `{ threw: { name, message, code, status } }` as the oracle printed a throw.
  std::string thrown(const std::exception_ptr& error) {
    json::object fields;
    try {
      std::rethrow_exception(error);
    } catch (const cerebras::CerebrasError& cerebrasError) {
      fields["name"] = cerebrasError.name;
      fields["message"] = cerebrasError.message;
      fields["code"] = cerebrasError.code;
      fields["status"] = rt::makeNumber(cerebrasError.status);
    } catch (const rt::JsError& jsError) {
      fields["name"] = jsError.name;
      fields["message"] = jsError.message;
      fields["code"] = jsError.code ? json::value(*jsError.code) : json::value(nullptr);
      fields["status"] = nullptr;
    } catch (...) {
      fields["name"] = nullptr;
      fields["message"] = rt::reasonToString(error);
      fields["code"] = nullptr;
      fields["status"] = nullptr;
    }
    json::object out;
    out["threw"] = std::move(fields);
    return rt::jsonStringify(json::value(std::move(out)));
  }

  rt::Response responseWithHeaders(const json::value& pairs) {
    rt::Headers headers;
    for (const json::value& pair : pairs.as_array()) headers.append(pair.as_array()[0].as_string(), pair.as_array()[1].as_string());
    rt::ResponseInit init;
    init.headers = headers;
    return rt::Response(rt::BodyInit(std::string("{}")), init);
  }

  std::string run(std::string_view fn, const json::array& args) {
    if (fn == "cerebrasUpstreamModelFor") return optionalString(cerebras::cerebrasUpstreamModelFor(std::string(argument(args, 0)->as_string())));
    if (fn == "normalizeCerebrasProviderRequestId") return optionalString(cerebras::normalizeCerebrasProviderRequestId(pointer(argument(args, 0))));
    if (fn == "getCerebrasProviderRequestId") return optionalString(cerebras::getCerebrasProviderRequestId(responseWithHeaders(*argument(args, 0))));
    if (fn == "projectCerebrasToolSchema") return unknownResult(cerebras::projectCerebrasToolSchema(pointer(argument(args, 0))));
    if (fn == "collapseCerebrasSystemMessages") return unknownResult(cerebras::collapseCerebrasSystemMessages(pointer(argument(args, 0))));
    if (fn == "projectCerebrasRequest") return rt::jsonStringify(json::value(cerebras::projectCerebrasRequest(argument(args, 0)->as_object())));
    if (fn == "normalizeCerebrasChatCompletion") return result(cerebras::normalizeCerebrasChatCompletion(pointer(argument(args, 0)), std::string(argument(args, 1)->as_string())));
    if (fn == "cerebrasCachedPromptTokens") return optionalNumber(cerebras::cerebrasCachedPromptTokens(argument(args, 0)->as_object(), rt::toNumber(*argument(args, 1))));
    if (fn == "cerebrasReasoningTokens") return optionalNumber(cerebras::cerebrasReasoningTokens(argument(args, 0)->as_object(), rt::toNumber(*argument(args, 1))));
    return "unknown fn " + std::string(fn);
  }

  // Replays every vector of `fn`, reporting each mismatch with its input.
  void replay(std::string_view fn, std::size_t expectedCount) {
    std::size_t count = 0;
    for (const json::value& entry : vectors()) {
      const json::object& vector = entry.as_object();
      if (vector.at("fn").as_string() != fn) continue;
      ++count;
      const json::array& args = vector.at("args").as_array();
      std::string actual;
      try {
        actual = run(fn, args);
      } catch (...) {
        actual = thrown(std::current_exception());
      }
      const std::string expected(vector.at("out").as_string());
      if (actual != expected) {
        rt::test::report(__FILE__, __LINE__, std::string(fn) + rt::jsonStringify(json::value(args)) + "\n      actual:   " + actual + "\n      expected: " + expected, false);
      }
    }
    EXPECT_EQ(count, expectedCount);
  }

  using Dispatch = api_key_policy::ApiKeyProviderDispatch;
  rt::Task<std::optional<Dispatch>> resolvedDispatch(std::optional<Dispatch> dispatch) { return rt::fulfilledTask(std::move(dispatch)); }

  template <class T> rt::Task<rt::Reason> rejectionOf(rt::Task<T> task) {
    rt::Reason failure;
    try {
      co_await std::move(task);
    } catch (...) {
      failure = std::current_exception();
    }
    co_return failure;
  }

  bool isCerebrasError(const rt::Reason& error, std::string_view code, double status) {
    if (!error) return false;
    try {
      std::rethrow_exception(error);
    } catch (const cerebras::CerebrasError& cerebrasError) {
      return cerebrasError.code == code && cerebrasError.status == status;
    } catch (...) {
      return false;
    }
  }

  rt::Task<rt::Response> rejectOnAbort(std::optional<rt::AbortSignal> signal) {
    const rt::Promise<rt::Response> pending = rt::newPromise<rt::Response>([&signal](const rt::Resolve<rt::Response>&, const rt::Reject& reject) {
      const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = signal->state();
      const rt::EventListener rejectWithReason([reject, weakSignal] {
        if (const std::shared_ptr<rt::detail::AbortSignalState> state = weakSignal.lock()) reject(rt::AbortSignal(state).reason());
      });
      if (signal->aborted()) rejectWithReason();
      else signal->addEventListener("abort", rejectWithReason, {.once = true});
    });
    rt::Response response = co_await pending.wait();
    co_return response;
  }
}

TEST("differential: cerebrasUpstreamModelFor matches Bun") { replay("cerebrasUpstreamModelFor", 8); }
TEST("differential: normalizeCerebrasProviderRequestId matches Bun") { replay("normalizeCerebrasProviderRequestId", 12); }
TEST("differential: getCerebrasProviderRequestId matches Bun") { replay("getCerebrasProviderRequestId", 7); }
TEST("differential: projectCerebrasToolSchema matches Bun") { replay("projectCerebrasToolSchema", 349); }
TEST("differential: collapseCerebrasSystemMessages matches Bun") { replay("collapseCerebrasSystemMessages", 18); }
TEST("differential: projectCerebrasRequest matches Bun") { replay("projectCerebrasRequest", 474); }
TEST("differential: normalizeCerebrasChatCompletion matches Bun") { replay("normalizeCerebrasChatCompletion", 504); }
TEST("differential: cerebrasCachedPromptTokens matches Bun") { replay("cerebrasCachedPromptTokens", 16); }
TEST("differential: cerebrasReasoningTokens matches Bun") { replay("cerebrasReasoningTokens", 16); }

TEST("cerebras: the API key is read from CEREBRAS_API_KEY, trimmed, and a blank value is absent") {
  {
    const rt::env::ScopedOverride environment{{"CEREBRAS_API_KEY", std::optional<std::string>("\tcsk-test ")}};
    EXPECT_EQ(cerebras::readCerebrasApiKey(), std::optional<std::string>("csk-test"));
  }
  const rt::env::ScopedOverride environment{{"CEREBRAS_API_KEY", std::optional<std::string>()}};
  EXPECT_EQ(cerebras::readCerebrasApiKey(), std::optional<std::string>());
}

ASYNC_TEST("cerebras transport: a dispatch sends the projected body after admission and reports its lifecycle in order") {
  const rt::env::ScopedOverride environment{{"CEREBRAS_API_KEY", std::optional<std::string>("csk-env")}};
  std::vector<std::string> events;
  std::optional<rt::RequestInit> dispatchedInit;
  std::optional<json::value> dispatchedBody;
  const cerebras::CerebrasFetch fetcher([&](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
    events.push_back("fetch:" + std::get<std::string>(input));
    dispatchedBody = rt::jsonParse(std::get<std::string>(*init.body));
    dispatchedInit = init;
    rt::ResponseInit responseInit;
    responseInit.status = 200;
    responseInit.headers = rt::Headers{{"X-Cerebras-Request-Id", "cb-req-1"}};
    return rt::Response::json(rt::jsonParse(R"({"id":"x"})"), responseInit);
  });
  Dispatch dispatch;
  dispatch.markTransportStarted = [&events] { events.push_back("markTransportStarted"); };
  dispatch.cancelBeforeTransport = [&events] {
    events.push_back("cancelBeforeTransport");
    return rt::fulfilledTask();
  };
  const upstream_capture::SentinelUpstreamRecorder recorder = upstream_capture::createSentinelUpstreamRecorder();
  cerebras::CerebrasChatCompletionsOptions options;
  options.fetcher = fetcher;
  options.beforeDispatch = [&events, dispatch] {
    events.push_back("beforeDispatch");
    return resolvedDispatch(dispatch);
  };
  options.onDispatch = [&events] { events.push_back("onDispatch"); };
  options.onHeaders = [&events] { events.push_back("onHeaders"); };
  options.sentinelUpstreamRecorder = recorder;
  const json::object body = rt::jsonParse(R"({"model":"qwen-3.8-27b","messages":[{"role":"user","content":"u"},{"role":"developer","content":"d"},)"
                                          R"({"role":"assistant","content":"a","reasoning_content":"r"}]})")
                              .as_object();
  auto fetching = cerebras::fetchCerebrasChatCompletions(body, options);
  const rt::Response response = co_await std::move(fetching);

  EXPECT_EQ(response.status(), 200);
  const std::vector<std::string> expectedEvents{"beforeDispatch", "markTransportStarted", "onDispatch", "fetch:" + std::string(cerebras::CEREBRAS_CHAT_COMPLETIONS_URL),
                                                "onHeaders"};
  EXPECT_EQ(events, expectedEvents);
  ASSERT(dispatchedBody.has_value());
  EXPECT_EQ(rt::jsonStringify(*dispatchedBody),
            std::string(R"({"model":"qwen-3.8-27b","messages":[{"role":"system","content":"d"},{"role":"user","content":"u"},{"role":"assistant","content":"a"}]})"));
  ASSERT(dispatchedInit.has_value());
  const rt::Headers headers(dispatchedInit->headers.value_or(rt::Headers()));
  EXPECT_EQ(headers.get("Authorization"), std::optional<std::string>("Bearer csk-env"));
  EXPECT_EQ(headers.get("Content-Type"), std::optional<std::string>("application/json"));
  EXPECT_EQ(headers.get("Accept"), std::optional<std::string>("application/json"));
  EXPECT_EQ(dispatchedInit->method, std::optional<std::string>("POST"));
  EXPECT_EQ(dispatchedInit->redirect, std::optional<std::string>("manual"));
  EXPECT(dispatchedInit->signal.has_value());
  EXPECT_EQ(cerebras::getCerebrasProviderRequestId(response), std::optional<std::string>("cb-req-1"));
  auto reading = response.text();
  const std::string text = co_await std::move(reading);
  EXPECT_EQ(text, std::string(R"({"id":"x"})"));
  const upstream_capture::SentinelUpstreamTrace trace = recorder.snapshotAndSeal();
  ASSERT(!trace.attempts.empty());
  EXPECT_EQ(trace.attempts[0].provider, std::string("cerebras"));
  EXPECT_EQ(trace.attempts[0].terminal, std::string("eof"));
  recorder.dispose();

  // Unsupported system content is the projection's own client error, before any dispatch.
  events.clear();
  const json::object badSystem = rt::jsonParse(R"({"messages":[{"role":"user","content":"u"},{"role":"system","content":[{"image_url":"x"}]}]})").as_object();
  auto badSystemTask = cerebras::fetchCerebrasChatCompletions(badSystem, options);
  auto badSystemSettled = rejectionOf(std::move(badSystemTask));
  const rt::Reason badSystemFailure = co_await std::move(badSystemSettled);
  EXPECT(isCerebrasError(badSystemFailure, "cerebras_request_invalid", 400));
  EXPECT(events.empty());

  // A null or blank key is not configured, even with the environment variable set.
  for (int blank = 0; blank < 2; ++blank) {
    cerebras::CerebrasChatCompletionsOptions keyed;
    if (blank) keyed.apiKey = std::string(" ");
    else keyed.apiKey = nullptr;
    keyed.fetcher = fetcher;
    auto keyedTask = cerebras::fetchCerebrasChatCompletions(body, keyed);
    auto keyedSettled = rejectionOf(std::move(keyedTask));
    const rt::Reason keyedFailure = co_await std::move(keyedSettled);
    EXPECT(isCerebrasError(keyedFailure, "cerebras_api_key_missing", 503));
  }
  EXPECT(events.empty());
}

ASYNC_TEST("cerebras transport: classifies unreachable, deadline, caller abort and quota failures") {
  const json::object body = rt::jsonParse(R"({"messages":[]})").as_object();
  const upstream_capture::SentinelUpstreamRecorder failedRecorder = upstream_capture::createSentinelUpstreamRecorder();
  cerebras::CerebrasChatCompletionsOptions unreachable;
  unreachable.apiKey = std::string("csk");
  unreachable.fetcher = cerebras::CerebrasFetch(
    [](rt::RequestInfo, rt::RequestInit) { return rt::Promise<rt::Response>::reject(rt::makeReason(rt::TypeError("socket closed"))).wait(); });
  unreachable.sentinelUpstreamRecorder = failedRecorder;
  auto unreachableTask = cerebras::fetchCerebrasChatCompletions(body, unreachable);
  auto unreachableSettled = rejectionOf(std::move(unreachableTask));
  const rt::Reason unreachableFailure = co_await std::move(unreachableSettled);
  EXPECT(isCerebrasError(unreachableFailure, "cerebras_upstream_unreachable", 502));
  const upstream_capture::SentinelUpstreamTrace failedTrace = failedRecorder.snapshotAndSeal();
  ASSERT(!failedTrace.attempts.empty());
  EXPECT_EQ(failedTrace.attempts[0].terminal, std::string("fetch_error"));
  failedRecorder.dispose();

  {
    cerebras::setCerebrasFetchTimeoutMsForTest(10);
    const auto restore = rt::scopeExit([] { cerebras::setCerebrasFetchTimeoutMsForTest(std::nullopt); });
    cerebras::CerebrasChatCompletionsOptions deadline;
    deadline.apiKey = std::string("csk");
    deadline.fetcher = cerebras::CerebrasFetch([](rt::RequestInfo, rt::RequestInit init) { return rejectOnAbort(init.signal); });
    auto deadlineTask = cerebras::fetchCerebrasChatCompletions(body, deadline);
    auto deadlineSettled = rejectionOf(std::move(deadlineTask));
    const rt::Reason deadlineFailure = co_await std::move(deadlineSettled);
    EXPECT(isCerebrasError(deadlineFailure, "gateway_timeout", 504));
  }

  const rt::Reason abortReason = rt::makeReason(rt::DOMException("client disconnected", "AbortError"));
  const rt::AbortController controller;
  cerebras::CerebrasChatCompletionsOptions callerAbort;
  callerAbort.apiKey = std::string("csk");
  callerAbort.signal = controller.signal();
  callerAbort.fetcher = cerebras::CerebrasFetch([controller, abortReason](rt::RequestInfo, rt::RequestInit init) {
    controller.abort(abortReason);
    return rejectOnAbort(init.signal);
  });
  auto callerAbortTask = cerebras::fetchCerebrasChatCompletions(body, callerAbort);
  EXPECT_REJECTS_REASON(std::move(callerAbortTask), abortReason);

  int cancelled = 0;
  int fetchCalls = 0;
  const rt::AbortController preAborted;
  preAborted.abort(abortReason);
  cerebras::CerebrasChatCompletionsOptions preAbortedOptions;
  preAbortedOptions.apiKey = std::string("csk");
  preAbortedOptions.signal = preAborted.signal();
  preAbortedOptions.beforeDispatch = [&cancelled] {
    Dispatch dispatch;
    dispatch.markTransportStarted = [] { throw rt::JsError("a cancelled request must not start transport"); };
    dispatch.cancelBeforeTransport = [&cancelled] {
      cancelled += 1;
      return rt::fulfilledTask();
    };
    return resolvedDispatch(std::move(dispatch));
  };
  preAbortedOptions.fetcher = cerebras::CerebrasFetch([&fetchCalls](rt::RequestInfo, rt::RequestInit) {
    fetchCalls += 1;
    return rt::Response::json(json::value(json::object()));
  });
  auto preAbortedTask = cerebras::fetchCerebrasChatCompletions(body, preAbortedOptions);
  EXPECT_REJECTS_REASON(std::move(preAbortedTask), abortReason);
  EXPECT_EQ(cancelled, 1);
  EXPECT_EQ(fetchCalls, 0);

  const rt::Reason quota = rt::makeReason(api_key_policy::ApiKeyQuotaDispatchError("quota reservation is gone"));
  cerebras::CerebrasChatCompletionsOptions quotaOptions;
  quotaOptions.apiKey = std::string("csk");
  quotaOptions.beforeDispatch = [quota]() -> rt::Task<std::optional<Dispatch>> { rt::rethrow(quota); };
  quotaOptions.fetcher = preAbortedOptions.fetcher;
  auto quotaTask = cerebras::fetchCerebrasChatCompletions(body, quotaOptions);
  EXPECT_REJECTS_REASON(std::move(quotaTask), quota);
  EXPECT_EQ(fetchCalls, 0);
}

TEST("cerebras: a property named `properties` has its own schema projected as a properties map (TS quirk, Bun output)") {
  const json::value schema = rt::jsonParse(
    R"({"type":"object","properties":{"properties":{"type":"string","format":"date","pattern":"x"},"other":{"type":"string","format":"date"}}})");
  EXPECT_EQ(rt::jsonStringify(cerebras::projectCerebrasToolSchema(schema)),
            std::string(R"({"type":"object","properties":{"properties":{"type":"string","format":"date","pattern":"x"},"other":{"type":"string"}}})"));
}
