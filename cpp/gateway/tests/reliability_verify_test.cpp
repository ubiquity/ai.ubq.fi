// Port of tests/reliability-verify.test.ts.
#include "uos/harmony/reliability/verify.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace {
  using namespace uos;
  using namespace uos::harmony::reliability::verify;
  using uos::harmony::reliability::loops::ResultLike;

  ResultLike patchOk(const std::string& path) {
    return ResultLike{
      .ok = true,
      .output = "patched " + path,
    };
  }
  ResultLike readOk(std::string content) { return ResultLike{.ok = true, .output = std::move(content)}; }
  ResultLike execOk() { return ResultLike{.ok = true, .output = "done"}; }
  ResultLike execFail() { return ResultLike{.ok = false, .error_code = "exec_failed", .error = "exit 1", .output = ""}; }

  // `Partial<Parameters<typeof guardFinal>[0]>`
  struct GuardOptions {
    std::optional<std::string> finalContent = std::nullopt;
    std::optional<double> lastActionSeq = std::nullopt;
    std::optional<std::vector<FinalAttempt>> previousFinals = std::nullopt;
    std::optional<double> semanticLoopStreak = std::nullopt;
    std::optional<bool> planUpdated = std::nullopt;
    std::optional<double> writes = std::nullopt;
  };

  FinalGuardDecision guard(const VerificationPolicy& policy, const VerificationTracker& tracker, const GuardOptions& opts = {}) {
    const std::vector<FinalAttempt> previousFinals = opts.previousFinals.value_or(std::vector<FinalAttempt>{});
    return guardFinal(FinalGuardInput{
      .finalContent = opts.finalContent.value_or("done"),
      .lastActionSeq = opts.lastActionSeq.value_or(0),
      .previousFinals = previousFinals,
      .semanticLoopStreak = opts.semanticLoopStreak.value_or(0),
      .planUpdated = opts.planUpdated.value_or(true),
      .writes = opts.writes.value_or(0),
      .tracker = tracker,
      .policy = policy,
    });
  }

  bool hasKind(const std::vector<FinalRequirement>& requirements, const std::string& kind) {
    return std::any_of(requirements.begin(), requirements.end(), [&](const FinalRequirement& r) { return r.kind == kind; });
  }
}

TEST("verify: a patch creates a pending verification satisfied by a later read with the marker") {
  VerificationTracker tracker;
  tracker.observe("editor.apply_patch", json::object{{"path", "a.txt"}, {"old", "x"}, {"new", "y"}}, patchOk("a.txt"));
  EXPECT_EQ(tracker.pending().size(), 1u);
  const auto resolution = tracker.observe("filesystem.read", json::object{{"path", "a.txt"}}, readOk("y now here"));
  EXPECT(resolution == (VerificationResolution{"read", {"a.txt"}}));
  EXPECT_EQ(tracker.allVerified(), true);
}

TEST("verify: a read without the marker does not satisfy a pending write") {
  VerificationTracker tracker;
  tracker.observe("editor.apply_patch", json::object{{"path", "a.txt"}, {"old", "x"}, {"new", "y"}}, patchOk("a.txt"));
  EXPECT(tracker.observe("filesystem.read", json::object{{"path", "a.txt"}}, readOk("nothing relevant")) == std::nullopt);
  EXPECT_EQ(tracker.pending().size(), 1u);
}

TEST("verify: the declared verification command satisfies all pending writes") {
  VerificationPolicy policy = DEFAULT_VERIFICATION_POLICY;
  policy.verificationCommand = "sh tests/run.sh";
  VerificationTracker tracker(policy);
  tracker.observe("editor.apply_patch", json::object{{"path", "a.txt"}, {"add", true}, {"new", "1"}}, patchOk("a.txt"));
  tracker.observe("editor.apply_patch", json::object{{"path", "b.txt"}, {"add", true}, {"new", "2"}}, patchOk("b.txt"));
  const auto resolution = tracker.observe("shell.exec", json::object{{"command", "sh tests/run.sh"}}, execOk());
  EXPECT(resolution == (VerificationResolution{"shell", {"a.txt", "b.txt"}}));
  EXPECT_EQ(tracker.allVerified(), true);
}

TEST("verify: a failed shell command is unresolved until a later successful command") {
  VerificationTracker tracker;
  tracker.observe("shell.exec", json::object{{"command", "grep -z nope file"}}, execFail());
  EXPECT_EQ(tracker.unresolvedCommands().size(), 1u);
  tracker.observe("shell.exec", json::object{{"command", "true"}}, execOk());
  EXPECT_EQ(tracker.unresolvedCommands().size(), 0u);
}

