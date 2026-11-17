// Differential cases for the reliability rule engines (loops.ts, verify.ts,
// state.ts) on inputs the ported bun tests do not cover: JS key ordering and
// `__proto__` in canonical arguments, fractional/zero/negative detector
// options, exact snapshot/contract/summary text. Expected values were recorded
// from the TypeScript modules under Bun 1.4.0 (Date.now() fixed at 1234).
#include "uos/harmony/reliability/hash.hpp"
#include "uos/harmony/reliability/loops.hpp"
#include "uos/harmony/reliability/state.hpp"
#include "uos/harmony/reliability/verify.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"

#include <functional>
#include <string>
#include <vector>

namespace {
  using namespace uos;
  namespace loops = uos::harmony::reliability::loops;
  namespace verify = uos::harmony::reliability::verify;
  namespace state = uos::harmony::reliability::state;

  json::value flagsJson(const loops::LoopFlags& flags) {
    json::object out;
    out["duplicate"] = flags.duplicate ? json::value(*flags.duplicate) : json::value(nullptr);
    out["semanticLoop"] = flags.semanticLoop;
    out["loopKind"] = flags.loopKind ? json::value(*flags.loopKind) : json::value(nullptr);
    out["streak"] = rt::makeNumber(flags.streak);
    return out;
  }

  json::value resolutionJson(const std::optional<verify::VerificationResolution>& resolution) {
    if (!resolution) return nullptr;
    json::array paths;
    for (const auto& path : resolution->paths) paths.emplace_back(path);
    return json::object{{"kind", resolution->kind}, {"paths", paths}};
  }

  json::value requirementJson(const verify::FinalRequirement& requirement) {
    json::object out{{"kind", requirement.kind}, {"message", requirement.message}};
    if (requirement.path) out["path"] = *requirement.path;
    if (requirement.command) out["command"] = *requirement.command;
    return out;
  }

  json::value stateJson(const state::StructuredTaskState& value) {
    json::array items;
    for (const auto& item : value.plan.items) items.emplace_back(item);
    json::array reads;
    for (const auto& r : value.reads) reads.emplace_back(json::object{{"path", r.path}, {"digest", r.digest}, {"seq", rt::makeNumber(r.seq)}});
    json::array writes;
    for (const auto& w : value.writes) {
      writes.emplace_back(json::object{{"path", w.path}, {"marker", w.marker}, {"add", w.add}, {"verified", w.verified}, {"verifiedBy", w.verifiedBy ? json::value(*w.verifiedBy) : json::value(nullptr)}, {"seq", rt::makeNumber(w.seq)}});
    }
    json::array errors;
    for (const auto& e : value.errors) errors.emplace_back(json::object{{"code", e.code}, {"tool", e.tool}, {"seq", rt::makeNumber(e.seq)}, {"digest", e.digest}});
    json::array pending;
    for (const auto& p : value.pendingVerification) pending.emplace_back(json::object{{"path", p.path}, {"seq", rt::makeNumber(p.seq)}});
    json::array commands;
    for (const auto& c : value.unresolvedCommands) commands.emplace_back(json::object{{"command", c.command}, {"seq", rt::makeNumber(c.seq)}});
    json::array edits;
    for (const auto& e : value.unresolvedEdits) edits.emplace_back(json::object{{"path", e.path}, {"seq", rt::makeNumber(e.seq)}});
    json::array finals;
    for (const auto& f : value.finals) finals.emplace_back(json::object{{"content", f.content}, {"accepted", f.accepted}, {"seq", rt::makeNumber(f.seq)}});
    return json::object{
      {"phase", value.phase},
      {"modelCalls", rt::makeNumber(value.modelCalls)},
      {"toolCalls", rt::makeNumber(value.toolCalls)},
      {"invalidCalls", rt::makeNumber(value.invalidCalls)},
      {"invalidCallStreak", rt::makeNumber(value.invalidCallStreak)},
      {"duplicateCalls", rt::makeNumber(value.duplicateCalls)},
      {"semanticLoops", rt::makeNumber(value.semanticLoops)},
      {"semanticLoopStreak", rt::makeNumber(value.semanticLoopStreak)},
      {"plan", json::object{{"items", items}, {"seq", value.plan.seq ? rt::makeNumber(*value.plan.seq) : json::value(nullptr)}}},
      {"reads", reads},
      {"writes", writes},
      {"errors", errors},
      {"pendingVerification", pending},
      {"unresolvedCommands", commands},
      {"unresolvedEdits", edits},
      {"lastActionSeq", rt::makeNumber(value.lastActionSeq)},
      {"finals", finals},
      {"finalAttempts", rt::makeNumber(value.finalAttempts)},
    };
  }

