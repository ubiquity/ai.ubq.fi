// Differential cases for src/codex/supervisor-config.ts beyond the ported bun tests: the expected
// values (JSON.stringify output) were recorded from Bun 1.4.0 running the TypeScript module on the
// same inputs, including resolveSupervisorConfig over the same files in a scratch directory.
#include "uos/codex/supervisor_config.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

namespace {
  using namespace uos::codex::supervisor_config;
  namespace rt = uos::rt;
  namespace json = uos::json;

  json::value optionalText(const std::optional<std::string>& text) { return text ? json::value(*text) : json::value(nullptr); }

  json::value sourceJson(const SupervisorSource& source) {
    json::object out;
    out["id"] = source.id;
    out["name"] = source.name;
    out["socketPath"] = source.socketPath;
    out["codexHome"] = optionalText(source.codexHome);
    return out;
  }

  json::array textsJson(const std::vector<std::string>& texts) {
    json::array out;
    for (const std::string& text : texts) out.emplace_back(text);
    return out;
  }

  json::array sourcesJson(const std::vector<SupervisorSource>& sources) {
    json::array out;
    for (const SupervisorSource& source : sources) out.push_back(sourceJson(source));
    return out;
  }

  // JSON.stringify(parseSupervisorConfig(raw))
  std::string parsed(const json::value& raw) {
    const ParsedSupervisorConfig result = parseSupervisorConfig(raw);
    json::object out;
    out["sources"] = sourcesJson(result.sources);
    out["notes"] = textsJson(result.notes);
    return rt::jsonStringify(out);
  }

  // JSON.stringify({ ...resolved, configPath: resolved.configPath.slice(root.length) })
  std::string resolvedJson(const SupervisorConfig& resolved, const std::string& root) {
    json::object out;
    out["sources"] = sourcesJson(resolved.sources);
    out["configPath"] = resolved.configPath.substr(root.size());
    out["configLoaded"] = resolved.configLoaded;
    out["notes"] = textsJson(resolved.notes);
    return rt::jsonStringify(out);
  }

  std::optional<double> epoch(double value) { return normalizeEpochMs(json::value(value)); }

  const std::string GRIN = "\xF0\x9F\x98\x80";
  const std::string E_ACUTE = "\xC3\xA9";
}

