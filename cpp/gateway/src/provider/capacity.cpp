#include "uos/provider/capacity.hpp"

#include "uos/cache/prompt_analytics_read.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/experiment.hpp"
#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/metered_quota.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/paid_fallback/ledger_stats.hpp"
#include "uos/provider/capacity_analytics.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/claude_capacity_history.hpp"
#include "uos/provider/claude_capacity_rollups.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/health.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/url.hpp"
#include "uos/utils.hpp"

#include <tuple>
#include <utility>

namespace uos::provider::capacity {
  using capacity_contract::CapacityLease;
  using capacity_contract::ProviderCapacityClaudeSource;
  using capacity_contract::ProviderCapacityClaudeStandardLimit;
  using capacity_contract::ProviderCapacityClaudeWindow;
  using capacity_events::ProviderCapacityDowntimeEvent;
  using capacity_events::ProviderCapacityRateLimitResetEvent;
  using capacity_events::ProviderCapacityResetEvent;
  using claude_code_accounts::ClaudeCodeAccountBinding;

  namespace {
    // Handler startup imports this module before inference; no admin visit is needed.
    [[maybe_unused]] const bool claudeObservationRecorderRegistered = [] {
      claude_code_quota::registerClaudeCodeObservationRecorder(&claude_capacity_history::recordClaudeCapacityObservation);
      return true;
    }();

    // `options.now ?? Date.now`
    std::function<double()> nowOf(const ProviderCapacitySnapshotOptions& options) {
      if (options.now) return options.now;
      return [] { return rt::dateNow(); };
    }

    // `options.kv === undefined ? await getKv() : options.kv`
    rt::Task<std::shared_ptr<rt::Kv>> optionsKv(std::optional<std::shared_ptr<rt::Kv>> kv) {
      if (kv) co_return *kv;
      auto opening = kv::getKv();
      std::shared_ptr<rt::Kv> opened = co_await opening.wait();
      co_return opened;
    }

    void cancelResponseBody(const rt::Response& response) {
      try {
        if (const std::optional<rt::ByteStream> body = response.body()) rt::spawn(body->cancel(), rt::ignoreRejection);
      } catch (...) {
        // The capacity response is already classified as unavailable.
      }
    }

    rt::Task<ProviderCapacityCodexSource> fetchCodexCapacitySource(codex::experiment::CodexCapacityAccount account, std::string accountCohortId, double snapshotAtMs,
                                                                   ProviderCapacityFetch fetcher, rt::AbortSignal signal) {
      rt::Headers headers;
      headers.append("Accept", "application/json");
      headers.append("Authorization", "Bearer " + account.access_token);
      headers.append("ChatGPT-Account-ID", account.account_id);
      headers.append("User-Agent", "codex_cli_rs/0.100.0 (ai.ubq.fi)");
      const std::string label = capacity_parse::codexAccountLabel(account.slot, account.email);
      std::optional<ProviderCapacityCodexSource> result;
      try {
        rt::RequestInit init;
        init.method = "GET";
        init.headers = headers;
        init.redirect = "manual";
        init.signal = signal;
        const std::string url = capacity_parse::codexUsageUrl();
        auto fetching = fetcher(url, std::move(init));
        rt::Response response = co_await std::move(fetching);
        if (!response.ok()) {
          cancelResponseBody(response);
          co_return capacity_parse::unavailableCodexSource(account.slot, snapshotAtMs, response.status() >= 500 ? "upstream_error" : "http_error", response.status(),
                                                           true, label, accountCohortId);
        }
        std::optional<json::value> payload;
        try {
          auto reading = response.json();
          payload = co_await std::move(reading);
        } catch (...) {
          payload.reset();
        }
        if (!payload) {
          co_return capacity_parse::unavailableCodexSource(account.slot, snapshotAtMs, "invalid_response", response.status(), true, label, accountCohortId);
        }
        std::optional<capacity_parse::ParseCodexUsageResult> windows = capacity_parse::parseCodexUsage(*payload);
        if (!windows) {
          co_return capacity_parse::unavailableCodexSource(account.slot, snapshotAtMs, "invalid_response", response.status(), true, label, accountCohortId);
        }
        result = ProviderCapacityCodexSource{
          .label = label,
          .slot = account.slot,
          .account_cohort_id = accountCohortId,
          .state = "available",
          .source_observed_at_ms = snapshotAtMs,
          .snapshot_at_ms = snapshotAtMs,
          .failure_kind = std::nullopt,
          .failure_status = std::nullopt,
          .windows =
            {
              .primary = windows->primary,
              .secondary = windows->secondary,
            },
          .additional_rate_limits = windows->additional_rate_limits,
        };
      } catch (...) {
        result.reset();
      }
      if (!result) co_return capacity_parse::unavailableCodexSource(account.slot, snapshotAtMs, "unreachable", std::nullopt, true, label, accountCohortId);
      co_return *result;
    }

    ProviderCapacitySource meteredCapacitySource(const std::optional<metered_quota::MeteredQuotaSnapshot>& snapshot, double snapshotAtMs) {
      if (!snapshot) return capacity_parse::unavailableMeteredSource(snapshotAtMs);
      const double sourceObservedAtMs = snapshot->state.observed_at_ms;
      const bool stale = snapshot->cache_state == "stale" || snapshotAtMs - sourceObservedAtMs >= metered_quota::METERED_QUOTA_FRESH_MS;
      const bool tokenUsage = snapshot->unlimited_quota || snapshot->total_available || snapshot->total_used;
      return ProviderCapacityMeteredSource{
        .label = "Metered fallback",
        .state = stale ? "stale" : "available",
        .source_observed_at_ms = sourceObservedAtMs,
        .snapshot_at_ms = snapshotAtMs,
        .wallet =
          {
            .balance_credits = snapshot->balance_credits,
            .baseline_credits = snapshot->baseline_credits,
            .refill_cycle_remaining_percent = snapshot->remaining_percent,
            .refill_cycle_used_percent = snapshot->used_percent,
            .unlimited_quota = snapshot->unlimited_quota,
            .total_available = snapshot->total_available,
            .total_granted = snapshot->total_granted,
            .total_used = snapshot->total_used,
            .cycle_started_at_ms = tokenUsage ? std::nullopt : std::optional<double>(snapshot->state.cycle_started_at_ms),
            .last_credit_at_ms = tokenUsage ? std::nullopt : snapshot->state.last_credit_at_ms,
            .confidence = tokenUsage ? std::nullopt : std::optional<std::string>(snapshot->state.confidence),
            .cache_state = snapshot->cache_state,
            // Metered exposes a refill cycle, not a scheduled reset window.
            .reset_at_ms = nullptr,
          },
      };
    }

    /** Unified headers report a fraction; normalize and clamp without guessing percent units. */
    std::optional<ProviderCapacityClaudeWindow> claudeCapacityWindow(const claude_code_quota::ClaudeCodeQuotaWindow* window) {
      if (!window) return std::nullopt;
      return ProviderCapacityClaudeWindow{
        .status = window->status,
        .used_percent = claude_capacity_history::claudeCapacityUsedPercent(window->utilization),
        .reset_at_ms = window->reset_at_ms,
        .observed_at_ms = window->observed_at_ms,
      };
    }

    std::vector<ProviderCapacityClaudeStandardLimit> claudeStandardLimits(const claude_code_quota::ClaudeCodeHeaderObservation* quota) {
      std::vector<ProviderCapacityClaudeStandardLimit> limits;
      if (!quota) return limits;
      for (const std::string& family : quota->standard.keys()) {
        const claude_code_quota::ClaudeCodeStandardWindow& window = *quota->standard.get(family);
        limits.push_back(ProviderCapacityClaudeStandardLimit{.family = family, .limit = window.limit, .remaining = window.remaining, .reset_at_ms = window.reset_at_ms});
      }
      return limits;
    }

    std::optional<double> claudeLatestObservationMs(const claude_code_quota::ClaudeCodeHeaderObservation* quota) {
      std::vector<double> candidates;
      const auto push = [&candidates](double value) {
        if (rt::Number::isFinite(value)) candidates.push_back(value);
      };
      if (quota) {
        for (const claude_code_quota::ClaudeCodeQuotaWindow* window : quota->windows.values()) push(window->observed_at_ms);
        for (const claude_code_quota::ClaudeCodeStandardWindow* window : quota->standard.values()) push(window->observed_at_ms);
      }
      if (candidates.empty()) return std::nullopt;
      return rt::Math::max(std::span<const double>(candidates));
    }

