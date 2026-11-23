// Differential cases for src/authority/native-api-key.ts beyond tests/native_api_key_conformance_test.cpp:
// every exported function over the inputs in tests/native_api_key_vectors.inc, compared by the SHA-256
// of the JSON text Bun 1.4.0 printed for each result. The stored rows in the inputs carry exactly the
// declared members in declaration order (see the header comment of uos/authority/native_api_key.hpp).
#include "uos/authority/native_api_key.hpp"
#include "uos/authority/native_contracts.hpp"

#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"
#include "uos/types.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "native_api_key_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::authority::native_api_key;
using uos::authority::native_contracts::ApiKeyPolicy;
using uos::authority::native_contracts::ApiKeyUsageDispatchPlan;
using uos::authority::native_contracts::ApiKeyUsageReleasePlan;
using uos::authority::native_contracts::ApiKeyUsageReleasePlanMissingInvalidSettled;
using uos::authority::native_contracts::ApiKeyUsageReservationPlan;
using uos::authority::native_contracts::ApiKeyUsageReservationPlanFull;
using uos::authority::native_contracts::ApiKeyUsageReservationPlanUnavailable;
using uos::authority::native_contracts::ApiKeyUsageSnapshot;
using uos::authority::native_contracts::ApiKeyUsageWritePlan;
using uos::types::ApiKeyUsageRequestV3;
using uos::types::ApiKeyUsageWindowV3;

namespace {
  json::object kindOnly(std::string_view kind) {
    json::object object;
    rt::objectDefine(object, "kind", json::value(json::string(kind)));
    return object;
  }
  json::value planJson(const ApiKeyUsageWritePlan& plan) {
    json::array checks;
    for (const std::string& check : plan.checks) checks.push_back(json::value(json::string(check)));
    json::object object = kindOnly(ApiKeyUsageWritePlan::kind);
    rt::objectDefine(object, "checks", std::move(checks));
    rt::objectDefine(object, "request", json::value_from(plan.request));
    rt::objectDefine(object, "window", json::value_from(plan.window));
    rt::objectDefine(object, "expireIn", rt::makeNumber(plan.expireIn));
    return object;
  }
  json::value planJson(const ApiKeyUsageReservationPlan& plan) {
    return std::visit(
      [](const auto& alternative) -> json::value {
        using T = std::decay_t<decltype(alternative)>;
        if constexpr (std::is_same_v<T, ApiKeyUsageWritePlan>) {
          return planJson(alternative);
        } else if constexpr (std::is_same_v<T, ApiKeyUsageReservationPlanFull>) {
          json::object object = kindOnly(T::kind);
          rt::objectDefine(object, "window", json::value_from(alternative.window));
          return object;
        } else if constexpr (std::is_same_v<T, ApiKeyUsageReservationPlanUnavailable>) {
          json::object object = kindOnly(T::kind);
          rt::objectDefine(object, "reason", json::value(json::string(alternative.reason)));
          return object;
        } else {
          return kindOnly(T::kind);
        }
      },
      plan);
  }
  json::value planJson(const ApiKeyUsageDispatchPlan& plan) {
    return std::visit(
      [](const auto& alternative) -> json::value {
        using T = std::decay_t<decltype(alternative)>;
        if constexpr (std::is_same_v<T, ApiKeyUsageWritePlan>) return planJson(alternative);
        else return kindOnly(T::kind);
      },
      plan);
  }
  json::value planJson(const ApiKeyUsageReleasePlan& plan) {
    if (const auto* write = std::get_if<ApiKeyUsageWritePlan>(&plan)) return planJson(*write);
    return kindOnly(std::get<ApiKeyUsageReleasePlanMissingInvalidSettled>(plan).kind);
  }
  json::value windowOrNull(const std::optional<ApiKeyUsageWindowV3>& window) { return window ? json::value_from(*window) : json::value(nullptr); }
  json::value requestOrNull(const std::optional<ApiKeyUsageRequestV3>& request) { return request ? json::value_from(*request) : json::value(nullptr); }
  json::value boundsJson(const ApiKeyUsageWindowBounds& bounds) {
    json::object object;
    rt::objectDefine(object, "start", rt::makeNumber(bounds.start));
    rt::objectDefine(object, "reset", rt::makeNumber(bounds.reset));
    return object;
  }