TEST("normalizeEpochMs rounds seconds to milliseconds and passes milliseconds through") {
  EXPECT_EQ(epoch(1.5), std::optional<double>(1'500));
  EXPECT_EQ(epoch(0.0004), std::optional<double>(0));
  EXPECT_EQ(epoch(0.0005), std::optional<double>(1));
  EXPECT_EQ(epoch(999999999999.9995), std::optional<double>(1'000'000'000'000'000));
  EXPECT_EQ(epoch(1e12), std::optional<double>(1'000'000'000'000));
  EXPECT_EQ(epoch(1e12 + 0.5), std::optional<double>(1'000'000'000'001));
  EXPECT_EQ(epoch(rt::Number::POSITIVE_INFINITY), std::nullopt);
  EXPECT_EQ(epoch(rt::Number::NaN), std::nullopt);
  EXPECT_EQ(normalizeEpochMs(json::value(true)), std::nullopt);
  EXPECT_EQ(epoch(1730831111.0004), std::optional<double>(1'730'831'111'000));
  EXPECT_EQ(epoch(1730831111.0005), std::optional<double>(1'730'831'111'001));
}

TEST("the unknown-value readers") {
  EXPECT_EQ(textOrNull(json::value("\xC2\xA0\xE2\x80\xA8 a b \xEF\xBB\xBF\t")), std::optional<std::string>("a b"));
  EXPECT_EQ(textOrNull(json::value(" \xE3\x80\x80\n")), std::nullopt);
  EXPECT_EQ(textOrNull(json::value("")), std::nullopt);
  EXPECT_EQ(textOrNull(json::value(5)), std::nullopt);
  EXPECT_EQ(textOrNull(nullptr), std::nullopt);
  EXPECT_EQ(asString(json::value("x")), std::optional<std::string>("x"));
  EXPECT_EQ(asString(json::value(1)), std::nullopt);
  EXPECT_EQ(asString(json::value(nullptr)), std::nullopt);
  EXPECT_EQ(asFiniteNumber(json::value(1.5)), std::optional<double>(1.5));
  EXPECT_EQ(asFiniteNumber(json::value(rt::Number::POSITIVE_INFINITY)), std::nullopt);
  EXPECT_EQ(asFiniteNumber(json::value(rt::Number::NaN)), std::nullopt);
  EXPECT_EQ(asFiniteNumber(json::value("1")), std::nullopt);
  EXPECT_EQ(asFiniteNumber(json::value(-0.0)), std::optional<double>(0));
  EXPECT_EQ(asBoolean(json::value(true)), std::optional<bool>(true));
  EXPECT_EQ(asBoolean(json::value(false)), std::optional<bool>(false));
  EXPECT_EQ(asBoolean(json::value(0)), std::nullopt);
  EXPECT_EQ(asBoolean(json::value(nullptr)), std::nullopt);
  EXPECT_EQ(isRecord(json::object{}), true);
  EXPECT_EQ(isRecord(json::array{}), false);
  EXPECT_EQ(isRecord(json::value(nullptr)), false);
  EXPECT_EQ(isRecord(json::value("x")), false);
  EXPECT_EQ(isUnknownArray(json::array{}), true);
  EXPECT_EQ(isUnknownArray(json::object{}), false);
  EXPECT_EQ(isUnknownArray(json::value(nullptr)), false);
}

TEST("joinPath, uniqueStrings and boundedText") {
  EXPECT_EQ(joinPath("/a", "b"), "/a/b");
  EXPECT_EQ(joinPath("/a/", "b"), "/a/b");
  EXPECT_EQ(joinPath("", "b"), "/b");
  EXPECT_EQ(joinPath("/", "b"), "/b");
  EXPECT_EQ(uniqueStrings({"b", "a", "b", "", "", "c", "a"}), (std::vector<std::string>{"b", "a", "", "c"}));
  // "ab😀cd" bounded at 3 keeps the lone high surrogate U+D83D.
  EXPECT_EQ(boundedText("ab" + GRIN + "cd", 3), "ab\xED\xA0\xBD…");
  EXPECT_EQ(boundedText("abcdef", 2.5), "ab…");
  EXPECT_EQ(boundedText("abc", 0), "…");
  EXPECT_EQ(boundedText("abc", -1), "ab…");
  EXPECT_EQ(boundedText("abc", rt::Number::NaN), "…");
  EXPECT_EQ(boundedText("", 0), "");
  EXPECT(rt::Number::isInteger(nowMs()));
}

TEST("the exported constants, patterns and lists") {
  EXPECT_EQ((std::vector<double>{EXTRA_LOADED_LIMIT, FOLLOW_HEARTBEAT_EVERY, FOLLOW_MAX_COMMAND_CHARS, FOLLOW_MAX_DURATION_MS, FOLLOW_MAX_ENTRIES,
                                 FOLLOW_MAX_TEXT_CHARS, FOLLOW_POLL_MS, INVENTORY_LIMIT, LIST_PAGE_LIMIT, LIVE_PROBE_LIMIT, MAX_LIST_PAGES, OVERALL_DEADLINE_MS,
                                 PROBE_CONCURRENCY, RECENT_METADATA_LIMIT, SNAPSHOT_TTL_MS, SOURCE_CONCURRENCY}),
            (std::vector<double>{8, 5, 400, 600000, 120, 4000, 2000, 60, 50, 24, 2, 9000, 4, 60, 4000, 2}));
  EXPECT_EQ(LIST_SOURCE_KINDS, (std::vector<std::string>{"cli", "vscode", "exec", "appServer", "subAgent", "subAgentReview", "subAgentCompact",
                                                         "subAgentThreadSpawn", "subAgentOther", "unknown"}));
  EXPECT_EQ(std::string(METADATA_COLUMNS),
            "id, title, name, preview, cwd, model, reasoning_effort, tokens_used, git_branch, updated_at, updated_at_ms, archived, thread_source, source");
  EXPECT_EQ(std::string(SQLITE_MODULE_SPECIFIER), "bun:sqlite");
  EXPECT_EQ(TERMINAL_TURN_STATES.size(), 8u);
  for (const char* state : {"completed", "interrupted", "failed", "declined", "cancelled", "canceled", "errored", "aborted"}) EXPECT(TERMINAL_TURN_STATES.contains(state));
  EXPECT_EQ(SOURCE_ID_PATTERN.toString(), "/^[a-z0-9][a-z0-9_-]{0,31}$/");
  EXPECT_EQ(THREAD_ID_PATTERN.toString(), "/^[A-Za-z0-9._:-]{1,200}$/");
  EXPECT_EQ(STATE_DATABASE_PATTERN.toString(), "/^state_(\\d+)\\.sqlite$/");
  EXPECT_EQ(SOURCE_ID_PATTERN.test("a"), true);
  EXPECT_EQ(SOURCE_ID_PATTERN.test("-a"), false);
  EXPECT_EQ(SOURCE_ID_PATTERN.test(rt::repeat("a", 32)), true);
  EXPECT_EQ(SOURCE_ID_PATTERN.test(rt::repeat("a", 33)), false);
  EXPECT_EQ(SOURCE_ID_PATTERN.test("ab\n"), false);
  EXPECT_EQ(THREAD_ID_PATTERN.test(rt::repeat("x", 200)), true);
  EXPECT_EQ(THREAD_ID_PATTERN.test(rt::repeat("x", 201)), false);
  EXPECT_EQ(THREAD_ID_PATTERN.test("a b"), false);
  const std::optional<rt::RegexMatch> database = STATE_DATABASE_PATTERN.exec("state_12.sqlite");
  ASSERT(database.has_value());
  EXPECT_EQ((*database)[1], std::optional<std::string>("12"));
  EXPECT(!STATE_DATABASE_PATTERN.exec("state_.sqlite"));
  for (const SupervisorState state : {SupervisorState::active, SupervisorState::idle, SupervisorState::stale, SupervisorState::unknown, SupervisorState::system_error})
    EXPECT(parseSupervisorState(toString(state)) == state);
  EXPECT(parseSupervisorState("systemError") == std::nullopt);
}

TEST("parseSupervisorConfig notes every rejected source in order") {
  EXPECT_EQ(parsed(rt::jsonParse("[1]")), R"({"sources":[],"notes":["configuration must be a JSON object"]})");
  EXPECT_EQ(parsed(json::value("x")), R"({"sources":[],"notes":["configuration must be a JSON object"]})");
  EXPECT_EQ(parsed(rt::jsonParse(R"({"sources":{"a":1}})")), R"({"sources":[],"notes":["configuration lists no sources"]})");
  const std::string all = R"({"sources":[7,null,[],{"id":" ok1 ","name":" One ","socketPath":" /s1 ","codexHome":" /h1 "},)"
                          R"({"id":"","name":"n","socketPath":"/s"},{"id":5,"name":"n","socketPath":"/s"},{"id":"ok1","name":"dupe","socketPath":"/s"},)"
                          R"({"id":"noname","socketPath":"/s"},{"id":"longname","name":")" +
                          rt::repeat("x", 65) + R"(","socketPath":"/s"},{"id":"pairname","name":")" + rt::repeat(GRIN, 32) +
                          R"(","socketPath":"/s"},{"id":"pairname2","name":")" + rt::repeat(GRIN, 33) + R"(","socketPath":"/s"}]})";
  EXPECT_EQ(parsed(rt::jsonParse(all)),
            R"({"sources":[{"id":"ok1","name":"One","socketPath":"/s1","codexHome":"/h1"}],"notes":["ignored a source that is not an object",)"
            R"("ignored a source that is not an object","ignored a source that is not an object","ignored a source whose id must match /^[a-z0-9][a-z0-9_-]{0,31}$/",)"
            R"("ignored a source whose id must match /^[a-z0-9][a-z0-9_-]{0,31}$/","ignored duplicate source id ok1",)"
            R"("ignored source noname: name must be 1-64 characters","only the first 8 sources are used"]})");
  EXPECT_EQ(parsed(rt::jsonParse(R"({"sources":[{"id":"nosock","name":"n"},{"id":"relsock","name":"n","socketPath":"s"},{"id":"wssock","name":"n","socketPath":"   "},)"
                                 R"({"id":"homeempty","name":"n","socketPath":"/s","codexHome":"  "},{"id":"homenum","name":"n","socketPath":"/s","codexHome":5},)"
                                 R"({"id":"homerel","name":"n","socketPath":"/s","codexHome":"rel"},{"id":"homenull","name":"n","socketPath":"/s","codexHome":null},)"
                                 R"({"id":"homeundef","name":"n","socketPath":"/s"}]})")),
            R"({"sources":[{"id":"homeempty","name":"n","socketPath":"/s","codexHome":null},{"id":"homenum","name":"n","socketPath":"/s","codexHome":null},)"
            R"({"id":"homenull","name":"n","socketPath":"/s","codexHome":null},{"id":"homeundef","name":"n","socketPath":"/s","codexHome":null}],)"
            R"("notes":["ignored source nosock: socketPath must be an absolute path","ignored source relsock: socketPath must be an absolute path",)"
            R"("ignored source wssock: socketPath must be an absolute path","ignored source homerel: codexHome must be an absolute path"]})");
  std::string ten = R"({"sources":[)";
  for (int index = 0; index < 9; ++index) ten += R"({"id":"s)" + std::to_string(index) + R"(","name":"n","socketPath":"/s"},)";
  ten += R"("late"]})";
  std::string kept;
  for (int index = 0; index < 8; ++index) kept += std::string(index ? "," : "") + R"({"id":"s)" + std::to_string(index) + R"(","name":"n","socketPath":"/s","codexHome":null})";
  EXPECT_EQ(parsed(rt::jsonParse(ten)), R"({"sources":[)" + kept + R"(],"notes":["only the first 8 sources are used"]})");
  std::string notObjects;
  for (int index = 0; index < 8; ++index) notObjects += R"("ignored a source that is not an object",)";
  EXPECT_EQ(parsed(rt::jsonParse(R"({"sources":[1,1,1,1,1,1,1,1,{"id":"a","name":"n","socketPath":"/s"}]})")),
            R"({"sources":[],"notes":[)" + notObjects + R"("only the first 8 sources are used"]})");
  EXPECT_EQ(parsed(rt::jsonParse(R"({"sources":[{"id":"é","name":"n","socketPath":"/s"},{"id":"A","name":"n","socketPath":"/s"}]})")),
            R"({"sources":[],"notes":["ignored a source whose id must match /^[a-z0-9][a-z0-9_-]{0,31}$/","ignored a source whose id must match /^[a-z0-9][a-z0-9_-]{0,31}$/"]})");
}

