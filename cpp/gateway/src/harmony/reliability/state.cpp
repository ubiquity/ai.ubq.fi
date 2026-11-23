#include "uos/harmony/reliability/state.hpp"

#include "uos/harmony/reliability/hash.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <cstddef>
#include <unordered_set>
#include <utility>

namespace uos::harmony::reliability::state {
  namespace {
    constexpr std::size_t MAX_ERRORS = 5;

    bool isGuardResult(const std::optional<loops::ResultLike>& result) {
      return result && result->error_code.hasValue() &&
             std::find(verify::GUARD_ERROR_CODES.begin(), verify::GUARD_ERROR_CODES.end(), *result->error_code) != verify::GUARD_ERROR_CODES.end();
    }

    TaskPhase phaseFor(const StructuredTaskState& state, const ToolObservation& obs) {
      if (state.semanticLoopStreak >= 3) return "stalled";
      if (std::any_of(state.finals.begin(), state.finals.end(), [](const FinalObservation& f) { return f.accepted; })) return "done";
      if (state.finalAttempts > 0 && !state.finals.empty()) {
        const FinalObservation& lastFinal = state.finals.back();
        if (!lastFinal.accepted) return "completing";
      }
      if (obs.result.hasValue() && !obs.result->ok && !isGuardResult(obs.result.toOptional())) {
        return "recovering";
      }
      if (!state.pendingVerification.empty() || !state.unresolvedEdits.empty()) return "verifying";
      if (!state.writes.empty()) return "acting";
      if (!state.plan.seq && state.reads.empty()) return "planning";
      return "exploring";
    }

    /**
     * Path argument of a tool call.  Values are untrusted, so a non-string `path`
     * is treated as absent instead of being stringified into a bogus record key.
     */
    std::string stringArg(const json::value* value) {
      const std::optional<std::string_view> text = rt::getString(value);
      return text ? std::string(*text) : "";
    }

    std::optional<std::string> stringField(const json::object& args, std::string_view key) {
      const std::optional<std::string_view> text = rt::getString(args, key);
      return text ? std::optional<std::string>(std::string(*text)) : std::nullopt;
    }

    void upsertRead(StructuredTaskState& state, const ToolObservation& obs) {
      const std::string path = stringArg(rt::get(obs.args, "path"));
      const std::string digest = hash::digestShort(obs.result.hasValue() ? obs.result->output.valueOr("") : "");
      std::vector<ReadRecord> reads;
      for (const ReadRecord& r : state.reads) {
        if (r.path != path) reads.push_back(r);
      }
      reads.push_back(ReadRecord{path, digest, obs.seq});
      rt::sort(reads, [](const ReadRecord& a, const ReadRecord& b) { return rt::localeCompare(a.path, b.path); });
      state.reads = std::move(reads);
    }

    void upsertWrite(StructuredTaskState& state, const ToolObservation& obs, const ObservationMeta& meta) {
      const std::string path = stringArg(rt::get(obs.args, "path"));
      const std::string marker = stringField(obs.args, "new").value_or("");
      std::vector<WriteRecord> writes;
      for (const WriteRecord& w : state.writes) {
        if (w.path != path) writes.push_back(w);
      }
      const bool verified = meta.verification.has_value();
      const json::value* add = rt::get(obs.args, "add");
      writes.push_back(WriteRecord{
        .path = path,
        .marker = marker,
        .add = add != nullptr && add->is_bool() && add->get_bool(),
        .verified = verified,
        .verifiedBy = meta.verification ? std::optional<std::string>(meta.verification->kind) : std::nullopt,
        .seq = obs.seq,
      });
      rt::sort(writes, [](const WriteRecord& a, const WriteRecord& b) { return rt::localeCompare(a.path, b.path); });
      state.writes = std::move(writes);
    }

    void recordError(StructuredTaskState& state, const ToolObservation& obs) {
      const std::optional<loops::ResultLike> result = obs.result.toOptional();
      if (!result || result->ok || isGuardResult(result)) return;
      std::vector<StateErrorRecord> errors = state.errors;
      errors.push_back(StateErrorRecord{
        .code = result->error_code.valueOr("failure"),
        .tool = obs.tool,
        .seq = obs.seq,
        .digest = hash::digestShort(result->error.valueOr("")),
      });
      if (errors.size() > MAX_ERRORS) errors.erase(errors.begin(), errors.end() - static_cast<std::ptrdiff_t>(MAX_ERRORS));
      state.errors = std::move(errors);
    }

