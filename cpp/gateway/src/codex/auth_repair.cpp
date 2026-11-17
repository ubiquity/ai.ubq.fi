#include "uos/codex/auth_repair.hpp"

#include "uos/codex/index.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/record.hpp"

#include <algorithm>
#include <utility>

namespace uos::codex::auth_repair {
  namespace {
    using ::uos::types::CodexAuthPoolState;
    using ::uos::types::CodexAuthState;
    using rt::record::Reader;
    using rt::record::Writer;

    std::optional<double> probeStatus(const CodexAuthProbeResult& probe) {
      if (const auto* inconclusive = std::get_if<CodexAuthProbeResultInconclusive>(&probe)) return inconclusive->status;
      if (const auto* valid = std::get_if<CodexAuthProbeResultValid>(&probe)) return valid->status;
      return std::get<CodexAuthProbeResultInvalid>(probe).status;
    }

    std::string_view probeOutcome(const CodexAuthProbeResult& probe) {
      return std::visit([](const auto& value) -> std::string_view { return value.outcome; }, probe);
    }

    // rankCodexAuthCandidates over the caller's objects: the usable ones, best first.
    std::vector<const AssessedCodexAuthCandidate*> rankedCandidates(const std::vector<const AssessedCodexAuthCandidate*>& candidates, double nowMs) {
      rt::OrderedMap<std::string, double> sourceOrder{
        {"local-auth-json", 0},
        {"vps-pool", 1},
        {"vps-auth-json", 2},
      };
      std::vector<const AssessedCodexAuthCandidate*> usable;
      for (const AssessedCodexAuthCandidate* assessed : candidates) {
        if (isCodexAuthCandidateUsable(*assessed, nowMs)) usable.push_back(assessed);
      }
      const auto orderOf = [&sourceOrder](const CodexAuthCandidateSource& source) {
        const double* order = sourceOrder.get(source);
        return order ? *order : 99;
      };
      rt::sort(usable, [&orderOf](const AssessedCodexAuthCandidate* left, const AssessedCodexAuthCandidate* right) -> double {
        const std::optional<double> leftExp = left->access_exp_ms;
        const std::optional<double> rightExp = right->access_exp_ms;
        if (leftExp != rightExp) {
          if (!leftExp) return 1;
          if (!rightExp) return -1;
          return *rightExp - *leftExp;
        }
        const std::optional<double> leftUpdated = left->candidate.updated_at_ms;
        const std::optional<double> rightUpdated = right->candidate.updated_at_ms;
        if (leftUpdated != rightUpdated) {
          if (!leftUpdated) return 1;
          if (!rightUpdated) return -1;
          return *rightUpdated - *leftUpdated;
        }
        return orderOf(left->candidate.source) - orderOf(right->candidate.source);
      });
      return usable;
    }

    CodexAuthRepairRejection rejectionOf(const AssessedCodexAuthCandidate& assessed) {
      return CodexAuthRepairRejection{.source = assessed.candidate.source, .outcome = std::string(probeOutcome(assessed.probe)), .status = probeStatus(assessed.probe)};
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthCandidate& value) {
    out = Writer()
            .required("source", value.source)
            .required("account_id", value.account_id)
            .required("access_token", value.access_token)
            .required("refresh_token", value.refresh_token)
            .orNull("updated_at_ms", value.updated_at_ms)
            .done();
  }