  void expectJson(const json::value& actual, const char* expected) {
    if (!rt::deepEqual(actual, rt::jsonParse(expected))) EXPECT_EQ(rt::jsonStringify(actual), expected);
  }

  struct FixedNow {
    std::function<double()> saved = rt::globalDateNow();
    FixedNow() {
      rt::globalDateNow() = [] { return 1234.0; };
    }
    ~FixedNow() { rt::globalDateNow() = saved; }
  };
}

TEST("differential: canonical arguments follow JS key order, localeCompare and [[Set]]") {
  EXPECT_EQ(loops::canonicalArgs(rt::jsonParse(R"({"b":1,"10":2,"9":3,"a":{"z":[3,{"y":1,"x":2}],"2":null},"B":true,"_":"u","é":1,"e":2})").as_object()),
            R"({"9":3,"10":2,"_":"u","a":{"2":null,"z":[3,{"x":2,"y":1}]},"b":1,"B":true,"e":2,"é":1})");
  EXPECT_EQ(loops::canonicalArgs(rt::jsonParse(R"({"__proto__": 1, "x": {"__proto__": {"a": 1}}})").as_object()), R"({"x":{}})");
  EXPECT_EQ(loops::canonicalArgs(rt::jsonParse(R"({"a-b":1,"a_b":2,"ab":3,"A":4,"a":5,"1a":6,"-1":7,"01":8})").as_object()),
            R"({"-1":7,"01":8,"1a":6,"a":5,"A":4,"a_b":2,"a-b":1,"ab":3})");
}

