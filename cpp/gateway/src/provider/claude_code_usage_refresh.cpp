#include "uos/provider/claude_code_usage_refresh.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
namespace uos::provider::claude_code_usage_refresh {
  namespace {
    constexpr double USAGE_LEASE_MS = 10'000;
    std::optional<double> usageRetryDeadline(const claude_code_usage::ClaudeCodeUsageResult& result) {
      const auto* failure = std::get_if<claude_code_usage::ClaudeCodeUsageFailure>(&result);
      if (!failure || failure->kind != "rate_limited" || !failure->retry_after_ms) return std::nullopt;
      const double atMs = failure->observed_at_ms + *failure->retry_after_ms;
      return rt::Number::isSafeInteger(atMs) && atMs > failure->observed_at_ms ? std::optional<double>(atMs) : std::nullopt;
    }
  }
  bool claudeCodeUsageIsFresh(const std::optional<claude_code_quota::ClaudeCodeQuotaState>& quota, double nowMs) {
    if (!quota || !quota->usage) return false;
    const auto& usage = *quota->usage;
    return nowMs >= usage.attempted_at_ms && (nowMs - usage.attempted_at_ms < CLAUDE_CODE_USAGE_FRESH_MS || (usage.state == "rate_limited" && usage.retry_at_ms.value_or(0) > nowMs));
  }
  rt::Task<ClaudeCodeUsageRefresh> refreshClaudeCodeUsage(std::shared_ptr<rt::Kv> kv, claude_code_accounts::ClaudeCodeAccountBinding binding) {
    return refreshClaudeCodeUsage(std::move(kv), std::move(binding), {});
  }
  rt::Task<ClaudeCodeUsageRefresh> refreshClaudeCodeUsage(std::shared_ptr<rt::Kv> kv, claude_code_accounts::ClaudeCodeAccountBinding binding, claude_code_usage::FetchClaudeCodeUsageOptions options) {
    const auto clock = options.now ? options.now : std::function<double()>(rt::dateNow);
    const double nowMs = rt::Math::trunc(clock());
    rt::Promise accountRead(claude_code_accounts::currentClaudeCodeAccountEntry(kv, binding));
    const auto account = co_await accountRead.wait();
    if (!account) co_return ClaudeCodeUsageRefreshChanged{};
    rt::Promise quotaRead(claude_code_quota::loadClaudeCodeQuotaState(kv, binding));
    const auto quota = co_await quotaRead.wait();
    if (quota && quota->usage && claudeCodeUsageIsFresh(quota, nowMs)) co_return ClaudeCodeUsageRefreshFresh{*quota->usage};
    const rt::KvKey key{"uos_ai", "claude_code_usage", "v1", "lease", binding.account_id, binding.account_cohort_id};
    const rt::KvGetOptions readOptions{.consistency = rt::KvConsistencyLevel::strong};
    auto reading = kv->get(key, readOptions);
    const auto entry = co_await std::move(reading);
    const auto lease = rt::storedJsonView(entry.value);
    if (entry.value.truthy() && rt::toNumber(rt::get(lease, "until_ms")) > nowMs) co_return ClaudeCodeUsageRefreshBusy{};
    const std::string owner = rt::crypto::randomUUID();
    const rt::KvValue nextLease(json::object{{"owner", owner}, {"until_ms", rt::makeNumber(nowMs + USAGE_LEASE_MS)}});
    auto operation = kv->atomic(); operation->check(*account).check(entry).set(key, nextLease, {.expireIn = USAGE_LEASE_MS * 2});
    const auto claimed = co_await operation->commit();
    if (!claimed.ok) co_return ClaudeCodeUsageRefreshBusy{};
    auto work = [kv, binding, options, clock]() -> rt::Task<ClaudeCodeUsageRefresh> {
      rt::Promise latestRead(claude_code_quota::loadClaudeCodeQuotaState(kv, binding));
      const auto latest = co_await latestRead.wait();
      if (latest && latest->usage && claudeCodeUsageIsFresh(latest, rt::Math::trunc(clock()))) co_return ClaudeCodeUsageRefreshFresh{*latest->usage};
      rt::Promise currentRead(claude_code_accounts::currentClaudeCodeAccountEntry(kv, binding));
      const auto current = co_await currentRead.wait();
      if (!current) co_return ClaudeCodeUsageRefreshChanged{};
      const claude_code::ClaudeCodeAuthHeaders credential{binding.credential.kind, binding.credential.secret};
      rt::Promise fetching(claude_code_usage::fetchClaudeCodeUsage(credential, options));
      const auto result = co_await fetching.wait();
      claude_code_quota::ClaudeCodeUsageState usage{};
      usage.source = "oauth_usage";
      claude_code_quota::QuotaOutcome outcome{};
      outcome.binding = binding;
      if (const auto* success = std::get_if<claude_code_usage::ClaudeCodeUsageSuccess>(&result)) {
        usage.state = "ok"; usage.status = success->status; usage.message = std::nullopt;
        usage.attempted_at_ms = success->observed_at_ms; usage.observed_at_ms = success->observed_at_ms;
        outcome.observation = success->observation;
      } else {
        const auto& failure = std::get<claude_code_usage::ClaudeCodeUsageFailure>(result);
        usage.state = failure.kind; usage.status = failure.status; usage.message = failure.message;
        usage.attempted_at_ms = failure.observed_at_ms; usage.observed_at_ms = std::nullopt;
      }
      usage.retry_at_ms = usageRetryDeadline(result);
      outcome.response_status = usage.status; outcome.response_observed_at_ms = usage.attempted_at_ms;
      outcome.usage = usage; outcome.nowMs = usage.attempted_at_ms;
      rt::Promise recording(claude_code_quota::recordClaudeCodeQuotaOutcome(kv, outcome));
      const auto recorded = co_await recording.wait();
      if (recorded) co_return ClaudeCodeUsageRefreshCompleted{result, usage};
      co_return ClaudeCodeUsageRefreshChanged{};
    };
    auto cleanup = [kv, key, owner]() -> rt::Task<void> {
      const rt::KvGetOptions cleanupReadOptions{.consistency = rt::KvConsistencyLevel::strong};
      auto checking = kv->get(key, cleanupReadOptions);
      const auto current = co_await std::move(checking);
      if (rt::getString(rt::storedJsonView(current.value), "owner") == owner) {
        auto deletion = kv->atomic(); deletion->check(current).delete_(key); co_await deletion->commit();
      }
    };
    auto executing = rt::async(work);
    auto finishing = rt::withFinally(std::move(executing), cleanup);
    const auto result = co_await std::move(finishing);
    co_return result;
  }
}
