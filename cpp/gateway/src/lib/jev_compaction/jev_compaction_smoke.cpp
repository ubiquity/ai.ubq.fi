#include "uos/lib/jev_compaction/jev_compaction_smoke.hpp"
#include "uos/lib/jev_compaction/types.hpp"
#include "uos/api_keys.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/handler/admission.hpp"
#include "uos/handler/index.hpp"
#include "uos/handler/serve_handler.hpp"
#include "uos/jev_compaction/compaction.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/storage/kv.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace uos::lib::jev_compaction::jev_compaction_smoke {
  namespace {
    inline constexpr std::string_view METADATA_HEADER = "x-codex-turn-metadata";
    inline constexpr std::string_view COMPACTION_PROMPT = "You are performing a CONTEXT CHECKPOINT COMPACTION. Create a handoff summary for another LLM.";
    inline constexpr std::string_view KEPT_MARKER = "SMOKE_KEEP_MARKER_release_notes";
    inline constexpr std::string_view SUMMARY_MARKER = "# fast-jev-compaction memory summary";
    inline constexpr std::string_view DUMMY_DEEPSEEK_KEY = "jev-smoke-dummy-deepseek-key";
    struct Report {
      std::string status = "FAIL";
      std::string failure_class = "not-started";
      double compaction_status = 0;
      double compaction_summary_chars = 0;
      double compaction_dropped = 0;
      std::string compaction_structural_header;
      double compaction_terminal_records = 0;
      bool compaction_completed = false;
      bool compaction_usage_observed = false;
      std::string compaction_provider;
      std::string compaction_reservation_state;
      double ordinary_status = 0;
      bool ordinary_upstream_saw_summary = false;
      bool ordinary_upstream_saw_new_message = false;
      double upstream_calls = 0;
      double jev_calls = 0;
      double jev_questions = 0;
      double fallback_status = 0;
      double fallback_upstream_calls = 0;
      double fallback_terminal_records = 0;
      bool fallback_completed = false;
      double fallback_committed_requests = 0;
      double fallback_reserved_requests = 0;
      std::string fallback_reservation_state;
      double total_ms = 0;
    };
    json::object toJson(const Report& report) {
      return {
        {"status", report.status}, {"failure_class", report.failure_class}, {"compaction_status", report.compaction_status},
        {"compaction_summary_chars", report.compaction_summary_chars}, {"compaction_dropped", report.compaction_dropped},
        {"compaction_structural_header", report.compaction_structural_header}, {"compaction_terminal_records", report.compaction_terminal_records},
        {"compaction_completed", report.compaction_completed}, {"compaction_usage_observed", report.compaction_usage_observed},
        {"compaction_provider", report.compaction_provider}, {"compaction_reservation_state", report.compaction_reservation_state},
        {"ordinary_status", report.ordinary_status}, {"ordinary_upstream_saw_summary", report.ordinary_upstream_saw_summary},
        {"ordinary_upstream_saw_new_message", report.ordinary_upstream_saw_new_message}, {"upstream_calls", report.upstream_calls},
        {"jev_calls", report.jev_calls}, {"jev_questions", report.jev_questions}, {"fallback_status", report.fallback_status},
        {"fallback_upstream_calls", report.fallback_upstream_calls}, {"fallback_terminal_records", report.fallback_terminal_records},
        {"fallback_completed", report.fallback_completed}, {"fallback_committed_requests", report.fallback_committed_requests},
        {"fallback_reserved_requests", report.fallback_reserved_requests}, {"fallback_reservation_state", report.fallback_reservation_state}, {"total_ms", report.total_ms},
      };
    }
    void assert_(bool condition, const std::string& message = "The expression evaluated to a falsy value") {
      if (!condition) throw rt::JsError(message, "AssertionError");
    }
    std::string assertionInspect(const json::value* value) {
      if (!value) return "undefined";
      if (value->is_number()) {
        const auto number = rt::toNumber(*value);
        return number == 0 && std::signbit(number) ? "-0" : rt::numberToString(number);
      }
      if (!value->is_string()) return rt::jsonStringify(*value);
      const std::string raw(value->as_string());
      const char quote = raw.find('\'') != std::string::npos && raw.find('"') == std::string::npos ? '"' : '\'';
      std::string out(1, quote);
      for (auto it = rt::codePoints(raw).begin(); it != rt::codePoints(raw).end(); ++it) {
        const auto point = *it;
        if (point == static_cast<unsigned char>(quote) || point == '\\') { out += '\\'; out += static_cast<char>(point); }
        else if (point == '\n') out += "\\n";
        else if (point == '\r') out += "\\r";
        else if (point == '\t') out += "\\t";
        else if (point == '\b') out += "\\b";
        else if (point == '\f') out += "\\f";
        else if (point == 11) out += "\\v";
        else if (point < 32 || point == 127) {
          const auto encoded = rt::jsonStringify(json::value(std::string(it.text())));
          out += encoded.substr(1, encoded.size() - 2);
        } else out += it.text();
      }
      out += quote;
      return out;
    }
    std::optional<json::value> assertionValue(const json::value* value) { return value ? std::optional(*value) : std::nullopt; }
    template<class T> std::optional<json::value> assertionValue(const T& value) { return json::value(value); }
    template<class T> std::optional<json::value> assertionValue(const std::optional<T>& value) { return value ? assertionValue(*value) : std::nullopt; }
    template<class Actual, class Expected>
    void assertEqual(const Actual& actual, const Expected& expected, const std::string& message = "Expected values to be strictly equal:") {
      const auto left = assertionValue(actual);
      const auto right = assertionValue(expected);
      if (!left && !right) return;
      if (left && right && (left->is_number() && right->is_number()
        ? rt::sameValue(rt::toNumber(*left), rt::toNumber(*right)) : rt::strictEquals(*left, *right))) return;
      const auto actualText = assertionInspect(left ? &*left : nullptr);
      const auto expectedText = assertionInspect(right ? &*right : nullptr);
      throw rt::JsError(message + "\n\n" + actualText + " !== " + expectedText + "\n", "AssertionError");
    }
    void assertMatch(const std::string& actual) {
      if (actual.find("calls_dropped=3") != std::string::npos) return;
      const json::value value(actual);
      throw rt::JsError("The input did not match the regular expression /calls_dropped=3/. Input:\n\n" + assertionInspect(&value) + "\n", "AssertionError");
    }
    std::vector<std::string> assertionLines(const json::value* value, std::size_t indent = 0) {
      const std::string padding(indent, ' ');
      if (!value || (!value->is_array() && !value->is_object())) return {padding + assertionInspect(value)};
      const bool array = value->is_array();
      const auto size = array ? value->as_array().size() : value->as_object().size();
      if (size == 0) return {padding + (array ? "[]" : "{}")};
      std::vector<std::string> lines{padding + (array ? "[" : "{")};
      std::vector<std::string> keys;
      if (!array) {
        for (const auto& entry : value->as_object()) keys.emplace_back(entry.key());
        std::sort(keys.begin(), keys.end(), rt::lessThan);
      }
      for (std::size_t i = 0; i < size; ++i) {
        const auto* child = array ? &value->as_array()[i] : value->as_object().if_contains(keys[i]);
        auto part = assertionLines(child, indent + 2);
        if (!array) {
          const auto& key = keys[i];
          bool identifier = !key.empty();
          for (std::size_t j = 0; j < key.size(); ++j) {
            const char c = key[j];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$' || (j > 0 && c >= '0' && c <= '9'))) identifier = false;
          }
          const json::value keyValue(key);
          part.front().insert(indent + 2, (identifier ? key : assertionInspect(&keyValue)) + ": ");
        }
        if (i + 1 < size) part.back() += ',';
        lines.insert(lines.end(), part.begin(), part.end());
      }
      lines.push_back(padding + (array ? "]" : "}"));
      return lines;
    }
    void assertDeepEqual(const json::value* actual, const json::value& expected, const std::string& message) {
      if (actual && rt::deepEqual(*actual, expected)) return;
      const auto left = assertionLines(actual);
      const auto right = assertionLines(&expected);
      const auto normalized = [](const std::string& line) { return line.ends_with(',') ? line.substr(0, line.size() - 1) : line; };
      std::vector<std::vector<std::size_t>> common(left.size() + 1, std::vector<std::size_t>(right.size() + 1));
      for (std::size_t i = left.size(); i-- > 0;) for (std::size_t j = right.size(); j-- > 0;)
        common[i][j] = normalized(left[i]) == normalized(right[j]) ? common[i + 1][j + 1] + 1 : std::max(common[i + 1][j], common[i][j + 1]);
      std::string diff = message + "\n+ actual - expected\n\n";
      std::size_t i = 0;
      std::size_t j = 0;
      while (i < left.size() || j < right.size()) {
        if (i < left.size() && j < right.size() && normalized(left[i]) == normalized(right[j])) { diff += "  " + right[j] + "\n"; ++i; ++j; }
        else if (i < left.size() && (j == right.size() || common[i + 1][j] >= common[i][j + 1])) diff += "+ " + left[i++] + "\n";
        else diff += "- " + right[j++] + "\n";
      }
      throw rt::JsError(diff, "AssertionError");
    }
    json::object text(const std::string& role, const std::string& value) {
      return {{"type", "message"}, {"role", role}, {"content", json::array{json::object{{"type", "input_text"}, {"text", value}}}}};
    }
    json::object call(const std::string& callId, const std::string& command) {
      return {{"type", "function_call"}, {"call_id", callId}, {"name", "shell"}, {"arguments", rt::jsonStringify(json::object{{"command", command}})}};
    }
    json::object output(const std::string& callId, const std::string& value) { return {{"type", "function_call_output"}, {"call_id", callId}, {"output", value}}; }
    json::array compactionInput() {
      return {
        text("user", "Goal: finish the release notes and keep " + std::string(KEPT_MARKER) + " verbatim."),
        text("assistant", "Starting with the old build logs."), call("old_a", "tail -n 400 /tmp/old/build.log"),
        output("old_a", rt::repeat("SMOKE_RESULT_A obsolete build log line. ", 60)), call("old_b", "grep -c deprecated /tmp/old/metrics.csv"),
        output("old_b", rt::repeat("SMOKE_RESULT_B obsolete metric line. ", 60)), call("old_c", "ls -la /tmp/old/cache"),
        output("old_c", rt::repeat("SMOKE_RESULT_C obsolete cache line. ", 60)),
        text("assistant", "The old investigation is finished; those logs are obsolete."), text("user", "Recent follow-up one: draft the release notes outline."),
        text("assistant", "Drafted the outline."), text("user", "Recent follow-up two: check the artifact wording."), text("assistant", "Checked."),
        text("user", "Final goal: " + std::string(KEPT_MARKER) + " stays verbatim."), text("user", std::string(COMPACTION_PROMPT)),
      };
    }
    class DropEverythingAsker final : public types::JevAsker {
      std::shared_ptr<Report> report_;
    public:
      explicit DropEverythingAsker(std::shared_ptr<Report> report) : report_(std::move(report)) {}
      rt::Task<types::JevResponse> ask(types::JevState, types::JevQuestions questions) override {
        json::object answers;
        for (const auto& name : questions.keys()) answers.emplace(name, json::object{{"noul", 0.1}});
        report_->jev_calls += 1;
        report_->jev_questions += static_cast<double>(questions.size());
        return rt::fulfilledTask(types::JevResponse{{"answers", answers}});
      }
    };
    std::shared_ptr<types::JevAsker> dropEverythingAsker(std::shared_ptr<Report> report) { return std::make_shared<DropEverythingAsker>(std::move(report)); }
    std::string chatChunk(const json::object& delta, const std::optional<std::string>& finishReason, const std::optional<json::object>& usage = std::nullopt) {
      json::object chunk{{"id", "chatcmpl-jev-smoke"}, {"object", "chat.completion.chunk"}, {"created", 1'780'000'009}, {"model", "deepseek-flash"},
        {"choices", json::array{json::object{{"index", 0}, {"delta", delta}, {"finish_reason", finishReason ? json::value(*finishReason) : json::value(nullptr)}}}}};
      if (usage) chunk.emplace("usage", *usage);
      return "data: " + rt::jsonStringify(chunk) + "\n\n";
    }
    std::string sseBody(const std::vector<std::string>& frames) { return rt::join(frames, "") + "data: [DONE]\n\n"; }
    rt::Promise<rt::Response> post(const std::string& base, const std::string& token, const json::object& payload,
                                  const std::optional<std::string>& metadata = std::nullopt, const std::optional<rt::AbortSignal>& signal = std::nullopt) {
      rt::Headers headers{{"Authorization", "Bearer " + token}, {"Content-Type", "application/json"}};
      if (metadata && !metadata->empty()) headers.set(METADATA_HEADER, *metadata);
      rt::RequestInit init{.method = "POST", .headers = headers, .body = rt::jsonStringify(payload), .signal = signal};
      return rt::Promise<rt::Response>(rt::fetch(base + "/v1/responses", init));
    }
    rt::Task<void> delay(double ms) { return rt::sleep(ms); }
    rt::Task<void> waitFor(std::function<bool()> predicate, std::string label, double timeoutMs = 4'000) {
      const auto deadline = rt::performanceNow() + timeoutMs;
      while (!predicate()) {
        if (rt::performanceNow() >= deadline) throw rt::JsError("Timed out waiting for " + label);
        rt::Promise<void> pending(delay(25));
        co_await pending.wait();
      }
    }
    struct Gateway {
      std::string url;
      std::string token;
      std::shared_ptr<rt::Kv> kv;
      authority::native_contracts::ApiKeyPolicy policy;
      std::function<rt::Task<void>()> stop;
    };
    rt::Task<void> stopGateway(std::shared_ptr<rt::Kv> store, runtime::http::HttpServer gateway) {
      kv::setKvForTest(nullptr);
      handler::admission::setInferenceAdmissionControllerForTest(std::nullopt);
      auto stopped = gateway.shutdown();
      co_await stopped.wait();
      store->close();
    }
    rt::Task<Gateway> startGateway() {
      rt::Promise<std::shared_ptr<rt::Kv>> opened(storage::kv::openKv(":memory:"));
      auto store = co_await opened.wait();
      const std::string keyId = "jev-compaction-smoke-key";
      const std::string token = "u_" + std::string(64, 'c');
      rt::Promise<std::string> hashed(utils::sha256Base64Url(token));
      const auto tokenHash = co_await hashed.wait();
      const auto now = rt::dateNow();
      const auto windowMs = 60 * 60'000;
      const json::object commonPolicy{{"expires_at_ms", -1}, {"revoked_at_ms", nullptr}, {"usage_limit_requests", 500}, {"usage_requests", 0},
        {"usage_reset_at_ms", now + windowMs}, {"window_ms", windowMs}, {"usage_quota_version", 3}, {"paid_fallback_enabled", false},
        {"paid_fallback_limit_microcredits", api_keys::PAID_FALLBACK_NO_LIMIT}, {"paid_fallback_spent_microcredits", 0},
        {"paid_fallback_reserved_microcredits", 0}, {"paid_fallback_reservation_request_id", nullptr}};
      json::object keyRecord{{"id", keyId}, {"name", "jev compaction smoke key"}, {"prefix", rt::slice(token, 0, 10)}, {"hash", tokenHash}, {"created_at_ms", now}};
      for (const auto& entry : commonPolicy) keyRecord.emplace(entry.key(), entry.value());
      keyRecord.emplace("paid_fallback_model_ids", json::array{});
      keyRecord.emplace("paid_fallback_quota_per_credit", 0);
      keyRecord.emplace("paid_fallback_max_exposure_microcredits", json::object{});
      keyRecord.emplace("paid_fallback_pricing_checked_at_ms", now);
      json::object hashRecord{{"id", keyId}};
      for (const auto& entry : commonPolicy) hashRecord.emplace(entry.key(), entry.value());
      rt::KvKey idKey{"ubq_ai", "api_keys", "id", keyId};
      rt::KvKey hashKey{"ubq_ai", "api_keys", "hash", tokenHash};
      auto setId = store->set(idKey, rt::KvValue(keyRecord));
      co_await std::move(setId);
      co_await rt::microtask();
      auto setHash = store->set(hashKey, rt::KvValue(hashRecord));
      co_await std::move(setHash);
      co_await rt::microtask();
      const auto policy = api_key_policy::apiKeyPolicyFromHashRecord(tokenHash, json::value(hashRecord), now);
      assert_(policy.has_value(), "the seeded key must resolve to a live policy");
      kv::setKvForTest(store);
      api_key_policy::resetApiKeyPolicyCacheForTest();
      handler::admission::setInferenceAdmissionControllerForTest(inference_admission::createInferenceAdmissionController({8, 8, 500}));
      co_await rt::microtask();
      co_await rt::microtask();
      handler::serve_handler::DeliveryAwareHandler routed = [](rt::Request request, handler::http::RequestDeliveryInfo delivery) {
        auto url = rt::URL(request.url());
        url.hostname("jev-smoke.example.invalid");
        return handler::index::handler(rt::Request(url, request), delivery);
      };
      runtime::http::ServeHttpOptions options{.hostname = "127.0.0.1", .port = 0};
      const auto gateway = runtime::http::serveHttp(options, handler::serve_handler::createServeHandler(routed));
      const auto address = co_await gateway.ready.wait();
      Gateway result{"http://127.0.0.1:" + rt::numberToString(address.port), token, store, *policy, [store, gateway] { return stopGateway(store, gateway); }};
      co_return result;
    }
    struct Upstream { std::string url; std::function<rt::Promise<void>()> stop; };
    rt::Task<rt::Response> upstreamResponse(std::shared_ptr<std::vector<std::string>> calls, rt::Request request) {
      rt::Promise<std::string> pending(request.text());
      auto body = co_await pending.wait();
      calls->push_back(body);
      std::vector<std::string> frames{chatChunk({{"role", "assistant"}, {"content", "smoke ordinary answer"}}, std::nullopt),
        chatChunk({}, "stop", json::object{{"prompt_tokens", 7}, {"completion_tokens", 3}, {"total_tokens", 10}})};
      rt::ResponseInit init{.headers = rt::Headers{{"Content-Type", "text/event-stream"}}};
      co_return rt::Response(sseBody(frames), init);
    }
    rt::Task<Upstream> startUpstream(std::shared_ptr<std::vector<std::string>> calls) {
      runtime::http::ServeHttpOptions options{.hostname = "127.0.0.1", .port = 0};
      runtime::http::ServeHandler respond = [calls](rt::Request request, runtime::http::ServeHandlerInfo) { return upstreamResponse(calls, request); };
      const auto server = runtime::http::serveHttp(options, respond);
      const auto address = co_await server.ready.wait();
      Upstream result{"http://127.0.0.1:" + rt::numberToString(address.port), server.shutdown};
      co_return result;
    }
    std::vector<json::object> terminalRecords(const rt::console::Capture& capture, const std::string& requestId) {
      std::vector<json::object> records;
      for (const auto& entry : capture.calls()) {
        if (entry.args.size() < 2 || entry.args[0].string() != "[ai.ubq.fi] request_terminal" || entry.args[1].kind() != rt::console::Value::Kind::string) continue;
        try {
          const auto parsed = rt::jsonParse(entry.args[1].string());
          if (parsed.is_object() && rt::getString(parsed, "request_id") == requestId) records.push_back(parsed.as_object());
        } catch (...) {}
      }
      return records;
    }
    rt::Task<json::value> readRow(std::shared_ptr<rt::Kv> store, rt::KvKey key) {
      const auto entry = co_await store->get(key);
      co_await rt::microtask();
      co_return rt::storedJsonView(entry.value);
    }
    rt::Task<void> main(std::shared_ptr<Report> report) {
      const auto started = rt::performanceNow();
      const auto inheritedDeepSeekKey = rt::env::get("DEEPSEEK_API_KEY");
      rt::env::set("DEEPSEEK_API_KEY", std::string(DUMMY_DEEPSEEK_KEY));
      auto capture = std::make_shared<rt::console::Capture>(std::vector{rt::console::Method::info}, true);
      auto upstreamCalls = std::make_shared<std::vector<std::string>>();
      rt::Promise<Upstream> upstreamPending(startUpstream(upstreamCalls));
      const auto upstream = co_await upstreamPending.wait();
      const auto originalFetch = rt::globalFetch();
      rt::Fetch fixtureFetch([upstream, originalFetch](rt::RequestInfo input, rt::RequestInit init) {
        std::string url;
        if (const auto* string = std::get_if<std::string>(&input)) url = *string;
        else if (const auto* parsed = std::get_if<rt::URL>(&input)) url = parsed->toString();
        else url = std::get<rt::Request>(input).url();
        if (url == deepseek::index::DEEPSEEK_CHAT_COMPLETIONS_URL) return originalFetch(upstream.url + "/chat/completions", init);
        return originalFetch(input, init);
      });
      rt::setGlobalFetch(fixtureFetch);
      rt::Promise<Gateway> gatewayPending(startGateway());
      const auto gateway = co_await gatewayPending.wait();
      ::uos::jev_compaction::compaction::setJevCompactionAskerForTest(dropEverythingAsker(report));
      rt::Reason failure;
      try {
        const std::string metadata = rt::jsonStringify(json::object{{"request_kind", "compaction"}, {"compaction", json::object{{"implementation", "responses"}}}});
        const json::object payload{{"model", "deepseek-flash"}, {"stream", true}, {"input", compactionInput()}};
        const auto compactionPending = post(gateway.url, gateway.token, payload, metadata, rt::AbortSignal::timeout(20'000));
        auto compaction = co_await compactionPending.wait();
        report->compaction_status = compaction.status();
        report->compaction_structural_header = compaction.headers().get("x-jev-compaction").value_or("");
        rt::Promise<std::string> compactionBody(compaction.text());
        const auto compactionText = co_await compactionBody.wait();
        assertEqual(compaction.status(), 200, "the marked compaction must be answered with a completed response");
        assert_(compactionText.find(SUMMARY_MARKER) != std::string::npos, "the Jev summary marker must be present");
        assert_(compactionText.find(KEPT_MARKER) != std::string::npos, "pinned content must be kept verbatim");
        assert_(compactionText.find("event: response.completed") != std::string::npos, "the summary must terminate with response.completed");
        assert_(compactionText.find("[tool call old_a]") != std::string::npos, "a dropped call must keep its provenance line");
        assert_(compactionText.find("SMOKE_RESULT_A") != std::string::npos, "a dropped result must keep its bounded head");
        assert_(compactionText.find(rt::repeat("SMOKE_RESULT_A obsolete build log line. ", 20)) == std::string::npos, "a dropped result must be bounded, not full");
        assertMatch(report->compaction_structural_header);
        report->compaction_dropped = 3;
        report->compaction_summary_chars = static_cast<double>(rt::length(compactionText));
        assertEqual(upstreamCalls->size(), 0, "the fake upstream must never see a marked compaction request");
        assertEqual(report->jev_calls, 1, "exactly one Jev batch is expected for the synthetic transcript");
        const auto summaryText = compactionText;
        const auto compactionRequestId = compaction.headers().get("x-uos-request-id");
        assert_(compactionRequestId && !compactionRequestId->empty(), "the compaction response must carry the gateway request id");
        std::function<bool()> hasCompactionTerminal = [capture, compactionRequestId] { return !terminalRecords(*capture, *compactionRequestId).empty(); };
        rt::Promise<void> terminalPending(waitFor(hasCompactionTerminal, "the compaction terminal record"));
        co_await terminalPending.wait();
        rt::Promise<void> delayed(delay(250));
        co_await delayed.wait();
        const auto compactionTerminals = terminalRecords(*capture, *compactionRequestId);
        report->compaction_terminal_records = static_cast<double>(compactionTerminals.size());
        assertEqual(compactionTerminals.size(), 1, "the compaction must settle exactly once");
        const json::value terminal = compactionTerminals.empty() ? json::object{} : compactionTerminals.front();
        report->compaction_completed = rt::getBool(terminal, "semantic_output_observed") == true && rt::getString(terminal, "stream_terminal_type") == "response.completed" && rt::get(terminal, "failure_kind") && rt::get(terminal, "failure_kind")->is_null();
        assert_(report->compaction_completed, "a successful local compaction must settle as completed");
        report->compaction_usage_observed = rt::getBool(terminal, "usage_observed") == true;
        assertEqual(rt::get(terminal, "usage_observed"), false, "a local answer must not report invented main-model usage");
        assertEqual(rt::get(terminal, "input_tokens") ? *rt::get(terminal, "input_tokens") : json::value(nullptr), nullptr, "input tokens must stay unknown");
        assertEqual(rt::get(terminal, "output_tokens") ? *rt::get(terminal, "output_tokens") : json::value(nullptr), nullptr, "output tokens must stay unknown");
        assertEqual(rt::get(terminal, "total_tokens") ? *rt::get(terminal, "total_tokens") : json::value(nullptr), nullptr, "total tokens must stay unknown");
        report->compaction_provider = rt::getString(terminal, "provider").value_or("");
        assertEqual(report->compaction_provider, "gateway", "provider provenance must stay the honest gateway label");
        auto usageWindowKey = api_key_policy::apiKeyUsageV3WindowKey(gateway.policy);
        rt::Promise<json::value> usageWindowPending(readRow(gateway.kv, usageWindowKey));
        const auto usageWindow = co_await usageWindowPending.wait();
        assertEqual(rt::getNumber(usageWindow, "committed_requests").value_or(0), 0, "a locally answered compaction must not invent a charged request");
        auto usageKey = api_key_policy::apiKeyUsageV3RequestKey(gateway.policy, *compactionRequestId);
        rt::Promise<json::value> usagePending(readRow(gateway.kv, usageKey));
        const auto usageRow = co_await usagePending.wait();
        report->compaction_reservation_state = rt::getString(usageRow, "state").value_or("absent");
        if (!usageRow.is_null()) assertEqual(rt::get(usageRow, "state"), "released", "the unconsumed API-key reservation must be released, never left reserved or dispatched");
        const std::string nextMessage = "Continue after compaction: publish the release notes.";
        const json::object ordinaryPayload{{"model", "deepseek-flash"}, {"stream", true}, {"input", json::array{text("user", summaryText + "\n\n" + nextMessage)}}};
        auto ordinaryPending = post(gateway.url, gateway.token, ordinaryPayload);
        auto ordinary = co_await ordinaryPending.wait();
        report->ordinary_status = ordinary.status();
        rt::Promise<std::string> ordinaryBody(ordinary.text());
        co_await ordinaryBody.wait();
        assertEqual(ordinary.status(), 200, "the ordinary route must still answer");
        assertEqual(upstreamCalls->size(), 1, "the ordinary request must reach the upstream");
        report->upstream_calls = static_cast<double>(upstreamCalls->size());
        assert_(!upstreamCalls->empty() && !upstreamCalls->front().empty(), "the ordinary upstream body must be recorded");
        const auto& ordinaryUpstreamBody = upstreamCalls->front();
        report->ordinary_upstream_saw_summary = ordinaryUpstreamBody.find(SUMMARY_MARKER) != std::string::npos;
        report->ordinary_upstream_saw_new_message = ordinaryUpstreamBody.find(nextMessage) != std::string::npos;
        assert_(report->ordinary_upstream_saw_summary, "the adopted memory must reach the upstream untouched");
        assert_(report->ordinary_upstream_saw_new_message, "the follow-up message must reach the upstream untouched");
        assertEqual(report->jev_calls, 1, "an ordinary message must not invoke Jev");
        const auto jevCallsBeforeFallback = report->jev_calls;
        const auto jevQuestionsBeforeFallback = report->jev_questions;
        rt::Promise<json::value> beforeFallbackPending(readRow(gateway.kv, usageWindowKey));
        const auto beforeFallbackWindow = co_await beforeFallbackPending.wait();
        assert_(!beforeFallbackWindow.is_null(), "the preceding ordinary request must have a durable usage window");
        assertEqual(rt::get(beforeFallbackWindow, "committed_requests"), 1, "only the preceding ordinary request was charged");
        assertEqual(rt::get(beforeFallbackWindow, "reserved_requests"), 0, "no reservation may precede the fallback request");
        const json::object fallbackPayload{{"model", "deepseek-flash"}, {"stream", true}, {"input", json::array{text("user", "short"), text("assistant", "short answer")}}};
        auto fallbackPending = post(gateway.url, gateway.token, fallbackPayload, metadata);
        auto fallback = co_await fallbackPending.wait();
        report->fallback_status = fallback.status();
        rt::Promise<std::string> fallbackBodyPending(fallback.text());
        const auto fallbackText = co_await fallbackBodyPending.wait();
        report->fallback_upstream_calls = static_cast<double>(upstreamCalls->size());
        assertEqual(fallback.status(), 200, "the ordinary provider must answer when no Jev candidates exist");
        assertEqual(upstreamCalls->size(), 2, "exactly one additional provider request must serve the fallback");
        assertEqual(report->jev_calls, jevCallsBeforeFallback, "no-candidate fallback must not invoke Jev");
        assertEqual(report->jev_questions, jevQuestionsBeforeFallback, "fallback must not invent a local decision question");
        assertEqual(fallback.headers().get("x-jev-compaction") ? json::value(*fallback.headers().get("x-jev-compaction")) : json::value(nullptr), nullptr, "a failed local decision must not claim a Jev summary");
        assert_(upstreamCalls->size() > 1 && !(*upstreamCalls)[1].empty(), "the exact fallback provider request must be recorded");
        const auto fallbackUpstreamBody = (*upstreamCalls)[1];
        const auto fallbackBody = rt::jsonParse(fallbackUpstreamBody);
        assertEqual(rt::get(fallbackBody, "stream"), true, "the original streaming intent must survive fallback");
        const json::array messages{json::object{{"role", "user"}, {"content", "short"}}, json::object{{"role", "assistant"}, {"content", "short answer"}}};
        assertDeepEqual(rt::get(fallbackBody, "messages"), json::value(messages), "both original input messages must survive provider conversion");
        assert_(fallbackUpstreamBody.find(SUMMARY_MARKER) == std::string::npos, "a failed local decision must not invent or adopt a summary");
        std::size_t completions = 0;
        for (std::size_t pos = 0; (pos = fallbackText.find("event: response.completed\n", pos)) != std::string::npos; pos += 26) ++completions;
        assertEqual(completions, 1, "one provider completion must be delivered");
        assert_(fallbackText.find("smoke ordinary answer") != std::string::npos, "the real fake-provider answer must reach the client");
        const auto fallbackRequestId = fallback.headers().get("x-uos-request-id");
        assert_(fallbackRequestId && !fallbackRequestId->empty(), "the fallback must retain one gateway request identity");
        std::function<bool()> hasFallbackTerminal = [capture, fallbackRequestId] { return !terminalRecords(*capture, *fallbackRequestId).empty(); };
        rt::Promise<void> fallbackTerminalPending(waitFor(hasFallbackTerminal, "the fallback terminal record"));
        co_await fallbackTerminalPending.wait();
        rt::Promise<void> fallbackDelayed(delay(250));
        co_await fallbackDelayed.wait();
        const auto fallbackTerminals = terminalRecords(*capture, *fallbackRequestId);
        report->fallback_terminal_records = static_cast<double>(fallbackTerminals.size());
        assertEqual(fallbackTerminals.size(), 1, "the local attempt and provider fallback must settle exactly once");
        const json::value fallbackTerminal = fallbackTerminals.empty() ? json::object{} : fallbackTerminals.front();
        report->fallback_completed = rt::getBool(fallbackTerminal, "semantic_output_observed") == true && rt::getString(fallbackTerminal, "stream_terminal_type") == "response.completed" && rt::get(fallbackTerminal, "failure_kind") && rt::get(fallbackTerminal, "failure_kind")->is_null();
        assert_(report->fallback_completed, "the provider answer must settle as completed");
        assertEqual(rt::get(fallbackTerminal, "delivery_outcome"), "delivered");
        assertEqual(rt::get(fallbackTerminal, "provider"), "deepseek", "fallback provenance must name the actual provider");
        assertEqual(rt::get(fallbackTerminal, "usage_observed"), true, "provider usage must be retained");
        assertEqual(rt::get(fallbackTerminal, "input_tokens"), 7);
        assertEqual(rt::get(fallbackTerminal, "output_tokens"), 3);
        assertEqual(rt::get(fallbackTerminal, "total_tokens"), 10);
        rt::Promise<json::value> fallbackWindowPending(readRow(gateway.kv, usageWindowKey));
        const auto fallbackWindow = co_await fallbackWindowPending.wait();
        assert_(!fallbackWindow.is_null(), "the fallback must retain its durable usage window");
        report->fallback_committed_requests = rt::getNumber(fallbackWindow, "committed_requests").value_or(0);
        report->fallback_reserved_requests = rt::getNumber(fallbackWindow, "reserved_requests").value_or(0);
        assertEqual(report->fallback_committed_requests, rt::getNumber(beforeFallbackWindow, "committed_requests").value_or(0) + 1, "fallback consumes exactly one additional request");
        assertEqual(report->fallback_reserved_requests, 0, "fallback must leave no reserved request");
        auto fallbackKey = api_key_policy::apiKeyUsageV3RequestKey(gateway.policy, *fallbackRequestId);
        rt::Promise<json::value> fallbackUsagePending(readRow(gateway.kv, fallbackKey));
        const auto fallbackUsage = co_await fallbackUsagePending.wait();
        report->fallback_reservation_state = rt::getString(fallbackUsage, "state").value_or("absent");
        assertEqual(report->fallback_reservation_state, "dispatched", "the single request reservation must be consumed by provider dispatch");
        assertEqual(rt::get(fallbackUsage, "provider"), "deepseek");
        report->status = "PASS";
        report->failure_class = "none";
      } catch (...) { failure = std::current_exception(); }
      report->total_ms = rt::Math::round(rt::performanceNow() - started);
      ::uos::jev_compaction::compaction::setJevCompactionAskerForTest(nullptr);
      capture.reset();
      if (inheritedDeepSeekKey) rt::env::set("DEEPSEEK_API_KEY", *inheritedDeepSeekKey);
      else rt::env::remove("DEEPSEEK_API_KEY");
      rt::setGlobalFetch(originalFetch);
      rt::Promise<void> gatewayStopped(gateway.stop());
      co_await gatewayStopped.wait();
      auto upstreamStopped = upstream.stop();
      co_await upstreamStopped.wait();
      if (failure) std::rethrow_exception(failure);
    }
  }

  rt::Task<void> importMetaMain() {
    auto report = std::make_shared<Report>();
    try {
      rt::Promise<void> pending(main(report));
      co_await pending.wait();
    } catch (...) {
      const auto error = std::current_exception();
      report->status = "FAIL";
      report->failure_class = rt::isError(error) ? rt::errorName(error).value_or("Error") + ": " + rt::slice(rt::errorMessage(error).value_or(""), 0, 200) : "unknown";
    }
    rt::console::log(rt::jsonStringify(toJson(*report)));
    rt::process::exit(report->status == "PASS" ? 0 : 1);
  }
}