TEST("verify: a failed patch is unresolved until the same path is written successfully") {
  VerificationTracker tracker;
  tracker.observe("editor.apply_patch", json::object{{"path", "a.txt"}, {"old", "missing"}, {"new", "y"}}, ResultLike{.ok = false, .error_code = "patch_failed", .error = "old text not found"});
  EXPECT_EQ(tracker.unresolvedEdits().size(), 1u);
  tracker.observe("editor.apply_patch", json::object{{"path", "a.txt"}, {"add", true}, {"new", "y"}}, patchOk("a.txt"));
  EXPECT_EQ(tracker.unresolvedEdits().size(), 0u);
}

TEST("verify: guard rejects a final with unverified writes and lists the requirement") {
  VerificationTracker tracker;
  tracker.observe("editor.apply_patch", json::object{{"path", "answer.txt"}, {"add", true}, {"new", "42"}}, patchOk("answer.txt"));
  const FinalGuardDecision decision = guard(DEFAULT_VERIFICATION_POLICY, tracker);
  EXPECT_EQ(decision.allowed, false);
  ASSERT(!decision.requirements.empty());
  EXPECT_EQ(decision.requirements[0].kind, "unverified_write");
  EXPECT_EQ(decision.requirements[0].path, std::optional<std::string>("answer.txt"));
  EXPECT(rt::Regex("unverified write: answer\\.txt").test(renderGuardRequirements(decision.requirements)));
}

TEST("verify: guard allows a final when everything is verified") {
  VerificationTracker tracker;
  tracker.observe("editor.apply_patch", json::object{{"path", "a.txt"}, {"add", true}, {"new", "42"}}, patchOk("a.txt"));
  tracker.observe("filesystem.read", json::object{{"path", "a.txt"}}, readOk("42"));
  const FinalGuardDecision decision = guard(DEFAULT_VERIFICATION_POLICY, tracker);
  EXPECT_EQ(decision.allowed, true);
  EXPECT(decision.requirements.empty());
}

TEST("verify: unresolved command and edit failures block the final") {
  VerificationTracker tracker;
  tracker.observe("shell.exec", json::object{{"command", "curl example"}}, execFail());
  tracker.observe("editor.apply_patch", json::object{{"path", "b.txt"}, {"old", "nope"}, {"new", "y"}}, ResultLike{.ok = false, .error_code = "patch_failed", .error = "not found"});
  const FinalGuardDecision decision = guard(DEFAULT_VERIFICATION_POLICY, tracker);
  EXPECT_EQ(decision.allowed, false);
  EXPECT(hasKind(decision.requirements, "unresolved_command"));
  EXPECT(hasKind(decision.requirements, "unresolved_edit"));
}

TEST("verify: an active semantic loop blocks the final") {
  VerificationTracker tracker;
  const FinalGuardDecision decision = guard(DEFAULT_VERIFICATION_POLICY, tracker, GuardOptions{.semanticLoopStreak = 3});
  EXPECT_EQ(decision.allowed, false);
  EXPECT(hasKind(decision.requirements, "active_loop"));
}

TEST("verify: a repeated final with no intervening action is a false completion") {
  VerificationTracker tracker;
  const VerificationPolicy& policy = DEFAULT_VERIFICATION_POLICY;
  const FinalGuardDecision first = guard(policy, tracker);  // rejected (nothing to verify here? pending empty => allowed)
  EXPECT_EQ(first.allowed, true);
  EXPECT_EQ(first.attempt.repetitions, 0);
  // Build the repeated-final case explicitly: first attempt rejected, same seq.
  const FinalAttempt rejected{.content = "done", .rejected = true, .lastActionSeq = 0, .repetitions = 0};
  const FinalGuardDecision second = guard(policy, tracker, GuardOptions{.lastActionSeq = 0, .previousFinals = std::vector<FinalAttempt>{rejected}});
  EXPECT_EQ(second.allowed, false);
  EXPECT_EQ(second.falseCompletion, true);
  EXPECT(hasKind(second.requirements, "false_completion"));
  EXPECT_EQ(second.attempt.repetitions, 1);
}

TEST("verify: requeued writes supersede earlier pending state per path") {
  VerificationTracker tracker;
  tracker.observe("editor.apply_patch", json::object{{"path", "a.txt"}, {"old", "x"}, {"new", "y"}}, patchOk("a.txt"));
  tracker.observe("editor.apply_patch", json::object{{"path", "a.txt"}, {"old", "y"}, {"new", "z"}}, patchOk("a.txt"));
  EXPECT_EQ(tracker.pending().size(), 1u);
  EXPECT_EQ(tracker.pending()[0].marker, "z");
}

TEST("verify: the guard prefix is stable") { EXPECT_EQ(GUARD_PREFIX, "[guard] final answer rejected"); }
