#include "uos/provider/health.hpp"
#include "uos/rt/memory.hpp"

#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uos::provider::health {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    using claude_code_accounts::ClaudeCodeAccountBinding;

    // `(typeof PROVIDER_RECORDS)[number]`
    constexpr std::array<std::string_view, 7> PROVIDER_RECORDS = {"current", "success", "reachable", "auth_invalid", "quota_exhausted", "upstream_error", "refresh"};

    struct ProviderTransition {
      ProviderHealthEvent event;
      std::optional<double> status;
      std::optional<std::string> providerRequestId;
      rt::Promise<void> tail;
    };

    std::unordered_map<std::string, double>& lastHeartbeatWriteAtMs() {
      static std::unordered_map<std::string, double> map;
      return map;
    }
    std::unordered_map<std::string, ProviderHealthObservation>& lastObservation() {
      static std::unordered_map<std::string, ProviderHealthObservation> map;
      return map;
    }
    std::unordered_map<std::string, ProviderTransition>& providerTransitionQueues() {
      static std::unordered_map<std::string, ProviderTransition> map;
      return map;
    }

    std::size_t observationCacheBytes = 0;

    std::size_t observationBytes(const std::string& key, const ProviderHealthObservation& observation) {
      return sizeof(ProviderHealthObservation) + sizeof(double) + 2 * (sizeof(key) + rt::memory::bytes(key)) +
             rt::memory::bytes(observation.event) +
             (observation.provider_request_id.hasValue() ? rt::memory::bytes(*observation.provider_request_id) : 0);
    }

    constexpr std::int64_t MAX_PROVIDER_REQUEST_ID_CHARS = 256;

    // `(value: unknown): string | null` over a string value.
    std::optional<std::string> normalizedProviderRequestId(std::string_view value) {
      std::string requestId = rt::trim(value);
      if (requestId.empty() || rt::length(requestId) > MAX_PROVIDER_REQUEST_ID_CHARS) return std::nullopt;
      for (const std::uint32_t code : rt::codePoints(requestId)) {
        if (code < 0x20 || code == 0x7f) return std::nullopt;
      }
      return requestId;
    }
    std::optional<std::string> normalizedProviderRequestId(const std::optional<std::string>& value) {
      if (!value) return std::nullopt;
      return normalizedProviderRequestId(std::string_view(*value));
    }
    std::optional<std::string> normalizedProviderRequestId(const rt::Nullable<std::string>& value) {
      if (!value.hasValue()) return std::nullopt;
      return normalizedProviderRequestId(std::string_view(*value));
    }
    std::optional<std::string> normalizedProviderRequestId(const rt::KvValue& value) {
      if (!value.isString()) return std::nullopt;
      return normalizedProviderRequestId(std::string_view(value.asString()));
    }

    rt::KvKey providerHealthKey(const RecordProvider& provider, const std::string& identity, std::string_view record) {
      return rt::KvKey{PROVIDER_HEALTH_KEY_PREFIX, provider, identity, std::string(record)};
    }

    bool isProviderEvent(const rt::KvValue& value) {
      if (!value.isString()) return false;
      const std::string& event = value.asString();
      return event == "success" || event == "reachable" || event == "auth_invalid" || event == "quota_exhausted" || event == "upstream_error" ||
             event == "refresh_success" || event == "refresh_failed";
    }

    // `Exclude<ProviderHealthState, "unknown">`
    ProviderHealthState stateForObservation(const ProviderHealthObservation& observation) {
      const ProviderHealthEvent& event = observation.event;
      if (event == "success") return "healthy";
      if (event == "auth_invalid") return "invalid";
      if (event == "refresh_failed") {
        return observation.status == 400.0 || observation.status == 401.0 || observation.status == 403.0 ? "invalid" : "degraded";
      }
      if (event == "quota_exhausted") return "exhausted";
      return "degraded";
    }

    bool shouldThrottleObservation(const RecordProvider& provider, const std::string& identity, const ProviderHealthEvent& event,
                                   const std::optional<double>& status, const std::optional<std::string>& providerRequestId, double nowMs) {
      const std::string throttleKey = provider + ":" + identity;
      const auto found = lastObservation().find(throttleKey);
      const ProviderHealthObservation* previousObservation = found == lastObservation().end() ? nullptr : &found->second;
      // A recovery is a transition, never a heartbeat. Persist it immediately
      // even when the previous healthy heartbeat was recent. The first available
      // correlation ID is also new evidence. Later IDs are sampled at the normal
      // heartbeat interval so per-request IDs do not turn this into a write path.
      const std::optional<std::string> previousProviderRequestId =
        previousObservation ? normalizedProviderRequestId(previousObservation->provider_request_id) : std::nullopt;
      if (!previousObservation) return false;
      if (previousObservation->event != event || previousObservation->status != status || (!previousProviderRequestId && providerRequestId)) {
        return false;
      }
      const auto previous = lastHeartbeatWriteAtMs().find(throttleKey);
      return previous != lastHeartbeatWriteAtMs().end() && nowMs - previous->second < PROVIDER_HEALTH_HEARTBEAT_WRITE_INTERVAL_MS;
    }

    // `(typeof PROVIDER_RECORDS)[number]`
    std::string_view historyRecordFor(const ProviderHealthEvent& event) {
      if (event == "success") return "success";
      if (event == "reachable") return "reachable";
      if (event == "auth_invalid") return "auth_invalid";
      if (event == "quota_exhausted") return "quota_exhausted";
      if (event == "upstream_error") return "upstream_error";
      return "refresh";
    }

    rt::Task<void> persistProviderHealth(RecordProvider provider, std::string identity, ProviderHealthEvent event, std::optional<double> status,
                                         double observedAtMs, std::optional<std::string> providerRequestId, std::optional<ClaudeCodeAccountBinding> binding) {
      try {
        if (shouldThrottleObservation(provider, identity, event, status, providerRequestId, observedAtMs)) co_return;
        auto kvPromise = kv::getKv();
        std::shared_ptr<rt::Kv> kv = co_await kvPromise.wait();
        if (!kv) co_return;
        std::optional<rt::KvEntryMaybe<>> accountEntry;
        if (binding) {
          rt::Promise<std::optional<rt::KvEntryMaybe<>>> current(claude_code_accounts::currentClaudeCodeAccountEntry(kv, *binding));
          accountEntry = co_await current.wait();
        }
        if (binding && !accountEntry) co_return;
        ProviderHealthObservation observation{
          .event = event,
          .status = status,
          .observed_at_ms = observedAtMs,
          .provider_request_id = rt::orNull(providerRequestId),
        };
        const rt::KvValue stored(json::value_from(observation));
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        if (accountEntry) operation->check(*accountEntry);
        operation->set(providerHealthKey(provider, identity, "current"), stored).set(providerHealthKey(provider, identity, historyRecordFor(event)), stored);
        auto committing = operation->commit();
        const rt::KvCommitResult commit = co_await std::move(committing);
        if (!commit.ok) co_return;
        const std::string throttleKey = provider + ":" + identity;
        auto& observations = lastObservation();
        if (auto previous = observations.find(throttleKey); previous != observations.end())
          observationCacheBytes -= observationBytes(previous->first, previous->second);
        auto entry = observations.insert_or_assign(throttleKey, std::move(observation)).first;
        lastHeartbeatWriteAtMs().insert_or_assign(throttleKey, observedAtMs);
        observationCacheBytes += observationBytes(entry->first, entry->second);
        rt::memory::setCache(rt::memory::Cache::provider_health, observations.size(), observationCacheBytes);
      } catch (...) {
        // Health observations are optional telemetry and must never affect routing.
      }
    }

    rt::Promise<void> recordProviderHealth(const RecordProvider& provider, const std::string& identity, const ProviderHealthEvent& event,
                                           const std::optional<double>& status, const std::function<double()>& now,
                                           const std::optional<std::string>& providerRequestId, const std::optional<ClaudeCodeAccountBinding>& binding) {
      const std::string queueKey = provider + ":" + identity;
      const std::optional<std::string> normalizedRequestId = normalizedProviderRequestId(providerRequestId);
      auto& queues = providerTransitionQueues();
      if (const auto found = queues.find(queueKey); found != queues.end()) {
        const ProviderTransition& current = found->second;
        if (current.event == event && current.status == status && (current.providerRequestId || !normalizedRequestId)) return current.tail;
      }
      double observedAtMs = 0;
      try {
        observedAtMs = rt::Math::trunc(now ? now() : rt::dateNow());
      } catch (...) {
        return rt::Promise<void>::resolve();
      }

      // Reserve the transition synchronously, before getKv() or any other await.
      // Identical cold-burst observations share this promise, while a different
      // event or the first available correlation ID is appended in arrival order.
      // The injected clock can reenter and change the queue.
      const auto latest = queues.find(queueKey);
      const rt::Promise<void> previous = latest != queues.end() ? latest->second.tail : rt::Promise<void>::resolve();
      const rt::Promise<void> tail = previous.catch_([](const rt::Reason&) {}).then([provider, identity, event, status, observedAtMs, normalizedRequestId, binding]() {
        return persistProviderHealth(provider, identity, event, status, observedAtMs, normalizedRequestId, binding);
      });
      queues.insert_or_assign(queueKey, ProviderTransition{.event = event, .status = status, .providerRequestId = normalizedRequestId, .tail = tail});
      (void)tail.finally_([queueKey, tail]() {
        auto& transitions = providerTransitionQueues();
        if (const auto entry = transitions.find(queueKey); entry != transitions.end() && entry->second.tail == tail) transitions.erase(entry);
      });
      return tail;
    }

    std::string claudeCodeIdentity(const std::optional<ClaudeCodeAccountBinding>& binding) {
      return binding ? binding->account_id + ":" + binding->account_cohort_id + ":" + binding->evidence_generation : std::string("default");
    }

    ProviderHealthView unknownView() {
      return ProviderHealthView{
        .state = "unknown",
        .stale = std::nullopt,
        .last_event = std::nullopt,
        .last_status = std::nullopt,
        .last_observed_at_ms = std::nullopt,
        .last_provider_request_id = std::nullopt,
        .last_provider_request_id_at_ms = std::nullopt,
        .last_success_at_ms = std::nullopt,
        .last_401_at_ms = std::nullopt,
        .last_429_at_ms = std::nullopt,
        .last_error_at_ms = std::nullopt,
        .last_refresh_at_ms = std::nullopt,
        .last_refresh_succeeded = std::nullopt,
      };
    }

    // `events?: readonly ProviderHealthEvent[]`: an empty list is undefined (every event).
    const ProviderHealthObservation* latestObservation(const std::vector<const ProviderHealthObservation*>& observations,
                                                       std::initializer_list<std::string_view> events = {}) {
      const ProviderHealthObservation* latest = nullptr;
      for (const ProviderHealthObservation* observation : observations) {
        if (events.size() != 0 && std::find(events.begin(), events.end(), observation->event) == events.end()) continue;
        if (!latest || observation->observed_at_ms > latest->observed_at_ms) latest = observation;
      }
      return latest;
    }

    ProviderHealthView toView(const std::vector<ProviderHealthObservation>& stored, double nowMs) {
      std::vector<const ProviderHealthObservation*> observations;
      for (const ProviderHealthObservation& observation : stored) observations.push_back(&observation);
      const ProviderHealthObservation* latest = latestObservation(observations);
      if (!latest) return unknownView();
      const ProviderHealthObservation* latestState =
        latestObservation(observations, {"success", "reachable", "auth_invalid", "quota_exhausted", "upstream_error", "refresh_success", "refresh_failed"});
      const ProviderHealthObservation* success = latestObservation(observations, {"success"});
      const ProviderHealthObservation* authInvalid = latestObservation(observations, {"auth_invalid"});
      const ProviderHealthObservation* exhausted = latestObservation(observations, {"quota_exhausted"});
      const ProviderHealthObservation* error = latestObservation(observations, {"upstream_error", "refresh_failed"});
      const ProviderHealthObservation* refresh = latestObservation(observations, {"refresh_success", "refresh_failed"});
      std::vector<const ProviderHealthObservation*> correlated;
      for (const ProviderHealthObservation* observation : observations) {
        if (normalizedProviderRequestId(observation->provider_request_id)) correlated.push_back(observation);
      }
      const ProviderHealthObservation* providerRequest = latestObservation(correlated);
      const auto observedAt = [](const ProviderHealthObservation* observation) {
        return observation ? std::optional<double>(observation->observed_at_ms) : std::nullopt;
      };
      return ProviderHealthView{
        .state = latestState ? stateForObservation(*latestState) : "unknown",
        .stale = nowMs - latest->observed_at_ms > PROVIDER_HEALTH_STALE_AFTER_MS,
        .last_event = latest->event,
        .last_status = latest->status,
        .last_observed_at_ms = latest->observed_at_ms,
        .last_provider_request_id = providerRequest ? normalizedProviderRequestId(providerRequest->provider_request_id) : std::nullopt,
        .last_provider_request_id_at_ms = observedAt(providerRequest),
        .last_success_at_ms = observedAt(success),
        .last_401_at_ms = observedAt(authInvalid),
        .last_429_at_ms = observedAt(exhausted),
        .last_error_at_ms = observedAt(error),
        .last_refresh_at_ms = observedAt(refresh),
        .last_refresh_succeeded = refresh ? std::optional<bool>(refresh->event == "refresh_success") : std::nullopt,
      };
    }

    rt::Task<ProviderHealthView> readProviderHealth(RecordProvider provider, std::string identity, std::function<double()> now) {
      auto kvPromise = kv::getKv();
      std::shared_ptr<rt::Kv> kv = co_await kvPromise.wait();
      if (!kv) co_return unknownView();
      std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
      for (const std::string_view record : PROVIDER_RECORDS) {
        auto read = kv->get(providerHealthKey(provider, identity, record));
        reads.emplace_back(rt::detail::keepAlive(kv, std::move(read)));
      }
      auto everything = rt::all(std::move(reads));
      const std::vector<rt::KvEntryMaybe<>> entries = co_await everything.wait();
      std::vector<ProviderHealthObservation> observations;
      for (const rt::KvEntryMaybe<>& entry : entries) {
        if (std::optional<ProviderHealthObservation> observation = parseProviderHealthObservation(entry.value)) observations.push_back(std::move(*observation));
      }
      co_return toView(observations, rt::Math::trunc(now ? now() : rt::dateNow()));
    }
  }

  std::optional<ProviderHealthObservation> parseProviderHealthObservation(const rt::KvValue& value) {
    // `isRecord(value)` admits every object; only a plain object can carry the members read here.
    if (!value.isObject()) return std::nullopt;
    const rt::KvValue& providerRequestId = value["provider_request_id"];
    const rt::KvValue& status = value["status"];
    const rt::KvValue& observedAtMs = value["observed_at_ms"];
    if (!isProviderEvent(value["event"]) || !(status.isNull() || (status.isNumber() && rt::Number::isFinite(status.asNumber()))) ||
        !(observedAtMs.isNumber() && rt::Number::isFinite(observedAtMs.asNumber()) && observedAtMs.asNumber() >= 0) ||
        !(providerRequestId.isUndefined() || providerRequestId.isNull() || normalizedProviderRequestId(providerRequestId))) {
      return std::nullopt;
    }
    rt::Nullable<std::string> storedRequestId;
    if (providerRequestId.isNull()) storedRequestId = nullptr;
    else if (providerRequestId.isString()) storedRequestId = providerRequestId.asString();
    return ProviderHealthObservation{
      .event = value["event"].asString(),
      .status = status.isNull() ? std::nullopt : std::optional<double>(status.asNumber()),
      .observed_at_ms = observedAtMs.asNumber(),
      .provider_request_id = std::move(storedRequestId),
    };
  }

  rt::Promise<void> recordCodexProviderHealth(std::string accountId, ProviderHealthEvent event, std::optional<double> status, std::function<double()> now,
                                              std::optional<std::string> providerRequestId) {
    return recordProviderHealth("codex", accountId, event, status, now, providerRequestId, std::nullopt);
  }

  rt::Promise<void> recordOpenRouterProviderHealth(ProviderHealthEvent event, std::optional<double> status, std::function<double()> now,
                                                   std::optional<std::string> providerRequestId) {
    return recordProviderHealth("openrouter", "default", event, status, now, providerRequestId, std::nullopt);
  }

  rt::Promise<void> recordMeteredProviderHealth(ProviderHealthEvent event, std::optional<double> status, std::function<double()> now,
                                                std::optional<std::string> providerRequestId) {
    return recordProviderHealth("metered", "default", event, status, now, providerRequestId, std::nullopt);
  }

  rt::Promise<void> recordSurplusProviderHealth(ProviderHealthEvent event, std::optional<double> status, std::function<double()> now,
                                                std::optional<std::string> providerRequestId) {
    return recordProviderHealth("surplus", "default", event, status, now, providerRequestId, std::nullopt);
  }

  rt::Promise<void> recordCerebrasProviderHealth(ProviderHealthEvent event, std::optional<double> status, std::function<double()> now,
                                                 std::optional<std::string> providerRequestId) {
    return recordProviderHealth("cerebras", "default", event, status, now, providerRequestId, std::nullopt);
  }

  rt::Promise<void> recordDeepSeekProviderHealth(ProviderHealthEvent event, std::optional<double> status, std::function<double()> now,
                                                 std::optional<std::string> providerRequestId) {
    return recordProviderHealth("deepseek", "default", event, status, now, providerRequestId, std::nullopt);
  }

  rt::Promise<void> recordLithosProviderHealth(ProviderHealthEvent event, std::optional<double> status, std::function<double()> now,
                                               std::optional<std::string> providerRequestId) {
    return recordProviderHealth("lithos", "default", event, status, now, providerRequestId, std::nullopt);
  }

  rt::Promise<void> recordGeminiProviderHealth(ProviderHealthEvent event, std::optional<double> status, std::function<double()> now,
                                               std::optional<std::string> providerRequestId) {
    return recordProviderHealth("gemini", "default", event, status, now, providerRequestId, std::nullopt);
  }

  rt::Promise<void> recordClaudeCodeProviderHealth(ProviderHealthEvent event, std::optional<double> status, std::function<double()> now,
                                                   std::optional<std::string> providerRequestId,
                                                   std::optional<claude_code_accounts::ClaudeCodeAccountBinding> binding) {
    return recordProviderHealth("claude_code", claudeCodeIdentity(binding), event, status, now, providerRequestId, binding);
  }

  rt::Task<ProviderHealthView> getCodexProviderHealth(std::string accountId) { return readProviderHealth("codex", std::move(accountId), {}); }
  rt::Task<ProviderHealthView> getCodexProviderHealth(std::string accountId, std::function<double()> now) {
    return readProviderHealth("codex", std::move(accountId), std::move(now));
  }

  rt::Task<ProviderHealthView> getMeteredProviderHealth() { return readProviderHealth("metered", "default", {}); }
  rt::Task<ProviderHealthView> getMeteredProviderHealth(std::function<double()> now) { return readProviderHealth("metered", "default", std::move(now)); }
  rt::Task<ProviderHealthView> getOpenRouterProviderHealth() { return readProviderHealth("openrouter", "default", {}); }
  rt::Task<ProviderHealthView> getOpenRouterProviderHealth(std::function<double()> now) { return readProviderHealth("openrouter", "default", std::move(now)); }

  rt::Task<ProviderHealthView> getSurplusProviderHealth() { return readProviderHealth("surplus", "default", {}); }
  rt::Task<ProviderHealthView> getSurplusProviderHealth(std::function<double()> now) { return readProviderHealth("surplus", "default", std::move(now)); }

  rt::Task<ProviderHealthView> getCerebrasProviderHealth() { return readProviderHealth("cerebras", "default", {}); }
  rt::Task<ProviderHealthView> getCerebrasProviderHealth(std::function<double()> now) { return readProviderHealth("cerebras", "default", std::move(now)); }

  rt::Task<ProviderHealthView> getDeepSeekProviderHealth() { return readProviderHealth("deepseek", "default", {}); }
  rt::Task<ProviderHealthView> getDeepSeekProviderHealth(std::function<double()> now) { return readProviderHealth("deepseek", "default", std::move(now)); }

  rt::Task<ProviderHealthView> getLithosProviderHealth() { return readProviderHealth("lithos", "default", {}); }
  rt::Task<ProviderHealthView> getLithosProviderHealth(std::function<double()> now) { return readProviderHealth("lithos", "default", std::move(now)); }
  rt::Task<ProviderHealthView> getGeminiProviderHealth() { return readProviderHealth("gemini", "default", {}); }
  rt::Task<ProviderHealthView> getGeminiProviderHealth(std::function<double()> now) { return readProviderHealth("gemini", "default", std::move(now)); }

  rt::Task<ProviderHealthView> getClaudeCodeProviderHealth() { return getClaudeCodeProviderHealth({}, std::nullopt); }
  rt::Task<ProviderHealthView> getClaudeCodeProviderHealth(std::function<double()> now) { return getClaudeCodeProviderHealth(std::move(now), std::nullopt); }
  rt::Task<ProviderHealthView> getClaudeCodeProviderHealth(std::function<double()> now, std::optional<claude_code_accounts::ClaudeCodeAccountBinding> binding) {
    rt::Promise<ProviderHealthView> reading(readProviderHealth("claude_code", claudeCodeIdentity(binding), std::move(now)));
    ProviderHealthView view = co_await reading.wait();
    auto kvPromise = kv::getKv();
    std::shared_ptr<rt::Kv> kv = co_await kvPromise.wait();
    if (binding) {
      if (!kv) co_return unknownView();
      rt::Promise<std::optional<rt::KvEntryMaybe<>>> current(claude_code_accounts::currentClaudeCodeAccountEntry(kv, *binding));
      const std::optional<rt::KvEntryMaybe<>> entry = co_await current.wait();
      if (!entry) co_return unknownView();
    }
    co_return view;
  }

  void resetProviderHealthThrottleForTest() {
    lastHeartbeatWriteAtMs().clear();
    lastObservation().clear();
    observationCacheBytes = 0;
    rt::memory::setCache(rt::memory::Cache::provider_health, 0, 0);
    providerTransitionQueues().clear();
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderHealthObservation& value) {
    out = Writer()
            .required("event", value.event)
            .orNull("status", value.status)
            .required("observed_at_ms", value.observed_at_ms)
            .nullable("provider_request_id", value.provider_request_id)
            .done();
  }

  ProviderHealthObservation tag_invoke(const json::value_to_tag<ProviderHealthObservation>&, const json::value& value) {
    const Reader in(value, "ProviderHealthObservation");
    return ProviderHealthObservation{
      .event = in.required<std::string>("event"),
      .status = in.orNull<double>("status"),
      .observed_at_ms = in.required<double>("observed_at_ms"),
      .provider_request_id = in.nullable<std::string>("provider_request_id"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderHealthView& value) {
    out = Writer()
            .required("state", value.state)
            .orNull("stale", value.stale)
            .orNull("last_event", value.last_event)
            .orNull("last_status", value.last_status)
            .orNull("last_observed_at_ms", value.last_observed_at_ms)
            .orNull("last_provider_request_id", value.last_provider_request_id)
            .orNull("last_provider_request_id_at_ms", value.last_provider_request_id_at_ms)
            .orNull("last_success_at_ms", value.last_success_at_ms)
            .orNull("last_401_at_ms", value.last_401_at_ms)
            .orNull("last_429_at_ms", value.last_429_at_ms)
            .orNull("last_error_at_ms", value.last_error_at_ms)
            .orNull("last_refresh_at_ms", value.last_refresh_at_ms)
            .orNull("last_refresh_succeeded", value.last_refresh_succeeded)
            .done();
  }

  ProviderHealthView tag_invoke(const json::value_to_tag<ProviderHealthView>&, const json::value& value) {
    const Reader in(value, "ProviderHealthView");
    return ProviderHealthView{
      .state = in.required<std::string>("state"),
      .stale = in.orNull<bool>("stale"),
      .last_event = in.orNull<std::string>("last_event"),
      .last_status = in.orNull<double>("last_status"),
      .last_observed_at_ms = in.orNull<double>("last_observed_at_ms"),
      .last_provider_request_id = in.orNull<std::string>("last_provider_request_id"),
      .last_provider_request_id_at_ms = in.orNull<double>("last_provider_request_id_at_ms"),
      .last_success_at_ms = in.orNull<double>("last_success_at_ms"),
      .last_401_at_ms = in.orNull<double>("last_401_at_ms"),
      .last_429_at_ms = in.orNull<double>("last_429_at_ms"),
      .last_error_at_ms = in.orNull<double>("last_error_at_ms"),
      .last_refresh_at_ms = in.orNull<double>("last_refresh_at_ms"),
      .last_refresh_succeeded = in.orNull<bool>("last_refresh_succeeded"),
    };
  }
}
