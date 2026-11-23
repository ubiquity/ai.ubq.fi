// Port of tests/reliability-loops.test.ts.
#include "uos/harmony/reliability/loops.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>

namespace {
  using namespace uos;
  using namespace uos::harmony::reliability::loops;

  ResultLike ok(std::string output = "content") { return ResultLike{.ok = true, .output = std::move(output)}; }
  // The wire field stays `error_code`; the parameter itself follows the repo's naming convention.
  ResultLike fail(std::string errorCode, std::string error = "boom") { return ResultLike{.ok = false, .error_code = std::move(errorCode), .error = std::move(error)}; }
  std::optional<std::string> flag(const char* value) { return std::string(value); }
}

TEST("loops: canonical arguments are order-independent and nested") {
  EXPECT_EQ(canonicalArgs(json::object{{"b", 1}, {"a", 2}}), canonicalArgs(json::object{{"a", 2}, {"b", 1}}));
  EXPECT_EQ(canonicalArgs(json::object{{"a", json::object{{"y", 1}, {"x", 2}}}}), canonicalArgs(json::object{{"a", json::object{{"x", 2}, {"y", 1}}}}));
  EXPECT_EQ(rt::jsonStringify(canonicalize(json::array{"b", "a"})), rt::jsonStringify(json::array{"b", "a"}));  // arrays keep order
}

TEST("loops: call identity and effect digests are deterministic") {
  const std::string a = callIdentity("filesystem.read", json::object{{"path", "x"}});
  const std::string b = callIdentity("filesystem.read", json::object{{"path", "x"}});
  EXPECT_EQ(a, b);
  EXPECT(a != callIdentity("filesystem.read", json::object{{"path", "y"}}));
  EXPECT_EQ(effectDigest(ok("same")), effectDigest(ok("same")));
  EXPECT(effectDigest(ok("same")) != effectDigest(ok("other")));
  EXPECT_EQ(effectSignature("filesystem.read", json::object{{"path", "x"}}, ok("c")), effectSignature("filesystem.read", json::object{{"path", "x"}}, ok("c")));
}

TEST("loops: adjacent identical call after success is flagged as repeat_after_success") {
  LoopDetector detector;
  detector.observe("filesystem.read", json::object{{"path", "x"}}, ok("v"));
  const auto dup = detector.checkDuplicate("filesystem.read", json::object{{"path", "x"}});
  EXPECT_EQ(dup, flag("repeat_after_success"));
  const LoopFlags flags = detector.observe("filesystem.read", json::object{{"path", "x"}}, ok("v"));
  EXPECT_EQ(flags.duplicate, flag("repeat_after_success"));
  EXPECT_EQ(flags.semanticLoop, false);
}

TEST("loops: adjacent call after a failure is exact_adjacent (retry, not duplication)") {
  LoopDetector detector;
  detector.observe("shell.exec", json::object{{"command", "false"}}, fail("exec_failed"));
  const auto dup = detector.checkDuplicate("shell.exec", json::object{{"command", "false"}});
  EXPECT_EQ(dup, flag("exact_adjacent"));
  const LoopFlags flags = detector.observe("shell.exec", json::object{{"command", "false"}}, fail("exec_failed"));
  EXPECT_EQ(flags.duplicate, flag("exact_adjacent"));
}

TEST("loops: a different call is never flagged") {
  LoopDetector detector;
  detector.observe("filesystem.read", json::object{{"path", "x"}}, ok("v"));
  EXPECT_EQ(detector.checkDuplicate("filesystem.read", json::object{{"path", "y"}}), std::nullopt);
  const LoopFlags flags = detector.observe("filesystem.read", json::object{{"path", "y"}}, ok("w"));
  EXPECT_EQ(flags.duplicate, std::nullopt);
  EXPECT_EQ(flags.semanticLoop, false);
}