  CodexAuthCandidate tag_invoke(const json::value_to_tag<CodexAuthCandidate>&, const json::value& value) {
    const Reader in(value, "CodexAuthCandidate");
    return CodexAuthCandidate{
      .source = in.required<std::string>("source"),
      .account_id = in.required<std::string>("account_id"),
      .access_token = in.required<std::string>("access_token"),
      .refresh_token = in.required<std::string>("refresh_token"),
      .updated_at_ms = in.orNull<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthProbeResultInconclusive& value) {
    out = Writer().required("outcome", std::string(CodexAuthProbeResultInconclusive::outcome)).orNull("status", value.status).done();
  }

  CodexAuthProbeResultInconclusive tag_invoke(const json::value_to_tag<CodexAuthProbeResultInconclusive>&, const json::value& value) {
    const Reader in(value, "CodexAuthProbeResult");
    const std::string outcome = in.required<std::string>("outcome");
    if (outcome != CodexAuthProbeResultInconclusive::outcome) in.fail("outcome", rt::get(value, "outcome"), "\"inconclusive\"");
    return CodexAuthProbeResultInconclusive{.status = in.orNull<double>("status")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthProbeResult& value) {
    if (const auto* inconclusive = std::get_if<CodexAuthProbeResultInconclusive>(&value)) {
      tag_invoke(json::value_from_tag(), out, *inconclusive);
      return;
    }
    out = Writer().required("outcome", std::string(probeOutcome(value))).required("status", *probeStatus(value)).done();
  }

  CodexAuthProbeResult tag_invoke(const json::value_to_tag<CodexAuthProbeResult>&, const json::value& value) {
    const Reader in(value, "CodexAuthProbeResult");
    const std::string outcome = in.required<std::string>("outcome");
    if (outcome == CodexAuthProbeResultValid::outcome) return CodexAuthProbeResultValid{.status = in.required<double>("status")};
    if (outcome == CodexAuthProbeResultInvalid::outcome) return CodexAuthProbeResultInvalid{.status = in.required<double>("status")};
    if (outcome == CodexAuthProbeResultInconclusive::outcome) return CodexAuthProbeResultInconclusive{.status = in.orNull<double>("status")};
    in.fail("outcome", rt::get(value, "outcome"), "\"valid\" | \"invalid\" | \"inconclusive\"");
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const AssessedCodexAuthCandidate& value) {
    out = Writer().required("candidate", value.candidate).orNull("access_exp_ms", value.access_exp_ms).required("probe", value.probe).done();
  }

  AssessedCodexAuthCandidate tag_invoke(const json::value_to_tag<AssessedCodexAuthCandidate>&, const json::value& value) {
    const Reader in(value, "AssessedCodexAuthCandidate");
    return AssessedCodexAuthCandidate{
      .candidate = in.required<CodexAuthCandidate>("candidate"),
      .access_exp_ms = in.orNull<double>("access_exp_ms"),
      .probe = in.required<CodexAuthProbeResult>("probe"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthRepairAccount& value) {
    out = Writer().required("slot", value.slot).required("account", value.account).required("probe", value.probe).orNull("refresh_diverged", value.refresh_diverged).done();
  }

  CodexAuthRepairAccount tag_invoke(const json::value_to_tag<CodexAuthRepairAccount>&, const json::value& value) {
    const Reader in(value, "CodexAuthRepairAccount");
    return CodexAuthRepairAccount{
      .slot = in.required<double>("slot"),
      .account = in.required<CodexAuthState>("account"),
      .probe = in.required<CodexAuthProbeResult>("probe"),
      .refresh_diverged = in.orNull<bool>("refresh_diverged"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthRepairRejection& value) {
    out = Writer().required("source", value.source).required("outcome", value.outcome).orNull("status", value.status).done();
  }

  CodexAuthRepairRejection tag_invoke(const json::value_to_tag<CodexAuthRepairRejection>&, const json::value& value) {
    const Reader in(value, "CodexAuthRepairRejection");
    return CodexAuthRepairRejection{.source = in.required<std::string>("source"), .outcome = in.required<std::string>("outcome"), .status = in.orNull<double>("status")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthRepairSelection& value) {
    out = Writer()
            .required("slot", value.slot)
            .required("account_id", value.account_id)
            .required("candidate", value.candidate)
            .orNull("access_exp_ms", value.access_exp_ms)
            .required("rejected", value.rejected)
            .done();
  }

  CodexAuthRepairSelection tag_invoke(const json::value_to_tag<CodexAuthRepairSelection>&, const json::value& value) {
    const Reader in(value, "CodexAuthRepairSelection");
    return CodexAuthRepairSelection{
      .slot = in.required<double>("slot"),
      .account_id = in.required<std::string>("account_id"),
      .candidate = in.required<CodexAuthCandidate>("candidate"),
      .access_exp_ms = in.orNull<double>("access_exp_ms"),
      .rejected = in.required<std::vector<CodexAuthRepairRejection>>("rejected"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthRepairPlan::Unrepairable& value) {
    out = Writer().required("slot", value.slot).required("account_id", value.account_id).required("reason", value.reason).required("rejected", value.rejected).done();
  }

  CodexAuthRepairPlan::Unrepairable tag_invoke(const json::value_to_tag<CodexAuthRepairPlan::Unrepairable>&, const json::value& value) {
    const Reader in(value, "CodexAuthRepairPlan.unrepairable");
    return CodexAuthRepairPlan::Unrepairable{
      .slot = in.required<double>("slot"),
      .account_id = in.required<std::string>("account_id"),
      .reason = in.required<std::string>("reason"),
      .rejected = in.required<std::vector<CodexAuthRepairRejection>>("rejected"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthRepairPlan::Healthy& value) {
    out = Writer().required("slot", value.slot).required("account_id", value.account_id).orNull("refresh_diverged", value.refresh_diverged).done();
  }

  CodexAuthRepairPlan::Healthy tag_invoke(const json::value_to_tag<CodexAuthRepairPlan::Healthy>&, const json::value& value) {
    const Reader in(value, "CodexAuthRepairPlan.healthy");
    return CodexAuthRepairPlan::Healthy{.slot = in.required<double>("slot"), .account_id = in.required<std::string>("account_id"), .refresh_diverged = in.orNull<bool>("refresh_diverged")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthRepairPlan::Inconclusive& value) {
    out = Writer().required("slot", value.slot).required("account_id", value.account_id).required("probe", value.probe).orNull("refresh_diverged", value.refresh_diverged).done();
  }

  CodexAuthRepairPlan::Inconclusive tag_invoke(const json::value_to_tag<CodexAuthRepairPlan::Inconclusive>&, const json::value& value) {
    const Reader in(value, "CodexAuthRepairPlan.inconclusive");
    return CodexAuthRepairPlan::Inconclusive{
      .slot = in.required<double>("slot"),
      .account_id = in.required<std::string>("account_id"),
      .probe = in.required<CodexAuthProbeResultInconclusive>("probe"),
      .refresh_diverged = in.orNull<bool>("refresh_diverged"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthRepairPlan& value) {
    out = Writer()
            .required("selections", value.selections)
            .required("unrepairable", value.unrepairable)
            .required("healthy", value.healthy)
            .required("inconclusive", value.inconclusive)
            .done();
  }

  CodexAuthRepairPlan tag_invoke(const json::value_to_tag<CodexAuthRepairPlan>&, const json::value& value) {
    const Reader in(value, "CodexAuthRepairPlan");
    return CodexAuthRepairPlan{
      .selections = in.required<std::vector<CodexAuthRepairSelection>>("selections"),
      .unrepairable = in.required<std::vector<CodexAuthRepairPlan::Unrepairable>>("unrepairable"),
      .healthy = in.required<std::vector<CodexAuthRepairPlan::Healthy>>("healthy"),
      .inconclusive = in.required<std::vector<CodexAuthRepairPlan::Inconclusive>>("inconclusive"),
    };
  }

  bool isCodexAuthCandidateUsable(const AssessedCodexAuthCandidate& assessed, double nowMs) {
    if (!std::holds_alternative<CodexAuthProbeResultValid>(assessed.probe)) return false;
    return !assessed.access_exp_ms || *assessed.access_exp_ms > nowMs;
  }

  bool needsCodexAuthReplacement(const CodexAuthProbeResult& probe, std::optional<double> accessExpMs, double nowMs) {
    return std::holds_alternative<CodexAuthProbeResultInvalid>(probe) || (accessExpMs && *accessExpMs <= nowMs);
  }

  std::vector<AssessedCodexAuthCandidate> rankCodexAuthCandidates(const std::vector<AssessedCodexAuthCandidate>& candidates, double nowMs) {
    std::vector<const AssessedCodexAuthCandidate*> all;
    for (const AssessedCodexAuthCandidate& candidate : candidates) all.push_back(&candidate);
    std::vector<AssessedCodexAuthCandidate> ranked;
    for (const AssessedCodexAuthCandidate* candidate : rankedCandidates(all, nowMs)) ranked.push_back(*candidate);
    return ranked;
  }

  CodexAuthRepairPlan planCodexAuthRepair(const PlanCodexAuthRepairInput& input) {
    CodexAuthRepairPlan plan;

    for (const CodexAuthRepairAccount& entry : input.accounts) {
      const std::optional<double> localExpMs = index::getJwtExpMs(std::string_view(entry.account.access_token));
      if (!needsCodexAuthReplacement(entry.probe, localExpMs, input.nowMs)) {
        if (const auto* inconclusive = std::get_if<CodexAuthProbeResultInconclusive>(&entry.probe)) {
          plan.inconclusive.push_back(CodexAuthRepairPlan::Inconclusive{
            .slot = entry.slot,
            .account_id = entry.account.account_id,
            .probe = *inconclusive,
            .refresh_diverged = entry.refresh_diverged,
          });
        } else {
          plan.healthy.push_back(CodexAuthRepairPlan::Healthy{.slot = entry.slot, .account_id = entry.account.account_id, .refresh_diverged = entry.refresh_diverged});
        }
        continue;
      }

      std::vector<const AssessedCodexAuthCandidate*> forAccount;
      for (const AssessedCodexAuthCandidate& assessed : input.candidates) {
        if (assessed.candidate.account_id == entry.account.account_id) forAccount.push_back(&assessed);
      }
      // `at()` returns `T | undefined`, unlike an unchecked index access, so the
      // empty-candidate case stays a real branch the type system can see.
      const std::vector<const AssessedCodexAuthCandidate*> ranked = rankedCandidates(forAccount, input.nowMs);
      const AssessedCodexAuthCandidate* chosen = ranked.empty() ? nullptr : ranked.front();
      // The adopted credential is reported separately, so it is not a rejection.
      std::vector<CodexAuthRepairRejection> rejected;
      for (const AssessedCodexAuthCandidate* assessed : forAccount) {
        if (assessed != chosen) rejected.push_back(rejectionOf(*assessed));
      }
      if (!chosen) {
        plan.unrepairable.push_back(CodexAuthRepairPlan::Unrepairable{
          .slot = entry.slot,
          .account_id = entry.account.account_id,
          .reason = forAccount.empty() ? "no_candidates" : "no_valid_candidate",
          .rejected = std::move(rejected),
        });
        continue;
      }
      plan.selections.push_back(CodexAuthRepairSelection{
        .slot = entry.slot,
        .account_id = entry.account.account_id,
        .candidate = chosen->candidate,
        .access_exp_ms = chosen->access_exp_ms,
        .rejected = std::move(rejected),
      });
    }

    return plan;
  }

  std::optional<CodexAuthPoolState> applyCodexAuthPlan(const CodexAuthPoolState& pool, const CodexAuthRepairPlan& plan, double nowMs) {
    const bool expired = std::any_of(plan.selections.begin(), plan.selections.end(), [nowMs](const CodexAuthRepairSelection& selection) {
      return selection.access_exp_ms && *selection.access_exp_ms <= nowMs;
    });
    if (expired) return std::nullopt;
    std::optional<CodexAuthPoolState> next = pool;
    for (const CodexAuthRepairSelection& selection : plan.selections) {
      if (!next) return std::nullopt;
      next = index::upsertCodexAuthAccount(next, CodexAuthState{
                                                   .access_token = selection.candidate.access_token,
                                                   .refresh_token = selection.candidate.refresh_token,
                                                   .account_id = selection.account_id,
                                                   .updated_at_ms = nowMs,
                                                 });
    }
    return next;
  }

  std::optional<CodexAuthCandidate> codexAuthCandidateFromAuthJson(const json::value& value, CodexAuthCandidateSource source, std::optional<double> updatedAtMs) {
    return codexAuthCandidateFromAuthJson(&value, std::move(source), updatedAtMs);
  }

  std::optional<CodexAuthCandidate> codexAuthCandidateFromAuthJson(const json::value* value, CodexAuthCandidateSource source, std::optional<double> updatedAtMs) {
    const std::optional<auth::ParseCodexAuthFromAuthJsonResult> parsed = index::parseCodexAuthFromAuthJson(value);
    if (!parsed) return std::nullopt;
    return CodexAuthCandidate{
      .source = std::move(source),
      .account_id = parsed->account_id,
      .access_token = parsed->access_token,
      .refresh_token = parsed->refresh_token,
      .updated_at_ms = updatedAtMs,
    };
  }

  std::vector<CodexAuthCandidate> codexAuthCandidatesFromPool(const json::value& value, CodexAuthCandidateSource source) {
    return codexAuthCandidatesFromPool(&value, std::move(source));
  }

  std::vector<CodexAuthCandidate> codexAuthCandidatesFromPool(const json::value* value, CodexAuthCandidateSource source) {
    const std::optional<CodexAuthPoolState> pool = index::parseCodexAuthPool(value);
    if (!pool) return {};
    std::vector<CodexAuthCandidate> candidates;
    for (const CodexAuthState& account : pool->accounts) {
      if (account.native_owner && account.native_owner->auth_mode == "chatgptAuthTokens") continue;
      candidates.push_back(CodexAuthCandidate{
        .source = source,
        .account_id = account.account_id,
        .access_token = account.access_token,
        .refresh_token = account.refresh_token,
        .updated_at_ms = account.updated_at_ms,
      });
    }
    return candidates;
  }

  std::string codexAuthCandidateKey(const CodexAuthCandidate& candidate) { return candidate.account_id + std::string(1, '\0') + candidate.access_token; }
}
