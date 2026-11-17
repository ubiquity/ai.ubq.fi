// Differential cases for src/kv-conversion/cli.ts (no bun tests): the module run as the program
// (`bun src/kv-conversion/cli.ts <flags>`) under Bun 1.4.0 recorded its stdout, stderr and exit
// status for each argument list (tests/kv_conversion_cli_differential_vectors.inc); here
// importMetaMain() runs over the same rt::process::argv() with console captured. The successful
// conversion uses the fake `deno` script of kv_conversion_convert_differential_test.cpp on an empty
// native database; its volatile file identities and time are compared as "<file>" placeholders.
// Fixtures live under rt::os::tmpdir() (TMPDIR).
// Links: tests/kv_conversion_cli_differential_test.cpp src/kv_conversion/cli.cpp src/kv_conversion/convert.cpp src/kv_conversion/cursors.cpp
//        src/kv_conversion/legacy_codec.cpp src/kv_conversion/native_schema.cpp src/kv_conversion/typed_value.cpp src/storage/kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp src/storage/value_compatibility.cpp src/storage/native_addon.cpp
//        src/storage/key_compatibility.cpp
#include "uos/kv_conversion/cli.hpp"

#include "uos/kv_conversion/legacy_codec.hpp"
#include "uos/kv_conversion/native_schema.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <optional>
#include <string>
#include <vector>

#include "kv_conversion_cli_differential_vectors.inc"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace native_schema = uos::kv_conversion::native_schema;
  namespace legacy_codec = uos::kv_conversion::legacy_codec;
  using rt::console::Method;

  const json::object& recorded() {
    static const json::object value = rt::jsonParse(CLI_RECORDED).as_object();
    return value;
  }

  rt::Task<void> runScenario(std::string name) {
    const json::object& expected = recorded().at(name).as_object();
    const std::string root = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "uos-cli-differential-"));
    const std::string fake = rt::path::join(root, "fake-deno");
    rt::fs::WriteFileOptions executable;
    executable.mode = 0700;
    rt::fs::writeFileSync(fake, std::string(recorded().at("script").as_string()), executable);
    auto digesting = legacy_codec::fileDigest(fake);
    const std::string sha = co_await std::move(digesting);
    const std::string source = rt::path::join(root, "source.sqlite3");
    {
      rt::sqlite::DatabaseOptions options;
      options.create = true;
      options.strict = true;
      options.safeIntegers = true;
      const rt::sqlite::Database db(source, options);
      db.run("PRAGMA journal_mode=DELETE");
      for (const std::string_view ddl : native_schema::NATIVE_DDL) db.run(ddl);
      db.run("INSERT INTO migration_state(k,version) VALUES(0,3)");
      db.run("INSERT INTO data_version(k,version,seq) VALUES(0,4,0)");
      db.close(true);
    }
    std::vector<std::string> argv{"/bun", "/cli.ts"};
    for (const json::value& argument : expected.at("args").as_array())
      argv.push_back(rt::replaceAll(rt::replaceAll(std::string(argument.as_string()), "<root>", root), "<sha>", sha));
    std::optional<rt::process::ScopedArgv> scope;
    scope.emplace(argv);
    std::optional<rt::console::Capture> capture;
    capture.emplace(std::vector<Method>{Method::log, Method::error});
    auto running = uos::kv_conversion::cli::importMetaMain();
    co_await std::move(running);
    const std::vector<rt::console::Call> logs = capture->calls(Method::log);
    const std::vector<rt::console::Call> errors = capture->calls(Method::error);
    capture.reset();
    scope.reset();
    const std::optional<int> exitCode = rt::process::exitCode();
    rt::process::setExitCode(std::nullopt);

    EXPECT_EQ(exitCode.value_or(0), static_cast<int>(expected.at("code").as_int64()));
    const std::string stderrText = std::string(expected.at("stderr").as_string());
    if (stderrText.empty()) {
      EXPECT_EQ(errors.size(), std::size_t{0});
    } else {
      ASSERT_EQ(errors.size(), std::size_t{1});
      EXPECT_EQ(rt::replaceAll(rt::replaceAll(errors[0].text, root, "<root>"), sha, "<sha>") + "\n", stderrText);
    }
    if (const json::value* normalized = expected.if_contains("stdout_normalized")) {
      ASSERT_EQ(logs.size(), std::size_t{1});
      const json::value printed = rt::jsonParse(logs[0].text);
      EXPECT_EQ(logs[0].text, rt::jsonStringify(printed, 2));
      json::value receipt = rt::jsonParse(rt::replaceAll(rt::replaceAll(logs[0].text, root, "<root>"), sha, "<sha>"));
      json::object& fields = receipt.as_object();
      fields["source"].as_object()["file"] = "<file>";
      fields["source"].as_object()["snapshot"] = "<file>";
      fields["destination"].as_object()["file"] = "<file>";
      fields["completed_at"] = "<completed_at>";
      EXPECT_EQ(rt::jsonStringify(receipt), rt::jsonStringify(*normalized));
    } else {
      EXPECT_EQ(logs.size(), std::size_t{0});
      EXPECT_EQ(std::string(expected.at("stdout").as_string()), std::string());
    }
    rt::fs::RmOptions removal;
    removal.recursive = true;
    removal.force = true;
    rt::fs::rmSync(root, removal);
  }
}

ASYNC_TEST("cli: no flags") {
  auto running = runScenario("no flags");
  co_await std::move(running);
}
ASYNC_TEST("cli: flag without value") {
  auto running = runScenario("flag without value");
  co_await std::move(running);
}
ASYNC_TEST("cli: key without dashes") {
  auto running = runScenario("key without dashes");
  co_await std::move(running);
}
ASYNC_TEST("cli: value that is a flag") {
  auto running = runScenario("value that is a flag");
  co_await std::move(running);
}
ASYNC_TEST("cli: repeated flag") {
  auto running = runScenario("repeated flag");
  co_await std::move(running);
}
ASYNC_TEST("cli: empty value") {
  auto running = runScenario("empty value");
  co_await std::move(running);
}
ASYNC_TEST("cli: unknown direction") {
  auto running = runScenario("unknown direction");
  co_await std::move(running);
}
ASYNC_TEST("cli: missing pin") {
  auto running = runScenario("missing pin");
  co_await std::move(running);
}
ASYNC_TEST("cli: missing source") {
  auto running = runScenario("missing source");
  co_await std::move(running);
}
ASYNC_TEST("cli: same source and destination") {
  auto running = runScenario("same source and destination");
  co_await std::move(running);
}
ASYNC_TEST("cli: repeated pin flag") {
  auto running = runScenario("repeated pin flag");
  co_await std::move(running);
}
ASYNC_TEST("cli: unused extra flag") {
  auto running = runScenario("unused extra flag");
  co_await std::move(running);
}
ASYNC_TEST("cli: converts an empty database") {
  auto running = runScenario("converts an empty database");
  co_await std::move(running);
}
