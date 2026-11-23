// Links: tests/lithos_differential_test.cpp src/provider/lithos.cpp src/provider/lithos_rate_limits.cpp src/utils.cpp
// src/provider/lithos-rate-limits.ts has no bun test runnable here (tests/lithos-tier-windows.test.ts
// and the wiring tests need the HTTP handlers), and lithos.ts's ported tests do not reach its
// failover-echo rule: differential vectors recorded from the TS under Bun 1.4.0
// (tests/lithos_differential_vectors.inc), then waitForLithosRetry and the telemetry recorders,
// whose Bun results (elapsed time, rejection reasons) are asserted directly.
#include "uos/provider/lithos.hpp"
#include "uos/provider/lithos_rate_limits.hpp"

#include "uos/openai_telemetry.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "lithos_differential_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
namespace lithos = uos::provider::lithos;
namespace rl = uos::provider::lithos_rate_limits;
namespace openai_telemetry = uos::openai_telemetry;

namespace {
  constexpr double NOW = 1'790'000'000'000;

  const json::array& vectors() {
    static const json::array all = [] {
      const std::vector<std::uint8_t> packed = rt::fromBase64(kLithosVectors);
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
  std::string text(const json::value& value) { return std::string(value.as_string()); }
  std::optional<std::string> optionalText(const std::optional<json::value>& value) {
    if (!value) return std::nullopt;
    return text(*value);
  }
  // A recorded number, with "NaN" / "Infinity" / "-Infinity" standing for the non-finite ones.
  double number(const json::value& value) { return value.is_string() ? rt::stringToNumber(value.as_string()) : rt::toNumber(value); }

  std::string boolean(bool value) { return value ? "true" : "false"; }
  std::string optionalString(const std::optional<std::string>& value) { return value ? rt::jsonQuote(*value) : "null"; }
  std::string waitResult(const std::optional<rl::LithosRateLimitWait>& wait) {
    if (!wait) return "null";
    json::object out;
    out["waitMs"] = rt::makeNumber(wait->waitMs);
    out["source"] = wait->source;
    return rt::jsonStringify(json::value(std::move(out)));
  }
  std::string result(const lithos::NormalizationResult<json::object>& value) {
    json::object out;
    if (const auto* ok = std::get_if<lithos::NormalizationResultOk<json::object>>(&value)) {
      out["ok"] = true;
      out["value"] = ok->value;
    } else {
      out["ok"] = false;
      out["message"] = std::get<lithos::NormalizationResultNotOk>(value).message;
    }
    return rt::jsonStringify(json::value(std::move(out)));
  }
  // `{ threw: { name, message, code, status } }` as the oracle printed a throw.
  std::string thrown(const std::exception_ptr& error) {
    json::object fields;
    try {
      std::rethrow_exception(error);
    } catch (const lithos::LithosError& lithosError) {
      fields["name"] = lithosError.name;
      fields["message"] = lithosError.message;
      fields["code"] = lithosError.code;
      fields["status"] = rt::makeNumber(lithosError.status);
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

  rt::Headers headersOf(const json::value& pairs) {
    rt::Headers headers;
    for (const json::value& pair : pairs.as_array()) headers.append(pair.as_array()[0].as_string(), pair.as_array()[1].as_string());
    return headers;
  }

  rl::LithosRefusalWaitPolicy policyNamed(std::string_view name) {
    if (name == "streamed") return rl::LITHOS_STREAMED_REFUSAL_WAIT_POLICY;
    if (name == "immediate") return rl::LITHOS_IMMEDIATE_FAILOVER_WAIT_POLICY;
    return rl::LITHOS_BUFFERED_REFUSAL_WAIT_POLICY;
  }

  std::string failoverScenario(const json::array& ops) {
    rl::clearLithosFailoverWindows();
    json::array results;
    for (const json::value& entry : ops) {
      const json::array& op = entry.as_array();
      const std::string kind = text(op[0]);
      if (kind == "open") {
        const std::optional<std::string> override_ = op[5].is_null() ? std::nullopt : std::optional<std::string>(text(op[5]));
        rl::lithosOpenFailoverWindow(text(op[1]), number(op[2]), number(op[3]), text(op[4]), override_);
        results.emplace_back("$undefined");
      } else if (kind == "target") {
        results.emplace_back(optionalString(rl::lithosFailoverTargetAt(text(op[1]), number(op[2]))));
      } else {
        rl::clearLithosFailoverWindows();
        results.emplace_back("$undefined");
      }
    }
    rl::clearLithosFailoverWindows();
    return rt::jsonStringify(json::value(std::move(results)));
  }

  std::string logs(const json::object& fields) {
    const rt::console::Capture capture({rt::console::Method::info});
    rl::logLithosRateLimitWait(fields);
    rl::logLithosRateLimitRefusal(fields);
    rl::logLithosRateLimitFailover(fields);
    json::array lines;
    for (const rt::console::Call& call : capture.calls()) lines.emplace_back(call.text);
    return rt::jsonStringify(json::value(std::move(lines)));
  }

  std::string run(std::string_view fn, const json::array& args) {
    if (fn == "lithosFailoverLadderFor") {
      json::array ladder;
      for (const std::string& rung : rl::lithosFailoverLadderFor(text(*argument(args, 0)))) ladder.emplace_back(rung);
      return rt::jsonStringify(json::value(std::move(ladder)));
    }
    if (fn == "lithosIsLadderTarget") return boolean(rl::lithosIsLadderTarget(text(*argument(args, 0)), text(*argument(args, 1))));
    if (fn == "lithosRateLimitWait") return waitResult(rl::lithosRateLimitWait(headersOf(*argument(args, 0)), number(*argument(args, 1))));
    if (fn == "lithosRetryProhibited") return boolean(rl::lithosRetryProhibited(headersOf(*argument(args, 0))));
    if (fn == "lithosRateLimitSnapshot") return rt::jsonStringify(json::value(rl::lithosRateLimitSnapshot(headersOf(*argument(args, 0)))));
    if (fn == "lithosRefusalWait") {
      const rt::Headers headers = headersOf(*argument(args, 0));
      const std::string policy = text(*argument(args, 3));
      if (policy == "default") return waitResult(rl::lithosRefusalWait(headers, number(*argument(args, 1)), number(*argument(args, 2))));
      return waitResult(rl::lithosRefusalWait(headers, number(*argument(args, 1)), number(*argument(args, 2)), policyNamed(policy)));
    }
    if (fn == "failoverScenario") return failoverScenario(argument(args, 0)->as_array());
    if (fn == "logs") return logs(argument(args, 0)->as_object());
    if (fn == "normalizeLithosChatCompletion") {
      return result(lithos::normalizeLithosChatCompletion(pointer(argument(args, 0)), text(*argument(args, 1)), optionalText(argument(args, 2))));
    }
    if (fn == "normalizeLithosChatCompletionChunk") {
      return result(lithos::normalizeLithosChatCompletionChunk(pointer(argument(args, 0)), text(*argument(args, 1)), optionalText(argument(args, 2))));
    }
    if (fn == "projectLithosRequest") return rt::jsonStringify(json::value(lithos::projectLithosRequest(argument(args, 0)->as_object(), text(*argument(args, 1)))));
    if (fn == "normalizeLithosProviderRequestId") return optionalString(lithos::normalizeLithosProviderRequestId(pointer(argument(args, 0))));
    return "unknown fn " + std::string(fn);
  }

  // Replays every vector of `fn` with Date.now pinned as the oracle pinned it.
  void replay(std::string_view fn, std::size_t expectedCount) {
    const std::function<double()> realNow = rt::globalDateNow();
    rt::globalDateNow() = [] { return NOW; };
    const auto restore = rt::scopeExit([&realNow] { rt::globalDateNow() = realNow; });
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

  // What a settled rt::Promise holds: its rejection reason, or a null Reason once fulfilled.
  template <class T> rt::Task<rt::Reason> rejectionOf(rt::Task<T> task) {
    rt::Reason failure;
    try {
      co_await std::move(task);
    } catch (...) {
      failure = std::current_exception();
    }
    co_return failure;
  }

}

TEST("differential: lithosFailoverLadderFor matches Bun") { replay("lithosFailoverLadderFor", 11); }
TEST("differential: lithosIsLadderTarget matches Bun") { replay("lithosIsLadderTarget", 121); }
TEST("differential: lithosRateLimitWait matches Bun") { replay("lithosRateLimitWait", 86); }
TEST("differential: lithosRetryProhibited matches Bun") { replay("lithosRetryProhibited", 43); }
TEST("differential: lithosRateLimitSnapshot matches Bun") { replay("lithosRateLimitSnapshot", 43); }
TEST("differential: lithosRefusalWait matches Bun for every policy") { replay("lithosRefusalWait", 688); }
TEST("differential: failover windows open, expire and clear as in Bun") { replay("failoverScenario", 8); }
TEST("differential: the three rate-limit log lines match Bun") { replay("logs", 3); }
TEST("differential: normalizeLithosChatCompletion matches Bun, failover echoes included") { replay("normalizeLithosChatCompletion", 919); }
TEST("differential: normalizeLithosChatCompletionChunk matches Bun, failover echoes included") { replay("normalizeLithosChatCompletionChunk", 856); }
TEST("differential: projectLithosRequest matches Bun") { replay("projectLithosRequest", 272); }
TEST("differential: normalizeLithosProviderRequestId matches Bun") { replay("normalizeLithosProviderRequestId", 11); }

TEST("lithos rate limits: the recorders write only through a usage context that carries telemetry") {
  rl::recordLithosFailoverModel(nullptr, std::optional<std::string>("x"));
  rl::recordLithosRateLimitWaitMs(nullptr, 5);
  openai_telemetry::UsageContext bare{.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt};
  rl::recordLithosFailoverModel(&bare, std::optional<std::string>("x"));
  rl::recordLithosRateLimitWaitMs(&bare, 5);
  EXPECT(bare.responseTelemetry == nullptr);

  openai_telemetry::UsageContext context{.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt};
  context.responseTelemetry = std::make_shared<openai_telemetry::ResponseTelemetryState>();
  rl::recordLithosFailoverModel(&context, std::optional<std::string>("moonshotai/Kimi-K3-fast"));
  rl::recordLithosRateLimitWaitMs(&context, 1234);
  EXPECT_EQ(context.responseTelemetry->rateLimitFailoverModel, std::optional<std::string>("moonshotai/Kimi-K3-fast"));
  EXPECT_EQ(context.responseTelemetry->rateLimitWaitMs, std::optional<double>(1234));
  // `model: null` clears it when the requested tier served.
  rl::recordLithosFailoverModel(&context, std::nullopt);
  EXPECT_EQ(context.responseTelemetry->rateLimitFailoverModel, std::optional<std::string>());
}

TEST("lithos rate limits: the policies and caps keep their published values") {
  EXPECT_EQ(rl::LITHOS_REFUSAL_WAIT_CAP_MS, 75'000.0);
  EXPECT_EQ(rl::LITHOS_REFUSAL_WAIT_TOTAL_CAP_MS, 120'000.0);
  EXPECT_EQ(rl::LITHOS_REFUSAL_WAIT_MAX_WAITS, 2.0);
  EXPECT(rl::LITHOS_BUFFERED_REFUSAL_WAIT_POLICY == (rl::LithosRefusalWaitPolicy{.maxWaits = 2, .totalCapMs = 120'000}));
  EXPECT(rl::LITHOS_STREAMED_REFUSAL_WAIT_POLICY == (rl::LithosRefusalWaitPolicy{.maxWaits = 5, .totalCapMs = 300'000}));
  EXPECT(rl::LITHOS_IMMEDIATE_FAILOVER_WAIT_POLICY == (rl::LithosRefusalWaitPolicy{.maxWaits = 0, .totalCapMs = 0}));
}

ASYNC_TEST("waitForLithosRetry: a non-positive wait reports 0 and resolves at once") {
  std::vector<double> seen;
  const std::function<void(double)> observe = [&seen](double elapsed) { seen.push_back(elapsed); };
  const rt::AbortController controller;
  const rt::Promise<void> zero = rl::waitForLithosRetry(0, controller.signal(), observe);
  EXPECT(zero.fulfilled());
  // A non-positive wait resolves even when the signal is already aborted (Bun: [0, 0]).
  const rt::Promise<void> negative = rl::waitForLithosRetry(-1, rt::AbortSignal::abort(), observe);
  EXPECT(negative.fulfilled());
  co_await zero.wait();
  co_await negative.wait();
  const std::vector<double> expected{0, 0};
  EXPECT_EQ(seen, expected);
  // Without an observer nothing is reported.
  const rt::Promise<void> silent = rl::waitForLithosRetry(0, controller.signal());
  co_await silent.wait();
}

ASYNC_TEST("waitForLithosRetry: an aborted signal rejects at once with an Error reason, or wraps any other reason") {
  // Bun: a reason that is an Error is the rejection itself; undefined (abort()) is Bun's own
  // AbortError DOMException; any other value becomes an Error named AbortError whose cause it is.
  const rt::Reason typeError = rt::makeReason(rt::TypeError("t"));
  std::vector<double> seen;
  const std::function<void(double)> observe = [&seen](double elapsed) { seen.push_back(elapsed); };
  const rt::AbortController withError;
  withError.abort(typeError);
  const rt::Promise<void> errorWait = rl::waitForLithosRetry(10, withError.signal(), observe);
  EXPECT(errorWait.rejected());
  EXPECT(errorWait.reason() == typeError);
  auto errorSettled = rejectionOf(errorWait.wait());
  const rt::Reason errorReason = co_await std::move(errorSettled);
  EXPECT(errorReason == typeError);

  const rt::AbortController withoutReason;
  withoutReason.abort();
  auto plainSettled = rejectionOf(rl::waitForLithosRetry(10, withoutReason.signal(), observe).wait());
  const rt::Reason plain = co_await std::move(plainSettled);
  EXPECT(plain == withoutReason.signal().reason());
  EXPECT_EQ(rt::errorName(plain), std::optional<std::string>("AbortError"));
  EXPECT_EQ(rt::errorMessage(plain), std::optional<std::string>("The operation was aborted."));

  std::vector<std::optional<json::value>> causes;
  causes.emplace_back(json::value("text"));
  causes.emplace_back(json::value(5));
  causes.emplace_back(json::value(nullptr));
  for (const std::optional<json::value>& cause : causes) {
    const rt::AbortController withValue;
    withValue.abort(rt::thrown(cause));
    auto valueSettled = rejectionOf(rl::waitForLithosRetry(10, withValue.signal(), observe).wait());
    const rt::Reason wrapped = co_await std::move(valueSettled);
    EXPECT_EQ(rt::errorName(wrapped), std::optional<std::string>("AbortError"));
    EXPECT_EQ(rt::errorMessage(wrapped), std::optional<std::string>("The request was aborted while waiting for the LithosAI rate limit to reset."));
    EXPECT(wrapped != withValue.signal().reason());
  }
  const std::vector<double> expected{0, 0, 0, 0, 0};
  EXPECT_EQ(seen, expected);
}

ASYNC_TEST("waitForLithosRetry: a wait resolves after its delay and reports the elapsed time once") {
  std::vector<double> seen;
  const std::function<void(double)> observe = [&seen](double elapsed) { seen.push_back(elapsed); };
  const rt::AbortController controller;
  const rt::Promise<void> waiting = rl::waitForLithosRetry(50, controller.signal(), observe);
  EXPECT(waiting.pending());
  co_await waiting.wait();
  const std::vector<double> expected{50};
  EXPECT_EQ(seen, expected);
  // The abort listener is gone once the timer fired: a later abort reports nothing.
  controller.abort(rt::makeReason(rt::DOMException("late", "AbortError")));
  EXPECT_EQ(seen, expected);
}

ASYNC_TEST("waitForLithosRetry: an abort during the wait clears the timer and rejects with the wrapped reason") {
  std::vector<double> seen;
  const std::function<void(double)> observe = [&seen](double elapsed) { seen.push_back(elapsed); };
  const rt::AbortController controller;
  const rt::Promise<void> waiting = rl::waitForLithosRetry(50, controller.signal(), observe);
  const rt::Timeout abortLater = rt::setTimeout([controller] { controller.abort(rt::thrown(json::value("late"))); }, 20);
  auto settled = rejectionOf(waiting.wait());
  const rt::Reason reason = co_await std::move(settled);
  EXPECT_EQ(rt::errorName(reason), std::optional<std::string>("AbortError"));
  EXPECT_EQ(rt::errorMessage(reason), std::optional<std::string>("The request was aborted while waiting for the LithosAI rate limit to reset."));
  const std::vector<double> expected{20};
  EXPECT_EQ(seen, expected);
  // The cleared timer never reports.
  co_await rt::sleep(100);
  EXPECT_EQ(seen, expected);
  EXPECT(!abortLater.active());
}

TEST("lithos: a message holding only `content: null` is accepted with empty content (TS quirk, Bun output)") {
  const json::value payload = rt::jsonParse(R"({"id":"a","created":1,"choices":[{"index":0,"message":{"role":"assistant","content":null}}]})");
  EXPECT_EQ(result(lithos::normalizeLithosChatCompletion(payload, "moonshotai/Kimi-K3")),
            std::string(R"({"ok":true,"value":{"id":"a","object":"chat.completion","created":1,"model":"moonshotai/Kimi-K3","choices":[{"index":0,"message":{"role":"assistant","content":""},"finish_reason":"stop"}]}})"));
}
