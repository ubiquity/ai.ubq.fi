// Differential cases for src/harmony/tools/backend.ts (no direct TS tests): every
// expected value was recorded from Bun 1.4.0 running the TypeScript module.
#include "uos/harmony/tools/backend.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::harmony::tools::backend;

  const char* const NORMALIZE_VECTORS = R"VEC([["",null],["/",null],["/etc/passwd",null],[".",""],["./",""],["a","a"],["a/b","a/b"],["a//b","a/b"],["./a/./b/","a/b"],["a/../b",null],["..",null],["../a",null],["a/..",null],["a/...","a/..."],["...","..."],["a/.b","a/.b"],[" a / b "," a / b "],["a\\b","a\\b"],["é/😀","é/😀"],["a/b/","a/b"],["//a",null],["a/./../b",null],["\ud83d/x","\ud83d/x"],["x/\ude00","x/\ude00"]])VEC";
  const char* const GLOB_VECTORS = R"VEC([["*","a",true],["*","a/b",false],["*","",true],["**","a/b/c",true],["**","",true],["**/*.ts","a/b/c.ts",true],["**/*.ts","c.ts",true],["**/*.ts","a/c.tsx",false],["src/**","src/a/b",true],["src/**","src",false],["src/**","src/",true],["src/**/x","src/x",true],["src/**/x","src/a/b/x",true],["a?c","abc",true],["a?c","a/c",false],["a?c","ac",false],["*.md","README.md",true],["*.md","docs/README.md",false],["a.b","a.b",true],["a.b","axb",false],["a+b","a+b",true],["a+b","aab",false],["(x)","(x)",true],["[ab]","[ab]",true],["[ab]","a",false],["{a,b}","{a,b}",true],["^$","^$",true],["a|b","a|b",true],["a|b","a",false],["a\\b","a\\b",true],["$x","$x",true],["***","abc",true],["***/x","a/b/x",true],["*/**","a",false],["*/**","a/b",true],["é*","éa",true],["😀?","😀x",true],["?","😀",false],["??","😀",true],["*","line\nbreak",true],["**","line\nbreak",false],["a/**/","a/",true],["a/**/","a/b/",true],["!x","!x",true],["**/","a/",true],["x\ud83d","x\ud83d",true],["\ude00*","\ude00z",true],["protected/**","protected/a",true],["protected/**","protectedx",false],["?*","",false],["?*","a",true],["a]b","a]b",true],["a-b","a-b",true]])VEC";

  // The interfaces are implementable as the TS fakes implement them.
  class EchoWorkspace final : public WorkspaceBackend {
  public:
    std::string label() const override { return "echo-workspace"; }
    std::string read(const std::string& rel) override { return rel; }
    std::vector<std::string> listFiles(const std::string& rel) override { return {rel}; }
    bool isAllowedWrite(const std::string& rel) override { return globMatch("**", rel); }
    std::string describeWriteScope() override { return "**"; }
    void write(const std::string&, const std::string&) override {}
    AppliedPatch applyPatch(const std::string& rel, const Patch& patch) override { return {true, rel + ":" + patch.old + "->" + patch.new_}; }
    rt::Task<ShellExecResult> execShell(std::string command, ExecShellOptions opts) override {
      ShellExecResult result{.exit_code = opts.timeoutMs, .stdout = command, .stderr = "", .timed_out = opts.signal.has_value()};
      co_return result;
    }
  };
}

TEST("differential: normalizeWorkspacePath matches Bun") {
  const json::value vectors = rt::jsonParse(NORMALIZE_VECTORS);
  for (const json::value& row : vectors.as_array()) {
    const json::array& vector = row.as_array();
    const std::string input(vector[0].as_string());
    const std::optional<std::string> expected = vector[1].is_null() ? std::nullopt : std::optional<std::string>(std::string(vector[1].as_string()));
    if (normalizeWorkspacePath(input) != expected) {
      EXPECT_EQ(normalizeWorkspacePath(input), expected);
      std::fprintf(stderr, "      input: %s\n", rt::jsonQuote(input).c_str());
    }
  }
}

TEST("differential: globMatch matches Bun") {
  const json::value vectors = rt::jsonParse(GLOB_VECTORS);
  for (const json::value& row : vectors.as_array()) {
    const json::array& vector = row.as_array();
    const std::string pattern(vector[0].as_string());
    const std::string path(vector[1].as_string());
    if (globMatch(pattern, path) != vector[2].as_bool()) {
      EXPECT_EQ(globMatch(pattern, path), vector[2].as_bool());
      std::fprintf(stderr, "      pattern: %s path: %s\n", rt::jsonQuote(pattern).c_str(), rt::jsonQuote(path).c_str());
    }
  }
}

ASYNC_TEST("backend interfaces are implementable") {
  ToolBackends backends;
  backends.workspace = std::make_shared<EchoWorkspace>();
  EXPECT(!backends.browser);
  EXPECT(!backends.plan);
  EXPECT_EQ(backends.workspace->label(), "echo-workspace");
  const WorkspaceBackend::Patch patch{"x", "y", false};
  EXPECT_EQ(backends.workspace->applyPatch("a.txt", patch).detail, "a.txt:x->y");
  WorkspaceBackend::ExecShellOptions options{5};
  const ShellExecResult result = co_await backends.workspace->execShell("true", options);
  EXPECT_EQ(result.exit_code, 5.0);
  EXPECT_EQ(result.stdout, "true");
  EXPECT_EQ(result.timed_out, false);
}

TEST("ShellExecResult converts to and from JSON in declaration order") {
  const ShellExecResult result{.exit_code = 127, .stdout = "", .stderr = "fake shell: no scripted response for \"ls\"", .timed_out = false};
  const json::value written = json::value_from(result);
  EXPECT_EQ(rt::jsonStringify(written), std::string(R"({"exit_code":127,"stdout":"","stderr":"fake shell: no scripted response for \"ls\"","timed_out":false})"));
  EXPECT(json::value_to<ShellExecResult>(written) == result);
  EXPECT_THROWS_MESSAGE(json::value_to<ShellExecResult>(rt::jsonParse(R"({"exit_code":"1","stdout":"","stderr":"","timed_out":false})")), rt::TypeError,
                        "ShellExecResult.exit_code is string, not number");
}