TEST("parseSupervisorConfig measures names in UTF-16 units, trims paths and dedupes accepted ids only") {
  const std::string input = R"({"sources":[{"id":"pairname","name":")" + rt::repeat(GRIN, 32) + R"(","socketPath":"/s"},{"id":"pairname2","name":")" + rt::repeat(GRIN, 33) +
                            R"(","socketPath":"/s"},{"id":"latin","name":")" + rt::repeat(E_ACUTE, 64) + R"(","socketPath":"/s"},{"id":"latin2","name":")" +
                            rt::repeat(E_ACUTE, 65) + R"(","socketPath":"/s"},{"id":"sockws","name":"n","socketPath":" /abs "},)" +
                            R"({"id":"homews","name":"n","socketPath":"/s","codexHome":"\t/abs\n"}]})";
  EXPECT_EQ(parsed(rt::jsonParse(input)), R"({"sources":[{"id":"pairname","name":")" + rt::repeat(GRIN, 32) + R"(","socketPath":"/s","codexHome":null},)" +
                                            R"({"id":"latin","name":")" + rt::repeat(E_ACUTE, 64) + R"(","socketPath":"/s","codexHome":null},)" +
                                            R"({"id":"sockws","name":"n","socketPath":"/abs","codexHome":null},{"id":"homews","name":"n","socketPath":"/s","codexHome":"/abs"}],)" +
                                            R"("notes":["ignored source pairname2: name must be 1-64 characters","ignored source latin2: name must be 1-64 characters"]})");
  EXPECT_EQ(parsed(rt::jsonParse(R"({"sources":[{"id":"a","name":"n","socketPath":"/s"},{"id":"a","name":"m","socketPath":"/t"},)"
                                 R"({"id":"b","name":"n","socketPath":"x"},{"id":"b","name":"n","socketPath":"/s"}]})")),
            R"({"sources":[{"id":"a","name":"n","socketPath":"/s","codexHome":null},{"id":"b","name":"n","socketPath":"/s","codexHome":null}],)"
            R"("notes":["ignored duplicate source id a","ignored source b: socketPath must be an absolute path"]})");
}