TEST("loops: effect repetition inside the window closes a semantic loop") {
  LoopDetector detector;
  // Same action + same effect three times => effect_repeat at the third read.
  detector.observe("filesystem.read", json::object{{"path", "x"}}, ok("v"));
  detector.observe("shell.exec", json::object{{"command", "ls"}}, ok("list"));
  detector.observe("filesystem.read", json::object{{"path", "x"}}, ok("v"));
  EXPECT_EQ(detector.observe("shell.exec", json::object{{"command", "ls"}}, ok("list")).semanticLoop, false);
  const LoopFlags fifth = detector.observe("filesystem.read", json::object{{"path", "x"}}, ok("v"));
  EXPECT_EQ(fifth.semanticLoop, true);
  EXPECT_EQ(fifth.loopKind, flag("effect_repeat"));
  EXPECT_EQ(fifth.streak, 1);
}

TEST("loops: alternating pattern recurrence closes a loop and tracks streak") {
  LoopDetector detector(LoopDetectorOptions{.window = 8, .patternLength = 2});
  const auto reads = [&] { return detector.observe("filesystem.read", json::object{{"path", "x"}}, ok("v")); };
  const auto patches = [&] { return detector.observe("editor.apply_patch", json::object{{"path", "y"}, {"old", "nope"}, {"new", "z"}}, fail("patch_failed")); };
  // R P R P => at the 4th call the last-2 pattern repeats the previous 2.
  reads();
  patches();
  reads();
  const LoopFlags fourth = patches();
  EXPECT_EQ(fourth.semanticLoop, true);
  EXPECT_EQ(fourth.loopKind, flag("pattern_recurrence"));
  const LoopFlags fifth = reads();
  EXPECT_EQ(fifth.semanticLoop, true);  // effect_repeat keeps the streak
  EXPECT_EQ(fifth.streak, 2);
}

TEST("loops: breaking the pattern resets the streak") {
  LoopDetector detector(LoopDetectorOptions{.window = 8, .patternLength = 2});
  const auto reads = [&] { return detector.observe("filesystem.read", json::object{{"path", "x"}}, ok("v")); };
  const auto patches = [&] { return detector.observe("editor.apply_patch", json::object{{"path", "y"}, {"old", "nope"}, {"new", "z"}}, fail("patch_failed")); };
  reads();
  patches();
  reads();
  patches();
  const LoopFlags fix = detector.observe("editor.apply_patch", json::object{{"path", "y"}, {"old", "x"}, {"new", "z"}}, ok("patched y"));
  EXPECT_EQ(fix.semanticLoop, false);
  EXPECT_EQ(fix.streak, 0);
}

TEST("loops: repeated guard rejections are detected as a loop themselves") {
  LoopDetector detector;
  detector.observe("filesystem.read", json::object{{"path", "x"}}, ok("v"));
  detector.observe("filesystem.read", json::object{{"path", "x"}}, ResultLike{.ok = false, .error_code = "duplicate_call", .error = "dup"});
  detector.observe("filesystem.read", json::object{{"path", "x"}}, ResultLike{.ok = false, .error_code = "duplicate_call", .error = "dup"});
  const LoopFlags third = detector.observe(
    "filesystem.read",
    json::object{{"path", "x"}},
    ResultLike{
      .ok = false,
      .error_code = "duplicate_call",
      .error = "dup",
    });
  EXPECT_EQ(third.semanticLoop, true);
}

TEST("loops: renderLoopFeedback is deterministic and only for loops") {
  LoopDetector detector(LoopDetectorOptions{.window = 8, .patternLength = 2});
  detector.observe("filesystem.read", json::object{{"path", "x"}}, ok("v"));
  detector.observe("shell.exec", json::object{{"command", "pwd"}}, ok("."));
  detector.observe("filesystem.read", json::object{{"path", "x"}}, ok("v"));
  detector.observe("shell.exec", json::object{{"command", "pwd"}}, ok("."));
  const LoopFlags other = detector.observe("filesystem.read", json::object{{"path", "x"}}, ok("v"));
  EXPECT_EQ(other.semanticLoop, true);
  EXPECT(other.loopKind != std::nullopt);
  const std::string text = renderLoopFeedback(other);
  EXPECT(rt::Regex("semantic loop detected").test(text));
}
