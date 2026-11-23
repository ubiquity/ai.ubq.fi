// Port of tests/reliability-state.test.ts.
#include "uos/harmony/reliability/state.hpp"
#include "uos/harmony/reliability/verify.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

#include <string>
#include <vector>

namespace {
  using namespace uos;
  using namespace uos::harmony::reliability::state;
  using uos::harmony::reliability::loops::ResultLike;
  namespace verify = uos::harmony::reliability::verify;

  ToolObservation read(double seq, const std::string& path, const std::string& output) {
    return ToolObservation{
      .seq = seq,
      .tool = "filesystem.read",
      .args = json::object{{"path", path}},
      .valid = true,
      .result = ResultLike{.ok = true, .output = output},
    };
  }
  ToolObservation patch(double seq, const std::string& path, const std::string& oldText, const std::string& newText, bool ok = true) {
    return ToolObservation{
      .seq = seq,
      .tool = "editor.apply_patch",
      .args = json::object{{"path", path}, {"old", oldText}, {"new", newText}},
      .valid = true,
      .result = ok ? ResultLike{.ok = true, .output = "patched " + path} : ResultLike{.ok = false, .error_code = "patch_failed", .error = "no match"},
    };
  }
  ToolObservation exec(double seq, const std::string& command, bool ok) {
    return ToolObservation{
      .seq = seq,
      .tool = "shell.exec",
      .args = json::object{{"command", command}},
      .valid = true,
      .result = ok ? ResultLike{.ok = true, .output = "ok"} : ResultLike{.ok = false, .error_code = "exec_failed", .error = "exit 1"},
    };
  }

  verify::VerificationPolicy policy() {
    verify::VerificationPolicy value = verify::DEFAULT_VERIFICATION_POLICY;
    value.verificationCommand = "sh tests/run.sh";
    return value;
  }
}

TEST("state: a verified edit run reaches phase done and a stable contract") {
  const ReliabilityRun run{
    .observations =
      {
        read(1, "src/a.txt", "x=0"),
        patch(2, "src/a.txt", "x=0", "x=1"),
        read(3, "src/a.txt", "x=1"),
        ToolObservation{
          .seq = 4,
          .tool = "task.update_plan",
          .args = json::object{{"plan", json::array{"read", "edit", "verify"}}},
          .valid = true,
          .result = ResultLike{.ok = true, .output = "plan updated (3 items)"},
        },
      },
    .finals = {FinalObservation{.content = "done", .accepted = true, .seq = 5}},
    .modelCalls = 6,
  };
  const StructuredTaskState state = stateFromRun(run);
  EXPECT_EQ(state.phase, "done");
  EXPECT_EQ(state.writes.size(), 1u);
  EXPECT_EQ(state.writes[0].verified, true);
  EXPECT_EQ(state.pendingVerification.size(), 0u);
  EXPECT_EQ(state.plan.items.size(), 3u);
  EXPECT_EQ(state.modelCalls, 6);
  const StructuredTaskState copy = state;
  EXPECT_EQ(stateContract(state), stateContract(copy));
}

TEST("state: unverified writes and unresolved failures surface in the contract") {
  const ReliabilityRun run{
    .observations = {patch(1, "answer.txt", "", "answer", true), exec(2, "curl https://example.invalid", false)},
    .finals = {FinalObservation{.content = "done", .accepted = false, .seq = 3}},
    .modelCalls = 2,
  };
  const StructuredTaskState state = stateFromRun(run);
  EXPECT_EQ(state.phase, "completing");
  EXPECT_EQ(state.pendingVerification.size(), 1u);
  EXPECT_EQ(state.unresolvedCommands.size(), 1u);
  EXPECT_EQ(state.finalAttempts, 1);
  const std::string contract = stateContract(state);
  EXPECT(rt::Regex("\"pendingVerification\":\\[\"answer\\.txt\"\\]").test(contract));
}

TEST("state: the declared verification command satisfies pending writes in the replay") {
  const ReliabilityRun run{
    .observations = {patch(1, "a.txt", "", "1", true), exec(2, "sh tests/run.sh", true)},
    .finals = {},
    .modelCalls = 0,
  };
  const std::vector<ObservationMeta> meta = replayMeta(run.observations, policy());
  const StructuredTaskState state = deriveStateWithMeta(run, meta);
  EXPECT_EQ(state.writes[0].verified, true);
  EXPECT_EQ(state.writes[0].verifiedBy, std::optional<std::string>("shell"));
  EXPECT_EQ(state.pendingVerification.size(), 0u);
}

TEST("state: semantic loops stall the phase and count in the contract") {
  std::vector<ToolObservation> observations;
  double seq = 0;
  for (int i = 0; i < 8; i++) {
    observations.push_back(read(++seq, "x.txt", "same"));
    observations.push_back(exec(++seq, "pwd", true));
  }
  const StructuredTaskState state = stateFromRun(ReliabilityRun{.observations = observations, .finals = {}, .modelCalls = 0});
  // Effect repetition of the read closes a loop; the streak persists while
  // the alternating pattern repeats.
  EXPECT(state.semanticLoops >= 1);  // "expected a semantic loop, got 0"
  if (!(state.semanticLoopStreak >= 1)) EXPECT(("expected streak, got " + rt::numberToString(state.semanticLoopStreak)).empty());
  EXPECT_EQ(state.phase, "stalled");
  EXPECT(state.semanticLoops >= 1);  // "loop counters live on the structured state"
  // Diagnostic counters are excluded from the durable-state contract by design.
  const std::string contract = stateContract(state);
  EXPECT_EQ(rt::includes(contract, "semanticLoops"), false);
}

TEST("state: invalid calls never produce verification or write records") {
  const ReliabilityRun run{
    .observations =
      {
        ToolObservation{
          .seq = 1,
          .tool = "filesystem.read",
          .args = json::object{{"path", 42}},
          .valid = false,
          .result = ResultLike{.ok = false, .error_code = "invalid_args", .error = "invalid arguments (1 issue(s))"},
        },
        ToolObservation{.seq = 2, .tool = "filesystem.read", .args = json::object{{"path", "a.txt"}}, .valid = true, .result = ResultLike{.ok = true, .output = "data"}},
      },
    .finals = {},
    .modelCalls = 1,
  };
  const StructuredTaskState state = stateFromRun(run);
  EXPECT_EQ(state.invalidCalls, 1);
  EXPECT_EQ(state.invalidCallStreak, 0);  // trailing valid call resets it
  EXPECT_EQ(state.reads.size(), 1u);
}

TEST("state: reduceFinalAttempt sorts finals and marks done") {
  StructuredTaskState state = emptyTaskState();
  state = reduceFinalAttempt(state, FinalObservation{.content = "one", .accepted = false, .seq = 5});
  state = reduceFinalAttempt(state, FinalObservation{.content = "two", .accepted = true, .seq = 6});
  EXPECT_EQ(state.phase, "done");
  EXPECT_EQ(state.finalAttempts, 2);
  EXPECT_EQ(state.finals[0].content, "one");
  EXPECT_EQ(state.finals[1].accepted, true);
}

TEST("state: replayed duplicates are counted from the observation stream") {
  const ReliabilityRun run{
    .observations = {read(1, "x", "v"), read(2, "x", "v")},
    .finals = {},
    .modelCalls = 0,
  };
  const StructuredTaskState state = stateFromRun(run);
  EXPECT_EQ(state.duplicateCalls, 1);
}