    /**
     * The live Claude subscription analytics row: the resolved credential
     * direction and workspace, the last observed unified or standard quota
     * windows, the recorded configuration guard, and the last upstream outcome. A
     * missing credential is `not_configured`; an active quota block, cooldown, or
     * recorded configuration error is `unavailable` with its status, because the
     * tier genuinely cannot serve until that guard clears. A never-observed but
     * eligible tier stays `available` (there is nothing to be stale yet).
     */
    rt::Task<ProviderCapacityClaudeSource> capacitySourceForClaudeAccount(std::shared_ptr<rt::Kv> kv, double snapshotAtMs, std::optional<ClaudeCodeAccountBinding> binding) {
      const std::optional<claude_code_credentials::ClaudeCodeResolvedCredential> credential =
        binding ? std::optional<claude_code_credentials::ClaudeCodeResolvedCredential>(binding->credential) : std::nullopt;
      const std::optional<claude_code_credentials::ClaudeCodeWorkspaceState> workspace = binding ? binding->workspace : std::nullopt;
      std::optional<claude_code_quota::ClaudeCodeQuotaState> quota;
      std::optional<health::ProviderHealthView> health;
      if (binding) {
        const rt::Promise<std::optional<claude_code_quota::ClaudeCodeQuotaState>> quotaLoad =
          rt::Promise<std::optional<claude_code_quota::ClaudeCodeQuotaState>>(claude_code_quota::loadClaudeCodeQuotaState(kv, binding))
            .catch_([](const rt::Reason&) -> std::optional<claude_code_quota::ClaudeCodeQuotaState> { return std::nullopt; });
        const std::function<double()> observedNow = [snapshotAtMs] { return snapshotAtMs; };
        const rt::Promise<std::optional<health::ProviderHealthView>> healthLoad =
          rt::Promise<health::ProviderHealthView>(health::getClaudeCodeProviderHealth(observedNow, binding))
            .then([](health::ProviderHealthView view) { return std::optional<health::ProviderHealthView>(std::move(view)); })
            .catch_([](const rt::Reason&) -> std::optional<health::ProviderHealthView> { return std::nullopt; });
        auto both = rt::all(quotaLoad, healthLoad);
        std::tuple<std::optional<claude_code_quota::ClaudeCodeQuotaState>, std::optional<health::ProviderHealthView>> loaded = co_await both.wait();
        quota = std::move(std::get<0>(loaded));
        health = std::move(std::get<1>(loaded));
      }
      const std::optional<double> blockedUntilMs = quota ? quota->blocked_until_ms : std::nullopt;
      const std::optional<double> cooldownUntilMs = quota ? quota->cooldown_until_ms : std::nullopt;
      const bool blocked = blockedUntilMs && *blockedUntilMs > snapshotAtMs;
      const bool cooling = cooldownUntilMs && *cooldownUntilMs > snapshotAtMs;
      const std::optional<claude_code_quota::ClaudeCodeQuotaError> lastError = quota ? quota->last_error : std::nullopt;
      // A healthy response without quota headers cannot refresh old quota evidence.
      const claude_code_quota::ClaudeCodeHeaderObservation* headerQuota = quota && quota->response_headers ? &*quota->response_headers : nullptr;
      const std::optional<double> observedAtMs = claudeLatestObservationMs(headerQuota);
      const bool failing = blocked || cooling || lastError.has_value();
      std::string state = "available";
      if (!credential || failing) state = "unavailable";
      else if (observedAtMs && snapshotAtMs - *observedAtMs > capacity_contract::PROVIDER_CAPACITY_SOURCE_STALE_MS) state = "stale";
      std::optional<std::string> failureKind;
      if (!credential) failureKind = "not_configured";
      else if (failing) failureKind = "upstream_error";
      std::optional<double> failureStatus = lastError ? lastError->status : std::nullopt;
      if (!failureStatus && blocked) failureStatus = 429;
      if (!failureStatus && cooling) failureStatus = health ? health->last_status : std::nullopt;
      ProviderCapacityClaudeSource source{
        .usage_snapshot = std::nullopt,
        .usage_stale = std::nullopt,
        .source = "claude_code",
        .label = "Claude subscription",
        .account_id = binding ? rt::Nullable<std::string>(binding->account_id) : rt::Nullable<std::string>(nullptr),
        .account_cohort_id = binding ? rt::Nullable<std::string>(binding->account_cohort_id) : rt::Nullable<std::string>(nullptr),
        .state = state,
        .source_observed_at_ms = observedAtMs,
        .snapshot_at_ms = snapshotAtMs,
        .failure_kind = failureKind,
        .failure_status = failureStatus,
        .credential_kind = credential ? std::optional<std::string>(credential->kind) : std::nullopt,
        .workspace_id = workspace ? std::optional<std::string>(workspace->id) : std::nullopt,
        .windows =
          {
            .five_hour = claudeCapacityWindow(headerQuota ? headerQuota->windows.get("5h") : nullptr),
            .seven_day = claudeCapacityWindow(headerQuota ? headerQuota->windows.get("7d") : nullptr),
          },
        .standard = claudeStandardLimits(headerQuota),
        .usage = std::nullopt,
        .blocked_until_ms = blockedUntilMs,
        .cooldown_until_ms = cooldownUntilMs,
        .last_error = lastError ? std::optional<capacity_contract::ProviderCapacityClaudeError>(capacity_contract::ProviderCapacityClaudeError{
                                    .kind = lastError->kind, .status = lastError->status, .message = lastError->message, .at_ms = lastError->at_ms})
                                : std::nullopt,
      };
      co_return source;
    }

    struct ClaudeCapacityRoster {
      ProviderCapacityClaudeSource claude;
      std::vector<ProviderCapacityClaudeSource> claude_accounts;
    };

    // `source.account_id === store.active_account_id` (string | null | undefined against string | null).
    bool sameAccountId(const rt::Nullable<std::string>& accountId, const std::optional<std::string>& activeAccountId) {
      if (accountId == rt::undefined) return false;
      if (accountId == nullptr) return !activeAccountId;
      return activeAccountId && *activeAccountId == *accountId;
    }

    rt::Task<ClaudeCapacityRoster> claudeCapacityRoster(std::shared_ptr<rt::Kv> kv, double snapshotAtMs) {
      if (!kv) co_return ClaudeCapacityRoster{.claude = unavailableClaudeCapacitySource(snapshotAtMs), .claude_accounts = {}};
      // The foundation validates the bounded roster before any per-account read.
      // Resolve once so old clients see exactly the active row from this same roster.
      rt::Promise<claude_code_accounts::ClaudeCodeAccountStore> loading(claude_code_accounts::loadClaudeCodeAccountStore(kv));
      const claude_code_accounts::ClaudeCodeAccountStore store = co_await loading.wait();
      std::vector<rt::Promise<ProviderCapacityClaudeSource>> sources;
      for (const ClaudeCodeAccountBinding& binding : store.accounts) {
        sources.push_back(rt::Promise<ProviderCapacityClaudeSource>(capacitySourceForClaudeAccount(kv, snapshotAtMs, binding)));
      }
      auto everything = rt::all(std::move(sources));
      std::vector<ProviderCapacityClaudeSource> accounts = co_await everything.wait();
      const ProviderCapacityClaudeSource* active = nullptr;
      for (const ProviderCapacityClaudeSource& source : accounts) {
        if (sameAccountId(source.account_id, store.active_account_id)) {
          active = &source;
          break;
        }
      }
      ClaudeCapacityRoster roster{.claude = active ? *active : unavailableClaudeCapacitySource(snapshotAtMs), .claude_accounts = std::move(accounts)};
      co_return roster;
    }

    // One slot of captureProviderCapacitySnapshot's `([1, 2] as const).map(async (slot) => ...)`.
    rt::Task<ProviderCapacityCodexSource> codexSlotSource(std::optional<codex::experiment::CodexCapacityAccount> account, double slot, double snapshotAtMs,
                                                          ProviderCapacityFetch fetcher, rt::AbortSignal signal) {
      if (!account) co_return capacity_parse::unavailableCodexSource(slot, snapshotAtMs);
      std::optional<ProviderCapacityCodexSource> source;
      try {
        rt::Promise<std::string> hashing(utils::sha256Hex(std::string("uos-prompt-cache-account-cohort-v1") + std::string(1, '\0') + account->account_id));
        std::string accountCohortId = co_await hashing.wait();
        rt::Promise<ProviderCapacityCodexSource> fetching(fetchCodexCapacitySource(*account, std::move(accountCohortId), snapshotAtMs, fetcher, signal));
        source = co_await fetching.wait();
      } catch (...) {
        source.reset();
      }
      if (!source) {
        co_return capacity_parse::unavailableCodexSource(slot, snapshotAtMs, "unreachable", std::nullopt, true,
                                                         capacity_parse::codexAccountLabel(account->slot, account->email));
      }
      co_return *source;
    }

