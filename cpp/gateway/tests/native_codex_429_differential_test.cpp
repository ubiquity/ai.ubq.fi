// Differential cases for src/authority/native-codex-429.ts beyond tests/native-codex429-conformance.test.ts:
// Retry-After values and clocks, buffered 429 bodies (duplicate and escaped keys, surrogates, BOM, invalid
// UTF-8, number grammar, nesting, the 64 KiB bound) under several headers, and every exported slot
// transition over parsed routing states, accounts and classifications. Each result is compared by the
// SHA-256 of the JSON text Bun 1.4.0 printed for it (tests/native_codex_429_differential_vectors.inc).
#include "uos/authority/native_codex_429.hpp"
#include "uos/authority/native_codex_contracts.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "native_codex_429_differential_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
namespace codex429 = uos::authority::native_codex_429;
using namespace uos::authority::native_codex_contracts;

namespace {
  struct Replay {
    std::size_t next = 0;
    int mismatches = 0;
    void push(const std::string& label, const json::value& value) {
      const std::string text = rt::jsonStringify(value);
      const std::string digest = rt::crypto::createHash("sha256").update(text).digest("hex").substr(0, 16);
      constexpr std::size_t total = sizeof(kCodex429Results) / sizeof(kCodex429Results[0]);
      if (next >= total) {
        EXPECT_EQ(label, "<no more recorded cases>");
        ++mismatches;
        return;
      }
      const std::string expectedLabel = kCodex429Results[next][0];
      const std::string expectedDigest = kCodex429Results[next][1];
      ++next;
      if (label != expectedLabel || digest != expectedDigest) {
        ++mismatches;
        if (mismatches <= 25) {
          EXPECT_EQ(label, expectedLabel);
          EXPECT_EQ(digest + " " + text, expectedDigest + " <recorded>");
        }
      }
    }
    // Replays start at their own section of the recorded list.
    void seek(const std::string& prefix) {
      constexpr std::size_t total = sizeof(kCodex429Results) / sizeof(kCodex429Results[0]);
      next = 0;
      while (next < total && !rt::startsWith(kCodex429Results[next][0], prefix)) ++next;
    }
  };

  json::value orNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
  json::value orNull(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }

  std::vector<rt::Headers::Entry> entriesOf(const json::value& list) {
    std::vector<rt::Headers::Entry> entries;
    for (const json::value& pair : list.get_array())
      entries.emplace_back(std::string(pair.get_array().at(0).get_string()), std::string(pair.get_array().at(1).get_string()));
    return entries;
  }

  json::value retryAfterJson(const std::optional<codex429::RetryAfterDeadline>& deadline) {
    if (!deadline) return nullptr;
    json::object object;
    object["deadlineMs"] = rt::makeNumber(deadline->deadlineMs);
    object["isStable"] = deadline->isStable;
    return object;
  }

  // The scratch script's `generate(spec)`.
  rt::Bytes generate(const json::value& spec) {
    if (const std::optional<std::string_view> text = rt::getString(spec, "text")) return rt::encodeUtf8(*text);
    if (const std::optional<std::string_view> hex = rt::getString(spec, "hex")) return rt::fromHex(*hex);
    const std::string generator(*rt::getString(spec, "generator"));
    const double size = *rt::getNumber(spec, "size");
    const std::string prefix = R"({"error":{"type":"usage_limit_reached","resets_at":1700000060},"x":)";
    if (generator == "nestedArrays") return rt::encodeUtf8(prefix + rt::repeat("[", size) + rt::repeat("]", size) + "}");
    if (generator == "nestedObjects") return rt::encodeUtf8(prefix + rt::repeat(R"({"a":)", size) + "1" + rt::repeat("}", size) + "}");
    const std::string base = R"({"error":{"type":"usage_limit_reached","resets_at":1700000060}})";
    return rt::encodeUtf8(rt::slice(base, 0, -1) + rt::repeat(" ", size - static_cast<double>(rt::length(base))) + "}");
  }

  json::value bytesDigest(const rt::Bytes& bytes) { return json::value(rt::crypto::createHash("sha256").update(bytes).digest("hex").substr(0, 16)); }

  std::optional<std::string> optionalString(const json::value& object, std::string_view key) {
    const json::value* value = rt::get(object, key);
    if (value == nullptr || value->is_null()) return std::nullopt;
    return std::string(value->get_string());
  }

  rt::Nullable<std::string> nullableString(const json::value& object, std::string_view key) {
    const json::value* value = rt::get(object, key);
    if (value == nullptr) return rt::undefined;
    if (value->is_null()) return nullptr;
    return std::string(value->get_string());
  }

