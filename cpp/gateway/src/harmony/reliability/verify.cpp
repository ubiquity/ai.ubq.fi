#include "uos/harmony/reliability/verify.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <utility>

namespace uos::harmony::reliability::verify {
  namespace {
    json::value toJson(const PendingVerification& pending) {
      json::object out;
      out["path"] = pending.path;
      out["marker"] = pending.marker;
      out["add"] = pending.add;
      out["since"] = rt::makeNumber(pending.since);
      return out;
    }

    json::value toJson(const UnresolvedCommand& unresolved) {
      json::object out;
      out["command"] = unresolved.command;
      out["code"] = unresolved.code;
      out["since"] = rt::makeNumber(unresolved.since);
      return out;
    }

    json::value toJson(const UnresolvedEdit& unresolved) {
      json::object out;
      out["path"] = unresolved.path;
      out["since"] = rt::makeNumber(unresolved.since);
      return out;
    }

    /** Requirements raised by writes that still await verification. */
    std::vector<FinalRequirement> unverifiedWriteRequirements(const VerificationTracker& tracker) {
      std::vector<FinalRequirement> requirements;
      for (const PendingVerification& pending : tracker.pending()) {
        requirements.push_back(FinalRequirement{
          .kind = "unverified_write",
          .message = "unverified write: " + pending.path + " — verify it before answering",
          .path = pending.path,
        });
      }
      return requirements;
    }

    /** Requirements raised by command and edit failures that were never recovered. */
    std::vector<FinalRequirement> unresolvedFailureRequirements(const VerificationTracker& tracker) {
      std::vector<FinalRequirement> requirements;
      for (const UnresolvedCommand& unresolved : tracker.unresolvedCommands()) {
        requirements.push_back(FinalRequirement{
          .kind = "unresolved_command",
          .message = "unresolved command failure: " + unresolved.command + " — recover with a successful command before answering",
          .command = unresolved.command,
        });
      }
      for (const UnresolvedEdit& unresolved : tracker.unresolvedEdits()) {
        requirements.push_back(FinalRequirement{
          .kind = "unresolved_edit",
          .message = "unresolved edit: " + unresolved.path + " — apply the change or verify the path before answering",
          .path = unresolved.path,
        });
      }
      return requirements;
    }
  }

  VerificationTracker::VerificationTracker(VerificationPolicy policy_) : policy(std::move(policy_)) {}

  std::optional<VerificationResolution> VerificationTracker::observe(const std::string& tool, const json::object& args, const loops::ResultLike& result) {
    const std::optional<std::string_view> pathArg = rt::getString(args, "path");
    const std::optional<std::string> path = pathArg ? std::optional<std::string>(std::string(*pathArg)) : std::nullopt;
    if (tool == "task.update_plan" && result.ok) {
      // A new plan is deterministic abandonment evidence: unsolved
      // command/edit failures of the previous approach no longer block.
      abandonUnresolved_();
      return std::nullopt;
    }
    if (tool == "editor.apply_patch") return observeEdit_(path, args, result);
    if (tool == "filesystem.read" && result.ok && path) return observeRead_(*path, result);
    if (tool == "shell.exec") return observeExec_(args, result);
    return std::nullopt;
  }

  void VerificationTracker::abandonUnresolved_() {
    unresolvedCommands_.clear();
    unresolvedEdits_.clear();
  }

  std::optional<VerificationResolution> VerificationTracker::observeEdit_(const std::optional<std::string>& path, const json::object& args, const loops::ResultLike& result) {
    if (!path) return std::nullopt;
    if (result.ok) {
      const std::optional<std::string_view> newText = rt::getString(args, "new");
      const std::string marker = newText ? std::string(*newText) : "";
      const json::value* add = rt::get(args, "add");
      required_ += 1;
      pending_.set(*path, PendingVerification{*path, marker, add && add->is_bool() && add->get_bool(), rt::dateNow()});
      unresolvedEdits_.delete_(*path);
      return std::nullopt;
    }
    unresolvedEdits_.set(*path, UnresolvedEdit{*path, rt::dateNow()});
    return std::nullopt;
  }

  std::optional<VerificationResolution> VerificationTracker::observeRead_(const std::string& path, const loops::ResultLike& result) {
    // Reading a path whose edit failed is deterministic review evidence:
    // the model inspected the actual state before moving on.
    unresolvedEdits_.delete_(path);
    const PendingVerification* pending = pending_.get(path);
    if (pending == nullptr) return std::nullopt;
    const std::string output = result.output.valueOr("");
    if (!pending->marker.empty() && !rt::includes(output, pending->marker)) return std::nullopt;
    pending_.delete_(path);
    satisfied_ += 1;
    return VerificationResolution{"read", {path}};
  }