TEST("defaultSupervisorSource trims both variables and keeps CODEX_HOME as given") {
  std::vector<std::string> notes;
  const auto both = defaultSupervisorSource([](std::string_view name) -> std::optional<std::string> { return name == "CODEX_HOME" ? " /x/ " : "/h"; }, notes);
  ASSERT(both.has_value());
  EXPECT_EQ(rt::jsonStringify(sourceJson(*both)), R"({"id":"local","name":"Local Codex","socketPath":"/x/app-server-control/app-server-control.sock","codexHome":"/x/"})");
  const auto fromHome = defaultSupervisorSource([](std::string_view name) -> std::optional<std::string> { return name == "CODEX_HOME" ? "  " : " /h "; }, notes);
  ASSERT(fromHome.has_value());
  EXPECT_EQ(rt::jsonStringify(sourceJson(*fromHome)), R"({"id":"local","name":"Local Codex","socketPath":"/h/.codex/app-server-control/app-server-control.sock","codexHome":"/h/.codex"})");
  EXPECT(defaultSupervisorSource([](std::string_view name) -> std::optional<std::string> { return name == "HOME" ? std::optional<std::string>("   ") : std::nullopt; }, notes) ==
         std::nullopt);
  EXPECT_EQ(notes, (std::vector<std::string>{"HOME and CODEX_HOME are both unavailable, so no local source could be derived"}));
}