  CodexRoutingAccount accountFrom(const json::value& value) {
    CodexRoutingAccount account;
    account.slot = *rt::getNumber(value, "slot");
    account.accountIdHash = *optionalString(value, "accountIdHash");
    account.credentialVersion = *optionalString(value, "credentialVersion");
    account.quotaHeadroom = rt::getNumber(rt::get(value, "quotaHeadroom"));
    account.probeRequired = *rt::getBool(value, "probeRequired");
    account.probeGeneration = rt::getNumber(rt::get(value, "probeGeneration"));
    account.probeToken = optionalString(value, "probeToken");
    account.probeCircuit = nullableString(value, "probeCircuit");
    account.routingGeneration = rt::getNumber(rt::get(value, "routingGeneration"));
    account.activeGeneration = rt::getNumber(rt::get(value, "activeGeneration"));
    account.activePoolVersionstamp = optionalString(value, "activePoolVersionstamp");
    account.requestedModel = nullableString(value, "requestedModel");
    return account;
  }

  // `{ response: new Response(null, { headers }), usageLimitReached, retryAtMs, quotaBlockSource, ... }`
  codex429::Codex429Classification classificationOf(const json::value& spec) {
    rt::ResponseInit init;
    init.headers = rt::Headers(entriesOf(*rt::get(spec, "headers")));
    return codex429::Codex429Classification{
      .response = rt::Response(nullptr, std::move(init)),
      .usageLimitReached = *rt::getBool(spec, "usageLimitReached"),
      .retryAtMs = rt::getNumber(rt::get(spec, "retryAtMs")),
      .quotaBlockSource = optionalString(spec, "quotaBlockSource"),
      .resetDeadlineIsStable = *rt::getBool(spec, "resetDeadlineIsStable"),
      .resetDeadlineConflict = *rt::getBool(spec, "resetDeadlineConflict"),
    };
  }

  json::value slotJson(const std::optional<CodexRoutingSlot>& slot) { return slot ? json::value(toJson(*slot)) : json::value(nullptr); }
  json::value stateJson(const std::optional<CodexAccountRoutingState>& state) { return state ? json::value(toJson(*state)) : json::value(nullptr); }
}

TEST("differential: futureRetryAfterDeadline over Retry-After values and clocks") {
  const json::value inputs = rt::jsonParse(kCodex429Inputs);
  Replay replay;
  replay.seek("ra");
  const json::array& lists = *rt::getArray(inputs, "retryAfterHeaders");
  const json::array& nows = *rt::getArray(inputs, "retryAfterNows");
  for (std::size_t h = 0; h < lists.size(); ++h) {
    rt::Headers headers;
    for (const auto& [name, value] : entriesOf(lists[h])) headers.append(name, value);
    for (std::size_t n = 0; n < nows.size(); ++n)
      replay.push("ra" + std::to_string(h) + ".n" + std::to_string(n), retryAfterJson(codex429::futureRetryAfterDeadline(headers, *rt::getNumber(nows[n]))));
  }
  EXPECT_EQ(replay.mismatches, 0);
  EXPECT_EQ(replay.next, lists.size() * nows.size());
}

ASYNC_TEST("differential: classifyCodex429 over buffered bodies and Retry-After headers") {
  const json::value inputs = rt::jsonParse(kCodex429Inputs);
  const double now = *rt::getNumber(inputs, "now");
  Replay replay;
  replay.seek("b0.");
  const std::size_t start = replay.next;
  const json::array& bodies = *rt::getArray(inputs, "bodySpecs");
  const json::array& headerSets = *rt::getArray(inputs, "classifyHeaderSets");
  for (std::size_t b = 0; b < bodies.size(); ++b) {
    const rt::Bytes bytes = generate(bodies[b]);
    for (std::size_t h = 0; h < headerSets.size(); ++h) {
      const std::vector<rt::Headers::Entry> entries = entriesOf(headerSets[h]);
      rt::ResponseInit init;
      init.status = 429;
      init.statusText = h % 2 == 0 ? "Too Many Requests" : "";
      init.headers = rt::Headers(entries);
      const rt::Response upstream(rt::BodyInit(bytes), std::move(init));
      auto classifying = codex429::classifyCodex429(upstream, now);
      const codex429::Codex429Classification parsed = co_await std::move(classifying);
      auto reading = parsed.response.arrayBuffer();
      const rt::Bytes responseBytes = co_await std::move(reading);
      json::array headers;
      for (const rt::Headers::Entry& entry : parsed.response.headers().entries()) headers.push_back(json::array{json::value(entry.first), json::value(entry.second)});
      json::object result;
      result["usageLimitReached"] = parsed.usageLimitReached;
      result["retryAtMs"] = orNull(parsed.retryAtMs);
      result["quotaBlockSource"] = orNull(parsed.quotaBlockSource);
      result["resetDeadlineIsStable"] = parsed.resetDeadlineIsStable;
      result["resetDeadlineConflict"] = parsed.resetDeadlineConflict;
      result["status"] = rt::makeNumber(static_cast<double>(parsed.response.status()));
      result["statusText"] = parsed.response.statusText();
      result["headers"] = std::move(headers);
      result["bytes"] = bytesDigest(responseBytes);
      result["length"] = rt::makeNumber(static_cast<double>(responseBytes.size()));
      replay.push("b" + std::to_string(b) + ".h" + std::to_string(h), result);
    }
  }
  EXPECT_EQ(replay.mismatches, 0);
  EXPECT_EQ(replay.next - start, bodies.size() * headerSets.size());
}