    using RoutingWindow = authority::native_codex_contracts::CodexCapacityRoutingWindow;

    std::optional<RoutingWindow> routingWindow(const std::optional<capacity_contract::ProviderCapacityWindow>& window) {
      if (!window) return std::nullopt;
      return RoutingWindow{.limit_window_seconds = window->limit_window_seconds, .used_percent = window->used_percent, .reset_at_ms = window->reset_at_ms};
    }

    authority::native_codex_contracts::CodexCapacityRoutingWindows routingWindows(const ProviderCapacityCodexSource::Windows& windows) {
      return {.primary = routingWindow(windows.primary), .secondary = routingWindow(windows.secondary)};
    }

    std::vector<authority::native_codex_contracts::CodexCapacityRoutingAdditionalRateLimit> routingLimits(
      const std::vector<capacity_contract::ProviderCapacityAdditionalRateLimit>& limits) {
      std::vector<authority::native_codex_contracts::CodexCapacityRoutingAdditionalRateLimit> out;
      for (const capacity_contract::ProviderCapacityAdditionalRateLimit& limit : limits) {
        out.push_back({.limit_name = limit.limit_name, .metered_feature = limit.metered_feature, .windows = routingWindows(limit.windows)});
      }
      return out;
    }

    rt::Task<ProviderCapacitySnapshot> captureProviderCapacitySnapshot(ProviderCapacitySnapshotOptions options, double snapshotAtMs, std::shared_ptr<rt::Kv> kv,
                                                                       bool forceMeteredRefresh) {
      std::vector<codex::experiment::CodexCapacityAccount> accounts;
      try {
        rt::Promise<std::vector<codex::experiment::CodexCapacityAccount>> loading(codex::experiment::getCodexCapacityAccounts());
        accounts = co_await loading.wait();
      } catch (...) {
        // Missing or malformed auth is represented by redacted unavailable slots.
      }

      const ProviderCapacityFetch fetcher =
        options.fetcher ? options.fetcher : ProviderCapacityFetch([](rt::RequestInfo input, rt::RequestInit init) { return rt::fetch(std::move(input), std::move(init)); });
      const rt::AbortSignal timeout = rt::AbortSignal::timeout(PROVIDER_CAPACITY_CODEX_TIMEOUT_MS);
      std::vector<rt::AbortSignal> signals;
      if (options.signal) signals = {*options.signal, timeout};
      const rt::AbortSignal signal = options.signal ? rt::AbortSignal::any(signals) : timeout;
      std::vector<rt::Promise<ProviderCapacityCodexSource>> slots;
      for (const double slot : {1.0, 2.0}) {
        std::optional<codex::experiment::CodexCapacityAccount> account;
        for (const codex::experiment::CodexCapacityAccount& candidate : accounts) {
          if (candidate.slot == slot) {
            account = candidate;
            break;
          }
        }
        slots.push_back(rt::Promise<ProviderCapacityCodexSource>(codexSlotSource(std::move(account), slot, snapshotAtMs, fetcher, signal)));
      }
      const rt::Promise<std::vector<ProviderCapacityCodexSource>> codexPromise = rt::all(std::move(slots));
      metered_quota::GetMeteredQuotaSnapshotOptions meteredOptions;
      meteredOptions.kv = kv;
      meteredOptions.fetcher = fetcher;
      meteredOptions.now = [snapshotAtMs] { return snapshotAtMs; };
      meteredOptions.signal = signal;
      meteredOptions.forceRefresh = forceMeteredRefresh;
      meteredOptions.createLeaseOwner = options.createLeaseOwner;
      const rt::Promise<std::optional<metered_quota::MeteredQuotaSnapshot>> meteredPromise =
        rt::Promise<std::optional<metered_quota::MeteredQuotaSnapshot>>(metered_quota::getConfiguredMeteredQuotaSnapshot(std::move(meteredOptions)))
          .catch_([](const rt::Reason&) -> std::optional<metered_quota::MeteredQuotaSnapshot> { return std::nullopt; });
      auto both = rt::all(codexPromise, meteredPromise);
      std::tuple<std::vector<ProviderCapacityCodexSource>, std::optional<metered_quota::MeteredQuotaSnapshot>> captured = co_await both.wait();
      std::vector<ProviderCapacityCodexSource>& codexSources = std::get<0>(captured);
      const std::optional<metered_quota::MeteredQuotaSnapshot>& meteredSnapshot = std::get<1>(captured);

      std::vector<codex::routing_state::CodexCapacityRoutingObservationInput> routingObservations;
      for (const codex::experiment::CodexCapacityAccount& account : accounts) {
        const ProviderCapacityCodexSource* source = nullptr;
        for (const ProviderCapacityCodexSource& candidate : codexSources) {
          if (candidate.slot == account.slot) {
            source = &candidate;
            break;
          }
        }
        if (!source) continue;
        routingObservations.push_back(codex::routing_state::CodexCapacityRoutingObservationInput{
          .slot = account.slot - 1,
          .account_id = account.account_id,
          .state = source->state,
          .source_observed_at_ms = source->source_observed_at_ms,
          .snapshot_at_ms = source->snapshot_at_ms,
          .windows = routingWindows(source->windows),
          .additional_rate_limits = routingLimits(capacity_parse::additionalRateLimitsForRouting(source->additional_rate_limits, snapshotAtMs)),
        });
      }
      rt::Promise<void> recording(codex::capacity_routing::recordCodexCapacityRoutingObservations(std::move(routingObservations), snapshotAtMs));
      co_await recording.wait();

      ProviderCapacitySnapshot snapshot{
        .snapshot_at_ms = snapshotAtMs,
        .stale_after_ms = PROVIDER_CAPACITY_SOURCE_STALE_MS,
        .sources = {codexSources[0], codexSources[1], meteredCapacitySource(meteredSnapshot, snapshotAtMs)},
      };
      co_return snapshot;
    }

    ProviderCapacitySource staleProviderCapacitySource(const ProviderCapacitySource& source, const ProviderCapacitySnapshot& snapshot, double nowMs) {
      if (const auto* codex = std::get_if<ProviderCapacityCodexSource>(&source)) {
        if (codex->state != "available") return source;
        if (nowMs >= codex->snapshot_at_ms + snapshot.stale_after_ms) {
          ProviderCapacityCodexSource stale = *codex;
          stale.state = "stale";
          return stale;
        }
        return source;
      }
      const ProviderCapacityMeteredSource& metered = std::get<ProviderCapacityMeteredSource>(source);
      if (metered.state != "available") return source;
      const bool meteredStale = metered.wallet.cache_state == std::optional<std::string>("stale") || !metered.source_observed_at_ms ||
                                nowMs - *metered.source_observed_at_ms >= metered_quota::METERED_QUOTA_FRESH_MS;
      if (!meteredStale) return source;
      ProviderCapacityMeteredSource stale = metered;
      stale.state = "stale";
      return stale;
    }

    ProviderCapacitySnapshot staleProviderSnapshot(const ProviderCapacitySnapshot& snapshot, double nowMs) {
      ProviderCapacitySnapshot current = snapshot;
      for (std::size_t index = 0; index < current.sources.size(); ++index) current.sources[index] = staleProviderCapacitySource(snapshot.sources[index], snapshot, nowMs);
      return current;
    }

    std::string_view stateOf(const ProviderCapacitySource& source) {
      return std::visit([](const auto& arm) -> std::string_view { return arm.state; }, source);
    }

    // `requestedState: Exclude<ProviderCapacityViewState, "unavailable">`
    ProviderCapacityView toCapacityView(const ProviderCapacitySnapshot& snapshot, std::string_view requestedState, std::vector<ProviderCapacityHistoryPoint> history,
                                        std::vector<ProviderCapacityResetEvent> resetEvents, std::vector<ProviderCapacityRateLimitResetEvent> rateLimitResetEvents,
                                        std::vector<ProviderCapacityDowntimeEvent> downtimeEvents, double nowMs) {
      const ProviderCapacitySnapshot current = staleProviderSnapshot(snapshot, nowMs);
      const bool stale = std::any_of(current.sources.begin(), current.sources.end(), [](const ProviderCapacitySource& source) { return stateOf(source) == "stale"; });
      ProviderCapacityView view{};
      static_cast<ProviderCapacitySnapshot&>(view) = current;
      view.cache_state = stale ? std::string("stale") : std::string(requestedState);
      view.history = std::move(history);
      view.reset_events = std::move(resetEvents);
      view.rate_limit_reset_events = std::move(rateLimitResetEvents);
      view.downtime_events = std::move(downtimeEvents);
      return view;
    }

