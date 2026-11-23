#include "uos/provider/capacity_contract.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/record.hpp"

#include <string_view>
#include <utility>

namespace uos::provider::capacity_contract {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    // A literal-typed member (`v: 1`): the number itself, else the Reader's TypeError.
    double readOne(const Reader& in, const json::value& value, std::string_view key) {
      const double number = in.required<double>(key);
      if (number != 1) in.fail(key, rt::get(value, key), "1");
      return number;
    }

    // A static discriminant (`source: "codex"`).
    void readDiscriminant(const Reader& in, const json::value& value, std::string_view key, std::string_view literal) {
      const std::string text = in.required<std::string>(key);
      if (text != literal) in.fail(key, rt::get(value, key), "\"" + std::string(literal) + "\"");
    }

    json::array writeSources(const std::array<ProviderCapacitySource, 3>& sources) {
      json::array out;
      for (const ProviderCapacitySource& source : sources) out.push_back(json::value_from(source));
      return out;
    }

    json::array writeHistorySources(const std::tuple<ProviderCapacityCodexSource, ProviderCapacityCodexSource, ProviderCapacityMeteredSource>& sources) {
      json::array out;
      out.push_back(json::value_from(std::get<0>(sources)));
      out.push_back(json::value_from(std::get<1>(sources)));
      out.push_back(json::value_from(std::get<2>(sources)));
      return out;
    }

    // A fixed-length `sources` tuple member: an array of exactly three objects.
    const json::array& readTriple(const Reader& in, const json::value& value) {
      const json::value* member = rt::get(value, "sources");
      if (!member || !member->is_array() || member->get_array().size() != 3) in.fail("sources", member, "[object, object, object]");
      return member->get_array();
    }

    template <class T> T readNested(const Reader& in, std::string_view key, const json::value& member) {
      if (!member.is_object()) in.fail(key, &member, "object");
      return json::value_to<T>(member);
    }

    // The Claude source's `usage: ClaudeCodeUsageState | null` member, converted here rather than through
    // claude-code-quota's own pair (the same members and order) so this contract module stays a leaf at link time,
    // as the TS type-only import is.
    json::value usageStateJson(const std::optional<claude_code_quota::ClaudeCodeUsageState>& usage) {
      if (!usage) return nullptr;
      return Writer()
        .required("source", usage->source)
        .required("state", usage->state)
        .orNull("status", usage->status)
        .orNull("message", usage->message)
        .required("attempted_at_ms", usage->attempted_at_ms)
        .orNull("observed_at_ms", usage->observed_at_ms)
        .orUndefined("retry_at_ms", usage->retry_at_ms)
        .done();
    }

    std::optional<claude_code_quota::ClaudeCodeUsageState> readUsageState(const Reader& outer, const json::value& owner) {
      const json::value* member = rt::get(owner, "usage");
      if (!member) outer.fail("usage", member, "object or null");
      if (member->is_null()) return std::nullopt;
      if (!member->is_object()) outer.fail("usage", member, "object or null");
      const Reader in(*member, "ClaudeCodeUsageState");
      const std::string source = in.required<std::string>("source");
      if (source != "oauth_usage") in.fail("source", rt::get(*member, "source"), "\"oauth_usage\"");
      return claude_code_quota::ClaudeCodeUsageState{
        .source = source,
        .state = in.required<std::string>("state"),
        .status = in.orNull<double>("status"),
        .message = in.orNull<std::string>("message"),
        .attempted_at_ms = in.required<double>("attempted_at_ms"),
        .observed_at_ms = in.orNull<double>("observed_at_ms"),
        .retry_at_ms = in.orUndefined<double>("retry_at_ms"),
      };
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityWindow& value) {
    out = Writer()
            .orNull("limit_window_seconds", value.limit_window_seconds)
            .orNull("used_percent", value.used_percent)
            .orNull("reset_at_ms", value.reset_at_ms)
            .done();
  }