ASYNC_TEST("Codex 429 requires a body within the JSON depth limit for quota proof") {
  constexpr double now = 1700000000000;
  for (const std::size_t depth : {rt::limits::kJsonDepth, rt::limits::kJsonDepth + 1}) {
    // The response root contributes one level to the generated array nesting.
    const auto bytes = generate(json::object{{"generator", "nestedArrays"}, {"size", rt::makeNumber(static_cast<double>(depth - 1))}});
    rt::ResponseInit init;
    init.status = 429;
    init.headers = rt::Headers{{"Retry-After", "15"}};
    const rt::Response upstream(rt::BodyInit(bytes), std::move(init));
    auto classifying = codex429::classifyCodex429(upstream, now);
    const auto parsed = co_await std::move(classifying);
    const bool withinLimit = depth == rt::limits::kJsonDepth;
    EXPECT_EQ(parsed.usageLimitReached, withinLimit);
    EXPECT_EQ(parsed.retryAtMs, std::optional<double>(now + (withinLimit ? 60000 : 15000)));
    EXPECT_EQ(parsed.quotaBlockSource.value_or(""), withinLimit ? "body_resets_at" : "header_retry_after");
    EXPECT_EQ(parsed.resetDeadlineIsStable, withinLimit);
    EXPECT(!parsed.resetDeadlineConflict);
    EXPECT_EQ(parsed.response.status(), 429);
    auto reading = parsed.response.arrayBuffer();
    const auto responseBytes = co_await std::move(reading);
    EXPECT_EQ(responseBytes, bytes);
  }
}

TEST("differential: slot transitions over parsed states, accounts and classifications") {
  const json::value inputs = rt::jsonParse(kCodex429Inputs);
  Replay replay;
  replay.seek("s0.");
  const std::size_t start = replay.next;
  std::vector<CodexAccountRoutingState> states;
  for (const json::value& raw : *rt::getArray(inputs, "rawStates")) {
    std::optional<CodexAccountRoutingState> parsed = parseCodexAccountRoutingState(raw);
    ASSERT(parsed.has_value());
    states.push_back(std::move(*parsed));
  }
  std::vector<CodexRoutingAccount> accounts;
  for (const json::value& account : *rt::getArray(inputs, "accounts")) accounts.push_back(accountFrom(account));
  const json::array& classifications = *rt::getArray(inputs, "classifications");
  std::vector<double> nows;
  for (const json::value& now : *rt::getArray(inputs, "nows")) nows.push_back(*rt::getNumber(now));
  const double base = *rt::getNumber(inputs, "now");
  std::size_t cases = 0;
  for (std::size_t s = 0; s < states.size(); ++s) {
    for (std::size_t a = 0; a < accounts.size(); ++a) {
      const std::string prefix = "s" + std::to_string(s) + ".a" + std::to_string(a);
      replay.push(prefix + ".nb", slotJson(codex429::nonBlockingQuotaProbeSlot(states[s], accounts[a])));
      replay.push(prefix + ".rel", stateJson(codex429::releaseNonBlockingQuotaProbe(states[s], accounts[a], base + 3)));
      cases += 2;
      for (std::size_t c = 0; c < classifications.size(); ++c) {
        for (std::size_t n = 0; n < nows.size(); ++n) {
          for (const bool recoveryProbe : {false, true}) {
            const codex429::Codex429Classification parsed = classificationOf(classifications[c]);
            const std::optional<CodexRoutingSlot> slot = codex429::quotaBlockedSlot(states[s], accounts[a], parsed, nows[n], recoveryProbe);
            const std::optional<CodexAccountRoutingState> transition = codex429::quotaBlockTransition(states[s], accounts[a], parsed, nows[n], recoveryProbe, nows[n] + 5);
            // quotaBlockTransition is withSlot over quotaBlockedSlot, so the recorded transition pins both
            // (withSlot places the slot only at an array index).
            EXPECT_EQ(slot.has_value(), transition.has_value());
            const double index = accounts[a].slot;
            if (slot && transition && index >= 0 && std::trunc(index) == index)
              EXPECT(toJson(*slot) == toJson(transition->slots.at(static_cast<std::size_t>(index))));
            replay.push(prefix + ".c" + std::to_string(c) + ".n" + std::to_string(n) + (recoveryProbe ? ".r" : ".o"), stateJson(transition));
            cases += 1;
          }
        }
      }
    }
  }
  EXPECT_EQ(replay.mismatches, 0);
  EXPECT_EQ(replay.next - start, cases);
}