    /** Applies one verification resolution to the pending set and the write records. */
    void applyVerificationToWrites(StructuredTaskState& next, const verify::VerificationResolution& verification) {
      const std::unordered_set<std::string> paths(verification.paths.begin(), verification.paths.end());
      std::vector<StructuredTaskState::PathEntry> pending;
      for (const auto& p : next.pendingVerification) {
        if (!paths.contains(p.path)) pending.push_back(p);
      }
      next.pendingVerification = std::move(pending);
      std::vector<WriteRecord> writes;
      for (const WriteRecord& w : next.writes) {
        if (paths.contains(w.path)) {
          WriteRecord updated = w;
          updated.verified = true;
          updated.verifiedBy = verification.kind;
          writes.push_back(std::move(updated));
        } else {
          writes.push_back(w);
        }
      }
      next.writes = std::move(writes);
    }

    void applyPlanUpdate(StructuredTaskState& next, const ToolObservation& obs, const std::optional<loops::ResultLike>& result) {
      const json::array* plan = rt::getArray(obs.args, "plan");
      if (!(result && result->ok && plan != nullptr)) return;
      std::vector<std::string> items;
      for (const json::value& item : *plan) {
        if (item.is_string()) items.emplace_back(item.get_string());
      }
      next.plan = StructuredTaskState::Plan{std::move(items), obs.seq};
      next.unresolvedCommands = {};
      next.unresolvedEdits = {};
    }

    void applyPatchObservation(StructuredTaskState& next, const ToolObservation& obs, const ObservationMeta& meta, const std::optional<loops::ResultLike>& result) {
      const std::optional<std::string> path = stringField(obs.args, "path");
      if (result && result->ok && path) {
        upsertWrite(next, obs, meta);
        std::vector<StructuredTaskState::PathEntry> pending;
        for (const auto& p : next.pendingVerification) {
          if (p.path != *path) pending.push_back(p);
        }
        pending.push_back(StructuredTaskState::PathEntry{*path, obs.seq});
        next.pendingVerification = std::move(pending);
      } else if (!isGuardResult(result) && path) {
        std::vector<StructuredTaskState::PathEntry> edits;
        for (const auto& e : next.unresolvedEdits) {
          if (e.path != *path) edits.push_back(e);
        }
        edits.push_back(StructuredTaskState::PathEntry{*path, obs.seq});
        next.unresolvedEdits = std::move(edits);
      }
    }

    void applyReadObservation(StructuredTaskState& next, const ToolObservation& obs, const ObservationMeta& meta, const std::optional<loops::ResultLike>& result) {
      if (!(result && result->ok)) return;
      upsertRead(next, obs);
      if (const std::optional<std::string> readPath = stringField(obs.args, "path")) {
        std::vector<StructuredTaskState::PathEntry> edits;
        for (const auto& e : next.unresolvedEdits) {
          if (e.path != *readPath) edits.push_back(e);
        }
        next.unresolvedEdits = std::move(edits);
      }
      const std::optional<verify::VerificationResolution>& verification = meta.verification;
      if (verification) applyVerificationToWrites(next, *verification);
    }

    void applyShellObservation(StructuredTaskState& next, const ToolObservation& obs, const ObservationMeta& meta, const std::optional<loops::ResultLike>& result) {
      const std::optional<std::string> command = stringField(obs.args, "command");
      if (result && result->ok) {
        next.unresolvedCommands = {};
        const std::optional<verify::VerificationResolution>& verification = meta.verification;
        if (verification) applyVerificationToWrites(next, *verification);
      } else if (command && !isGuardResult(result)) {
        std::vector<StructuredTaskState::CommandEntry> commands;
        for (const auto& c : next.unresolvedCommands) {
          if (c.command != *command) commands.push_back(c);
        }
        commands.push_back(StructuredTaskState::CommandEntry{*command, obs.seq});
        next.unresolvedCommands = std::move(commands);
      }
    }

    /** Routes one observation to the handler for its tool. */
    void applyObservation(StructuredTaskState& next, const ToolObservation& obs, const ObservationMeta& meta, const std::optional<loops::ResultLike>& result) {
      if (obs.tool == "task.update_plan") {
        applyPlanUpdate(next, obs, result);
      } else if (obs.tool == "editor.apply_patch") {
        applyPatchObservation(next, obs, meta, result);
      } else if (obs.tool == "filesystem.read") {
        applyReadObservation(next, obs, meta, result);
      } else if (obs.tool == "shell.exec") {
        applyShellObservation(next, obs, meta, result);
      }
    }

    /**
     * Code-unit ascending order — the exact order `Array.prototype.sort` applies by
     * default, stated explicitly so the canonical contract stays deterministic and
     * locale independent (paths such as `README.md` must keep sorting before
     * `docs/x`, which locale-aware collation would reverse).
     */
    int compareCodeUnits(const std::string& a, const std::string& b) {
      if (rt::lessThan(a, b)) return -1;
      if (rt::lessThan(b, a)) return 1;
      return 0;
    }

    json::array sortedStrings(std::vector<std::string> values) {
      rt::sort(values, compareCodeUnits);
      json::array out;
      for (std::string& value : values) out.emplace_back(std::move(value));
      return out;
    }