    ProviderCapacitySnapshot unavailableSnapshot(double snapshotAtMs) {
      return ProviderCapacitySnapshot{
        .snapshot_at_ms = snapshotAtMs,
        .stale_after_ms = PROVIDER_CAPACITY_SOURCE_STALE_MS,
        .sources = {capacity_parse::unavailableCodexSource(1, snapshotAtMs), capacity_parse::unavailableCodexSource(2, snapshotAtMs),
                    capacity_parse::unavailableMeteredSource(snapshotAtMs)},
      };
    }

    ProviderCapacityView unavailableView(double snapshotAtMs, std::vector<ProviderCapacityHistoryPoint> history, std::vector<ProviderCapacityResetEvent> resetEvents,
                                         std::vector<ProviderCapacityRateLimitResetEvent> rateLimitResetEvents, std::vector<ProviderCapacityDowntimeEvent> downtimeEvents) {
      ProviderCapacityView view{};
      static_cast<ProviderCapacitySnapshot&>(view) = unavailableSnapshot(snapshotAtMs);
      view.cache_state = "unavailable";
      view.history = std::move(history);
      view.reset_events = std::move(resetEvents);
      view.rate_limit_reset_events = std::move(rateLimitResetEvents);
      view.downtime_events = std::move(downtimeEvents);
      return view;
    }

    ProviderCapacityView emptyUnavailableView(double snapshotAtMs) { return unavailableView(snapshotAtMs, {}, {}, {}, {}); }

    // `String(value)` of a stored primitive, for the relational comparison below (null and undefined
    // array elements print as "").
    std::optional<std::string> primitiveText(const rt::KvValue& value) {
      if (value.isString()) return value.asString();
      if (value.isNumber()) return rt::numberToString(value.asNumber());
      if (value.isBool()) return std::string(value.asBool() ? "true" : "false");
      if (value.isBigInt()) return value.asBigInt().toString();
      return std::nullopt;
    }

    // ToNumber(ToPrimitive(value, number)) of a stored value (NaN where JS reaches NaN).
    double storedToNumber(const rt::KvValue& value) {
      if (value.isNumber()) return value.asNumber();
      if (value.isString()) return rt::stringToNumber(value.asString());
      if (value.isBool()) return value.asBool() ? 1 : 0;
      if (value.isNull()) return 0;
      if (value.isBigInt()) return value.asBigInt().toNumber();
      if (value.is<rt::KvDate>()) return value.as<rt::KvDate>().time;
      if (value.isArray()) {
        // Array.prototype.toString: elements joined by ",", null and undefined as "".
        std::string text;
        bool first = true;
        for (const rt::KvValue& item : value.asArray()) {
          if (!first) text += ",";
          first = false;
          if (item.isNullish() || item.is<rt::KvHole>()) continue;
          const std::optional<std::string> part = primitiveText(item);
          if (!part) return rt::Number::NaN;
          text += *part;
        }
        return rt::stringToNumber(text);
      }
      return rt::Number::NaN;
    }

    // `entry.value && entry.value.lease_until_ms > nowMs`
    bool leaseHeld(const rt::KvValue& value, double nowMs) { return value.truthy() && storedToNumber(value["lease_until_ms"]) > nowMs; }

    // `entry.value?.owner === owner`
    bool leaseOwnedBy(const rt::KvValue& value, const std::string& owner) { return value["owner"].isString() && value["owner"].asString() == owner; }

    struct CapacityLeaseAttempt {
      bool acquired;
      rt::KvEntryMaybe<> entry;
    };

    CapacityLeaseAttempt unacquiredLease() {
      return CapacityLeaseAttempt{.acquired = false, .entry = rt::KvEntryMaybe<>{.key = PROVIDER_CAPACITY_LEASE_KEY, .value = rt::KvValue(nullptr), .versionstamp = std::nullopt}};
    }

    rt::Task<CapacityLeaseAttempt> acquireCapacityLease(std::shared_ptr<rt::Kv> kv, std::string owner, double nowMs) {
      auto reading = kv->get(PROVIDER_CAPACITY_LEASE_KEY);
      const rt::KvEntryMaybe<> entry = co_await std::move(reading);
      if (leaseHeld(entry.value, nowMs)) co_return CapacityLeaseAttempt{.acquired = false, .entry = entry};
      const CapacityLease lease{.owner = owner, .lease_until_ms = nowMs + PROVIDER_CAPACITY_LEASE_MS};
      std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      const rt::KvSetOptions options{.expireIn = PROVIDER_CAPACITY_LEASE_MS * 2};
      operation->check(entry).set(PROVIDER_CAPACITY_LEASE_KEY, rt::KvValue(json::value_from(lease)), options);
      auto committing = operation->commit();
      const rt::KvCommitResult committed = co_await std::move(committing);
      if (!committed.ok) co_return CapacityLeaseAttempt{.acquired = false, .entry = entry};
      auto confirming = kv->get(PROVIDER_CAPACITY_LEASE_KEY);
      const rt::KvEntryMaybe<> acquiredEntry = co_await std::move(confirming);
      co_return CapacityLeaseAttempt{.acquired = leaseOwnedBy(acquiredEntry.value, owner), .entry = acquiredEntry};
    }

    // `acquireCapacityLease(kv, owner, nowMs).catch(() => ({ acquired: false, entry: { key, value: null, versionstamp: null } }))`
    rt::Task<CapacityLeaseAttempt> acquireCapacityLeaseOrNone(std::shared_ptr<rt::Kv> kv, std::string owner, double nowMs) {
      std::optional<CapacityLeaseAttempt> attempt;
      try {
        rt::Promise<CapacityLeaseAttempt> acquiring(acquireCapacityLease(kv, owner, nowMs));
        attempt = co_await acquiring.wait();
      } catch (...) {
        attempt.reset();
      }
      if (!attempt) co_return unacquiredLease();
      co_return *attempt;
    }

    rt::Task<void> releaseCapacityLease(std::shared_ptr<rt::Kv> kv, std::string owner) {
      try {
        auto reading = kv->get(PROVIDER_CAPACITY_LEASE_KEY);
        const rt::KvEntryMaybe<> entry = co_await std::move(reading);
        if (!leaseOwnedBy(entry.value, owner)) co_return;
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry).delete_(PROVIDER_CAPACITY_LEASE_KEY);
        auto committing = operation->commit();
        co_await std::move(committing);
      } catch (...) {
        // A short-lived lease can expire without affecting the redacted snapshot.
      }
    }

    std::optional<capacity_rollups::ProviderCapacityRollupWindowInput> rollupWindow(const std::optional<capacity_contract::ProviderCapacityWindow>& window) {
      if (!window) return std::nullopt;
      return capacity_rollups::ProviderCapacityRollupWindowInput{
        .limit_window_seconds = window->limit_window_seconds, .used_percent = window->used_percent, .reset_at_ms = window->reset_at_ms};
    }

    capacity_rollups::ProviderCapacityRollupInput capacityRollupInputForHistoryPoint(const ProviderCapacityHistoryPoint& history) {
      const auto slotInput = [&history](double slot) -> capacity_rollups::ProviderCapacityRollupSlotInput {
        const ProviderCapacityCodexSource* source = nullptr;
        if (std::get<0>(history.sources).slot == slot) source = &std::get<0>(history.sources);
        else if (std::get<1>(history.sources).slot == slot) source = &std::get<1>(history.sources);
        return capacity_rollups::ProviderCapacityRollupSlotInput{
          .slot = slot,
          .state = source ? source->state : std::string("unavailable"),
          .primary = source ? rollupWindow(source->windows.primary) : std::nullopt,
          .secondary = source ? rollupWindow(source->windows.secondary) : std::nullopt,
        };
      };
      return capacity_rollups::ProviderCapacityRollupInput{
        .bucket_start_at_ms = capacity_rollups::providerCapacityRollupBucketStartAtMs(history.bucket_start_at_ms),
        .sampled_at_ms = history.sampled_at_ms,
        .slots = {slotInput(1), slotInput(2)},
      };
    }