ASYNC_TEST("resolveSupervisorConfig over missing, valid, malformed and unreadable configuration files") {
  const std::string root = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "resolve-"));
  const auto cleanup = rt::scopeExit([root] { rt::fs::rmSync(root, {.recursive = true, .force = true}); });
  const std::vector<std::pair<std::string, std::optional<std::string>>> cases{
    {"missing", std::nullopt},
    {"valid", R"({"sources":[{"id":"a","name":"A","socketPath":"/s"}]})"},
    {"invalid", "{"},
    {"empty", R"({"sources":[]})"},
    {"notobject", "[1]"},
    {"nousable", R"({"sources":[5]})"},
    {"dir", "DIR"},
    {"bom", "\xEF\xBB\xBF{}"},
  };
  const std::string fallback = R"({"id":"local","name":"Local Codex","socketPath":"/home/t/.codex/app-server-control/app-server-control.sock","codexHome":"/home/t/.codex"})";
  const std::string unreadable = R"(".data/codex-supervisor.json could not be read; using the default local source")";
  const std::string noUsable = R"("no usable source in the configuration file; using the default local source")";
  const std::vector<std::string> expected{
    R"({"sources":[)" + fallback + R"(],"configPath":"/missing/.data/codex-supervisor.json","configLoaded":false,"notes":[]})",
    R"({"sources":[{"id":"a","name":"A","socketPath":"/s","codexHome":null}],"configPath":"/valid/.data/codex-supervisor.json","configLoaded":true,"notes":[]})",
    R"({"sources":[)" + fallback + R"(],"configPath":"/invalid/.data/codex-supervisor.json","configLoaded":false,"notes":[)" + unreadable + "]}",
    R"({"sources":[)" + fallback + R"(],"configPath":"/empty/.data/codex-supervisor.json","configLoaded":true,"notes":["configuration lists no sources",)" + noUsable + "]}",
    R"({"sources":[)" + fallback + R"(],"configPath":"/notobject/.data/codex-supervisor.json","configLoaded":true,"notes":["configuration must be a JSON object",)" + noUsable +
      "]}",
    R"({"sources":[)" + fallback + R"(],"configPath":"/nousable/.data/codex-supervisor.json","configLoaded":true,"notes":["ignored a source that is not an object",)" +
      noUsable + "]}",
    R"({"sources":[)" + fallback + R"(],"configPath":"/dir/.data/codex-supervisor.json","configLoaded":false,"notes":[)" + unreadable + "]}",
    R"({"sources":[)" + fallback + R"(],"configPath":"/bom/.data/codex-supervisor.json","configLoaded":false,"notes":[)" + unreadable + "]}",
  };
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const std::string cwd = rt::path::join(root, cases[index].first);
    rt::fs::mkdirSync(rt::path::join(cwd, ".data"), {.recursive = true});
    const std::string config = rt::path::join(cwd, ".data", "codex-supervisor.json");
    if (cases[index].second == "DIR") rt::fs::mkdirSync(config);
    else if (cases[index].second) rt::fs::writeFileSync(config, *cases[index].second);
    const ResolveSupervisorConfigOptions options{.cwd = cwd, .env = [](std::string_view name) -> std::optional<std::string> {
                                                   return name == "HOME" ? std::optional<std::string>("/home/t") : std::nullopt;
                                                 }};
    rt::Task<SupervisorConfig> resolving = resolveSupervisorConfig(options);
    const SupervisorConfig resolved = co_await std::move(resolving);
    EXPECT_EQ(resolvedJson(resolved, root), expected[index]);
  }
  const ResolveSupervisorConfigOptions noEnvironment{.cwd = rt::path::join(root, "missing"), .env = [](std::string_view) -> std::optional<std::string> { return std::nullopt; }};
  rt::Task<SupervisorConfig> resolvingWithoutEnvironment = resolveSupervisorConfig(noEnvironment);
  const SupervisorConfig withoutEnvironment = co_await std::move(resolvingWithoutEnvironment);
  EXPECT_EQ(resolvedJson(withoutEnvironment, root),
            R"({"sources":[],"configPath":"/missing/.data/codex-supervisor.json","configLoaded":false,"notes":["HOME and CODEX_HOME are both unavailable, so no local source could be derived"]})");
  const ResolveSupervisorConfigOptions trailingSlash{.cwd = rt::path::join(root, "valid") + "/", .env = {}};
  rt::Task<SupervisorConfig> resolvingWithSlash = resolveSupervisorConfig(trailingSlash);
  const SupervisorConfig withSlash = co_await std::move(resolvingWithSlash);
  EXPECT_EQ(withSlash.configPath.substr(root.size()), "/valid/.data/codex-supervisor.json");
  EXPECT_EQ(withSlash.configLoaded, true);
}

