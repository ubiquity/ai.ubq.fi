// Port of tests/native-codex429-conformance.test.ts, with its shared vector block
// (runCodex429ConformanceVectors). The second case compares everything the vectors return with
// the JSON Bun 1.4.0 printed for them (tests/native_codex429_conformance_vectors.inc).
#include "uos/authority/native_codex_429.hpp"
#include "uos/authority/native_codex_capacity.hpp"
#include "uos/authority/native_codex_contracts.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "native_codex429_conformance_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
namespace codex429 = uos::authority::native_codex_429;
namespace capacity = uos::authority::native_codex_capacity;
using namespace uos::authority::native_codex_contracts;

namespace {
  // bun:test arms a per-test timeout timer (5 s by default), which keeps the event loop alive
  // while the stalled body waits for readBoundedResponseBody's unreferenced AbortSignal.timeout.
  struct BunTestTimeout {
    rt::Timeout timer = rt::setTimeout([] {}, 5'000);
    BunTestTimeout() = default;
    BunTestTimeout(const BunTestTimeout&) = delete;
    BunTestTimeout& operator=(const BunTestTimeout&) = delete;
    ~BunTestTimeout() { rt::clearTimeout(timer); }
  };

  // BEGIN SHARED_CODEX429_VECTORS
  void proveCodex429(bool condition, const std::string& message) {
    if (!condition) throw rt::JsError(message);
  }

  template <class T> T requireCodex429(std::optional<T> value, const std::string& message) {
    if (!value) throw rt::JsError(message);
    return std::move(*value);
  }

  json::value orNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
  json::value orNull(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }

  json::value bytesJson(const rt::Bytes& bytes) {
    json::array array;
    for (const std::uint8_t byte : bytes) array.push_back(rt::makeNumber(static_cast<double>(byte)));
    return array;
  }

  json::value headersJson(const rt::Headers& headers) {
    json::array array;
    for (const rt::Headers::Entry& entry : headers.entries()) array.push_back(json::array{json::value(entry.first), json::value(entry.second)});
    return array;
  }

  // `{ ...parsed, response: { bytes, headers: [...parsed.response.headers], status } }`
  json::value classifiedJson(const codex429::Codex429Classification& parsed, const rt::Bytes& responseBytes) {
    json::object response;
    response["bytes"] = bytesJson(responseBytes);
    response["headers"] = headersJson(parsed.response.headers());
    response["status"] = rt::makeNumber(static_cast<double>(parsed.response.status()));
    json::object object;
    object["response"] = std::move(response);
    object["usageLimitReached"] = parsed.usageLimitReached;
    object["retryAtMs"] = orNull(parsed.retryAtMs);
    object["quotaBlockSource"] = orNull(parsed.quotaBlockSource);
    object["resetDeadlineIsStable"] = parsed.resetDeadlineIsStable;
    object["resetDeadlineConflict"] = parsed.resetDeadlineConflict;
    return object;
  }

  // `body.error.code`, which throws TypeError when `error` is missing.
  std::string errorCode(const json::value& body) {
    const json::value* error = rt::get(body, "error");
    if (error == nullptr) throw rt::TypeError("undefined is not an object (evaluating 'body.error.code')");
    return rt::toString(rt::get(*error, "code"));
  }

  json::value retryAfterJson(const std::optional<codex429::RetryAfterDeadline>& deadline) {
    if (!deadline) return nullptr;
    return json::object{{"deadlineMs", rt::makeNumber(deadline->deadlineMs)}, {"isStable", deadline->isStable}};
  }

  struct ClassifyState {
    double now;
    json::object classified;
  };