    rt::Task<bool> persistCapacitySnapshot(std::shared_ptr<rt::Kv> kv, rt::KvEntryMaybe<> leaseEntry, ProviderCapacitySnapshot snapshot) {
      const ProviderCapacityHistoryPoint history = capacity_history::historyPointForSnapshot(snapshot);
      const capacity_rollups::ProviderCapacityRollupInput rollupInput = capacityRollupInputForHistoryPoint(history);
      const rt::KvKey rollupKey = capacity_rollups::providerCapacityRollupKey(rollupInput.bucket_start_at_ms);
      const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
      std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
      reads.push_back(rt::Promise<rt::KvEntryMaybe<>>(kv->get(PROVIDER_CAPACITY_SNAPSHOT_KEY, strong)));
      reads.push_back(rt::Promise<rt::KvEntryMaybe<>>(kv->get(capacity_history::providerCapacityHistoryKey(history.bucket_start_at_ms), strong)));
      for (const double slot : {1.0, 2.0}) reads.push_back(rt::Promise<rt::KvEntryMaybe<>>(kv->get(capacity_parse::providerCapacityLastAvailableKey(slot), strong)));
      reads.push_back(rt::Promise<rt::KvEntryMaybe<>>(kv->get(rollupKey, strong)));
      auto settling = rt::allSettled(std::move(reads));
      const std::vector<rt::SettledResult<rt::KvEntryMaybe<>>> comparisonReads = co_await settling.wait();
      std::vector<rt::KvEntryMaybe<>> storedEntries;
      for (std::size_t index = 0; index < 4; ++index) {
        if (comparisonReads[index].status == "rejected") co_return false;
        storedEntries.push_back(*comparisonReads[index].value);
      }
      // The rollup is long-run research telemetry: a failed rollup read must not
      // block the operational snapshot and history write.
      const rt::SettledResult<rt::KvEntryMaybe<>>& rollupRead = comparisonReads[4];
      // comparisonReads is the five-element array literal above, so index 4 exists.
      const std::optional<rt::KvEntryMaybe<>> rollupEntry = rollupRead.status == "fulfilled" ? rollupRead.value : std::nullopt;
      const std::optional<ProviderCapacitySnapshot> previousSnapshot = capacity_parse::readStoredSnapshot(storedEntries[0].value);
      const std::optional<ProviderCapacityHistoryPoint> previousHistory = capacity_parse::readStoredHistoryPoint(storedEntries[1].value);
      std::vector<capacity_contract::StoredRateLimitObservation> lastAvailableObservations;
      for (std::size_t index = 2; index < 4; ++index) {
        if (std::optional<capacity_contract::StoredRateLimitObservation> observation = capacity_history::readStoredRateLimitObservation(storedEntries[index].value))
          lastAvailableObservations.push_back(std::move(*observation));
      }
      const std::vector<double> recoverySlots = capacity_history::recoverySlotsForSnapshot(snapshot, previousSnapshot, lastAvailableObservations);
      rt::Promise<std::vector<capacity_contract::StoredRateLimitObservation>> recovering(capacity_history::recoverRateLimitObservations(kv, snapshot, recoverySlots));
      const std::vector<capacity_contract::StoredRateLimitObservation> recoveredObservations = co_await recovering.wait();
      std::vector<capacity_contract::StoredRateLimitObservation> observations = lastAvailableObservations;
      observations.insert(observations.end(), recoveredObservations.begin(), recoveredObservations.end());
      const std::vector<ProviderCapacityRateLimitResetEvent> rateLimitResetEvents = capacity_history::observedRateLimitResetEvents(previousSnapshot, snapshot, observations);
      const bool preserveTransition = !rateLimitResetEvents.empty() || capacity_history::codexResetTransitionObserved(previousHistory, history) ||
                                      capacity_history::codexAvailabilityTransitionObserved(previousHistory, history);
      std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      const rt::KvSetOptions snapshotRetention{.expireIn = PROVIDER_CAPACITY_SNAPSHOT_RETENTION_MS};
      const rt::KvSetOptions eventRetention{.expireIn = capacity_events::PROVIDER_CAPACITY_RESET_EVENT_RETENTION_MS};
      const rt::KvSetOptions historyRetention{.expireIn = PROVIDER_CAPACITY_HISTORY_RETENTION_MS};
      operation->check(leaseEntry).set(PROVIDER_CAPACITY_SNAPSHOT_KEY, rt::KvValue(json::value_from(snapshot)), snapshotRetention);
      for (const ProviderCapacityRateLimitResetEvent& event : rateLimitResetEvents) {
        operation->set(capacity_events::providerCapacityRateLimitResetEventKey(event.event_id), rt::KvValue(json::value_from(event)), eventRetention);
      }
      for (const capacity_history::LastAvailableCapacityEntry& entry : capacity_history::lastAvailableCapacityEntries(snapshot)) {
        operation->set(capacity_parse::providerCapacityLastAvailableKey(entry.slot), rt::KvValue(json::value_from(entry.value)), eventRetention);
      }
      if (rollupEntry) {
        const std::optional<capacity_rollups::ProviderCapacityRollupPoint> existingRollup = capacity_rollups::providerCapacityRollupPointOf(rollupEntry->value);
        operation->check(*rollupEntry).set(rollupKey, rt::KvValue(json::value_from(capacity_rollups::mergeProviderCapacityRollup(existingRollup, rollupInput))));
      }
      if (preserveTransition && previousHistory) {
        operation->set(capacity_history::providerCapacityHistoryTransitionKey(history.bucket_start_at_ms, previousHistory->sampled_at_ms),
                       rt::KvValue(json::value_from(*previousHistory)), historyRetention);
      }
      operation->set(capacity_history::providerCapacityHistoryKey(history.bucket_start_at_ms), rt::KvValue(json::value_from(history)), historyRetention)
        .delete_(PROVIDER_CAPACITY_LEASE_KEY);
      auto committing = operation->commit();
      const rt::KvCommitResult committed = co_await std::move(committing);
      co_return committed.ok;
    }

    // `persistCapacitySnapshot(kv, lease.entry, snapshot).catch(() => false)`
    rt::Task<bool> persistCapacitySnapshotOrFalse(std::shared_ptr<rt::Kv> kv, rt::KvEntryMaybe<> leaseEntry, ProviderCapacitySnapshot snapshot) {
      bool persisted = false;
      try {
        rt::Promise<bool> persisting(persistCapacitySnapshot(kv, std::move(leaseEntry), std::move(snapshot)));
        persisted = co_await persisting.wait();
      } catch (...) {
        persisted = false;
      }
      co_return persisted;
    }

    rt::Task<std::optional<ProviderCapacitySnapshot>> waitForCapacitySnapshot(std::shared_ptr<rt::Kv> kv, std::optional<double> previousSnapshotAtMs) {
      const double deadline = rt::dateNow() + PROVIDER_CAPACITY_COLD_WAIT_MS;
      while (rt::dateNow() < deadline) {
        co_await rt::sleep(50);
        // Read the lease before the snapshot. The durable commit that writes a new
        // snapshot deletes the lease in the same atomic operation, so once a
        // released lease is observed the following snapshot read already includes
        // that commit. Reading both concurrently could pair the old snapshot with
        // the released lease and return stale data while fresh data is durable.
        std::optional<rt::KvEntryMaybe<>> lease;
        try {
          auto reading = kv->get(PROVIDER_CAPACITY_LEASE_KEY);
          lease = co_await std::move(reading);
        } catch (...) {
          lease.reset();
        }
        rt::Promise<std::optional<ProviderCapacitySnapshot>> snapshotRead(capacity_parse::readCapacitySnapshot(kv));
        std::optional<ProviderCapacitySnapshot> snapshot = co_await snapshotRead.wait();
        if (snapshot && (!previousSnapshotAtMs || snapshot->snapshot_at_ms != *previousSnapshotAtMs)) co_return snapshot;
        if (!lease || !lease->value.truthy()) co_return snapshot;
      }
      co_return std::nullopt;
    }

    // `{ kv, now: () => nowMs }` of the event listings.
    capacity_events::ListProviderCapacityEventsOptions eventOptions(const std::shared_ptr<rt::Kv>& kv, double nowMs) {
      capacity_events::ListProviderCapacityEventsOptions options;
      options.kv = kv;
      options.now = [nowMs] { return nowMs; };
      return options;
    }