    std::string number(double value) { return rt::numberToString(value); }
  }

  StructuredTaskState emptyTaskState() {
    return StructuredTaskState{
      .phase = "planning",
      .modelCalls = 0,
      .toolCalls = 0,
      .invalidCalls = 0,
      .invalidCallStreak = 0,
      .duplicateCalls = 0,
      .semanticLoops = 0,
      .semanticLoopStreak = 0,
      .plan = StructuredTaskState::Plan{{}, std::nullopt},
      .reads = {},
      .writes = {},
      .errors = {},
      .pendingVerification = {},
      .unresolvedCommands = {},
      .unresolvedEdits = {},
      .lastActionSeq = 0,
      .finals = {},
      .finalAttempts = 0,
    };
  }

  std::optional<loops::ResultLike> toToolResult(const rt::Nullable<loops::ResultLike>& result) {
    if (result.isNullish()) return std::nullopt;
    // `value ?? undefined`: a null member becomes undefined.
    const auto orUndefined = [](const rt::Nullable<std::string>& value) { return value.hasValue() ? rt::Nullable<std::string>(*value) : rt::Nullable<std::string>(); };
    return loops::ResultLike{
      .ok = result->ok,
      .error_code = orUndefined(result->error_code),
      .error = orUndefined(result->error),
      .output = orUndefined(result->output),
    };
  }

  std::vector<ObservationMeta> replayMeta(const std::vector<ToolObservation>& observations, const verify::VerificationPolicy& policy) {
    loops::LoopDetector detector;
    verify::VerificationTracker tracker(policy);
    std::vector<ObservationMeta> meta;
    for (const ToolObservation& obs : observations) {
      const loops::ResultLike result = toToolResult(obs.result).value_or(loops::ResultLike{.ok = false, .error_code = "internal", .error = "missing result"});
      const bool isExecuted = obs.valid && !isGuardResult(obs.result.toOptional());
      const loops::LoopFlags flags = detector.observe(obs.tool, obs.args, result);
      std::optional<verify::VerificationResolution> verification = isExecuted ? tracker.observe(obs.tool, obs.args, result) : std::nullopt;
      meta.push_back(ObservationMeta{
        .duplicate = flags.duplicate,
        .semanticLoop = flags.semanticLoop,
        .verification = std::move(verification),
      });
    }
    return meta;
  }

  StructuredTaskState reduceToolObservation(const StructuredTaskState& state, const ToolObservation& obs, const ObservationMeta& meta) {
    StructuredTaskState next = state;
    next.toolCalls = state.toolCalls + 1;
    if (!obs.valid) {
      next.invalidCalls += 1;
      next.invalidCallStreak = state.invalidCallStreak + 1;
    } else {
      next.invalidCallStreak = 0;
    }
    if (meta.duplicate && !meta.duplicate->empty()) next.duplicateCalls += 1;
    if (meta.semanticLoop) {
      next.semanticLoops += 1;
      next.semanticLoopStreak = state.semanticLoopStreak + 1;
    } else {
      next.semanticLoopStreak = 0;
    }

    const std::optional<loops::ResultLike> result = obs.result.toOptional();
    applyObservation(next, obs, meta, result);

    recordError(next, obs);
    if (result && result->ok && !isGuardResult(result)) next.lastActionSeq = obs.seq;
    next.phase = phaseFor(next, obs);
    return next;
  }

  StructuredTaskState reduceFinalAttempt(const StructuredTaskState& state, const FinalObservation& final) {
    std::vector<FinalObservation> finals;
    for (const FinalObservation& f : state.finals) {
      if (f.seq != final.seq) finals.push_back(f);
    }
    finals.push_back(final);
    rt::sort(finals, [](const FinalObservation& a, const FinalObservation& b) { return a.seq - b.seq; });
    TaskPhase phase = "completing";
    if (final.accepted) {
      phase = "done";
    } else if (state.semanticLoopStreak >= 3) {
      phase = "stalled";
    }
    StructuredTaskState next = state;
    next.finals = std::move(finals);
    next.finalAttempts = state.finalAttempts + 1;
    next.phase = std::move(phase);
    return next;
  }

  StructuredTaskState stateFromRun(const ReliabilityRun& run) { return deriveStateWithMeta(run, replayMeta(run.observations)); }

  StructuredTaskState deriveStateWithMeta(const ReliabilityRun& run, const std::vector<ObservationMeta>& meta) {
    if (meta.size() != run.observations.size()) {
      throw rt::JsError("meta and observations must have the same length");
    }
    StructuredTaskState state = emptyTaskState();
    state.modelCalls = run.modelCalls;
    for (std::size_t index = 0; index < run.observations.size(); ++index) {
      state = reduceToolObservation(state, run.observations[index], meta[index]);
    }
    for (const FinalObservation& final : run.finals) state = reduceFinalAttempt(state, final);
    return state;
  }