  ProviderCapacityWindow tag_invoke(const json::value_to_tag<ProviderCapacityWindow>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityWindow");
    return ProviderCapacityWindow{
      .limit_window_seconds = in.orNull<double>("limit_window_seconds"),
      .used_percent = in.orNull<double>("used_percent"),
      .reset_at_ms = in.orNull<double>("reset_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityAdditionalRateLimit::Windows& value) {
    out = Writer().orNull("primary", value.primary).orNull("secondary", value.secondary).done();
  }

  ProviderCapacityAdditionalRateLimit::Windows tag_invoke(const json::value_to_tag<ProviderCapacityAdditionalRateLimit::Windows>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityAdditionalRateLimit.windows");
    return ProviderCapacityAdditionalRateLimit::Windows{
      .primary = in.orNull<ProviderCapacityWindow>("primary"),
      .secondary = in.orNull<ProviderCapacityWindow>("secondary"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityAdditionalRateLimit& value) {
    out = Writer().required("limit_name", value.limit_name).orNull("metered_feature", value.metered_feature).required("windows", value.windows).done();
  }

  ProviderCapacityAdditionalRateLimit tag_invoke(const json::value_to_tag<ProviderCapacityAdditionalRateLimit>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityAdditionalRateLimit");
    return ProviderCapacityAdditionalRateLimit{
      .limit_name = in.required<std::string>("limit_name"),
      .metered_feature = in.orNull<std::string>("metered_feature"),
      .windows = in.required<ProviderCapacityAdditionalRateLimit::Windows>("windows"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityCodexSource& value) {
    out = Writer()
            .required("source", std::string(ProviderCapacityCodexSource::source))
            .required("label", value.label)
            .required("slot", value.slot)
            .orNull("account_cohort_id", value.account_cohort_id)
            .required("state", value.state)
            .orNull("source_observed_at_ms", value.source_observed_at_ms)
            .required("snapshot_at_ms", value.snapshot_at_ms)
            .orNull("failure_kind", value.failure_kind)
            .orNull("failure_status", value.failure_status)
            .required("windows", value.windows)
            .required("additional_rate_limits", value.additional_rate_limits)
            .done();
  }

  ProviderCapacityCodexSource tag_invoke(const json::value_to_tag<ProviderCapacityCodexSource>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityCodexSource");
    readDiscriminant(in, value, "source", ProviderCapacityCodexSource::source);
    return ProviderCapacityCodexSource{
      .label = in.required<std::string>("label"),
      .slot = in.required<double>("slot"),
      .account_cohort_id = in.orNull<std::string>("account_cohort_id"),
      .state = in.required<std::string>("state"),
      .source_observed_at_ms = in.orNull<double>("source_observed_at_ms"),
      .snapshot_at_ms = in.required<double>("snapshot_at_ms"),
      .failure_kind = in.orNull<std::string>("failure_kind"),
      .failure_status = in.orNull<double>("failure_status"),
      .windows = in.required<ProviderCapacityCodexSource::Windows>("windows"),
      .additional_rate_limits = in.required<std::vector<ProviderCapacityAdditionalRateLimit>>("additional_rate_limits"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityMeteredSource::Wallet& value) {
    json::value written = Writer()
                            .orNull("balance_credits", value.balance_credits)
                            .orNull("baseline_credits", value.baseline_credits)
                            .orNull("refill_cycle_remaining_percent", value.refill_cycle_remaining_percent)
                            .orNull("refill_cycle_used_percent", value.refill_cycle_used_percent)
                            .orNull("unlimited_quota", value.unlimited_quota)
                            .orNull("total_available", value.total_available)
                            .orNull("total_granted", value.total_granted)
                            .orNull("total_used", value.total_used)
                            .orNull("cycle_started_at_ms", value.cycle_started_at_ms)
                            .orNull("last_credit_at_ms", value.last_credit_at_ms)
                            .orNull("confidence", value.confidence)
                            .orNull("cache_state", value.cache_state)
                            .done();
    written.as_object()["reset_at_ms"] = nullptr;
    out = std::move(written);
  }

  ProviderCapacityMeteredSource::Wallet tag_invoke(const json::value_to_tag<ProviderCapacityMeteredSource::Wallet>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityMeteredSource.wallet");
    ProviderCapacityMeteredSource::Wallet wallet{
      .balance_credits = in.orNull<double>("balance_credits"),
      .baseline_credits = in.orNull<double>("baseline_credits"),
      .refill_cycle_remaining_percent = in.orNull<double>("refill_cycle_remaining_percent"),
      .refill_cycle_used_percent = in.orNull<double>("refill_cycle_used_percent"),
      .unlimited_quota = in.orNull<bool>("unlimited_quota"),
      .total_available = in.orNull<double>("total_available"),
      .total_granted = in.orNull<double>("total_granted"),
      .total_used = in.orNull<double>("total_used"),
      .cycle_started_at_ms = in.orNull<double>("cycle_started_at_ms"),
      .last_credit_at_ms = in.orNull<double>("last_credit_at_ms"),
      .confidence = in.orNull<std::string>("confidence"),
      .cache_state = in.orNull<std::string>("cache_state"),
      .reset_at_ms = nullptr,
    };
    const json::value* resetAtMs = rt::get(value, "reset_at_ms");
    if (!resetAtMs || !resetAtMs->is_null()) in.fail("reset_at_ms", resetAtMs, "null");
    return wallet;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityMeteredSource& value) {
    out = Writer()
            .required("source", std::string(ProviderCapacityMeteredSource::source))
            .required("label", value.label)
            .required("state", value.state)
            .orNull("source_observed_at_ms", value.source_observed_at_ms)
            .required("snapshot_at_ms", value.snapshot_at_ms)
            .required("wallet", value.wallet)
            .done();
  }

  ProviderCapacityMeteredSource tag_invoke(const json::value_to_tag<ProviderCapacityMeteredSource>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityMeteredSource");
    readDiscriminant(in, value, "source", ProviderCapacityMeteredSource::source);
    return ProviderCapacityMeteredSource{
      .label = in.required<std::string>("label"),
      .state = in.required<std::string>("state"),
      .source_observed_at_ms = in.orNull<double>("source_observed_at_ms"),
      .snapshot_at_ms = in.required<double>("snapshot_at_ms"),
      .wallet = in.required<ProviderCapacityMeteredSource::Wallet>("wallet"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacitySource& value) {
    std::visit([&out](const auto& source) { out = json::value_from(source); }, value);
  }

  ProviderCapacitySource tag_invoke(const json::value_to_tag<ProviderCapacitySource>&, const json::value& value) {
    const Reader in(value, "ProviderCapacitySource");
    const std::string source = in.required<std::string>("source");
    if (source == ProviderCapacityCodexSource::source) return json::value_to<ProviderCapacityCodexSource>(value);
    if (source == ProviderCapacityMeteredSource::source) return json::value_to<ProviderCapacityMeteredSource>(value);
    in.fail("source", rt::get(value, "source"), "\"codex\" | \"metered\"");
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityClaudeWindow& value) {
    out = Writer()
            .orNull("status", value.status)
            .orNull("used_percent", value.used_percent)
            .orNull("reset_at_ms", value.reset_at_ms)
            .orNull("observed_at_ms", value.observed_at_ms)
            .done();
  }

  ProviderCapacityClaudeWindow tag_invoke(const json::value_to_tag<ProviderCapacityClaudeWindow>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityClaudeWindow");
    return ProviderCapacityClaudeWindow{
      .status = in.orNull<std::string>("status"),
      .used_percent = in.orNull<double>("used_percent"),
      .reset_at_ms = in.orNull<double>("reset_at_ms"),
      .observed_at_ms = in.orNull<double>("observed_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityClaudeStandardLimit& value) {
    out = Writer()
            .required("family", value.family)
            .orNull("limit", value.limit)
            .orNull("remaining", value.remaining)
            .orNull("reset_at_ms", value.reset_at_ms)
            .done();
  }

  ProviderCapacityClaudeStandardLimit tag_invoke(const json::value_to_tag<ProviderCapacityClaudeStandardLimit>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityClaudeStandardLimit");
    return ProviderCapacityClaudeStandardLimit{
      .family = in.required<std::string>("family"),
      .limit = in.orNull<double>("limit"),
      .remaining = in.orNull<double>("remaining"),
      .reset_at_ms = in.orNull<double>("reset_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityClaudeError& value) {
    out = Writer().required("kind", value.kind).orNull("status", value.status).orNull("message", value.message).required("at_ms", value.at_ms).done();
  }

  ProviderCapacityClaudeError tag_invoke(const json::value_to_tag<ProviderCapacityClaudeError>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityClaudeError");
    return ProviderCapacityClaudeError{
      .kind = in.required<std::string>("kind"),
      .status = in.orNull<double>("status"),
      .message = in.orNull<std::string>("message"),
      .at_ms = in.required<double>("at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityClaudeSource::Windows& value) {
    out = Writer().orNull("five_hour", value.five_hour).orNull("seven_day", value.seven_day).done();
  }

  ProviderCapacityClaudeSource::Windows tag_invoke(const json::value_to_tag<ProviderCapacityClaudeSource::Windows>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityClaudeSource.windows");
    return ProviderCapacityClaudeSource::Windows{
      .five_hour = in.orNull<ProviderCapacityClaudeWindow>("five_hour"),
      .seven_day = in.orNull<ProviderCapacityClaudeWindow>("seven_day"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityClaudeSource& value) {
    out = Writer()
            .orUndefined("usage_snapshot", value.usage_snapshot)
            .orUndefined("usage_stale", value.usage_stale)
            .required("source", value.source)
            .required("label", value.label)
            .nullable("account_id", value.account_id)
            .nullable("account_cohort_id", value.account_cohort_id)
            .required("state", value.state)
            .orNull("source_observed_at_ms", value.source_observed_at_ms)
            .required("snapshot_at_ms", value.snapshot_at_ms)
            .orNull("failure_kind", value.failure_kind)
            .orNull("failure_status", value.failure_status)
            .orNull("credential_kind", value.credential_kind)
            .orNull("workspace_id", value.workspace_id)
            .required("windows", value.windows)
            .required("standard", value.standard)
            .required("usage", usageStateJson(value.usage))
            .orNull("blocked_until_ms", value.blocked_until_ms)
            .orNull("cooldown_until_ms", value.cooldown_until_ms)
            .orNull("last_error", value.last_error)
            .done();
  }

  ProviderCapacityClaudeSource tag_invoke(const json::value_to_tag<ProviderCapacityClaudeSource>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityClaudeSource");
    return ProviderCapacityClaudeSource{
      .usage_snapshot = in.orUndefined<json::object>("usage_snapshot"),
      .usage_stale = in.orUndefined<bool>("usage_stale"),
      .source = in.required<std::string>("source"),
      .label = in.required<std::string>("label"),
      .account_id = in.nullable<std::string>("account_id"),
      .account_cohort_id = in.nullable<std::string>("account_cohort_id"),
      .state = in.required<std::string>("state"),
      .source_observed_at_ms = in.orNull<double>("source_observed_at_ms"),
      .snapshot_at_ms = in.required<double>("snapshot_at_ms"),
      .failure_kind = in.orNull<std::string>("failure_kind"),
      .failure_status = in.orNull<double>("failure_status"),
      .credential_kind = in.orNull<std::string>("credential_kind"),
      .workspace_id = in.orNull<std::string>("workspace_id"),
      .windows = in.required<ProviderCapacityClaudeSource::Windows>("windows"),
      .standard = in.required<std::vector<ProviderCapacityClaudeStandardLimit>>("standard"),
      .usage = readUsageState(in, value),
      .blocked_until_ms = in.orNull<double>("blocked_until_ms"),
      .cooldown_until_ms = in.orNull<double>("cooldown_until_ms"),
      .last_error = in.orNull<ProviderCapacityClaudeError>("last_error"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacitySnapshot& value) {
    out = Writer()
            .required("snapshot_at_ms", value.snapshot_at_ms)
            .required("stale_after_ms", value.stale_after_ms)
            .required("sources", writeSources(value.sources))
            .done();
  }

  ProviderCapacitySnapshot tag_invoke(const json::value_to_tag<ProviderCapacitySnapshot>&, const json::value& value) {
    const Reader in(value, "ProviderCapacitySnapshot");
    const double snapshotAtMs = in.required<double>("snapshot_at_ms");
    const double staleAfterMs = in.required<double>("stale_after_ms");
    const json::array& sources = readTriple(in, value);
    return ProviderCapacitySnapshot{
      .snapshot_at_ms = snapshotAtMs,
      .stale_after_ms = staleAfterMs,
      .sources = {readNested<ProviderCapacitySource>(in, "sources", sources[0]), readNested<ProviderCapacitySource>(in, "sources", sources[1]),
                  readNested<ProviderCapacitySource>(in, "sources", sources[2])},
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityHistoryPoint& value) {
    out = Writer()
            .required("bucket_start_at_ms", value.bucket_start_at_ms)
            .required("sampled_at_ms", value.sampled_at_ms)
            .required("sources", writeHistorySources(value.sources))
            .done();
  }

  ProviderCapacityHistoryPoint tag_invoke(const json::value_to_tag<ProviderCapacityHistoryPoint>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityHistoryPoint");
    const double bucketStartAtMs = in.required<double>("bucket_start_at_ms");
    const double sampledAtMs = in.required<double>("sampled_at_ms");
    const json::array& sources = readTriple(in, value);
    return ProviderCapacityHistoryPoint{
      .bucket_start_at_ms = bucketStartAtMs,
      .sampled_at_ms = sampledAtMs,
      .sources = {readNested<ProviderCapacityCodexSource>(in, "sources", sources[0]), readNested<ProviderCapacityCodexSource>(in, "sources", sources[1]),
                  readNested<ProviderCapacityMeteredSource>(in, "sources", sources[2])},
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCapacityHistoryPoint::StandardLimit& value) {
    out = Writer()
            .required("family", value.family)
            .orNull("limit", value.limit)
            .orNull("remaining", value.remaining)
            .orNull("reset_at_ms", value.reset_at_ms)
            .orNull("observed_at_ms", value.observed_at_ms)
            .done();
  }

  ClaudeCapacityHistoryPoint::StandardLimit tag_invoke(const json::value_to_tag<ClaudeCapacityHistoryPoint::StandardLimit>&, const json::value& value) {
    const Reader in(value, "ClaudeCapacityHistoryPoint.standard");
    ClaudeCapacityHistoryPoint::StandardLimit limit;
    limit.family = in.required<std::string>("family");
    limit.limit = in.orNull<double>("limit");
    limit.remaining = in.orNull<double>("remaining");
    limit.reset_at_ms = in.orNull<double>("reset_at_ms");
    limit.observed_at_ms = in.orNull<double>("observed_at_ms");
    return limit;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCapacityHistoryPoint& value) {
    out = Writer()
            .required("v", value.v)
            .required("point_id", value.point_id)
            .required("bucket_start_at_ms", value.bucket_start_at_ms)
            .required("sampled_at_ms", value.sampled_at_ms)
            .orNull("source_observed_at_ms", value.source_observed_at_ms)
            .required("account_id", value.account_id)
            .required("account_cohort_id", value.account_cohort_id)
            .required("credential_kind", value.credential_kind)
            .orNull("workspace_id", value.workspace_id)
            .required("state", value.state)
            .orNull("response_status", value.response_status)
            .orUndefined("source", value.source)
            .required("windows", value.windows)
            .orUndefined("usage_snapshot", value.usage_snapshot)
            .required("standard", value.standard)
            .done();
  }

  ClaudeCapacityHistoryPoint tag_invoke(const json::value_to_tag<ClaudeCapacityHistoryPoint>&, const json::value& value) {
    const Reader in(value, "ClaudeCapacityHistoryPoint");
    return ClaudeCapacityHistoryPoint{
      .v = readOne(in, value, "v"),
      .point_id = in.required<std::string>("point_id"),
      .bucket_start_at_ms = in.required<double>("bucket_start_at_ms"),
      .sampled_at_ms = in.required<double>("sampled_at_ms"),
      .source_observed_at_ms = in.orNull<double>("source_observed_at_ms"),
      .account_id = in.required<std::string>("account_id"),
      .account_cohort_id = in.required<std::string>("account_cohort_id"),
      .credential_kind = in.required<std::string>("credential_kind"),
      .workspace_id = in.orNull<std::string>("workspace_id"),
      .state = in.required<std::string>("state"),
      .response_status = in.orNull<double>("response_status"),
      .source = in.orUndefined<std::string>("source"),
      .windows = in.required<ProviderCapacityClaudeSource::Windows>("windows"),
      .usage_snapshot = in.orUndefined<json::object>("usage_snapshot"),
      .standard = in.required<std::vector<ClaudeCapacityHistoryPoint::StandardLimit>>("standard"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCapacityHistoryView& value) {
    out = Writer()
            .required("v", value.v)
            .required("bucket_ms", value.bucket_ms)
            .required("retention_ms", value.retention_ms)
            .required("stale_after_ms", value.stale_after_ms)
            .required("scan", value.scan)
            .required("truncated", value.truncated)
            .required("points", value.points)
            .done();
  }

  ClaudeCapacityHistoryView tag_invoke(const json::value_to_tag<ClaudeCapacityHistoryView>&, const json::value& value) {
    const Reader in(value, "ClaudeCapacityHistoryView");
    return ClaudeCapacityHistoryView{
      .v = readOne(in, value, "v"),
      .bucket_ms = in.required<double>("bucket_ms"),
      .retention_ms = in.required<double>("retention_ms"),
      .stale_after_ms = in.required<double>("stale_after_ms"),
      .scan = in.required<std::string>("scan"),
      .truncated = in.required<bool>("truncated"),
      .points = in.required<std::vector<ClaudeCapacityHistoryPoint>>("points"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityView& value) {
    out = Writer()
            .required("snapshot_at_ms", value.snapshot_at_ms)
            .required("stale_after_ms", value.stale_after_ms)
            .required("sources", writeSources(value.sources))
            .required("cache_state", value.cache_state)
            .required("history", value.history)
            .required("reset_events", value.reset_events)
            .required("rate_limit_reset_events", value.rate_limit_reset_events)
            .required("downtime_events", value.downtime_events)
            .nullable("claude", value.claude)
            .orUndefined("claude_accounts", value.claude_accounts)
            .orUndefined("claude_history", value.claude_history)
            .done();
  }

  ProviderCapacityView tag_invoke(const json::value_to_tag<ProviderCapacityView>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityView");
    ProviderCapacityView view;
    static_cast<ProviderCapacitySnapshot&>(view) = json::value_to<ProviderCapacitySnapshot>(value);
    view.cache_state = in.required<std::string>("cache_state");
    view.history = in.required<std::vector<ProviderCapacityHistoryPoint>>("history");
    view.reset_events = in.required<std::vector<capacity_events::ProviderCapacityResetEvent>>("reset_events");
    view.rate_limit_reset_events = in.required<std::vector<capacity_events::ProviderCapacityRateLimitResetEvent>>("rate_limit_reset_events");
    view.downtime_events = in.required<std::vector<capacity_events::ProviderCapacityDowntimeEvent>>("downtime_events");
    view.claude = in.nullable<ProviderCapacityClaudeSource>("claude");
    view.claude_accounts = in.orUndefined<std::vector<ProviderCapacityClaudeSource>>("claude_accounts");
    view.claude_history = in.orUndefined<ClaudeCapacityHistoryView>("claude_history");
    return view;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CapacityLease& value) {
    out = Writer().required("owner", value.owner).required("lease_until_ms", value.lease_until_ms).done();
  }

  CapacityLease tag_invoke(const json::value_to_tag<CapacityLease>&, const json::value& value) {
    const Reader in(value, "CapacityLease");
    return CapacityLease{.owner = in.required<std::string>("owner"), .lease_until_ms = in.required<double>("lease_until_ms")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const StoredRateLimitObservation& value) {
    out = Writer().required("sampled_at_ms", value.sampled_at_ms).required("source", value.source).done();
  }

  StoredRateLimitObservation tag_invoke(const json::value_to_tag<StoredRateLimitObservation>&, const json::value& value) {
    const Reader in(value, "StoredRateLimitObservation");
    return StoredRateLimitObservation{
      .sampled_at_ms = in.required<double>("sampled_at_ms"),
      .source = in.required<ProviderCapacityCodexSource>("source"),
    };
  }
}

// The stored chart events' JSON conversions (declared in uos/provider/capacity_events.hpp) are defined here, beside the
// contract records that embed them in ProviderCapacityView, so linking this leaf module never pulls in the event
// recorders (capacity_events.cpp reaches the sampler); capacity_events.cpp links this file in any case.
namespace uos::provider::capacity_events {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    double readOne(const Reader& in, const json::value& value, std::string_view key) {
      const double number = in.required<double>(key);
      if (number != 1) in.fail(key, rt::get(value, key), "1");
      return number;
    }

    std::string readOpenAi(const Reader& in, const json::value& value) {
      std::string provider = in.required<std::string>("provider");
      if (provider != "openai") in.fail("provider", rt::get(value, "provider"), "\"openai\"");
      return provider;
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityResetEvent& value) {
    out = Writer().required("v", value.v).required("event_id", value.event_id).required("slot", value.slot).required("observed_at_ms", value.observed_at_ms).done();
  }

  ProviderCapacityResetEvent tag_invoke(const json::value_to_tag<ProviderCapacityResetEvent>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityResetEvent");
    return ProviderCapacityResetEvent{
      .v = readOne(in, value, "v"),
      .event_id = in.required<std::string>("event_id"),
      .slot = in.required<double>("slot"),
      .observed_at_ms = in.required<double>("observed_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityRateLimitResetEvent& value) {
    out = Writer()
            .required("v", value.v)
            .required("event_id", value.event_id)
            .required("provider", value.provider)
            .required("slot", value.slot)
            .required("window", value.window)
            .required("observed_at_ms", value.observed_at_ms)
            .required("previous_sampled_at_ms", value.previous_sampled_at_ms)
            .required("previous_reset_at_ms", value.previous_reset_at_ms)
            .required("reset_at_ms", value.reset_at_ms)
            .required("previous_used_percent", value.previous_used_percent)
            .required("current_used_percent", value.current_used_percent)
            .required("capacity_gain_percentage_points", value.capacity_gain_percentage_points)
            .done();
  }

  ProviderCapacityRateLimitResetEvent tag_invoke(const json::value_to_tag<ProviderCapacityRateLimitResetEvent>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityRateLimitResetEvent");
    return ProviderCapacityRateLimitResetEvent{
      .v = readOne(in, value, "v"),
      .event_id = in.required<std::string>("event_id"),
      .provider = readOpenAi(in, value),
      .slot = in.required<double>("slot"),
      .window = in.required<std::string>("window"),
      .observed_at_ms = in.required<double>("observed_at_ms"),
      .previous_sampled_at_ms = in.required<double>("previous_sampled_at_ms"),
      .previous_reset_at_ms = in.required<double>("previous_reset_at_ms"),
      .reset_at_ms = in.required<double>("reset_at_ms"),
      .previous_used_percent = in.required<double>("previous_used_percent"),
      .current_used_percent = in.required<double>("current_used_percent"),
      .capacity_gain_percentage_points = in.required<double>("capacity_gain_percentage_points"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityDowntimeEvent& value) {
    out = Writer()
            .required("v", value.v)
            .required("event_id", value.event_id)
            .required("provider", value.provider)
            .required("failure_kind", value.failure_kind)
            .orNull("status", value.status)
            .required("observed_at_ms", value.observed_at_ms)
            .done();
  }

  ProviderCapacityDowntimeEvent tag_invoke(const json::value_to_tag<ProviderCapacityDowntimeEvent>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityDowntimeEvent");
    return ProviderCapacityDowntimeEvent{
      .v = readOne(in, value, "v"),
      .event_id = in.required<std::string>("event_id"),
      .provider = readOpenAi(in, value),
      .failure_kind = in.required<std::string>("failure_kind"),
      .status = in.orNull<double>("status"),
      .observed_at_ms = in.required<double>("observed_at_ms"),
    };
  }
}