    // The five reads `Promise.all([readCapacitySnapshot, readCapacityHistory, list reset, list rate-limit reset, list downtime])`.
    struct CapacityReads {
      std::optional<ProviderCapacitySnapshot> snapshot;
      std::vector<ProviderCapacityHistoryPoint> history;
      std::vector<ProviderCapacityResetEvent> resetEvents;
      std::vector<ProviderCapacityRateLimitResetEvent> rateLimitResetEvents;
      std::vector<ProviderCapacityDowntimeEvent> downtimeEvents;
    };

    rt::Task<CapacityReads> readCapacityState(std::shared_ptr<rt::Kv> kv, double nowMs) {
      const rt::Promise<std::optional<ProviderCapacitySnapshot>> snapshot(capacity_parse::readCapacitySnapshot(kv));
      const rt::Promise<std::vector<ProviderCapacityHistoryPoint>> history(capacity_history::readCapacityHistory(kv, nowMs));
      const rt::Promise<std::vector<ProviderCapacityResetEvent>> resetEvents(capacity_events::listProviderCapacityResetEvents(eventOptions(kv, nowMs)));
      const rt::Promise<std::vector<ProviderCapacityRateLimitResetEvent>> rateLimitResetEvents(
        capacity_events::listProviderCapacityRateLimitResetEvents(eventOptions(kv, nowMs)));
      const rt::Promise<std::vector<ProviderCapacityDowntimeEvent>> downtimeEvents(capacity_events::listProviderCapacityDowntimeEvents(eventOptions(kv, nowMs)));
      auto everything = rt::all(snapshot, history, resetEvents, rateLimitResetEvents, downtimeEvents);
      auto results = co_await everything.wait();
      CapacityReads reads{
        .snapshot = std::move(std::get<0>(results)),
        .history = std::move(std::get<1>(results)),
        .resetEvents = std::move(std::get<2>(results)),
        .rateLimitResetEvents = std::move(std::get<3>(results)),
        .downtimeEvents = std::move(std::get<4>(results)),
      };
      co_return reads;
    }

    // The sequential re-reads after a refresh or a coalesced wait: each `.catch(() => before)`.
    rt::Task<CapacityReads> rereadCapacityState(std::shared_ptr<rt::Kv> kv, double nowMs, CapacityReads before) {
      CapacityReads reads;
      try {
        rt::Promise<std::vector<ProviderCapacityHistoryPoint>> reading(capacity_history::readCapacityHistory(kv, nowMs));
        reads.history = co_await reading.wait();
      } catch (...) {
        reads.history = before.history;
      }
      try {
        rt::Promise<std::vector<ProviderCapacityResetEvent>> reading(capacity_events::listProviderCapacityResetEvents(eventOptions(kv, nowMs)));
        reads.resetEvents = co_await reading.wait();
      } catch (...) {
        reads.resetEvents = before.resetEvents;
      }
      try {
        rt::Promise<std::vector<ProviderCapacityRateLimitResetEvent>> reading(capacity_events::listProviderCapacityRateLimitResetEvents(eventOptions(kv, nowMs)));
        reads.rateLimitResetEvents = co_await reading.wait();
      } catch (...) {
        reads.rateLimitResetEvents = before.rateLimitResetEvents;
      }
      try {
        rt::Promise<std::vector<ProviderCapacityDowntimeEvent>> reading(capacity_events::listProviderCapacityDowntimeEvents(eventOptions(kv, nowMs)));
        reads.downtimeEvents = co_await reading.wait();
      } catch (...) {
        reads.downtimeEvents = before.downtimeEvents;
      }
      co_return reads;
    }

    rt::Task<std::vector<ProviderCapacityRateLimitResetEvent>> mergedRateLimitEvents(std::shared_ptr<rt::Kv> kv, const CapacityReads& reads) {
      rt::Promise<std::vector<ProviderCapacityRateLimitResetEvent>> merging(
        capacity_history::mergeHistoricalRateLimitResetEvents(kv, reads.history, reads.rateLimitResetEvents));
      std::vector<ProviderCapacityRateLimitResetEvent> merged = co_await merging.wait();
      co_return merged;
    }

    std::string leaseOwner(const ProviderCapacitySnapshotOptions& options) { return options.createLeaseOwner ? options.createLeaseOwner() : rt::crypto::randomUUID(); }

    rt::Task<ProviderCapacityView> refreshProviderCapacityInternal(ProviderCapacitySnapshotOptions options, bool forceMeteredRefresh) {
      const double nowMs = capacity_parse::safeNow(nowOf(options));
      rt::Promise<std::shared_ptr<rt::Kv>> resolving(optionsKv(options.kv));
      const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
      if (!kv) {
        rt::Promise<ProviderCapacitySnapshot> capturing(captureProviderCapacitySnapshot(options, nowMs, nullptr, forceMeteredRefresh));
        const ProviderCapacitySnapshot snapshot = co_await capturing.wait();
        std::vector<ProviderCapacityHistoryPoint> history{capacity_history::historyPointForSnapshot(snapshot)};
        co_return toCapacityView(snapshot, "live", std::move(history), {}, {}, {}, nowMs);
      }

      rt::Promise<CapacityReads> readingBefore(readCapacityState(kv, nowMs));
      const CapacityReads before = co_await readingBefore.wait();
      const std::string owner = leaseOwner(options);
      rt::Promise<CapacityLeaseAttempt> acquiring(acquireCapacityLeaseOrNone(kv, owner, nowMs));
      const CapacityLeaseAttempt lease = co_await acquiring.wait();
      if (!lease.acquired) {
        std::optional<ProviderCapacitySnapshot> coalesced;
        try {
          const std::optional<double> previousSnapshotAtMs = before.snapshot ? std::optional<double>(before.snapshot->snapshot_at_ms) : std::nullopt;
          rt::Promise<std::optional<ProviderCapacitySnapshot>> waiting(waitForCapacitySnapshot(kv, previousSnapshotAtMs));
          coalesced = co_await waiting.wait();
        } catch (...) {
          coalesced.reset();
        }
        const std::optional<ProviderCapacitySnapshot> snapshot = coalesced ? coalesced : before.snapshot;
        rt::Promise<CapacityReads> rereading(rereadCapacityState(kv, nowMs, before));
        const CapacityReads after = co_await rereading.wait();
        rt::Promise<std::vector<ProviderCapacityRateLimitResetEvent>> merging(mergedRateLimitEvents(kv, after));
        std::vector<ProviderCapacityRateLimitResetEvent> merged = co_await merging.wait();
        if (snapshot) co_return toCapacityView(*snapshot, "persisted", after.history, after.resetEvents, std::move(merged), after.downtimeEvents, nowMs);
        co_return unavailableView(nowMs, after.history, after.resetEvents, std::move(merged), after.downtimeEvents);
      }

      std::optional<ProviderCapacityView> view;
      rt::Reason failure;
      try {
        rt::Promise<ProviderCapacitySnapshot> capturing(captureProviderCapacitySnapshot(options, nowMs, kv, forceMeteredRefresh));
        const ProviderCapacitySnapshot snapshot = co_await capturing.wait();
        rt::Promise<bool> persisting(persistCapacitySnapshotOrFalse(kv, lease.entry, snapshot));
        const bool persisted = co_await persisting.wait();
        rt::Promise<CapacityReads> rereading(rereadCapacityState(kv, nowMs, before));
        const CapacityReads after = co_await rereading.wait();
        rt::Promise<std::vector<ProviderCapacityRateLimitResetEvent>> merging(mergedRateLimitEvents(kv, after));
        std::vector<ProviderCapacityRateLimitResetEvent> merged = co_await merging.wait();
        std::vector<ProviderCapacityHistoryPoint> history =
          persisted ? after.history : capacity_history::mergeHistoryPoints(after.history, capacity_history::historyPointForSnapshot(snapshot));
        view = toCapacityView(snapshot, "live", std::move(history), after.resetEvents, std::move(merged), after.downtimeEvents, nowMs);
      } catch (...) {
        failure = std::current_exception();
      }
      rt::Promise<void> releasing(releaseCapacityLease(kv, owner));
      co_await releasing.wait();
      if (failure) std::rethrow_exception(failure);
      co_return std::move(*view);
    }

    // `options.kv === undefined ? await getKv() : options.kv` with the view helpers of handleProviderCapacity.
    struct ClaudeViewContext {
      std::shared_ptr<rt::Kv> kv;
      std::function<double()> now;
      rt::Promise<cache::prompt_analytics_core::PromptCacheAnalyticsView> promptCache;
      paid_fallback::ledger_stats::PaidFallbackLedgerGrowthView ledgerGrowth;
      std::vector<capacity_analytics::RemoteCapacityObservation> remoteHistory;
    };