TEST("differential: identities, effect digests and signatures") {
  EXPECT_EQ(rt::jsonQuote(loops::callIdentity("t", json::object{{"q", "v"}})), R"("t\u0000{\"q\":\"v\"}")");
  EXPECT_EQ(loops::effectDigest(loops::ResultLike{.ok = true, .output = "x"}), "ok:af63f54c8602");
  EXPECT_EQ(loops::effectDigest(loops::ResultLike{.ok = true}), "ok:cbf29ce48422");
  EXPECT_EQ(loops::effectDigest(loops::ResultLike{.ok = false, .error = "bad"}), "fail:none:00391e191339");
  EXPECT_EQ(loops::effectDigest(loops::ResultLike{.ok = false}), "fail:none:cbf29ce48422");
  EXPECT_EQ(rt::jsonQuote(loops::effectSignature("t", json::object{{"a", 1}}, loops::ResultLike{.ok = false, .error_code = "c"})), R"("t\u0000{\"a\":1}\u0000fail:c:cbf29ce48422")");
}

TEST("differential: detector options at zero, fractional and negative values") {
  loops::LoopDetector zero(loops::LoopDetectorOptions{.window = 0, .patternLength = 0});
  expectJson(flagsJson(zero.observe("t", json::object{}, loops::ResultLike{.ok = true})), R"({"duplicate":null,"semanticLoop":true,"loopKind":"pattern_recurrence","streak":1})");
  expectJson(flagsJson(zero.observe("t", json::object{}, loops::ResultLike{.ok = true})), R"({"duplicate":"repeat_after_success","semanticLoop":true,"loopKind":"pattern_recurrence","streak":2})");

  loops::LoopDetector fractional(loops::LoopDetectorOptions{.window = 3, .patternLength = 1.5, .effectRepeatThreshold = 2.5});
  const char* expected[] = {
    R"({"duplicate":null,"semanticLoop":false,"loopKind":null,"streak":0})",
    R"({"duplicate":null,"semanticLoop":false,"loopKind":null,"streak":0})",
    R"({"duplicate":null,"semanticLoop":true,"loopKind":"pattern_recurrence","streak":1})",
    R"({"duplicate":null,"semanticLoop":true,"loopKind":"pattern_recurrence","streak":2})",
    R"({"duplicate":null,"semanticLoop":true,"loopKind":"pattern_recurrence","streak":3})",
  };
  for (int i = 1; i <= 5; ++i) {
    const loops::LoopFlags flags = fractional.observe("t", json::object{{"i", i % 2}}, loops::ResultLike{.ok = true, .output = rt::numberToString(i % 2)});
    expectJson(flagsJson(flags), expected[i - 1]);
  }

  loops::LoopDetector negative(loops::LoopDetectorOptions{.window = -1});
  EXPECT_THROWS_MESSAGE(negative.observe("t", json::object{}, loops::ResultLike{.ok = true}), rt::JsError,
                        "loop detector: cannot evict a call identity from an empty history (window must not be negative)");

  EXPECT_EQ(loops::renderLoopFeedback(loops::LoopFlags{.duplicate = std::nullopt, .semanticLoop = true, .loopKind = "pattern_recurrence", .streak = 1}),
            "semantic loop detected (the same call sequence keeps repeating); take a materially different action or verify the result — repeating this action will not change the outcome");
  EXPECT_EQ(loops::renderLoopFeedback(loops::LoopFlags{.duplicate = std::nullopt, .semanticLoop = true, .loopKind = std::nullopt, .streak = 1}),
            "semantic loop detected (the same action produces the same result); take a materially different action or verify the result — repeating this action will not change the outcome");
}

TEST("differential: tracker snapshots, the final guard and its rendering") {
  FixedNow now;
  verify::VerificationPolicy trimmed = verify::DEFAULT_VERIFICATION_POLICY;
  trimmed.verificationCommand = "  make check  ";
  verify::VerificationTracker tracker(trimmed);
  tracker.observe("editor.apply_patch", json::object{{"path", "b.txt"}, {"new", "B"}, {"add", true}}, loops::ResultLike{.ok = true});
  tracker.observe("editor.apply_patch", json::object{{"path", "a.txt"}, {"new", "A"}}, loops::ResultLike{.ok = true});
  tracker.observe("editor.apply_patch", json::object{{"path", "c.txt"}}, loops::ResultLike{.ok = false, .error_code = "patch_failed"});
  tracker.observe("shell.exec", json::object{{"command", "zz"}}, loops::ResultLike{.ok = false});
  tracker.observe("shell.exec", json::object{{"command", "aa"}}, loops::ResultLike{.ok = false, .error_code = "exec_failed"});
  expectJson(tracker.snapshot(), R"({"pending":[{"path":"a.txt","marker":"A","add":false,"since":1234},{"path":"b.txt","marker":"B","add":true,"since":1234}],"unresolvedCommands":[{"command":"aa","code":"exec_failed","since":1234},{"command":"zz","code":"failure","since":1234}],"unresolvedEdits":[{"path":"c.txt","since":1234}],"totals":{"required":2,"satisfied":0}})");

  verify::VerificationPolicy planned = verify::DEFAULT_VERIFICATION_POLICY;
  planned.requirePlanBeforeWrites = true;
  const verify::FinalGuardDecision decision = verify::guardFinal(verify::FinalGuardInput{
    .finalContent = "x",
    .lastActionSeq = 2,
    .previousFinals = {verify::FinalAttempt{.content = "x", .rejected = true, .lastActionSeq = 2, .repetitions = 4}, verify::FinalAttempt{.content = "y", .rejected = true, .lastActionSeq = 2, .repetitions = 9}},
    .semanticLoopStreak = 5,
    .planUpdated = false,
    .writes = 2,
    .tracker = tracker,
    .policy = planned,
  });
  json::array requirements;
  for (const auto& requirement : decision.requirements) requirements.push_back(requirementJson(requirement));
  expectJson(json::object{
               {"allowed", decision.allowed},
               {"requirements", requirements},
               {"falseCompletion", decision.falseCompletion},
               {"attempt", json::object{{"content", decision.attempt.content}, {"rejected", decision.attempt.rejected}, {"lastActionSeq", rt::makeNumber(decision.attempt.lastActionSeq)}, {"repetitions", rt::makeNumber(decision.attempt.repetitions)}}},
             },
             R"({"allowed":false,"requirements":[{"kind":"unverified_write","message":"unverified write: b.txt — verify it before answering","path":"b.txt"},{"kind":"unverified_write","message":"unverified write: a.txt — verify it before answering","path":"a.txt"},{"kind":"unresolved_command","message":"unresolved command failure: zz — recover with a successful command before answering","command":"zz"},{"kind":"unresolved_command","message":"unresolved command failure: aa — recover with a successful command before answering","command":"aa"},{"kind":"unresolved_edit","message":"unresolved edit: c.txt — apply the change or verify the path before answering","path":"c.txt"},{"kind":"active_loop","message":"active semantic loop — take a materially different action before answering"},{"kind":"false_completion","message":"final answer repeated without any intervening action — the previous claim was rejected; perform a verification or a different action first"},{"kind":"plan_required","message":"writes happened without a plan — call task.update_plan before answering"}],"falseCompletion":true,"attempt":{"content":"x","rejected":true,"lastActionSeq":2,"repetitions":5}})");
  EXPECT_EQ(verify::renderGuardRequirements(decision.requirements),
            "[unverified_write] unverified write: b.txt — verify it before answering; [unverified_write] unverified write: a.txt — verify it before answering; [unresolved_command] unresolved command failure: zz — recover with a successful command before answering; [unresolved_command] unresolved command failure: aa — recover with a successful command before answering; [unresolved_edit] unresolved edit: c.txt — apply the change or verify the path before answering; [active_loop] active semantic loop — take a materially different action before answering; [false_completion] final answer repeated without any intervening action — the previous claim was rejected; perform a verification or a different action first; [plan_required] writes happened without a plan — call task.update_plan before answering");

  expectJson(resolutionJson(tracker.observe("shell.exec", json::object{{"command", "make check"}}, loops::ResultLike{.ok = true})), R"({"kind":"shell","paths":["b.txt","a.txt"]})");
  expectJson(tracker.snapshot(), R"({"pending":[],"unresolvedCommands":[],"unresolvedEdits":[{"path":"c.txt","since":1234}],"totals":{"required":2,"satisfied":2}})");
}

TEST("differential: replayed meta, derived state, contract and summary") {
  using state::ToolObservation;
  const state::ReliabilityRun run{
    .observations =
      {
        ToolObservation{.seq = 1, .tool = "filesystem.read", .args = json::object{{"path", "README.md"}}, .valid = true, .result = loops::ResultLike{.ok = true, .output = "hello"}},
        ToolObservation{.seq = 2, .tool = "filesystem.read", .args = json::object{{"path", "docs/x"}}, .valid = true, .result = loops::ResultLike{.ok = true, .output = "doc"}},
        ToolObservation{.seq = 3, .tool = "editor.apply_patch", .args = json::object{{"path", "docs/x"}, {"old", "doc"}, {"new", "DOC"}}, .valid = true, .result = loops::ResultLike{.ok = true, .output = "patched"}},
        ToolObservation{.seq = 4, .tool = "editor.apply_patch", .args = json::object{{"path", "Zed"}, {"new", "z"}}, .valid = true, .result = loops::ResultLike{.ok = false, .error_code = "patch_failed", .error = "nope"}},
        ToolObservation{.seq = 5, .tool = "shell.exec", .args = json::object{{"command", "false"}}, .valid = true, .result = loops::ResultLike{.ok = false, .error_code = "exec_failed", .error = "exit 1"}},
        ToolObservation{.seq = 6, .tool = "shell.exec", .args = json::object{{"command", "false"}}, .valid = true, .result = loops::ResultLike{.ok = false, .error_code = "duplicate_call", .error = "dup"}},
        ToolObservation{.seq = 7, .tool = "task.update_plan", .args = json::object{{"plan", json::array{"a", 1, "b"}}}, .valid = true, .result = loops::ResultLike{.ok = true}},
        ToolObservation{.seq = 8, .tool = "shell.exec", .args = json::object{{"command", "cat docs/x"}}, .valid = true, .result = loops::ResultLike{.ok = true, .output = "DOC"}},
        ToolObservation{.seq = 9, .tool = "filesystem.read", .args = json::object{{"path", 42}}, .valid = false, .result = nullptr},
        ToolObservation{.seq = 10, .tool = "editor.apply_patch", .args = json::object{{"path", "a"}}, .valid = true},
      },
    .finals = {state::FinalObservation{.content = "f2", .accepted = false, .seq = 12}, state::FinalObservation{.content = "f1", .accepted = false, .seq = 11}},
    .modelCalls = 3,
  };
  const std::vector<state::ObservationMeta> meta = state::replayMeta(run.observations);
  json::array metaJson;
  for (const auto& entry : meta) {
    metaJson.emplace_back(json::object{{"duplicate", entry.duplicate ? json::value(*entry.duplicate) : json::value(nullptr)}, {"semanticLoop", entry.semanticLoop}, {"verification", resolutionJson(entry.verification)}});
  }
  expectJson(metaJson, R"([{"duplicate":null,"semanticLoop":false,"verification":null},{"duplicate":null,"semanticLoop":false,"verification":null},{"duplicate":null,"semanticLoop":false,"verification":null},{"duplicate":null,"semanticLoop":false,"verification":null},{"duplicate":null,"semanticLoop":false,"verification":null},{"duplicate":"exact_adjacent","semanticLoop":false,"verification":null},{"duplicate":null,"semanticLoop":false,"verification":null},{"duplicate":null,"semanticLoop":false,"verification":{"kind":"shell","paths":["docs/x"]}},{"duplicate":null,"semanticLoop":false,"verification":null},{"duplicate":null,"semanticLoop":false,"verification":null}])");
  const state::StructuredTaskState derived = state::stateFromRun(run);
  expectJson(stateJson(derived), R"({"phase":"completing","modelCalls":3,"toolCalls":10,"invalidCalls":1,"invalidCallStreak":0,"duplicateCalls":1,"semanticLoops":0,"semanticLoopStreak":0,"plan":{"items":["a","b"],"seq":7},"reads":[{"path":"docs/x","digest":"caaf3f18f474","seq":2},{"path":"README.md","digest":"a430d84680aa","seq":1}],"writes":[{"path":"docs/x","marker":"DOC","add":false,"verified":true,"verifiedBy":"shell","seq":3}],"errors":[{"code":"patch_failed","tool":"editor.apply_patch","seq":4,"digest":"3beb1bbad14c"},{"code":"exec_failed","tool":"shell.exec","seq":5,"digest":"add86cd3b0a5"}],"pendingVerification":[],"unresolvedCommands":[],"unresolvedEdits":[{"path":"a","seq":10}],"lastActionSeq":8,"finals":[{"content":"f1","accepted":false,"seq":11},{"content":"f2","accepted":false,"seq":12}],"finalAttempts":2})");
  EXPECT_EQ(state::stateContract(derived),
            R"({"phase":"completing","plan":{"items":["a","b"],"updated":true},"reads":[{"path":"docs/x","digest":"caaf3f18f474"},{"path":"README.md","digest":"a430d84680aa"}],"writes":[{"path":"docs/x","marker":"DOC","add":false,"verified":true,"verifiedBy":"shell"}],"errors":[{"code":"patch_failed","tool":"editor.apply_patch","digest":"3beb1bbad14c"},{"code":"exec_failed","tool":"shell.exec","digest":"add86cd3b0a5"}],"pendingVerification":[],"unresolvedCommands":[],"unresolvedEdits":["a"],"lastActionHadSuccess":true,"finalAttempts":2,"finals":[{"content":"f1","accepted":false},{"content":"f2","accepted":false}]})");
  EXPECT_EQ(state::renderStateSummary(derived),
            "phase: completing\nmodel calls: 3\ntool calls: 10 (invalid: 1, duplicates: 1)\nplan: 2 item(s)\nwrites: docs/x (verified by shell)\npending verification: none\nunresolved: 0 command(s), 1 edit(s)\nrecent failures: editor.apply_patch:patch_failed, shell.exec:exec_failed\nloop: none");
}

TEST("codec: ResultLike round-trips through its Boost.JSON tag_invoke pair") {
  const loops::ResultLike guarded{.ok = false, .error_code = nullptr, .error = std::string("x")};
  EXPECT_EQ(rt::jsonStringify(json::value_from(guarded)), R"({"ok":false,"error_code":null,"error":"x"})");
  const loops::ResultLike parsed = json::value_to<loops::ResultLike>(rt::jsonParse(R"({"ok":true,"output":"o","extra":1})"));
  EXPECT(parsed == (loops::ResultLike{.ok = true, .output = std::string("o")}));
  EXPECT(parsed.error_code == rt::undefined);
  EXPECT_THROWS_MESSAGE(json::value_to<loops::ResultLike>(rt::jsonParse(R"({"ok":1})")), rt::TypeError, "ResultLike.ok is number, not boolean");
}