ASYNC_TEST("resolveSupervisorConfig without an env option reads process.env") {
  const std::string root = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "envres-"));
  const auto cleanup = rt::scopeExit([root] { rt::fs::rmSync(root, {.recursive = true, .force = true}); });
  const ResolveSupervisorConfigOptions options{.cwd = root, .env = {}};
  std::string first;
  {
    const rt::env::ScopedOverride scope({{"HOME", "/home/scoped"}, {"CODEX_HOME", std::nullopt}});
    rt::Task<SupervisorConfig> resolving = resolveSupervisorConfig(options);
    const SupervisorConfig resolved = co_await std::move(resolving);
    first = rt::jsonStringify(sourcesJson(resolved.sources)) + rt::jsonStringify(textsJson(resolved.notes)) + (resolved.configLoaded ? "true" : "false");
  }
  std::string second;
  {
    const rt::env::ScopedOverride scope({{"HOME", "/home/scoped"}, {"CODEX_HOME", "  /codex/scoped  "}});
    rt::Task<SupervisorConfig> resolving = resolveSupervisorConfig(options);
    const SupervisorConfig resolved = co_await std::move(resolving);
    second = rt::jsonStringify(sourcesJson(resolved.sources)) + rt::jsonStringify(textsJson(resolved.notes));
  }
  EXPECT_EQ(first, R"([{"id":"local","name":"Local Codex","socketPath":"/home/scoped/.codex/app-server-control/app-server-control.sock","codexHome":"/home/scoped/.codex"}][]false)");
  EXPECT_EQ(second, R"([{"id":"local","name":"Local Codex","socketPath":"/codex/scoped/app-server-control/app-server-control.sock","codexHome":"/codex/scoped"}][])");
}