    ProviderCapacityView withAnalyticsHistory(ProviderCapacityView view, const std::vector<capacity_analytics::RemoteCapacityObservation>& remoteHistory) {
      if (remoteHistory.empty()) return view;
      view.history = capacity_analytics::mergeCapacityHistory(view.history, remoteHistory);
      return view;
    }

    // `{ ...view, ...roster, claude_history: history, prompt_cache: await promptCache, ledger_growth: ledgerGrowth }`
    rt::Task<json::value> withClaude(ProviderCapacityView view, std::shared_ptr<ClaudeViewContext> context) {
      const double observedNow = capacity_parse::safeNow(context->now);
      const rt::Promise<ClaudeCapacityRoster> rosterLoad =
        rt::Promise<ClaudeCapacityRoster>(claudeCapacityRoster(context->kv, observedNow)).catch_([observedNow](const rt::Reason&) {
          return ClaudeCapacityRoster{.claude = unavailableClaudeCapacitySource(observedNow), .claude_accounts = {}};
        });
      const rt::Promise<capacity_contract::ClaudeCapacityHistoryView> historyLoad(claude_capacity_history::readClaudeCapacityHistory(context->kv, observedNow));
      auto both = rt::all(rosterLoad, historyLoad);
      std::tuple<ClaudeCapacityRoster, capacity_contract::ClaudeCapacityHistoryView> loaded = co_await both.wait();
      view.claude = std::move(std::get<0>(loaded).claude);
      view.claude_accounts = std::move(std::get<0>(loaded).claude_accounts);
      view.claude_history = std::move(std::get<1>(loaded));
      const cache::prompt_analytics_core::PromptCacheAnalyticsView promptCache = co_await context->promptCache.wait();
      json::value body = json::value_from(view);
      body.as_object()["prompt_cache"] = json::value_from(promptCache);
      body.as_object()["ledger_growth"] = paid_fallback::ledger_stats::toJson(context->ledgerGrowth);
      co_return body;
    }

    rt::Headers noStore() {
      rt::Headers headers;
      headers.append("Cache-Control", "no-store");
      return headers;
    }

    bool capacityViewIsFresh(const ProviderCapacityView& view, double nowMs) {
      return view.cache_state != "unavailable" && nowMs - view.snapshot_at_ms < PROVIDER_CAPACITY_READ_FRESH_MS;
    }

