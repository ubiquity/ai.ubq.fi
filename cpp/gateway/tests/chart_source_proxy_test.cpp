// Port of tests/chart-source-proxy.test.ts.
// Links: tests/chart_source_proxy_test.cpp src/admin/chart_source_proxy.cpp src/auth/local_admin.cpp
// src/provider/capacity_parse.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
// src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/http.cpp src/utils.cpp
// src/config.cpp src/release.cpp src/auth/relay.cpp src/provider/cerebras_rate_limits.cpp
// src/runtime/http.cpp src/runtime/http_upgrade.cpp src/runtime/websocket.cpp
#include "uos/admin/chart_source_proxy.hpp"
#include "uos/auth/local_admin.hpp"
#include "uos/provider/capacity_analytics.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch_transport.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/runtime/http.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace proxy = uos::admin::chart_source_proxy;
  namespace local = uos::auth::local_admin;
  using proxy::proxyAdminChartSource;
  constexpr double BUCKET_MS = 900000;
  const std::string CAPACITY_URL = "http://localhost:7999/admin/providers/capacity";
  rt::Response jsonResponse(const json::value& value, double status = 200) { return rt::Response(rt::jsonStringify(value), rt::ResponseInit{.status = status, .headers = rt::Headers{{"Content-Type", "application/json"}}}); }
  json::object sample(double index) { return {{"bucket_start_at_ms", rt::makeNumber(index * BUCKET_MS)}, {"sampled_at_ms", rt::makeNumber(index * BUCKET_MS + 1000)}, {"sources", json::array()}}; }
  double sampled(const json::value& value) { return *rt::getNumber(value, "sampled_at_ms"); }
  json::object capacityPayload(const std::string& label, double used, double time) {
    const json::object windows{{"primary", json::object{{"used_percent", rt::makeNumber(used)}}}};
    const json::object source{{"source", "codex"}, {"label", label}, {"slot", 1}, {"state", "available"}, {"windows", windows}};
    const json::object historySource{{"source", "codex"}, {"slot", 1}, {"state", "available"}, {"windows", windows}};
    json::object body{{"snapshot_at_ms", rt::makeNumber(time)}, {"stale_after_ms", 30000}, {"sources", json::array{source}}, {"history", json::array{json::object{{"sampled_at_ms", rt::makeNumber(time)}, {"sources", json::array{historySource}}}}}};
    body["reset_events"] = json::array{json::object{{"event_id", label + "-reset"}}};
    body["rate_limit_reset_events"] = json::array{json::object{{"event_id", label + "-rate-reset"}}};
    body["downtime_events"] = json::array{json::object{{"event_id", label + "-downtime"}}};
    body["prompt_cache"] = json::object{{"status", "ready"}, {"bucket_ms", 60000}, {"buckets", json::array{json::object{{"bucket_start_at_ms", rt::makeNumber(time)}, {"cached_share", rt::makeNumber(used / 100)}}}}};
    body["ledger_growth"] = json::object{{"estimated_bytes", rt::makeNumber(used)}};
    return body;
  }
  json::object localWithHistory(const json::array& history, double snapshot = 522 * BUCKET_MS) { auto body = capacityPayload("local", 7, snapshot); body["history"] = history; return body; }
  json::object remotePayload() { return capacityPayload("remote", 0, 2000); }
  struct Call { std::string method; std::string url; std::optional<std::string> authorization; std::optional<std::string> userAgent; json::value body; };
  using Calls = std::shared_ptr<std::vector<Call>>;
  using Responder = std::function<rt::Task<rt::Response>(Call)>;
  rt::Task<rt::Response> acknowledgePush(Call call) {
    if (call.method != "POST") return rt::fulfilledTask(jsonResponse(remotePayload()));
    const json::object body{{"accepted", rt::makeNumber(static_cast<double>(rt::getArray(call.body, "history")->size()))}, {"host", *rt::get(call.body, "host")}};
    return rt::fulfilledTask(jsonResponse(body));
  }
  rt::Fetch recordingFetch(Calls calls, Responder respond = acknowledgePush) {
    return rt::Fetch([calls, respond](rt::RequestInfo input, rt::RequestInit init) {
      const auto url = std::visit([](const auto& value) -> std::string { using T = std::decay_t<decltype(value)>; if constexpr (std::is_same_v<T, std::string>) return value; else if constexpr (std::is_same_v<T, rt::URL>) return value.href(); else return value.url(); }, input);
      const auto headers = init.headers.value_or(rt::Headers());
      json::value body = nullptr;
      if (init.body) if (const auto* text = std::get_if<std::string>(&*init.body)) body = rt::jsonParse(*text);
      const Call call{rt::toUpperCase(init.method.value_or("GET")), url, headers.get("Authorization"), headers.get("User-Agent"), body};
      calls->push_back(call);
      return respond(call);
    });
  }
  std::shared_ptr<proxy::CapacityPushState> state() { return std::make_shared<proxy::CapacityPushState>(); }
  proxy::ChartSourceProxyDeps deps(Calls calls) { proxy::ChartSourceProxyDeps result; result.isLoopbackAdmin = [](rt::Request) { return true; }; result.token = std::string("test-token"); result.remoteOrigin = "https://vps.example"; result.fetch = recordingFetch(calls); return result; }
  proxy::ChartSourceProxyDeps macDeps(Calls calls, std::shared_ptr<proxy::CapacityPushState> pushState) { auto result = deps(calls); result.isMacCompanionService = [] { return true; }; result.hostId = std::string("mac"); result.pushState = std::move(pushState); return result; }
  rt::Task<void> settle(std::shared_ptr<proxy::CapacityPushState> pushState) { if (pushState->inFlight) { const auto pending = *pushState->inFlight; co_await pending.wait(); } else co_await rt::microtask(); }
  rt::Task<rt::Response> poll(json::array history, std::shared_ptr<proxy::CapacityPushState> pushState, Calls calls, Responder respond = acknowledgePush, double snapshot = 522 * BUCKET_MS) {
    auto options = macDeps(calls, pushState); options.fetch = recordingFetch(calls, respond);
    auto task = proxyAdminChartSource(rt::Request(CAPACITY_URL), jsonResponse(localWithHistory(history, snapshot)), options);
    const auto response = co_await std::move(task);
    co_await settle(pushState);
    co_return response;
  }
  std::vector<std::vector<double>> pushedSamples(Calls calls) { std::vector<std::vector<double>> pushes; for (const auto& call : *calls) if (call.method == "POST") { std::vector<double> batch; for (const auto& point : *rt::getArray(call.body, "history")) batch.push_back(sampled(point)); pushes.push_back(batch); } return pushes; }
  std::vector<std::string> methods(Calls calls) { std::vector<std::string> out; for (const auto& call : *calls) out.push_back(call.method); return out; }
  json::object overlay(double at, double count) { return {{"object", "list"}, {"data", json::array()}, {"five_xx_buckets", json::array{json::object{{"bucket_start_at_ms", rt::makeNumber(at)}, {"count", rt::makeNumber(count)}}}}}; }
}
ASYNC_TEST("chart proxy leaves non-loopback admin responses local") {
  auto calls = std::make_shared<std::vector<Call>>(); auto options = deps(calls); options.isLoopbackAdmin = [](rt::Request) { return false; };
  const auto localResponse = jsonResponse(capacityPayload("local", 7, 1000)); AWAIT(response, proxyAdminChartSource(rt::Request("https://ai.ubq.fi/admin/providers/capacity"), localResponse, options));
  EXPECT(response == localResponse); EXPECT(calls->empty());
}
ASYNC_TEST("the Mac service pushes its samples oldest first beside the canonical chart read") {
  auto pushState = state(); auto calls = std::make_shared<std::vector<Call>>(); auto options = macDeps(calls, pushState);
  const auto localPayload = localWithHistory(json::array{sample(2), sample(1)});
  AWAIT(response, proxyAdminChartSource(rt::Request(CAPACITY_URL + "?refresh=live", rt::RequestInit{.headers = rt::Headers{{"Authorization", "Bearer browser-token"}}}), jsonResponse(localPayload), options)); co_await settle(pushState);
  AWAIT(body, response.json()); ASSERT_EQ(calls->size(), std::size_t(2)); EXPECT_EQ((*calls)[0].url, "https://vps.example/admin/providers/capacity/observations"); EXPECT_EQ((*calls)[1].url, "https://vps.example/admin/providers/capacity");
  EXPECT_EQ(methods(calls), (std::vector<std::string>{"POST", "GET"}));
  for (const auto& call : *calls) EXPECT_EQ(call.authorization, std::optional<std::string>("Bearer test-token"));
  EXPECT(rt::includes((*calls)[0].userAgent.value_or(""), "Mozilla")); EXPECT(rt::deepEqual((*calls)[0].body, json::object{{"host", "mac"}, {"history", json::array{sample(1), sample(2)}}}));
  EXPECT_EQ(pushState->acknowledgedSampledAtMs, std::optional(sampled(sample(2)))); EXPECT(rt::deepEqual(*rt::get(body, "sources"), *rt::get(localPayload, "sources"))); EXPECT(rt::deepEqual(*rt::get(body, "snapshot_at_ms"), *rt::get(localPayload, "snapshot_at_ms")));
  const auto remote = remotePayload(); for (const std::string field : {"history", "reset_events", "rate_limit_reset_events", "downtime_events", "prompt_cache", "ledger_growth"}) EXPECT(rt::deepEqual(*rt::get(body, field), *rt::get(remote, field)));
}
ASYNC_TEST("the chart read never waits for the push, and only one push runs at a time") {
  auto pushState = state(); auto calls = std::make_shared<std::vector<Call>>(); auto deferred = rt::withResolvers<rt::Response>();
  const auto pending = deferred.promise; const auto release = deferred.resolve;
  auto options = macDeps(calls, pushState); options.fetch = recordingFetch(calls, [pending](Call call) { return call.method == "POST" ? pending.wait() : rt::fulfilledTask(jsonResponse(remotePayload())); });
  AWAIT(first, proxyAdminChartSource(rt::Request(CAPACITY_URL), jsonResponse(localWithHistory(json::array{sample(1)})), options));
  AWAIT(second, proxyAdminChartSource(rt::Request(CAPACITY_URL), jsonResponse(localWithHistory(json::array{sample(1)})), options));
  EXPECT_EQ(first.status(), 200); EXPECT_EQ(second.status(), 200); EXPECT_EQ(methods(calls), (std::vector<std::string>{"POST", "GET", "GET"})); EXPECT(!pushState->acknowledgedSampledAtMs);
  release(jsonResponse(json::object{{"accepted", 1}, {"host", "mac"}})); co_await settle(pushState); EXPECT_EQ(pushState->acknowledgedSampledAtMs, std::optional(sampled(sample(1))));
}
ASYNC_TEST("each sample is pushed once instead of the whole week on every poll") {
  auto s = state(); auto calls = std::make_shared<std::vector<Call>>();
  AWAIT(a, poll(json::array{sample(1), sample(2)}, s, calls)); AWAIT(b, poll(json::array{sample(1), sample(2)}, s, calls)); AWAIT(c, poll(json::array{sample(1), sample(2), sample(3)}, s, calls));
  EXPECT_EQ(pushedSamples(calls), (std::vector<std::vector<double>>{{sampled(sample(1)), sampled(sample(2))}, {sampled(sample(3))}})); EXPECT_EQ(std::count_if(calls->begin(), calls->end(), [](const auto& call) { return call.method == "GET"; }), 3);
}
ASYNC_TEST("a failed or unacknowledged push is retried with the same samples") {
  auto s = state(); auto calls = std::make_shared<std::vector<Call>>(); auto number = std::make_shared<int>(0);
  const Responder respond = [number](Call call) { if (call.method != "POST") return acknowledgePush(call); const int n = (*number)++; return rt::fulfilledTask(n == 0 ? rt::Response("push unavailable", rt::ResponseInit{.status = 503}) : n == 1 ? jsonResponse(json::object{{"ok", true}}) : jsonResponse(json::object{{"accepted", 1}, {"host", "mac"}})); };
  for (int i = 0; i < 4; ++i) { AWAIT(response, poll(json::array{sample(1)}, s, calls, respond)); EXPECT_EQ(response.status(), 200); }
  EXPECT_EQ(pushedSamples(calls), (std::vector<std::vector<double>>(3, {sampled(sample(1))})));
}
ASYNC_TEST("a single sample the VPS refuses is skipped instead of blocking later samples") {
  const std::vector<int> statuses{400, 413};
  for (const auto status : statuses) { auto s = state(); auto calls = std::make_shared<std::vector<Call>>(); auto refused = std::make_shared<bool>(false);
    const Responder respond = [refused, status](Call call) { if (call.method == "POST" && !*refused) { *refused = true; return rt::fulfilledTask(jsonResponse(json::object(), status)); } return acknowledgePush(call); };
    AWAIT(a, poll(json::array{sample(1)}, s, calls, respond)); AWAIT(b, poll(json::array{sample(1), sample(2)}, s, calls, respond)); EXPECT_EQ(pushedSamples(calls), (std::vector<std::vector<double>>{{sampled(sample(1))}, {sampled(sample(2))}})); }
}
ASYNC_TEST("an open bucket sends only immutable transition points until it closes") {
  auto s = state(); auto calls = std::make_shared<std::vector<Call>>(); const auto closed = sample(1), live = sample(2); auto transition = live, latest = live; transition["sampled_at_ms"] = rt::makeNumber(sampled(live) + 30000); latest["sampled_at_ms"] = rt::makeNumber(sampled(live) + 60000); auto final = latest; final["sampled_at_ms"] = rt::makeNumber(sampled(latest) + 30000);
  AWAIT(a, poll(json::array{closed, live}, s, calls, acknowledgePush, sampled(live))); AWAIT(b, poll(json::array{closed, transition}, s, calls, acknowledgePush, sampled(transition))); AWAIT(c, poll(json::array{closed, transition, latest}, s, calls, acknowledgePush, sampled(latest))); AWAIT(d, poll(json::array{closed, transition, final}, s, calls, acknowledgePush, sampled(final))); AWAIT(e, poll(json::array{closed, transition, final}, s, calls, acknowledgePush, 3 * BUCKET_MS));
  EXPECT_EQ(pushedSamples(calls), (std::vector<std::vector<double>>{{sampled(closed)}, {sampled(transition)}, {sampled(final)}}));
}
ASYNC_TEST("a refused catch-up batch isolates only the bad row and delivers every valid row") {
  json::array history; for (int i = 0; i < 512; ++i) history.push_back(sample(i)); const std::vector<int> statuses{400, 413}, indexes{0, 250, 511};
  for (const auto status : statuses) for (const auto badIndex : indexes) { auto s = state(); auto calls = std::make_shared<std::vector<Call>>(); auto stored = std::make_shared<std::vector<double>>(); const double bad = sampled(sample(badIndex));
    const Responder respond = [status, bad, stored](Call call) { if (call.method == "POST") { std::vector<double> batch; for (const auto& point : *rt::getArray(call.body, "history")) batch.push_back(sampled(point)); if (std::find(batch.begin(), batch.end(), bad) != batch.end()) return rt::fulfilledTask(jsonResponse(json::object(), status)); stored->insert(stored->end(), batch.begin(), batch.end()); } return acknowledgePush(call); };
    for (int i = 0; i < 12 && s->acknowledgedSampledAtMs.value_or(-1) < sampled(history.back()); ++i) { AWAIT(response, poll(history, s, calls, respond)); EXPECT_EQ(response.status(), 200); }
    std::vector<double> expected; for (int i = 0; i < 512; ++i) if (i != badIndex) expected.push_back(sampled(sample(i))); EXPECT_EQ(*stored, expected); EXPECT_EQ(s->acknowledgedSampledAtMs, std::optional(sampled(history.back())));
    const auto pushes = pushedSamples(calls); EXPECT(std::any_of(pushes.begin(), pushes.end(), [bad](const auto& batch) { return batch.size() == 1 && batch[0] == bad; })); }
}
ASYNC_TEST("timeouts halve later catch-up batches without acknowledging or losing samples") {
  auto s = state(); auto calls = std::make_shared<std::vector<Call>>(); auto stored = std::make_shared<std::vector<double>>(); json::array history; for (int i = 0; i < 517; ++i) history.push_back(sample(i));
  const Responder respond = [stored](Call call) { if (call.method == "POST") { const auto* batch = rt::getArray(call.body, "history"); if (batch->size() > 64) return rt::Promise<rt::Response>::reject(rt::makeReason(rt::DOMException("Slow upload", "TimeoutError"))).wait(); for (const auto& point : *batch) stored->push_back(sampled(point)); } return acknowledgePush(call); };
  const std::vector<double> sizes{256, 128, 64}; for (const auto size : sizes) { AWAIT(response, poll(history, s, calls, respond)); EXPECT(!s->acknowledgedSampledAtMs); EXPECT_EQ(s->batchSize, std::optional(size)); }
  for (int i = 0; i < 10; ++i) { AWAIT(response, poll(history, s, calls, respond)); EXPECT_EQ(response.status(), 200); }
  std::vector<double> expected; for (const auto& point : history) expected.push_back(sampled(point)); EXPECT_EQ(*stored, expected); std::vector<std::size_t> lengths; for (const auto& batch : pushedSamples(calls)) lengths.push_back(batch.size()); EXPECT_EQ(lengths, (std::vector<std::size_t>{512, 256, 128, 64, 64, 64, 64, 64, 64, 64, 64, 5})); EXPECT_EQ(s->batchSize, std::optional(64.0)); EXPECT(!s->inFlight);
}
ASYNC_TEST("a timeout at the one-row floor keeps that row pending until acknowledged") {
  auto s = state(); auto calls = std::make_shared<std::vector<Call>>(); const Responder timeout = [](Call call) { return call.method == "POST" ? rt::Promise<rt::Response>::reject(rt::makeReason(rt::DOMException("Slow upload", "TimeoutError"))).wait() : acknowledgePush(call); };
  for (int i = 0; i < 2; ++i) { AWAIT(response, poll(json::array{sample(1)}, s, calls, timeout)); EXPECT(!s->acknowledgedSampledAtMs); EXPECT_EQ(s->batchSize, std::optional(1.0)); } AWAIT(response, poll(json::array{sample(1)}, s, calls)); EXPECT_EQ(pushedSamples(calls), (std::vector<std::vector<double>>(3, {sampled(sample(1))}))); EXPECT_EQ(s->acknowledgedSampledAtMs, std::optional(sampled(sample(1))));
}
ASYNC_TEST("a long history is pushed oldest first, one ingest batch per poll") {
  auto s = state(); auto calls = std::make_shared<std::vector<Call>>(); json::array history; for (int i = 516; i >= 0; --i) history.push_back(sample(i));
  for (int i = 0; i < 3; ++i) { AWAIT(response, poll(history, s, calls)); EXPECT_EQ(response.status(), 200); } const auto pushes = pushedSamples(calls); ASSERT_EQ(pushes.size(), std::size_t(2)); EXPECT_EQ(pushes[0].size(), std::size_t(512)); EXPECT_EQ(pushes[1].size(), std::size_t(5)); EXPECT_EQ(pushes[0][0], sampled(sample(0))); EXPECT_EQ(pushes[1][0], sampled(sample(512)));
}
ASYNC_TEST("only the Mac companion service pushes; a development server with the same token only reads") {
  const uos::runtime::http::Addr address = uos::runtime::http::NetAddr{"tcp", "127.0.0.1", 0}; const local::ServeRuntimeOptions enabled{true}, disabled{false}; auto cleanup = rt::scopeExit([&] { local::configureAdminAuthForListener(disabled, address); });
  for (int i = 0; i < 3; ++i) { if (i == 1) local::configureMacLocalAdminAuthBypassForListener(uos::runtime::http::NetAddr{"tcp", "0.0.0.0", 0}); else local::configureAdminAuthForListener(enabled, address);
    auto s = state(); auto calls = std::make_shared<std::vector<Call>>(); auto options = deps(calls); options.hostId = std::string("mac"); options.pushState = s;
    AWAIT(response, proxyAdminChartSource(rt::Request(CAPACITY_URL), jsonResponse(localWithHistory(json::array{sample(1)})), options)); co_await settle(s); EXPECT_EQ(methods(calls), i == 1 ? (std::vector<std::string>{"POST", "GET"}) : (std::vector<std::string>{"GET"})); }
}
ASYNC_TEST("the push defaults to the Mac identity and validates an explicitly injected host") {
  const std::vector<rt::Nullable<std::string>> hosts{rt::undefined, std::string("mac-studio"), std::string("mac-mini"), std::string(""), std::string("Mac Mini"), nullptr}; const std::vector<std::optional<std::string>> expected{std::string("mac"), std::string("mac-studio"), std::string("mac-mini"), std::nullopt, std::nullopt, std::nullopt};
  for (std::size_t i = 0; i < hosts.size(); ++i) { auto s = state(); auto calls = std::make_shared<std::vector<Call>>(); auto options = macDeps(calls, s); options.hostId = hosts[i]; AWAIT(response, proxyAdminChartSource(rt::Request(CAPACITY_URL), jsonResponse(localWithHistory(json::array{sample(1)})), options)); co_await settle(s); std::vector<std::string> seen; for (const auto& call : *calls) if (call.method == "POST") seen.emplace_back(*rt::getString(call.body, "host")); EXPECT_EQ(seen, expected[i] ? std::vector<std::string>{*expected[i]} : std::vector<std::string>()); }
}
ASYNC_TEST("chart proxy serves only the canonical five-xx buckets and keeps the local error rows") {
  auto calls = std::make_shared<std::vector<Call>>(); auto options = deps(calls); auto localPayload = overlay(1000, 1), remote = overlay(2000, 9); localPayload["data"] = json::array{json::object{{"status", 500}, {"id", "local-latest"}}}; localPayload["retention"] = json::object{{"status", "local"}}; remote["data"] = json::array{json::object{{"status", 502}, {"id", "vps-latest"}}}; remote["retention"] = json::object{{"status", "vps"}}; options.fetch = recordingFetch(calls, [remote](Call) { return rt::fulfilledTask(jsonResponse(remote)); });
  AWAIT(response, proxyAdminChartSource(rt::Request("http://localhost:7999/admin/errors?limit=1&refresh=live"), jsonResponse(localPayload), options)); AWAIT(body, response.json()); ASSERT_EQ(calls->size(), std::size_t(1)); EXPECT_EQ((*calls)[0].url, "https://vps.example/admin/errors?limit=1"); EXPECT_EQ(response.headers().get("Cache-Control"), std::optional<std::string>("no-store")); localPayload["five_xx_buckets"] = *rt::get(remote, "five_xx_buckets"); EXPECT(rt::deepEqual(body, localPayload));
}
ASYNC_TEST("chart proxy re-encodes a compressed five-xx overlay instead of forwarding its transfer headers") {
  const auto remote = overlay(2000, 9); const auto compressed = rt::gzip::gzipSync(rt::encodeUtf8(rt::jsonStringify(remote))); auto encodings = std::make_shared<std::vector<std::optional<std::string>>>();
  const uos::runtime::http::ServeHandler handler = [compressed, encodings](rt::Request request, uos::runtime::http::ServeHandlerInfo) { encodings->push_back(request.headers().get("Accept-Encoding")); return rt::fulfilledTask(rt::Response(compressed, rt::ResponseInit{.headers = rt::Headers{{"Content-Type", "application/json"}, {"Content-Encoding", "gzip"}, {"Content-Length", rt::numberToString(static_cast<double>(compressed.size()))}}})); };
  const uos::runtime::http::ServeHttpOptions serverOptions{.hostname = "127.0.0.1", .port = 0}; auto server = uos::runtime::http::serveHttp(serverOptions, handler); const auto ready = co_await server.ready.wait();
  const auto previous = rt::fetchTransport(); auto transport = rt::installHttpClientTransport(); auto cleanup = rt::scopeExit([&] { transport->closeIdleConnections(); rt::setFetchTransport(previous); });
  auto calls = std::make_shared<std::vector<Call>>(); auto options = deps(calls); options.fetch = rt::defaultFetch(); options.remoteOrigin = "http://127.0.0.1:" + rt::numberToString(ready.port);
  const json::object localOverlay{{"object", "list"}, {"data", json::array()}, {"five_xx_buckets", json::array()}}; AWAIT(response, proxyAdminChartSource(rt::Request("http://localhost:7999/admin/errors?limit=1"), jsonResponse(localOverlay), options)); AWAIT(body, response.json());
  EXPECT_EQ(encodings->size(), std::size_t(1)); EXPECT(rt::includes(encodings->at(0).value_or(""), "gzip")); EXPECT_EQ(response.status(), 200); EXPECT(!response.headers().get("Content-Encoding")); EXPECT(response.headers().get("Content-Length") != std::optional(rt::numberToString(static_cast<double>(compressed.size())))); EXPECT(rt::deepEqual(body, remote)); transport->closeIdleConnections(); co_await server.shutdown().wait();
}
ASYNC_TEST("chart proxy drops stale decoded-body transfer headers from the five-xx overlay") {
  const auto remote = overlay(2000, 9); const auto decoded = rt::Response(rt::jsonStringify(remote), rt::ResponseInit{.headers = rt::Headers{{"Content-Type", "application/json"}, {"Content-Encoding", "br"}, {"Content-Length", "17"}}}); auto calls = std::make_shared<std::vector<Call>>(); auto options = deps(calls); options.fetch = rt::Fetch([decoded](rt::RequestInfo, rt::RequestInit) { return rt::fulfilledTask(decoded); }); AWAIT(response, proxyAdminChartSource(rt::Request("http://localhost:7999/admin/errors?limit=1"), jsonResponse(overlay(1000, 1)), options)); AWAIT(body, response.json()); EXPECT(!response.headers().get("Content-Encoding")); EXPECT(response.headers().get("Content-Length") != std::optional<std::string>("17")); EXPECT(rt::deepEqual(body, remote));
}
ASYNC_TEST("chart proxy keeps the local five-xx overlay when the remote body is unusable") {
  const std::vector<rt::Response> remotes{rt::Response("not json"), jsonResponse(json::object{{"object", "list"}, {"data", json::array()}}), jsonResponse(json::object{{"five_xx_buckets", "9"}})};
  for (const auto& remote : remotes) { auto calls = std::make_shared<std::vector<Call>>(); auto options = deps(calls); options.fetch = rt::Fetch([remote](rt::RequestInfo, rt::RequestInit) { return rt::fulfilledTask(remote); }); const auto localResponse = jsonResponse(overlay(1000, 1)); AWAIT(response, proxyAdminChartSource(rt::Request("http://localhost:7999/admin/errors?limit=1"), localResponse, options)); EXPECT(response == localResponse); }
}
ASYNC_TEST("chart proxy releases the body of a failed remote read") {
  auto cancelled = std::make_shared<int>(0); rt::UnderlyingSource<rt::ByteChunk> source; source.pull = [](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) { controller.enqueue(rt::encodeUtf8("upstream unavailable ")); }; source.cancel = [cancelled](rt::Reason) { ++*cancelled; }; const rt::ByteStream stream(source); const auto remote = rt::Response(stream, rt::ResponseInit{.status = 503}); auto calls = std::make_shared<std::vector<Call>>(); auto options = deps(calls); options.fetch = rt::Fetch([remote](rt::RequestInfo, rt::RequestInit) { return rt::fulfilledTask(remote); }); const auto localResponse = jsonResponse(overlay(1000, 1)); AWAIT(response, proxyAdminChartSource(rt::Request("http://localhost:7999/admin/errors?limit=1"), localResponse, options)); EXPECT(response == localResponse); EXPECT_EQ(*cancelled, 1);
}
ASYNC_TEST("chart proxy leaves the fuller errors view local") {
  auto calls = std::make_shared<std::vector<Call>>(); auto options = deps(calls); const auto localResponse = jsonResponse(overlay(1000, 1)); AWAIT(response, proxyAdminChartSource(rt::Request("http://localhost:7999/admin/errors?limit=200"), localResponse, options)); EXPECT(response == localResponse); EXPECT(calls->empty());
}
ASYNC_TEST("chart proxy falls back to local on a missing token or failed remote read") {
  auto calls = std::make_shared<std::vector<Call>>(); auto options = deps(calls); options.token = nullptr; const auto localResponse = jsonResponse(capacityPayload("local", 7, 1000)); AWAIT(missing, proxyAdminChartSource(rt::Request(CAPACITY_URL), localResponse, options)); EXPECT(missing == localResponse); options.token = std::string("test-token"); options.fetch = rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return rt::fulfilledTask(rt::Response("upstream unavailable", rt::ResponseInit{.status = 503})); }); AWAIT(failed, proxyAdminChartSource(rt::Request(CAPACITY_URL), localResponse, options)); EXPECT(failed == localResponse);
}
ASYNC_TEST("chart proxy still serves the canonical chart when only the push fails") {
  auto s = state(); auto calls = std::make_shared<std::vector<Call>>(); const Responder respond = [](Call call) { return rt::fulfilledTask(call.method == "POST" ? rt::Response("push unavailable", rt::ResponseInit{.status = 503}) : jsonResponse(remotePayload())); }; AWAIT(response, poll(json::array{sample(1)}, s, calls, respond)); AWAIT(body, response.json()); EXPECT_EQ(methods(calls), (std::vector<std::string>{"POST", "GET"})); EXPECT(!s->acknowledgedSampledAtMs); const auto remote = remotePayload(), localPayload = localWithHistory(json::array{sample(1)}); EXPECT(rt::deepEqual(*rt::get(body, "history"), *rt::get(remote, "history"))); EXPECT(rt::deepEqual(*rt::get(body, "sources"), *rt::get(localPayload, "sources")));
}
ASYNC_TEST("chart proxy reads the admin token from the Bun process environment") {
  rt::env::ScopedEnvironment environment({}); const std::vector<std::pair<std::string, std::string>> cases{{"env-token", "Bearer env-token"}, {" first-token,\nsecond-token ", "Bearer first-token"}};
  for (const auto& [configured, expected] : cases) { rt::env::set("DENO_DEPLOY_TOKEN", configured); auto calls = std::make_shared<std::vector<Call>>(); auto options = deps(calls); options.token = rt::undefined; options.fetch = recordingFetch(calls, [](Call) { return rt::fulfilledTask(jsonResponse(overlay(2000, 9))); }); AWAIT(response, proxyAdminChartSource(rt::Request("http://localhost:7999/admin/errors?limit=1"), jsonResponse(overlay(1000, 1)), options)); AWAIT(body, response.json()); ASSERT_EQ(calls->size(), std::size_t(1)); EXPECT_EQ((*calls)[0].authorization, std::optional(expected)); const auto remote = overlay(2000, 9); EXPECT(rt::deepEqual(*rt::get(body, "five_xx_buckets"), *rt::get(remote, "five_xx_buckets"))); }
}