  // `classify(name, bytes, retryAfter?)` with `bytes: string | Uint8Array`.
  rt::Task<codex429::Codex429Classification> classify(std::shared_ptr<ClassifyState> state, std::string name, rt::BodyInit bytes,
                                                      std::optional<std::string> retryAfter) {
    rt::Headers headers{{"content-type", "application/json"}, {"x-request-id", "synthetic-429"}};
    if (retryAfter) headers.set("retry-after", *retryAfter);
    const rt::Bytes expectedBytes = std::holds_alternative<std::string>(bytes) ? rt::encodeUtf8(std::get<std::string>(bytes)) : std::get<rt::Bytes>(bytes);
    rt::ResponseInit init;
    init.status = 429;
    init.statusText = "Too Many Requests";
    init.headers = std::move(headers);
    const rt::Response upstream(std::move(bytes), std::move(init));
    auto classifying = codex429::classifyCodex429(upstream, state->now);
    codex429::Codex429Classification parsed = co_await std::move(classifying);
    const rt::Response copy = parsed.response.clone();
    auto reading = copy.arrayBuffer();
    const rt::Bytes responseBytes = co_await std::move(reading);
    proveCodex429(rt::jsonStringify(bytesJson(responseBytes)) == rt::jsonStringify(bytesJson(expectedBytes)), name + ": complete upstream bytes changed");
    proveCodex429(parsed.response.status() == 429 && parsed.response.headers().get("x-request-id") == "synthetic-429", name + ": response metadata changed");
    state->classified[name] = classifiedJson(parsed, responseBytes);
    co_return parsed;
  }

  struct ClassificationVectors {
    json::object classified;
    json::object retryAfter;
    json::object oversized;
    json::object incomplete;
    codex429::Codex429Classification absolute;
    codex429::Codex429Classification relative;
    codex429::Codex429Classification invalidReset;
  };