ASYNC_TEST("mapWithConcurrency with NaN, zero, negative, fractional, large and infinite limits") {
  const std::vector<std::pair<double, int>> limits{{rt::Number::NaN, 0}, {0, 1}, {-3, 1}, {2.5, 3}, {10, 5}, {rt::Number::POSITIVE_INFINITY, 5}};
  for (const auto& [limit, expectedPeak] : limits) {
    int active = 0;
    int peak = 0;
    std::vector<double> order;
    const auto run = [&active, &peak, &order](double item) -> rt::Task<double> {
      active += 1;
      peak = std::max(peak, active);
      order.push_back(item);
      co_await rt::sleep(1);
      active -= 1;
      co_return item * 10;
    };
    std::vector<double> items{1, 2, 3, 4, 5};
    rt::Task<std::vector<double>> mapping = mapWithConcurrency(std::move(items), limit, run);
    const std::vector<double> results = co_await std::move(mapping);
    const bool none = rt::Number::isNaN(limit);
    EXPECT_EQ(results, (none ? std::vector<double>{} : std::vector<double>{10, 20, 30, 40, 50}));
    EXPECT_EQ(peak, expectedPeak);
    EXPECT_EQ(order, (none ? std::vector<double>{} : std::vector<double>{1, 2, 3, 4, 5}));
  }
}

ASYNC_TEST("mapWithConcurrency rejects with the first failure while the other workers keep going") {
  std::vector<double> started;
  std::vector<double> finished;
  const auto run = [&started, &finished](double item) -> rt::Task<double> {
    started.push_back(item);
    co_await rt::sleep(1);
    if (item == 2) throw rt::JsError("boom 2");
    finished.push_back(item);
    co_return item;
  };
  std::vector<double> items{1, 2, 3, 4};
  rt::Task<std::vector<double>> mapping = mapWithConcurrency(std::move(items), 2, run);
  std::string rejected;
  try {
    co_await std::move(mapping);
  } catch (const rt::JsError& error) {
    rejected = error.message;
  }
  EXPECT_EQ(rejected, "boom 2");
  EXPECT_EQ(started, (std::vector<double>{1, 2, 3}));
  EXPECT_EQ(finished, (std::vector<double>{1}));
  co_await rt::sleep(20);
  EXPECT_EQ(started, (std::vector<double>{1, 2, 3, 4}));
  EXPECT_EQ(finished, (std::vector<double>{1, 3, 4}));
}

ASYNC_TEST("waitForPoll on an already aborted signal waits its full deadline; an abort mid-wait ends it") {
  const rt::AbortController aborted;
  aborted.abort();
  const double startedAt = rt::performanceNow();
  rt::Promise<void> fullWait = waitForPoll(40, aborted.signal());
  co_await fullWait.wait();
  EXPECT(rt::performanceNow() - startedAt >= 35);
  const rt::AbortController controller;
  const double midStart = rt::performanceNow();
  rt::Promise<void> pending = waitForPoll(40, controller.signal());
  rt::setTimeout([controller] { controller.abort(); }, 10);
  co_await pending.wait();
  const double elapsed = rt::performanceNow() - midStart;
  EXPECT(elapsed >= 8 && elapsed < 35);
}