  struct Replay {
    std::size_t next = 0;
    int mismatches = 0;
    void push(const std::string& label, const json::value& value) {
      const std::string text = rt::jsonStringify(value);
      const std::string digest = rt::crypto::createHash("sha256").update(text).digest("hex").substr(0, 16);
      constexpr std::size_t total = sizeof(kApiKeyResults) / sizeof(kApiKeyResults[0]);
      if (next >= total) {
        EXPECT_EQ(label, "<no more recorded cases>");
        ++mismatches;
        return;
      }
      const std::string expectedLabel = kApiKeyResults[next][0];
      const std::string expectedDigest = kApiKeyResults[next][1];
      ++next;
      if (label != expectedLabel || digest != expectedDigest) {
        ++mismatches;
        if (mismatches <= 25) {
          EXPECT_EQ(label, expectedLabel);
          EXPECT_EQ(digest + " " + text, expectedDigest + " <recorded>");
        }
      }
    }
  };
}

TEST("differential: native API key usage plans over recorded inputs") {
  const json::value inputs = rt::jsonParse(kApiKeyInputs);
  const double now = 10'000'000;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  std::vector<ApiKeyPolicy> policies;
  for (const json::value& policy : *rt::getArray(inputs, "policies")) policies.push_back(json::value_to<ApiKeyPolicy>(policy));
  const json::array& rawWindows = *rt::getArray(inputs, "rawWindows");
  const json::array& rawRequests = *rt::getArray(inputs, "rawRequests");
  const json::array& scalars = *rt::getArray(inputs, "scalars");
  const json::array& snapshotRequests = *rt::getArray(inputs, "snapshotRequests");
  const json::array& snapshotWindows = *rt::getArray(inputs, "snapshotWindows");
  const std::vector<double> policyVersionNumbers = {60'000, 1.5, -0.0, nan, infinity, -infinity, 1e21, 0.1, 3'600'000, 123456789.125};
  const std::vector<std::array<double, 3>> windowArgs = {
    {now + 60'000, 60'000, now},
    {now + 60'000, 60'000, now + 60'000},
    {now + 60'000, 60'000, now + 59'999},
    {now + 60'000, 60'000, now + 60'001},
    {now + 60'000, 60'000, now + 1'000'000},
    {now + 60'000, 60'000, now - 1'000'000},
    {now + 60'000, 7'000, now + 1'000'000},
    {now + 60'000, 0, now + 60'000},
    {now + 60'000, -60'000, now + 120'000},
    {now + 60'000, 1.5, now + 60'000.75},
    {nan, 60'000, now},
    {now + 60'000, nan, now + 70'000},
    {now + 60'000, 60'000, nan},
    {now + 60'000, infinity, now + 70'000},
    {0, 60'000, 0},
    {-60'000, 60'000, 5},
  };
  const std::vector<std::pair<double, double>> retentionArgs = {
    {now + 60'000, now}, {now + 60'000, now + 1}, {now + 60'000, now + 700'000'000}, {now + 60'000, now + 604'860'000},
    {now + 60'000, now + 604'859'999}, {nan, now}, {now, infinity}, {0.5, 0},
  };
  const std::vector<std::string> requestIds = {"request", "other"};
  const std::vector<std::string> routes = {"responses", "chat.completions"};
  const std::vector<std::string> providers = {"claude_code", "surplus"};
  const std::vector<std::string> reasons = {"done", "lease_expired"};
  const std::vector<double> nows = {now, now + 300'000};
  const std::vector<std::optional<std::string>> expectedRoutes = {std::nullopt, "responses", "chat.completions"};
  const json::value* undefined = nullptr;

  Replay replay;
  json::array providerList;
  for (const std::string_view provider : API_KEY_USAGE_PROVIDERS) providerList.push_back(json::value(json::string(provider)));
  replay.push("constants", json::array{rt::makeNumber(API_KEY_USAGE_V3_RESERVATION_LEASE_MS), rt::makeNumber(API_KEY_USAGE_V3_RETENTION_MS), providerList});
  for (std::size_t i = 0; i < windowArgs.size(); ++i) {
    replay.push("currentWindow" + std::to_string(i), boundsJson(currentApiKeyUsageWindow(windowArgs[i][0], windowArgs[i][1], windowArgs[i][2])));
  }
  for (std::size_t i = 0; i < policies.size(); ++i) {
    replay.push("makeWindow" + std::to_string(i),
                json::array{json::value_from(makeApiKeyUsageWindowV3(policies[i], now)), json::value_from(makeApiKeyUsageWindowV3(policies[i], now + 7))});
  }
  for (std::size_t i = 0; i < scalars.size(); ++i) replay.push("safe" + std::to_string(i), isSafeNonNegativeInteger(scalars[i]));
  replay.push("safe.special", json::array{isSafeNonNegativeInteger(nan), isSafeNonNegativeInteger(infinity), isSafeNonNegativeInteger(-0.0),
                                          isSafeNonNegativeInteger(undefined)});
  for (std::size_t i = 0; i < rawWindows.size(); ++i) {
    replay.push("window" + std::to_string(i) + ".normalize", windowOrNull(normalizeApiKeyUsageWindowV3(rawWindows[i])));
    json::array matching;
    for (const ApiKeyPolicy& policy : policies) matching.push_back(windowOrNull(matchingApiKeyUsageWindowV3(rawWindows[i], policy)));
    replay.push("window" + std::to_string(i) + ".matching", matching);
  }
  replay.push("window.undefined", json::array{windowOrNull(normalizeApiKeyUsageWindowV3(undefined)), windowOrNull(matchingApiKeyUsageWindowV3(undefined, policies[0]))});
  for (std::size_t i = 0; i < rawRequests.size(); ++i) {
    const std::optional<ApiKeyUsageRequestV3> normalized = normalizeApiKeyUsageRequestV3(rawRequests[i]);
    replay.push("request" + std::to_string(i) + ".normalize", requestOrNull(normalized));
    if (!normalized) continue;
    json::array expired;
    for (const double nowMs : {now - 1, now + 299'989, now + 299'990, now + 299'991}) expired.push_back(isApiKeyUsageReservationExpired(*normalized, nowMs));
    replay.push("request" + std::to_string(i) + ".expired", expired);
  }
  replay.push("request.undefined", requestOrNull(normalizeApiKeyUsageRequestV3(undefined)));
  for (std::size_t i = 0; i < policyVersionNumbers.size(); ++i) replay.push("policyVersion" + std::to_string(i), json::value(apiKeyUsageV3PolicyVersion(policyVersionNumbers[i])));
  for (std::size_t i = 0; i < retentionArgs.size(); ++i) {
    replay.push("retention" + std::to_string(i), rt::makeNumber(apiKeyUsageV3RetentionMs(retentionArgs[i].first, retentionArgs[i].second)));
  }
  for (std::size_t pi = 0; pi < policies.size(); ++pi) {
    const ApiKeyPolicy& policy = policies[pi];
    for (std::size_t ri = 0; ri < snapshotRequests.size(); ++ri) {
      for (std::size_t wi = 0; wi < snapshotWindows.size(); ++wi) {
        const ApiKeyUsageSnapshot snapshot{snapshotRequests[ri], snapshotWindows[wi]};
        const std::string label = "policy" + std::to_string(pi) + ".request" + std::to_string(ri) + ".window" + std::to_string(wi);
        json::array reservations;
        for (const std::string& requestId : requestIds) {
          for (const std::string& route : routes) {
            for (const double nowMs : nows) reservations.push_back(planJson(planApiKeyUsageReservation(policy, requestId, route, snapshot, nowMs)));
          }
        }
        replay.push(label + ".reservation", reservations);
        json::array dispatches;
        for (const std::string& requestId : requestIds) {
          for (const std::string& route : routes) {
            for (const std::string& provider : providers) {
              for (const double nowMs : nows) dispatches.push_back(planJson(planApiKeyUsageDispatch(policy, requestId, route, provider, snapshot, nowMs)));
            }
          }
        }
        replay.push(label + ".dispatch", dispatches);
        json::array releases;
        for (const std::string& requestId : requestIds) {
          for (const std::string& reason : reasons) {
            for (const std::optional<std::string>& expectedRoute : expectedRoutes) {
              releases.push_back(planJson(expectedRoute ? planApiKeyUsageReservedRelease(policy, requestId, reason, snapshot, now + 3, expectedRoute)
                                                        : planApiKeyUsageReservedRelease(policy, requestId, reason, snapshot, now + 3)));
            }
          }
        }
        replay.push(label + ".release", releases);
        json::array cancellations;
        for (const std::string& requestId : requestIds) {
          for (const std::string& route : routes) {
            for (const std::string& provider : providers) cancellations.push_back(planJson(planApiKeyUsageDispatchCancellation(policy, requestId, route, provider, snapshot, now + 4)));
          }
        }
        replay.push(label + ".cancellation", cancellations);
      }
    }
  }

  EXPECT_EQ(replay.next, sizeof(kApiKeyResults) / sizeof(kApiKeyResults[0]));
  EXPECT_EQ(replay.mismatches, 0);
}
