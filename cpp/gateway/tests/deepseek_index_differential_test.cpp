// Links: tests/deepseek_index_differential_test.cpp src/deepseek/index.cpp src/utils.cpp
// src/deepseek/index.ts has no direct bun test runnable here (its tests drive the Responses
// translation and the HTTP handlers): differential vectors recorded from the TS under Bun 1.4.0
// (tests/deepseek_index_differential_vectors.inc) for every pure export, then the transport's
// dispatch lifecycle, failure classification and projection observer, mirrored from the LithosAI
// transport tests (the three transports share that code shape).
#include "uos/deepseek/index.hpp"

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

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "deepseek_index_differential_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
namespace ds = uos::deepseek::index;
namespace api_key_policy = uos::api_key_policy;

namespace {
  const json::array& vectors() {
    static const json::array all = [] {
      const std::vector<std::uint8_t> packed = rt::fromBase64(kDeepSeekIndexVectors);
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

  std::string quote(std::string_view text) { return rt::jsonQuote(text); }
  std::string optionalString(const std::optional<std::string>& value) { return value ? quote(*value) : "null"; }
  std::string optionalNumber(const std::optional<double>& value) { return value ? rt::jsonStringify(rt::makeNumber(*value)) : "null"; }
  std::string boolean(bool value) { return value ? "true" : "false"; }
  std::string result(const ds::NormalizationResult<json::object>& value) {
    json::object out;
    if (const auto* ok = std::get_if<ds::NormalizationResultOk<json::object>>(&value)) {
      out["ok"] = true;
      out["value"] = ok->value;
    } else {
      out["ok"] = false;
      out["message"] = std::get<ds::NormalizationResultNotOk>(value).message;
    }
    return rt::jsonStringify(json::value(std::move(out)));
  }
  std::string disposition(const ds::DeepSeekFinishDisposition& value) {
    json::object out;
    std::visit([&out](const auto& arm) { out["kind"] = arm.kind; }, value);
    if (const auto* incomplete = std::get_if<ds::DeepSeekFinishDispositionIncomplete>(&value)) out["reason"] = incomplete->reason;
    if (const auto* failed = std::get_if<ds::DeepSeekFinishDispositionFailed>(&value)) out["code"] = failed->code;
    if (const auto* unknown = std::get_if<ds::DeepSeekFinishDispositionUnknown>(&value)) out["value"] = unknown->value;
    return rt::jsonStringify(json::value(std::move(out)));
  }
  // `{ threw: { name, message, code, status } }` as the oracle printed a throw.
  std::string thrown(const std::exception_ptr& error) {
    json::object fields;
    try {
      std::rethrow_exception(error);
    } catch (const ds::DeepSeekError& deepSeekError) {
      fields["name"] = deepSeekError.name;
      fields["message"] = deepSeekError.message;
      fields["code"] = deepSeekError.code;
      fields["status"] = rt::makeNumber(deepSeekError.status);
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
    if (fn == "deepSeekThinkingModeActive") return boolean(ds::deepSeekThinkingModeActive(pointer(argument(args, 0)), pointer(argument(args, 1))));
    if (fn == "deepSeekThinkingModeForbiddenToolChoice") return optionalString(ds::deepSeekThinkingModeForbiddenToolChoice(pointer(argument(args, 0))));
    if (fn == "deepSeekThinkingToolChoiceConflict") {
      return optionalString(ds::deepSeekThinkingToolChoiceConflict(pointer(argument(args, 0)), pointer(argument(args, 1)), pointer(argument(args, 2))));
    }
    if (fn == "deepSeekToolChoiceThinkingConflictMessage") {
      return quote(ds::deepSeekToolChoiceThinkingConflictMessage(std::string(argument(args, 0)->as_string()), std::string(argument(args, 1)->as_string())));
    }
    if (fn == "deepSeekFinishDisposition") return disposition(ds::deepSeekFinishDisposition(pointer(argument(args, 0))));
    if (fn == "normalizeDeepSeekProviderRequestId") return optionalString(ds::normalizeDeepSeekProviderRequestId(pointer(argument(args, 0))));
    if (fn == "getDeepSeekProviderRequestId") return optionalString(ds::getDeepSeekProviderRequestId(responseWithHeaders(*argument(args, 0))));
    if (fn == "deepSeekUpstreamModelFor") return optionalString(ds::deepSeekUpstreamModelFor(std::string(argument(args, 0)->as_string())));
    if (fn == "projectDeepSeekReasoningEffort") {
      const std::optional<json::value> effort = ds::projectDeepSeekReasoningEffort(std::string(argument(args, 0)->as_string()));
      if (!effort) return quote("$function");
      if (effort->is_object()) return quote("$Object.prototype");
      return rt::jsonStringify(*effort);
    }
    if (fn == "deepSeekDefaultOutputAllowance") return optionalNumber(ds::deepSeekDefaultOutputAllowance(std::string(argument(args, 0)->as_string())));
    if (fn == "projectDeepSeekRequest") {
      return rt::jsonStringify(json::value(ds::projectDeepSeekRequest(argument(args, 0)->as_object(), std::string(argument(args, 1)->as_string()))));
    }
    if (fn == "deepSeekCachedPromptTokens") return optionalNumber(ds::deepSeekCachedPromptTokens(argument(args, 0)->as_object(), rt::toNumber(*argument(args, 1))));
    if (fn == "deepSeekReasoningTokens") return optionalNumber(ds::deepSeekReasoningTokens(argument(args, 0)->as_object(), rt::toNumber(*argument(args, 1))));
    if (fn == "normalizeDeepSeekChatCompletion") return result(ds::normalizeDeepSeekChatCompletion(pointer(argument(args, 0)), std::string(argument(args, 1)->as_string())));
    if (fn == "normalizeDeepSeekChatCompletionChunk") {
      return result(ds::normalizeDeepSeekChatCompletionChunk(pointer(argument(args, 0)), std::string(argument(args, 1)->as_string())));
    }
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

  bool isDeepSeekError(const rt::Reason& error, std::string_view code, double status) {
    if (!error) return false;
    try {
      std::rethrow_exception(error);
    } catch (const ds::DeepSeekError& deepSeekError) {
      return deepSeekError.code == code && deepSeekError.status == status;
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

TEST("differential: deepSeekThinkingModeActive matches Bun") { replay("deepSeekThinkingModeActive", 168); }
TEST("differential: deepSeekThinkingModeForbiddenToolChoice matches Bun") { replay("deepSeekThinkingModeForbiddenToolChoice", 24); }
TEST("differential: deepSeekThinkingToolChoiceConflict matches Bun") { replay("deepSeekThinkingToolChoiceConflict", 75); }
TEST("differential: deepSeekToolChoiceThinkingConflictMessage matches Bun") { replay("deepSeekToolChoiceThinkingConflictMessage", 4); }
TEST("differential: deepSeekFinishDisposition matches Bun") { replay("deepSeekFinishDisposition", 18); }
TEST("differential: normalizeDeepSeekProviderRequestId matches Bun") { replay("normalizeDeepSeekProviderRequestId", 18); }
TEST("differential: getDeepSeekProviderRequestId matches Bun") { replay("getDeepSeekProviderRequestId", 11); }
TEST("differential: deepSeekUpstreamModelFor matches Bun") { replay("deepSeekUpstreamModelFor", 13); }
TEST("differential: projectDeepSeekReasoningEffort matches Bun, inherited Object.prototype names included") { replay("projectDeepSeekReasoningEffort", 23); }
TEST("differential: deepSeekDefaultOutputAllowance matches Bun") { replay("deepSeekDefaultOutputAllowance", 23); }
TEST("differential: projectDeepSeekRequest matches Bun") { replay("projectDeepSeekRequest", 506); }
TEST("differential: deepSeekCachedPromptTokens matches Bun") { replay("deepSeekCachedPromptTokens", 48); }
TEST("differential: deepSeekReasoningTokens matches Bun") { replay("deepSeekReasoningTokens", 48); }
TEST("differential: normalizeDeepSeekChatCompletion matches Bun") { replay("normalizeDeepSeekChatCompletion", 476); }
TEST("differential: normalizeDeepSeekChatCompletionChunk matches Bun") { replay("normalizeDeepSeekChatCompletionChunk", 407); }

TEST("deepseek: isTimeoutError reads the name of an Error only") {
  EXPECT(ds::isTimeoutError(rt::makeReason(rt::DOMException("late", "TimeoutError"))));
  rt::JsError named("late");
  named.name = "TimeoutError";
  EXPECT(ds::isTimeoutError(rt::makeReason(named)));
  EXPECT(!ds::isTimeoutError(rt::makeReason(rt::DOMException("gone", "AbortError"))));
  EXPECT(!ds::isTimeoutError(rt::makeReason(rt::TypeError("TimeoutError"))));
  EXPECT(!ds::isTimeoutError(rt::thrown(json::value("TimeoutError"))));
  EXPECT(!ds::isTimeoutError(rt::thrown(rt::jsonParse(R"({"name":"TimeoutError"})"))));
  EXPECT(!ds::isTimeoutError(nullptr));
}

TEST("deepseek: the API key is read from DEEPSEEK_API_KEY, trimmed, and a blank value is absent") {
  {
    const rt::env::ScopedOverride environment{{"DEEPSEEK_API_KEY", std::optional<std::string>("  sk-test  ")}};
    EXPECT_EQ(ds::readDeepSeekApiKey(), std::optional<std::string>("sk-test"));
  }
  {
    const rt::env::ScopedOverride environment{{"DEEPSEEK_API_KEY", std::optional<std::string>("   ")}};
    EXPECT_EQ(ds::readDeepSeekApiKey(), std::optional<std::string>());
  }
  const rt::env::ScopedOverride environment{{"DEEPSEEK_API_KEY", std::optional<std::string>()}};
  EXPECT_EQ(ds::readDeepSeekApiKey(), std::optional<std::string>());
}

ASYNC_TEST("deepseek transport: observes the projected body, then dispatches it after admission, in order") {
  const rt::env::ScopedOverride environment{{"DEEPSEEK_API_KEY", std::optional<std::string>("sk-env")}};
  std::vector<std::string> events;
  std::optional<rt::RequestInit> dispatchedInit;
  std::optional<json::value> dispatchedBody;
  std::optional<json::object> observedBody;
  const ds::DeepSeekFetch fetcher([&](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
    events.push_back("fetch:" + std::get<std::string>(input));
    dispatchedBody = rt::jsonParse(std::get<std::string>(*init.body));
    dispatchedInit = init;
    rt::ResponseInit responseInit;
    responseInit.status = 200;
    responseInit.headers = rt::Headers{{"X-Ds-Request-Id", "ds-req-1"}};
    return rt::Response::json(rt::jsonParse(R"({"id":"x"})"), responseInit);
  });
  Dispatch dispatch;
  dispatch.markTransportStarted = [&events] { events.push_back("markTransportStarted"); };
  dispatch.cancelBeforeTransport = [&events] {
    events.push_back("cancelBeforeTransport");
    return rt::fulfilledTask();
  };
  ds::DeepSeekChatCompletionsOptions options;
  options.fetcher = fetcher;
  options.onProjectedBody = [&](const json::object& body) {
    events.push_back("onProjectedBody");
    observedBody = body;
  };
  options.beforeDispatch = [&events, dispatch] {
    events.push_back("beforeDispatch");
    return resolvedDispatch(dispatch);
  };
  options.onDispatch = [&events] { events.push_back("onDispatch"); };
  options.onHeaders = [&events] { events.push_back("onHeaders"); };
  const json::object body = rt::jsonParse(
                              R"({"model":"x","messages":[{"role":"developer","content":"d"}],"max_completion_tokens":32,"reasoning_effort":"Ultra","stream":true})")
                              .as_object();
  auto fetching = ds::fetchDeepSeekChatCompletions(body, "DeepSeek-V4-Flash", options);
  const rt::Response response = co_await std::move(fetching);

  EXPECT_EQ(response.status(), 200);
  const std::vector<std::string> expectedEvents{"onProjectedBody", "beforeDispatch", "markTransportStarted", "onDispatch",
                                                "fetch:" + std::string(ds::DEEPSEEK_CHAT_COMPLETIONS_URL), "onHeaders"};
  EXPECT_EQ(events, expectedEvents);
  const json::value projected =
    rt::jsonParse(R"({"model":"deepseek-flash","messages":[{"role":"system","content":"d"}],"reasoning_effort":"max","stream":true,"max_tokens":32})");
  ASSERT(observedBody.has_value() && dispatchedBody.has_value());
  EXPECT(rt::deepEqual(json::value(*observedBody), projected));
  EXPECT(rt::deepEqual(*dispatchedBody, projected));
  EXPECT_EQ(rt::jsonStringify(*dispatchedBody), rt::jsonStringify(projected));
  ASSERT(dispatchedInit.has_value());
  const rt::Headers headers(dispatchedInit->headers.value_or(rt::Headers()));
  EXPECT_EQ(headers.get("Authorization"), std::optional<std::string>("Bearer sk-env"));
  EXPECT_EQ(headers.get("Content-Type"), std::optional<std::string>("application/json"));
  EXPECT_EQ(headers.get("Accept"), std::optional<std::string>("application/json"));
  EXPECT_EQ(dispatchedInit->method, std::optional<std::string>("POST"));
  EXPECT_EQ(dispatchedInit->redirect, std::optional<std::string>("manual"));
  EXPECT(dispatchedInit->signal.has_value());
  EXPECT_EQ(ds::getDeepSeekProviderRequestId(response), std::optional<std::string>("ds-req-1"));

  // The projection's own error is rethrown unchanged; nothing is observed or dispatched.
  events.clear();
  auto unknownModel = ds::fetchDeepSeekChatCompletions(body, "deepseek-v4.1-flash", options);
  auto unknownSettled = rejectionOf(std::move(unknownModel));
  const rt::Reason unknownFailure = co_await std::move(unknownSettled);
  EXPECT(isDeepSeekError(unknownFailure, "deepseek_request_invalid", 400));
  EXPECT(events.empty());

  // A body JSON.stringify refuses becomes the JSON-serializable error before any key work.
  json::value nested = json::object();
  for (std::size_t level = 0; level <= rt::kJsonMaxDepth; ++level) {
    json::object wrapper;
    wrapper["self"] = std::move(nested);
    nested = std::move(wrapper);
  }
  auto deep = ds::fetchDeepSeekChatCompletions(nested.as_object(), "deepseek-flash", options);
  auto deepSettled = rejectionOf(std::move(deep));
  const rt::Reason deepFailure = co_await std::move(deepSettled);
  EXPECT(isDeepSeekError(deepFailure, "deepseek_request_invalid", 400));
  EXPECT(events.empty());

  // A null key is not configured, even with the environment variable set.
  ds::DeepSeekChatCompletionsOptions nullKey;
  nullKey.apiKey = nullptr;
  nullKey.fetcher = fetcher;
  auto nullKeyTask = ds::fetchDeepSeekChatCompletions(body, "deepseek-flash", nullKey);
  auto nullKeySettled = rejectionOf(std::move(nullKeyTask));
  const rt::Reason nullKeyFailure = co_await std::move(nullKeySettled);
  EXPECT(isDeepSeekError(nullKeyFailure, "deepseek_api_key_missing", 503));
}

ASYNC_TEST("deepseek transport: classifies unreachable, deadline, caller abort and quota failures") {
  const json::object body = rt::jsonParse(R"({"messages":[]})").as_object();
  ds::DeepSeekChatCompletionsOptions unreachable;
  unreachable.apiKey = std::string("sk");
  unreachable.fetcher = ds::DeepSeekFetch(
    [](rt::RequestInfo, rt::RequestInit) { return rt::Promise<rt::Response>::reject(rt::makeReason(rt::TypeError("socket closed"))).wait(); });
  auto unreachableTask = ds::fetchDeepSeekChatCompletions(body, "deepseek-flash", unreachable);
  auto unreachableSettled = rejectionOf(std::move(unreachableTask));
  const rt::Reason unreachableFailure = co_await std::move(unreachableSettled);
  EXPECT(isDeepSeekError(unreachableFailure, "deepseek_upstream_unreachable", 502));

  // A TimeoutError from the transport itself is the gateway timeout.
  ds::DeepSeekChatCompletionsOptions transportTimeout;
  transportTimeout.apiKey = std::string("sk");
  transportTimeout.fetcher = ds::DeepSeekFetch([](rt::RequestInfo, rt::RequestInit) {
    return rt::Promise<rt::Response>::reject(rt::makeReason(rt::DOMException("slow", "TimeoutError"))).wait();
  });
  auto transportTimeoutTask = ds::fetchDeepSeekChatCompletions(body, "deepseek-flash", transportTimeout);
  auto transportTimeoutSettled = rejectionOf(std::move(transportTimeoutTask));
  const rt::Reason transportTimeoutFailure = co_await std::move(transportTimeoutSettled);
  EXPECT(isDeepSeekError(transportTimeoutFailure, "gateway_timeout", 504));

  {
    ds::setDeepSeekFetchTimeoutMsForTest(10);
    const auto restore = rt::scopeExit([] { ds::setDeepSeekFetchTimeoutMsForTest(std::nullopt); });
    ds::DeepSeekChatCompletionsOptions deadline;
    deadline.apiKey = std::string("sk");
    deadline.fetcher = ds::DeepSeekFetch([](rt::RequestInfo, rt::RequestInit init) { return rejectOnAbort(init.signal); });
    auto deadlineTask = ds::fetchDeepSeekChatCompletions(body, "deepseek-flash", deadline);
    auto deadlineSettled = rejectionOf(std::move(deadlineTask));
    const rt::Reason deadlineFailure = co_await std::move(deadlineSettled);
    EXPECT(isDeepSeekError(deadlineFailure, "gateway_timeout", 504));
  }

  // A caller abort keeps the caller's own reason, during transport and before it.
  const rt::Reason abortReason = rt::makeReason(rt::DOMException("client disconnected", "AbortError"));
  const rt::AbortController controller;
  ds::DeepSeekChatCompletionsOptions callerAbort;
  callerAbort.apiKey = std::string("sk");
  callerAbort.signal = controller.signal();
  callerAbort.fetcher = ds::DeepSeekFetch([controller, abortReason](rt::RequestInfo, rt::RequestInit init) {
    controller.abort(abortReason);
    return rejectOnAbort(init.signal);
  });
  auto callerAbortTask = ds::fetchDeepSeekChatCompletions(body, "deepseek-flash", callerAbort);
  EXPECT_REJECTS_REASON(std::move(callerAbortTask), abortReason);

  int cancelled = 0;
  int fetchCalls = 0;
  const rt::AbortController preAborted;
  preAborted.abort(abortReason);
  ds::DeepSeekChatCompletionsOptions preAbortedOptions;
  preAbortedOptions.apiKey = std::string("sk");
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
  preAbortedOptions.fetcher = ds::DeepSeekFetch([&fetchCalls](rt::RequestInfo, rt::RequestInit) {
    fetchCalls += 1;
    return rt::Response::json(json::value(json::object()));
  });
  auto preAbortedTask = ds::fetchDeepSeekChatCompletions(body, "deepseek-flash", preAbortedOptions);
  EXPECT_REJECTS_REASON(std::move(preAbortedTask), abortReason);
  EXPECT_EQ(cancelled, 1);
  EXPECT_EQ(fetchCalls, 0);

  // A caller TimeoutError identity becomes the gateway timeout.
  const rt::AbortController callerTimeout;
  callerTimeout.abort(rt::makeReason(rt::DOMException("caller deadline", "TimeoutError")));
  ds::DeepSeekChatCompletionsOptions callerTimeoutOptions;
  callerTimeoutOptions.apiKey = std::string("sk");
  callerTimeoutOptions.signal = callerTimeout.signal();
  callerTimeoutOptions.fetcher = preAbortedOptions.fetcher;
  auto callerTimeoutTask = ds::fetchDeepSeekChatCompletions(body, "deepseek-flash", callerTimeoutOptions);
  auto callerTimeoutSettled = rejectionOf(std::move(callerTimeoutTask));
  const rt::Reason callerTimeoutFailure = co_await std::move(callerTimeoutSettled);
  EXPECT(isDeepSeekError(callerTimeoutFailure, "gateway_timeout", 504));
  EXPECT_EQ(fetchCalls, 0);

  // A quota admission failure is the caller's to render, never reclassified.
  const rt::Reason quota = rt::makeReason(api_key_policy::ApiKeyQuotaDispatchError("quota reservation is gone"));
  ds::DeepSeekChatCompletionsOptions quotaOptions;
  quotaOptions.apiKey = std::string("sk");
  quotaOptions.beforeDispatch = [quota]() -> rt::Task<std::optional<Dispatch>> { rt::rethrow(quota); };
  quotaOptions.fetcher = preAbortedOptions.fetcher;
  auto quotaTask = ds::fetchDeepSeekChatCompletions(body, "deepseek-flash", quotaOptions);
  EXPECT_REJECTS_REASON(std::move(quotaTask), quota);
  EXPECT_EQ(fetchCalls, 0);
}

TEST("deepseek: a message holding only `reasoning_content: null` is accepted with empty content (TS quirk, Bun output)") {
  const json::value payload = rt::jsonParse(R"({"id":"a","created":1,"choices":[{"index":0,"message":{"role":"assistant","reasoning_content":null}}]})");
  EXPECT_EQ(result(ds::normalizeDeepSeekChatCompletion(payload, "deepseek-flash")),
            std::string(R"({"ok":true,"value":{"id":"a","object":"chat.completion","created":1,"model":"deepseek-flash","choices":[{"index":0,"message":{"role":"assistant","content":""},"finish_reason":"stop"}]}})"));
}

TEST("deepseek: reasoning_effort `constructor` leaves the wire body and `__proto__` becomes {} (TS quirk, Bun output)") {
  const json::object constructorBody = rt::jsonParse(R"({"model":"x","reasoning_effort":" Constructor ","messages":[]})").as_object();
  EXPECT_EQ(rt::jsonStringify(json::value(ds::projectDeepSeekRequest(constructorBody, "deepseek-flash"))), std::string(R"({"model":"deepseek-flash","messages":[]})"));
  const json::object protoBody = rt::jsonParse(R"({"model":"x","reasoning_effort":"__PROTO__","messages":[]})").as_object();
  EXPECT_EQ(rt::jsonStringify(json::value(ds::projectDeepSeekRequest(protoBody, "deepseek-flash"))),
            std::string(R"({"model":"deepseek-flash","reasoning_effort":{},"messages":[]})"));
  EXPECT_EQ(ds::deepSeekDefaultOutputAllowance("constructor"), std::optional<double>());
  // Only an exact "none" maps: the projection returns any tier it does not map unchanged.
  EXPECT_EQ(ds::deepSeekDefaultOutputAllowance("none"), std::optional<double>(8192));
  EXPECT_EQ(ds::deepSeekDefaultOutputAllowance(" NONE "), std::optional<double>());
}