  std::string stateContract(const StructuredTaskState& state) {
    json::object plan;
    json::array items;
    for (const std::string& item : state.plan.items) items.emplace_back(item);
    plan["items"] = std::move(items);
    plan["updated"] = state.plan.seq.has_value();
    json::array reads;
    for (const ReadRecord& r : state.reads) {
      json::object entry;
      entry["path"] = r.path;
      entry["digest"] = r.digest;
      reads.emplace_back(std::move(entry));
    }
    json::array writes;
    for (const WriteRecord& w : state.writes) {
      json::object entry;
      entry["path"] = w.path;
      entry["marker"] = w.marker;
      entry["add"] = w.add;
      entry["verified"] = w.verified;
      entry["verifiedBy"] = w.verifiedBy ? json::value(*w.verifiedBy) : json::value(nullptr);
      writes.emplace_back(std::move(entry));
    }
    json::array errors;
    for (const StateErrorRecord& e : state.errors) {
      json::object entry;
      entry["code"] = e.code;
      entry["tool"] = e.tool;
      entry["digest"] = e.digest;
      errors.emplace_back(std::move(entry));
    }
    std::vector<std::string> pendingPaths;
    for (const auto& p : state.pendingVerification) pendingPaths.push_back(p.path);
    std::vector<std::string> commands;
    for (const auto& c : state.unresolvedCommands) commands.push_back(c.command);
    std::vector<std::string> editPaths;
    for (const auto& e : state.unresolvedEdits) editPaths.push_back(e.path);
    json::array finals;
    for (const FinalObservation& f : state.finals) {
      json::object entry;
      entry["content"] = f.content;
      entry["accepted"] = f.accepted;
      finals.emplace_back(std::move(entry));
    }
    json::object projection;
    projection["phase"] = state.phase;
    projection["plan"] = std::move(plan);
    projection["reads"] = std::move(reads);
    projection["writes"] = std::move(writes);
    projection["errors"] = std::move(errors);
    projection["pendingVerification"] = sortedStrings(std::move(pendingPaths));
    projection["unresolvedCommands"] = sortedStrings(std::move(commands));
    projection["unresolvedEdits"] = sortedStrings(std::move(editPaths));
    projection["lastActionHadSuccess"] = state.lastActionSeq > 0;
    projection["finalAttempts"] = rt::makeNumber(state.finalAttempts);
    projection["finals"] = std::move(finals);
    return rt::jsonStringify(projection);
  }

  std::string renderStateSummary(const StructuredTaskState& state) {
    const std::string planLabel = !state.plan.seq ? "not set" : number(static_cast<double>(state.plan.items.size())) + " item(s)";
    const auto renderWrite = [](const WriteRecord& w) -> std::string {
      const std::string verificationLabel = w.verified ? " (verified by " + w.verifiedBy.value_or("null") + ")" : " (unverified)";
      return w.path + verificationLabel;
    };
    std::string writesLabel = "none";
    if (!state.writes.empty()) {
      writesLabel.clear();
      for (std::size_t index = 0; index < state.writes.size(); ++index) writesLabel += (index > 0 ? "; " : "") + renderWrite(state.writes[index]);
    }
    std::string pendingLabel = "none";
    if (!state.pendingVerification.empty()) {
      pendingLabel.clear();
      for (std::size_t index = 0; index < state.pendingVerification.size(); ++index) pendingLabel += (index > 0 ? ", " : "") + state.pendingVerification[index].path;
    }
    std::string failuresLabel = "none";
    if (!state.errors.empty()) {
      failuresLabel.clear();
      for (std::size_t index = 0; index < state.errors.size(); ++index) {
        failuresLabel += (index > 0 ? ", " : "") + state.errors[index].tool + ":" + state.errors[index].code;
      }
    }
    const std::string loopLabel = state.semanticLoopStreak >= 3 ? "active (streak " + number(state.semanticLoopStreak) + ")" : "none";
    const std::vector<std::string> lines{
      "phase: " + state.phase,
      "model calls: " + number(state.modelCalls),
      "tool calls: " + number(state.toolCalls) + " (invalid: " + number(state.invalidCalls) + ", duplicates: " + number(state.duplicateCalls) + ")",
      "plan: " + planLabel,
      "writes: " + writesLabel,
      "pending verification: " + pendingLabel,
      "unresolved: " + number(static_cast<double>(state.unresolvedCommands.size())) + " command(s), " + number(static_cast<double>(state.unresolvedEdits.size())) + " edit(s)",
      "recent failures: " + failuresLabel,
      "loop: " + loopLabel,
    };
    std::string out;
    for (std::size_t index = 0; index < lines.size(); ++index) out += (index > 0 ? "\n" : "") + lines[index];
    return out;
  }
}