  std::optional<VerificationResolution> VerificationTracker::observeExec_(const json::object& args, const loops::ResultLike& result) {
    const std::optional<std::string_view> commandArg = rt::getString(args, "command");
    const std::string command = commandArg ? std::string(*commandArg) : "";
    if (!result.ok) {
      if (!command.empty()) {
        // Replace the previous failure record for the same command.
        unresolvedCommands_.set(command, UnresolvedCommand{command, result.error_code.valueOr("failure"), rt::dateNow()});
      }
      return std::nullopt;
    }
    // Any successful command is recovery evidence for earlier failures.
    unresolvedCommands_.clear();
    const std::string declared = policy.verificationCommand ? rt::trim(*policy.verificationCommand) : "";
    if (!declared.empty() && rt::trim(command) == declared) {
      std::vector<std::string> paths = rt::arrayFrom(pending_.keys());
      pending_.clear();
      satisfied_ += static_cast<double>(paths.size());
      if (paths.empty()) return std::nullopt;
      return VerificationResolution{"shell", std::move(paths)};
    }
    for (const std::string& pendingPath : rt::arrayFrom(pending_.keys())) {
      if (rt::includes(command, pendingPath)) {
        pending_.delete_(pendingPath);
        satisfied_ += 1;
        return VerificationResolution{"shell", {pendingPath}};
      }
    }
    return std::nullopt;
  }

  std::vector<PendingVerification> VerificationTracker::pending() const { return rt::arrayFrom(pending_.values()); }

  std::vector<UnresolvedCommand> VerificationTracker::unresolvedCommands() const { return rt::arrayFrom(unresolvedCommands_.values()); }

  std::vector<UnresolvedEdit> VerificationTracker::unresolvedEdits() const { return rt::arrayFrom(unresolvedEdits_.values()); }

  bool VerificationTracker::allVerified() const { return pending_.size() == 0; }

  VerificationTracker::Totals VerificationTracker::totals() const { return Totals{required_, satisfied_}; }

  json::object VerificationTracker::snapshot() const {
    std::vector<PendingVerification> pendingValues = pending();
    rt::sort(pendingValues, [](const PendingVerification& a, const PendingVerification& b) { return rt::localeCompare(a.path, b.path); });
    std::vector<UnresolvedCommand> commandValues = unresolvedCommands();
    rt::sort(commandValues, [](const UnresolvedCommand& a, const UnresolvedCommand& b) { return rt::localeCompare(a.command, b.command); });
    std::vector<UnresolvedEdit> editValues = unresolvedEdits();
    rt::sort(editValues, [](const UnresolvedEdit& a, const UnresolvedEdit& b) { return rt::localeCompare(a.path, b.path); });
    json::array pendingJson;
    for (const auto& value : pendingValues) pendingJson.push_back(toJson(value));
    json::array commandJson;
    for (const auto& value : commandValues) commandJson.push_back(toJson(value));
    json::array editJson;
    for (const auto& value : editValues) editJson.push_back(toJson(value));
    const Totals counts = totals();
    json::object totalsJson;
    totalsJson["required"] = rt::makeNumber(counts.required);
    totalsJson["satisfied"] = rt::makeNumber(counts.satisfied);
    json::object out;
    out["pending"] = std::move(pendingJson);
    out["unresolvedCommands"] = std::move(commandJson);
    out["unresolvedEdits"] = std::move(editJson);
    out["totals"] = std::move(totalsJson);
    return out;
  }

  FinalGuardDecision guardFinal(const FinalGuardInput& input) {
    const VerificationPolicy& policy = input.policy;
    std::vector<FinalRequirement> requirements;
    const FinalAttempt* prior = nullptr;
    for (auto it = input.previousFinals.rbegin(); it != input.previousFinals.rend(); ++it) {
      if (it->content == input.finalContent) {
        prior = &*it;
        break;
      }
    }
    const double repetitions = prior == nullptr ? 0 : prior->repetitions + 1;
    const bool falseCompletion = prior != nullptr && prior->lastActionSeq == input.lastActionSeq;

    if (policy.requireVerificationBeforeFinal) {
      for (FinalRequirement& requirement : unverifiedWriteRequirements(input.tracker)) requirements.push_back(std::move(requirement));
    }
    if (policy.requireRecoveryBeforeFinal) {
      for (FinalRequirement& requirement : unresolvedFailureRequirements(input.tracker)) requirements.push_back(std::move(requirement));
    }
    if (policy.rejectFinalDuringLoop && input.semanticLoopStreak >= 3) {
      requirements.push_back(FinalRequirement{
        .kind = "active_loop",
        .message = "active semantic loop — take a materially different action before answering",
      });
    }
    if (falseCompletion && repetitions >= 1) {
      requirements.push_back(FinalRequirement{
        .kind = "false_completion",
        .message = "final answer repeated without any intervening action — the previous claim was rejected; perform a verification or a different action first",
      });
    }
    if (policy.requirePlanBeforeWrites && input.writes > 0 && !input.planUpdated) {
      requirements.push_back(FinalRequirement{
        .kind = "plan_required",
        .message = "writes happened without a plan — call task.update_plan before answering",
      });
    }

    const FinalAttempt attempt{
      .content = input.finalContent,
      .rejected = !requirements.empty(),
      .lastActionSeq = input.lastActionSeq,
      .repetitions = repetitions,
    };
    const bool allowed = requirements.empty();
    return FinalGuardDecision{allowed, std::move(requirements), falseCompletion, attempt};
  }

  std::string renderGuardRequirements(const std::vector<FinalRequirement>& requirements) {
    if (requirements.empty()) return "";
    std::string out;
    for (std::size_t index = 0; index < requirements.size(); ++index) {
      if (index > 0) out += "; ";
      out += "[" + requirements[index].kind + "] " + requirements[index].message;
    }
    return out;
  }
}