    double providerCapacityResearchWindowDays(const std::optional<std::string>& raw) {
      const double parsed = rt::parseInt(raw.value_or(""), 10);
      if (!rt::Number::isInteger(parsed) || parsed < 1) return PROVIDER_CAPACITY_RESEARCH_DEFAULT_WINDOW_DAYS;
      return rt::Math::min(parsed, PROVIDER_CAPACITY_RESEARCH_MAX_WINDOW_DAYS);
    }
  }

  capacity_contract::ProviderCapacityClaudeSource unavailableClaudeCapacitySource(double snapshotAtMs) {
    return ProviderCapacityClaudeSource{
      .usage_snapshot = std::nullopt,
      .usage_stale = std::nullopt,
      .source = "claude_code",
      .label = "Claude subscription",
      .account_id = rt::undefined,
      .account_cohort_id = rt::undefined,
      .state = "unavailable",
      .source_observed_at_ms = std::nullopt,
      .snapshot_at_ms = snapshotAtMs,
      .failure_kind = "not_configured",
      .failure_status = std::nullopt,
      .credential_kind = std::nullopt,
      .workspace_id = std::nullopt,
      .windows = {.five_hour = std::nullopt, .seven_day = std::nullopt},
      .standard = {},
      .usage = std::nullopt,
      .blocked_until_ms = std::nullopt,
      .cooldown_until_ms = std::nullopt,
      .last_error = std::nullopt,
    };
  }

  rt::Task<capacity_contract::ProviderCapacityClaudeSource> claudeCapacitySource(std::shared_ptr<rt::Kv> kv, double snapshotAtMs) {
    std::optional<ClaudeCodeAccountBinding> binding;
    try {
      rt::Promise<std::optional<ClaudeCodeAccountBinding>> resolving(claude_code_accounts::resolveClaudeCodeAccount(kv));
      binding = co_await resolving.wait();
    } catch (...) {
      binding.reset();
    }
    rt::Promise<ProviderCapacityClaudeSource> building(capacitySourceForClaudeAccount(kv, snapshotAtMs, std::move(binding)));
    ProviderCapacityClaudeSource source = co_await building.wait();
    co_return source;
  }

  rt::Task<capacity_contract::ProviderCapacityClaudeSource> claudeCapacitySource(std::shared_ptr<rt::Kv> kv, double snapshotAtMs, claude_code::ClaudeCodeEnvLookup env) {
    std::optional<ClaudeCodeAccountBinding> binding;
    try {
      rt::Promise<std::optional<ClaudeCodeAccountBinding>> resolving(claude_code_accounts::resolveClaudeCodeAccount(kv, std::move(env)));
      binding = co_await resolving.wait();
    } catch (...) {
      binding.reset();
    }
    rt::Promise<ProviderCapacityClaudeSource> building(capacitySourceForClaudeAccount(kv, snapshotAtMs, std::move(binding)));
    ProviderCapacityClaudeSource source = co_await building.wait();
    co_return source;
  }

  rt::Task<capacity_contract::ProviderCapacityView> getPersistedProviderCapacityView() {
    ProviderCapacitySnapshotOptions options;
    auto reading = getPersistedProviderCapacityView(std::move(options));
    ProviderCapacityView view = co_await std::move(reading);
    co_return view;
  }

  rt::Task<capacity_contract::ProviderCapacityView> getPersistedProviderCapacityView(capacity_contract::ProviderCapacitySnapshotOptions options) {
    const double nowMs = capacity_parse::safeNow(nowOf(options));
    rt::Promise<std::shared_ptr<rt::Kv>> resolving(optionsKv(options.kv));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) co_return emptyUnavailableView(nowMs);
    rt::Promise<CapacityReads> reading(readCapacityState(kv, nowMs));
    const CapacityReads reads = co_await reading.wait();
    rt::Promise<std::vector<ProviderCapacityRateLimitResetEvent>> merging(mergedRateLimitEvents(kv, reads));
    std::vector<ProviderCapacityRateLimitResetEvent> merged = co_await merging.wait();
    if (reads.snapshot) co_return toCapacityView(*reads.snapshot, "persisted", reads.history, reads.resetEvents, std::move(merged), reads.downtimeEvents, nowMs);
    co_return unavailableView(nowMs, reads.history, reads.resetEvents, std::move(merged), reads.downtimeEvents);
  }

  rt::Task<capacity_contract::ProviderCapacityView> refreshProviderCapacity() {
    ProviderCapacitySnapshotOptions options;
    auto refreshing = refreshProviderCapacityInternal(std::move(options), true);
    ProviderCapacityView view = co_await std::move(refreshing);
    co_return view;
  }

  rt::Task<capacity_contract::ProviderCapacityView> refreshProviderCapacity(capacity_contract::ProviderCapacitySnapshotOptions options) {
    auto refreshing = refreshProviderCapacityInternal(std::move(options), true);
    ProviderCapacityView view = co_await std::move(refreshing);
    co_return view;
  }

  rt::Task<void> sampleProviderCapacityOnEvent() {
    ProviderCapacitySnapshotOptions options;
    auto sampling = sampleProviderCapacityOnEvent(std::move(options));
    co_await std::move(sampling);
  }

  rt::Task<void> sampleProviderCapacityOnEvent(capacity_contract::ProviderCapacitySnapshotOptions options) {
    const double nowMs = capacity_parse::safeNow(nowOf(options));
    rt::Promise<std::shared_ptr<rt::Kv>> resolving(optionsKv(options.kv));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    // A scheduled sample without durable storage would only create provider
    // traffic while leaving routing and history state stale.
    if (!kv) co_return;

    const std::string owner = leaseOwner(options);
    rt::Promise<CapacityLeaseAttempt> acquiring(acquireCapacityLeaseOrNone(kv, owner, nowMs));
    const CapacityLeaseAttempt lease = co_await acquiring.wait();
    // The event caller has no view to return. A competing live refresh owns the
    // only probe, so avoid both a duplicate probe and the old coalesced-view
    // polling loop.
    if (!lease.acquired) co_return;

    bool persisted = false;
    rt::Reason failure;
    try {
      rt::Promise<ProviderCapacitySnapshot> capturing(captureProviderCapacitySnapshot(options, nowMs, kv, true));
      const ProviderCapacitySnapshot snapshot = co_await capturing.wait();
      rt::Promise<bool> persisting(persistCapacitySnapshotOrFalse(kv, lease.entry, snapshot));
      persisted = co_await persisting.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    // A successful atomic persist deletes the checked lease. Only failed or
    // interrupted persistence needs the conservative owner-checked release.
    if (!persisted) {
      rt::Promise<void> releasing(releaseCapacityLease(kv, owner));
      co_await releasing.wait();
    }
    if (failure) std::rethrow_exception(failure);
  }

  rt::Task<rt::Response> handleProviderCapacityRollups() {
    rt::Request request("https://ai.ubq.fi/admin/providers/capacity/rollups");
    ProviderCapacitySnapshotOptions options;
    auto handling = handleProviderCapacityRollups(std::move(request), std::move(options));
    rt::Response response = co_await std::move(handling);
    co_return response;
  }

  rt::Task<rt::Response> handleProviderCapacityRollups(rt::Request request) {
    ProviderCapacitySnapshotOptions options;
    auto handling = handleProviderCapacityRollups(std::move(request), std::move(options));
    rt::Response response = co_await std::move(handling);
    co_return response;
  }

  rt::Task<rt::Response> handleProviderCapacityRollups(rt::Request request, capacity_contract::ProviderCapacitySnapshotOptions options) {
    const double nowMs = capacity_parse::safeNow(nowOf(options));
    rt::Promise<std::shared_ptr<rt::Kv>> resolving(optionsKv(options.kv));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    const double windowDays = providerCapacityResearchWindowDays(rt::URL(request.url()).searchParams().get("window_days"));
    const double windowMs = windowDays * 24 * 60 * 60'000;
    const double sinceMs = rt::Math::max(0.0, nowMs - windowMs);
    std::optional<std::vector<capacity_rollups::ProviderCapacityRollupPoint>> rollups;
    if (kv) {
      try {
        rt::Promise<std::vector<capacity_rollups::ProviderCapacityRollupPoint>> listing(
          capacity_rollups::listProviderCapacityRollups(kv, capacity_rollups::ListProviderCapacityRollupsOptions{.sinceMs = sinceMs, .nowMs = nowMs}));
        rollups = co_await listing.wait();
      } catch (...) {
        rollups.reset();
      }
    }
    rt::Promise<claude_capacity_rollups::ListClaudeCapacityRollupsResult> claudeListing(claude_capacity_rollups::listClaudeCapacityRollups(kv, sinceMs, nowMs));
    const claude_capacity_rollups::ListClaudeCapacityRollupsResult claudeRollups = co_await claudeListing.wait();
    rt::Promise<claude_capacity_rollups::ListClaudeUsageRollupsResult> usageListing(claude_capacity_rollups::listClaudeUsageRollups(kv, sinceMs, nowMs));
    const claude_capacity_rollups::ListClaudeUsageRollupsResult claudeUsageRollups = co_await usageListing.wait();
    json::object retention;
    retention["rollup_bucket_ms"] = rt::makeNumber(PROVIDER_CAPACITY_ROLLUP_BUCKET_MS);
    retention["rollup_window_ms"] = rt::makeNumber(windowMs);
    json::object body;
    body["snapshot_at_ms"] = rt::makeNumber(nowMs);
    body["window_days"] = rt::makeNumber(windowDays);
    body["retention"] = std::move(retention);
    // A failed scan must not masquerade as an empty curve.
    body["rollup_scan"] = rollups ? "ok" : "unavailable";
    body["rollups"] = rollups ? json::value_from(*rollups) : json::value(json::array());
    body["claude_rollup_scan"] = json::string(claudeRollups.scan);
    body["claude_rollup_truncated"] = claudeRollups.truncated;
    body["claude_rollups"] = json::value_from(claudeRollups.points);
    body["claude_usage_rollups"] = json::value_from(claudeUsageRollups);
    co_return http::json(200, body, noStore());
  }

  rt::Task<rt::Response> handleProviderCapacity() {
    rt::Request request("https://ai.ubq.fi/admin/providers/capacity");
    ProviderCapacitySnapshotOptions options;
    auto handling = handleProviderCapacity(std::move(request), std::move(options));
    rt::Response response = co_await std::move(handling);
    co_return response;
  }

  rt::Task<rt::Response> handleProviderCapacity(rt::Request request) {
    ProviderCapacitySnapshotOptions options;
    auto handling = handleProviderCapacity(std::move(request), std::move(options));
    rt::Response response = co_await std::move(handling);
    co_return response;
  }

  rt::Task<rt::Response> handleProviderCapacity(rt::Request request, capacity_contract::ProviderCapacitySnapshotOptions options) {
    cache::prompt_analytics_core::PromptCacheAnalyticsReadOptions promptOptions;
    promptOptions.kv = options.kv;
    promptOptions.now = options.now;
    auto context = std::make_shared<ClaudeViewContext>();
    context->promptCache = rt::Promise<cache::prompt_analytics_core::PromptCacheAnalyticsView>(cache::prompt_analytics_read::readPromptCacheAnalytics(std::move(promptOptions)));
    // The admin Provider analytics card is the current operator surface for the
    // paid-fallback ledger growth estimate. This is a bounded daily-counter scan
    // (retention rows only) and never touches the capacity rollups.
    const double nowMs = capacity_parse::safeNow(nowOf(options));
    rt::Promise<std::shared_ptr<rt::Kv>> resolving(optionsKv(options.kv));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    context->kv = kv;
    context->now = nowOf(options);
    rt::Promise<paid_fallback::ledger_stats::PaidFallbackLedgerGrowthView> growth(paid_fallback::ledger_stats::readPaidFallbackLedgerGrowth(
      kv, paid_fallback::ledger_stats::PaidFallbackLedgerGrowthOptions{.nowMs = nowMs, .retentionMs = paid_fallback::ledger_state::PAID_FALLBACK_REQUEST_LOG_RETENTION_MS}));
    context->ledgerGrowth = co_await growth.wait();
    if (kv) {
      try {
        rt::Promise<std::vector<capacity_analytics::RemoteCapacityObservation>> remote(capacity_analytics::readRemoteCapacityObservations(kv, nowMs));
        context->remoteHistory = co_await remote.wait();
      } catch (...) {
        context->remoteHistory.clear();
      }
    }

    std::optional<json::value> body;
    try {
      const bool live = rt::URL(request.url()).searchParams().get("refresh") == std::optional<std::string>("live");
      // `?refresh=live` keeps forcing an operator probe. A normal read serves the
      // persisted snapshot while it is inside the read freshness window and
      // otherwise revalidates through the same lease-guarded refresh, so the
      // thirty-second admin poll sees current quota without stacking duplicate
      // upstream calls: concurrent stale reads coalesce on the shared lease and
      // its bounded cold wait. A refresh that throws keeps the last known
      // snapshot; a refresh that reaches upstream but fails leaves the affected
      // source unavailable rather than fabricating a percentage.
      std::optional<ProviderCapacityView> cached;
      try {
        rt::Promise<ProviderCapacityView> reading(getPersistedProviderCapacityView(options));
        cached = co_await reading.wait();
      } catch (...) {
        cached.reset();
      }
      if (!live && cached && capacityViewIsFresh(*cached, capacity_parse::safeNow(nowOf(options)))) {
        rt::Promise<json::value> building(withClaude(withAnalyticsHistory(*cached, context->remoteHistory), context));
        body = co_await building.wait();
      } else {
        std::optional<ProviderCapacityView> refreshed;
        try {
          rt::Promise<ProviderCapacityView> refreshing(refreshProviderCapacityInternal(options, live));
          refreshed = co_await refreshing.wait();
        } catch (...) {
          refreshed.reset();
        }
        ProviderCapacityView view = refreshed ? std::move(*refreshed) : cached ? std::move(*cached) : emptyUnavailableView(rt::dateNow());
        rt::Promise<json::value> building(withClaude(withAnalyticsHistory(std::move(view), context->remoteHistory), context));
        body = co_await building.wait();
      }
    } catch (...) {
      body.reset();
    }
    if (!body) {
      rt::Promise<json::value> building(withClaude(withAnalyticsHistory(emptyUnavailableView(rt::dateNow()), context->remoteHistory), context));
      body = co_await building.wait();
    }
    co_return http::json(200, *body, noStore());
  }
}