  rt::Task<ClassificationVectors> classifyCodex429Vectors(double now) {
    const double deadline = now + 60'000;
    const std::string usage = rt::jsonStringify(json::object{{"error", json::object{{"type", "usage_limit_reached"}}}});
    const std::string stableBody =
      rt::jsonStringify(json::object{{"error", json::object{{"type", "usage_limit_reached"}, {"resets_at", rt::makeNumber(deadline / 1'000)}}}});
    auto state = std::make_shared<ClassifyState>();
    state->now = now;

    auto absoluteTask = classify(state, "body-absolute", rt::BodyInit(stableBody), std::nullopt);
    const codex429::Codex429Classification absolute = co_await std::move(absoluteTask);
    proveCodex429(absolute.usageLimitReached && absolute.retryAtMs == deadline && absolute.resetDeadlineIsStable, "body deadline lost");
    auto relativeTask = classify(state, "header-relative", rt::BodyInit(usage), std::string("120"));
    const codex429::Codex429Classification relative = co_await std::move(relativeTask);
    proveCodex429(relative.retryAtMs == now + 120'000 && !relative.resetDeadlineIsStable, "relative deadline became stable");
    auto dateTask = classify(state, "header-absolute", rt::BodyInit(usage), rt::Date(deadline).toUTCString());
    const codex429::Codex429Classification date = co_await std::move(dateTask);
    proveCodex429(date.retryAtMs == deadline && date.resetDeadlineIsStable && date.quotaBlockSource == "header_retry_after", "HTTP date lost");
    auto conflictTask = classify(state, "absolute-conflict", rt::BodyInit(stableBody), rt::Date(now + 120'000).toUTCString());
    const codex429::Codex429Classification conflict = co_await std::move(conflictTask);
    proveCodex429(conflict.retryAtMs == now + 120'000 && conflict.resetDeadlineConflict && !conflict.resetDeadlineIsStable,
                  "absolute conflict reopened redemption");
    auto shorterTask = classify(state, "relative-shorter", rt::BodyInit(stableBody), std::string("30"));
    const codex429::Codex429Classification shorter = co_await std::move(shorterTask);
    proveCodex429(shorter.retryAtMs == deadline && !shorter.resetDeadlineConflict && shorter.resetDeadlineIsStable, "shorter relative delay changed identity");
    auto longerTask = classify(state, "relative-longer", rt::BodyInit(stableBody), std::string("120"));
    const codex429::Codex429Classification longer = co_await std::move(longerTask);
    proveCodex429(longer.retryAtMs == now + 120'000 && longer.resetDeadlineConflict && !longer.resetDeadlineIsStable, "longer relative delay lost ambiguity");

    const std::vector<std::pair<std::string, std::string>> ambiguous = {
      {"duplicate-error", R"({"error":{"type":"other"},"error":{"type":"usage_limit_reached","resets_at":1700000060}})"},
      {"duplicate-escaped-key", R"({"error":{"type":"other","t\u0079pe":"usage_limit_reached","resets_at":1700000060}})"},
      {"malformed-json", R"({"error":{"type":"usage_limit_reached"})"},
      {"wrong-shape", R"([{"error":{"type":"usage_limit_reached"}}])"},
      {"ordinary-429", R"({"error":{"type":"rate_limit_error"}})"},
    };
    for (const auto& [name, body] : ambiguous) {
      auto task = classify(state, name, rt::BodyInit(body), std::string("15"));
      const codex429::Codex429Classification parsed = co_await std::move(task);
      proveCodex429(!parsed.usageLimitReached && parsed.retryAtMs == now + 15'000 && !parsed.resetDeadlineIsStable, name + ": ambiguous body became exhaustion");
    }
    rt::Bytes malformedBytes{0xff};
    const rt::Bytes stableBytes = rt::encodeUtf8(stableBody);
    malformedBytes.insert(malformedBytes.end(), stableBytes.begin(), stableBytes.end());
    auto malformedTask = classify(state, "malformed-utf8", rt::BodyInit(std::move(malformedBytes)), std::string("15"));
    const codex429::Codex429Classification malformedUtf8 = co_await std::move(malformedTask);
    proveCodex429(!malformedUtf8.usageLimitReached, "malformed UTF-8 became exhaustion");
    auto invalidResetTask = classify(state, "invalid-body-reset", rt::BodyInit(std::string(R"({"error":{"type":"usage_limit_reached","resets_at":1700000060.5}})")), std::nullopt);
    const codex429::Codex429Classification invalidReset = co_await std::move(invalidResetTask);
    proveCodex429(invalidReset.usageLimitReached && !invalidReset.retryAtMs, "fractional reset became a deadline");
    json::object retryAfter;
    std::vector<std::pair<std::string, std::optional<codex429::RetryAfterDeadline>>> retryAfterEntries;
    for (const std::string value : {"0", "-1", "+1", "1.5", "1e2", "9007199254740991", "Tue, 14 Nov 2023 22:14:20 GMT", "Wed, 14 Nov 2023 22:14:20 GMT"}) {
      const rt::Headers headers{{"retry-after", value}};
      const std::optional<codex429::RetryAfterDeadline> parsed = codex429::futureRetryAfterDeadline(headers, now);
      rt::objectSet(retryAfter, value, retryAfterJson(parsed));
      retryAfterEntries.emplace_back(value, parsed);
    }
    const auto entryFor = [&retryAfterEntries](const std::string& key) {
      for (const auto& [value, parsed] : retryAfterEntries)
        if (value == key) return parsed;
      return std::optional<codex429::RetryAfterDeadline>();
    };
    proveCodex429(entryFor("Tue, 14 Nov 2023 22:14:20 GMT").has_value() && !entryFor("Wed, 14 Nov 2023 22:14:20 GMT").has_value(), "noncanonical date was accepted");
    bool everyOtherNull = true;
    for (const auto& [value, parsed] : retryAfterEntries)
      if (value != "Tue, 14 Nov 2023 22:14:20 GMT") everyOtherNull = everyOtherNull && !parsed.has_value();
    proveCodex429(everyOtherNull, "invalid Retry-After was accepted");

    rt::ResponseInit oversizedInit;
    oversizedInit.status = 429;
    oversizedInit.headers = rt::Headers{{"retry-after", "15"}};
    const rt::Response oversizedResponse(rt::BodyInit(std::string(65'537, 'x')), std::move(oversizedInit));
    auto oversizedTask = codex429::classifyCodex429(oversizedResponse, now);
    const codex429::Codex429Classification oversized = co_await std::move(oversizedTask);
    auto oversizedRead = oversized.response.json();
    const json::value oversizedBody = co_await std::move(oversizedRead);
    proveCodex429(!oversized.usageLimitReached && errorCode(oversizedBody) == "codex_rate_limit_response_truncated", "oversized response was forwarded");
    auto cancellations = std::make_shared<std::vector<std::string>>();
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [stableBody](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) { controller.enqueue(rt::encodeUtf8(stableBody)); };
    source.cancel = [cancellations](rt::Reason) { cancellations->push_back("cancelled"); };
    rt::ResponseInit stalledInit;
    stalledInit.status = 429;
    stalledInit.headers = rt::Headers{{"retry-after", "15"}};
    const rt::Response stalledResponse(rt::BodyInit(rt::ByteStream(std::move(source))), std::move(stalledInit));
    auto stalledTask = codex429::classifyCodex429(stalledResponse, now);
    const codex429::Codex429Classification stalled = co_await std::move(stalledTask);
    auto stalledRead = stalled.response.json();
    const json::value stalledBody = co_await std::move(stalledRead);
    proveCodex429(cancellations->size() == 1 && !stalled.usageLimitReached && errorCode(stalledBody) == "codex_rate_limit_response_truncated",
                  "incomplete response became exhaustion");

    ClassificationVectors vectors{
      .classified = std::move(state->classified),
      .retryAfter = std::move(retryAfter),
      .oversized = json::object{{"body", oversizedBody}, {"retryAtMs", orNull(oversized.retryAtMs)}},
      .incomplete = json::object{{"body", stalledBody}, {"cancelled", cancellations->size() == 1}, {"retryAtMs", orNull(stalled.retryAtMs)}},
      .absolute = absolute,
      .relative = relative,
      .invalidReset = invalidReset,
    };
    co_return vectors;
  }

  struct Transitions {
    CodexAccountRoutingState first;
    CodexAccountRoutingState later;
    CodexAccountRoutingState recovery;
    CodexAccountRoutingState foreign;
    CodexAccountRoutingState spark;
    CodexAccountRoutingState released;
    CodexAccountRoutingState timeout;
  };

  // `{ ...classification, transitions }`
  struct ConformanceVectors {
    json::object classified;
    json::object retryAfter;
    json::object oversized;
    json::object incomplete;
    Transitions transitions;
  };

  rt::Task<ConformanceVectors> runCodex429ConformanceVectors() {
    const double now = 1'700'000'000'000;
    const double deadline = now + 60'000;
    auto classifying = classifyCodex429Vectors(now);
    ClassificationVectors classification = co_await std::move(classifying);
    const codex429::Codex429Classification absolute = classification.absolute;
    const codex429::Codex429Classification relative = classification.relative;
    const codex429::Codex429Classification invalidReset = classification.invalidReset;
    CodexRoutingAccount account;
    account.slot = 0;
    account.accountIdHash = "opaque-account";
    account.credentialVersion = "opaque-credential";
    account.quotaHeadroom = std::nullopt;
    account.probeRequired = false;
    account.probeGeneration = std::nullopt;
    account.probeToken = std::nullopt;
    account.requestedModel = std::string("gpt-test");
    account.routingGeneration = 7;
    account.activeGeneration = 3;
    account.activePoolVersionstamp = "pool-generation";
    CodexRoutingSlot neutral = capacity::neutralSlot(account.credentialVersion, account.accountIdHash);
    neutral.generation = 7;
    CodexAccountRoutingState state;
    state.v = 2;
    state.updated_at_ms = now - 1;
    state.banked_reset_legacy_identity_unresolved = false;
    state.slots = {neutral};
    const std::string before = rt::jsonStringify(toJson(state));
    const CodexAccountRoutingState first = requireCodex429(codex429::quotaBlockTransition(state, account, absolute, now, false, now + 1), "first block missing");
    proveCodex429(rt::jsonStringify(toJson(state)) == before && first.updated_at_ms == now + 1, "transition mutated input or lost injected clock");
    const CodexQuotaClassBlock* firstStandard = first.slots.at(0).quota_blocks_by_class ? first.slots.at(0).quota_blocks_by_class->get("standard") : nullptr;
    proveCodex429(firstStandard != nullptr && firstStandard->blocked_until_ms == deadline && first.slots.at(0).generation == 8, "standard block changed");
    const CodexAccountRoutingState later = requireCodex429(codex429::quotaBlockTransition(first, account, relative, now + 1, false, now + 2), "later block missing");
    proveCodex429(later.slots.at(0).observed_reset_at_ms == deadline && later.slots.at(0).banked_reset_generation_ambiguous, "relative update minted a new reset identity");
    const CodexAccountRoutingState recovery = requireCodex429(codex429::quotaBlockTransition(state, account, absolute, now, true, now + 1), "recovery block missing");
    proveCodex429(recovery.slots.at(0).quota_blocked_until_ms == now + 30'000 && recovery.slots.at(0).banked_reset_generation_ambiguous, "recovery probe became a long circuit");

    const CodexProbeLease lease{.token = "probe-token", .expires_at_ms = now + 30'000, .generation = 7, .circuit = "quota", .quota_class = CodexQuotaClass("spark")};
    CodexAccountRoutingState leasedState = state;
    leasedState.slots.at(0).probe_lease = lease;
    const CodexAccountRoutingState foreign =
      requireCodex429(codex429::quotaBlockTransition(leasedState, account, absolute, now, false, now + 1), "foreign-class block missing");
    // `foreign.slots[0].probe_lease === lease`: the slot keeps the very lease object, so its value is unchanged.
    proveCodex429(foreign.slots.at(0).probe_lease == lease && foreign.slots.at(0).generation == 7, "ordinary request replaced foreign-class lease");
    CodexRoutingAccount sparkAccount = account;
    sparkAccount.requestedModel = std::string("gpt-5.3-codex-spark");
    proveCodex429(!codex429::quotaBlockTransition(leasedState, sparkAccount, absolute, now, false, now), "ordinary request overwrote same-class probe");
    CodexRoutingAccount probe = sparkAccount;
    probe.probeRequired = true;
    probe.probeGeneration = 7;
    probe.probeToken = lease.token;
    probe.probeCircuit = CodexProbeCircuit("quota");
    const CodexAccountRoutingState spark = requireCodex429(codex429::quotaBlockTransition(leasedState, probe, absolute, now, false, now + 1), "matching probe block missing");
    const CodexQuotaClassBlock* sparkBlock = spark.slots.at(0).quota_blocks_by_class ? spark.slots.at(0).quota_blocks_by_class->get("spark") : nullptr;
    proveCodex429(sparkBlock != nullptr && sparkBlock->blocked_until_ms == deadline && !spark.slots.at(0).probe_lease, "matching Spark probe failed to block");
    CodexRoutingAccount staleToken = probe;
    staleToken.probeToken = "stale-token";
    proveCodex429(!codex429::quotaBlockTransition(leasedState, staleToken, absolute, now, false, now), "stale token wrote a block");
    CodexRoutingAccount staleCredential = account;
    staleCredential.credentialVersion = "stale-credential";
    proveCodex429(!codex429::quotaBlockTransition(state, staleCredential, absolute, now, false, now), "stale credential wrote a block");
    CodexRoutingAccount otherAccount = account;
    otherAccount.accountIdHash = "other-account";
    proveCodex429(!codex429::quotaBlockTransition(state, otherAccount, absolute, now, false, now), "foreign account wrote a block");
    proveCodex429(!codex429::quotaBlockTransition(state, account, invalidReset, now, false, now), "missing reset wrote a block");

    const CodexAccountRoutingState released =
      requireCodex429(codex429::releaseNonBlockingQuotaProbe(leasedState, probe, now + 1), "matching nonblocking probe was not released");
    proveCodex429(!released.slots.at(0).probe_lease && released.updated_at_ms == now + 1, "probe release lost row timestamp or lease");
    proveCodex429(!codex429::releaseNonBlockingQuotaProbe(leasedState, staleToken, now), "stale nonblocking probe cleared a lease");
    CodexRoutingSlot timeoutSlot = first.slots.at(0);
    timeoutSlot.generation = 7;
    timeoutSlot.upstream_timeout_blocked_until_ms = deadline;
    timeoutSlot.banked_reset_recovery_probe_pending = true;
    CodexProbeLease timeoutLease = lease;
    timeoutLease.circuit = "upstream_timeout";
    timeoutLease.quota_class = nullptr;
    timeoutSlot.probe_lease = timeoutLease;
    CodexAccountRoutingState timeoutState = state;
    timeoutState.slots = {timeoutSlot};
    CodexRoutingAccount timeoutProbe = probe;
    timeoutProbe.probeCircuit = CodexProbeCircuit("upstream_timeout");
    const CodexAccountRoutingState timeout = requireCodex429(codex429::releaseNonBlockingQuotaProbe(timeoutState, timeoutProbe, now + 1), "timeout release missing");
    proveCodex429(timeout.slots.at(0).upstream_timeout_blocked_until_ms == nullptr && timeout.slots.at(0).banked_reset_recovery_probe_pending,
                  "timeout release changed quota recovery flags");
    ConformanceVectors vectors{
      .classified = std::move(classification.classified),
      .retryAfter = std::move(classification.retryAfter),
      .oversized = std::move(classification.oversized),
      .incomplete = std::move(classification.incomplete),
      .transitions = Transitions{first, later, recovery, foreign, spark, released, timeout},
    };
    co_return vectors;
  }
  // END SHARED_CODEX429_VECTORS

  // JSON.stringify(vectors)
  json::value vectorsJson(const ConformanceVectors& vectors) {
    json::object transitions;
    transitions["first"] = toJson(vectors.transitions.first);
    transitions["later"] = toJson(vectors.transitions.later);
    transitions["recovery"] = toJson(vectors.transitions.recovery);
    transitions["foreign"] = toJson(vectors.transitions.foreign);
    transitions["spark"] = toJson(vectors.transitions.spark);
    transitions["released"] = toJson(vectors.transitions.released);
    transitions["timeout"] = toJson(vectors.transitions.timeout);
    json::object object;
    object["classified"] = vectors.classified;
    object["retryAfter"] = vectors.retryAfter;
    object["oversized"] = vectors.oversized;
    object["incomplete"] = vectors.incomplete;
    object["transitions"] = std::move(transitions);
    return object;
  }
}

ASYNC_TEST("native Codex 429 policy preserves bounded response bytes, reset identity, class leases and injected clocks") {
  const BunTestTimeout bunTimeout;
  auto running = runCodex429ConformanceVectors();
  const ConformanceVectors vectors = co_await std::move(running);
  EXPECT_EQ(vectors.classified.size(), static_cast<std::size_t>(13));
  const std::optional<CodexProbeLease>& foreignLease = vectors.transitions.foreign.slots.at(0).probe_lease;
  EXPECT_EQ(foreignLease ? std::optional<std::string>(foreignLease->token) : std::nullopt, std::optional<std::string>("probe-token"));
  EXPECT_EQ(vectors.transitions.later.slots.at(0).banked_reset_generation_ambiguous, true);
}

ASYNC_TEST("differential: the shared vectors equal what Bun recorded for them") {
  const BunTestTimeout bunTimeout;
  auto running = runCodex429ConformanceVectors();
  const ConformanceVectors vectors = co_await std::move(running);
  const json::value actual = vectorsJson(vectors);
  const json::value recorded = rt::jsonParse(kCodex429ConformanceVectorsJson);
  for (const std::string key : {"classified", "retryAfter", "oversized", "incomplete"}) {
    if (!rt::deepEqual(*rt::get(actual, key), *rt::get(recorded, key))) EXPECT_EQ(rt::jsonStringify(*rt::get(actual, key)), rt::jsonStringify(*rt::get(recorded, key)));
  }
  for (const std::string name : {"first", "later", "recovery", "foreign", "spark", "released", "timeout"}) {
    const json::value& mine = *rt::get(rt::get(actual, "transitions"), name);
    const json::value& theirs = *rt::get(rt::get(recorded, "transitions"), name);
    if (!rt::deepEqual(mine, theirs)) EXPECT_EQ(name + " " + rt::jsonStringify(mine), name + " " + rt::jsonStringify(theirs));
  }
  // Key order too, except where the TS test writes its lease literal as { token, generation, expires_at_ms }
  // and the CodexProbeLease struct writes { token, expires_at_ms, generation }.
  for (const std::string name : {"first", "later", "recovery", "spark", "released", "timeout"}) {
    EXPECT_EQ(rt::jsonStringify(*rt::get(rt::get(actual, "transitions"), name)), rt::jsonStringify(*rt::get(rt::get(recorded, "transitions"), name)));
  }
  EXPECT_EQ(rt::jsonStringify(*rt::get(actual, "classified")), rt::jsonStringify(*rt::get(recorded, "classified")));
  EXPECT_EQ(rt::jsonStringify(*rt::get(actual, "retryAfter")), rt::jsonStringify(*rt::get(recorded, "retryAfter")));
}
