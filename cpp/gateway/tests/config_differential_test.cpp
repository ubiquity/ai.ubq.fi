// Differential cases for src/config.ts and src/release.ts (no direct TS tests): the expected
// values were recorded from Bun 1.4.0 importing the TypeScript modules in fresh processes
// with exactly these environments. `config` is loaded when the module is initialized, so
// each scenario runs this test binary again as a child with that environment.
#include "uos/config.hpp"

#include "uos/release.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include <cstdio>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using uos::config::config;

  const char* const SCENARIOS = R"VEC([[{}, {"isDeploy": false, "isProductionTimeline": false, "allowOrigin": "*", "authTokens": [], "adminTokens": [], "codexBaseUrl": "https://chatgpt.com/backend-api/codex", "codexAuthJsonB64": "", "runtimeGitSha": "unknown", "runtimeDeploymentId": "unknown", "RELEASE_GIT_SHA": "unknown"}], [{"DENO_DEPLOY": "", "DENO_DEPLOYMENT_ID": "dep"}, {"isDeploy": false, "isProductionTimeline": false, "allowOrigin": "*", "authTokens": [], "adminTokens": [], "codexBaseUrl": "https://chatgpt.com/backend-api/codex", "codexAuthJsonB64": "", "runtimeGitSha": "unknown", "runtimeDeploymentId": "dep", "RELEASE_GIT_SHA": "unknown"}], [{"DENO_REGION": "x"}, {"isDeploy": true, "isProductionTimeline": false, "allowOrigin": "*", "authTokens": [], "adminTokens": [], "codexBaseUrl": "https://chatgpt.com/backend-api/codex", "codexAuthJsonB64": "", "runtimeGitSha": "unknown", "runtimeDeploymentId": "unknown", "RELEASE_GIT_SHA": "unknown"}], [{"DENO_DEPLOY": "1", "DENO_DEPLOY_BUILD_ID": "  build-7  "}, {"isDeploy": true, "isProductionTimeline": false, "allowOrigin": "*", "authTokens": [], "adminTokens": [], "codexBaseUrl": "https://chatgpt.com/backend-api/codex", "codexAuthJsonB64": "", "runtimeGitSha": "unknown", "runtimeDeploymentId": "build-7", "RELEASE_GIT_SHA": "unknown"}], [{"DENO_TIMELINE": "production"}, {"isDeploy": false, "isProductionTimeline": true, "allowOrigin": "*", "authTokens": [], "adminTokens": [], "codexBaseUrl": "https://chatgpt.com/backend-api/codex", "codexAuthJsonB64": "", "runtimeGitSha": "unknown", "runtimeDeploymentId": "unknown", "RELEASE_GIT_SHA": "unknown"}], [{"DENO_TIMELINE": "Production"}, {"isDeploy": false, "isProductionTimeline": false, "allowOrigin": "*", "authTokens": [], "adminTokens": [], "codexBaseUrl": "https://chatgpt.com/backend-api/codex", "codexAuthJsonB64": "", "runtimeGitSha": "unknown", "runtimeDeploymentId": "unknown", "RELEASE_GIT_SHA": "unknown"}], [{"UOS_AI_TOKEN": " a , b\nc,, ,a\r\n", "DENO_DEPLOY_TOKEN": "x"}, {"isDeploy": false, "isProductionTimeline": false, "allowOrigin": "*", "authTokens": ["a", "b", "c"], "adminTokens": ["x"], "codexBaseUrl": "https://chatgpt.com/backend-api/codex", "codexAuthJsonB64": "", "runtimeGitSha": "unknown", "runtimeDeploymentId": "unknown", "RELEASE_GIT_SHA": "unknown"}], [{"UOS_AI_TOKEN": " tok ,﻿two　", "DENO_DEPLOY_TOKEN": ""}, {"isDeploy": false, "isProductionTimeline": false, "allowOrigin": "*", "authTokens": ["tok", "two"], "adminTokens": [], "codexBaseUrl": "https://chatgpt.com/backend-api/codex", "codexAuthJsonB64": "", "runtimeGitSha": "unknown", "runtimeDeploymentId": "unknown", "RELEASE_GIT_SHA": "unknown"}], [{"CODEX_AUTH_JSON_B64": "  abc  \n"}, {"isDeploy": false, "isProductionTimeline": false, "allowOrigin": "*", "authTokens": [], "adminTokens": [], "codexBaseUrl": "https://chatgpt.com/backend-api/codex", "codexAuthJsonB64": "abc", "runtimeGitSha": "unknown", "runtimeDeploymentId": "unknown", "RELEASE_GIT_SHA": "unknown"}], [{"DENO_DEPLOY_BUILD_ID": "   ", "DENO_DEPLOYMENT_ID": " dep2 "}, {"isDeploy": true, "isProductionTimeline": false, "allowOrigin": "*", "authTokens": [], "adminTokens": [], "codexBaseUrl": "https://chatgpt.com/backend-api/codex", "codexAuthJsonB64": "", "runtimeGitSha": "unknown", "runtimeDeploymentId": "dep2", "RELEASE_GIT_SHA": "unknown"}], [{"DENO_DEPLOY_BUILD_ID": "", "DENO_DEPLOYMENT_ID": ""}, {"isDeploy": false, "isProductionTimeline": false, "allowOrigin": "*", "authTokens": [], "adminTokens": [], "codexBaseUrl": "https://chatgpt.com/backend-api/codex", "codexAuthJsonB64": "", "runtimeGitSha": "unknown", "runtimeDeploymentId": "unknown", "RELEASE_GIT_SHA": "unknown"}], [{"DENO_DEPLOYMENT_ID": "  "}, {"isDeploy": true, "isProductionTimeline": false, "allowOrigin": "*", "authTokens": [], "adminTokens": [], "codexBaseUrl": "https://chatgpt.com/backend-api/codex", "codexAuthJsonB64": "", "runtimeGitSha": "unknown", "runtimeDeploymentId": "unknown", "RELEASE_GIT_SHA": "unknown"}]])VEC";
  constexpr std::string_view CHILD_MARKER = "UOS_CONFIG_DIFFERENTIAL_CHILD";
  constexpr const char* CHILD_TEST = "config differential child";

  json::array sortedTokens(const std::unordered_set<std::string>& tokens) {
    std::vector<std::string> list(tokens.begin(), tokens.end());
    rt::sort(list);
    json::array out;
    for (const auto& token : list) out.push_back(json::string(token));
    return out;
  }

  json::value snapshot() {
    json::object out;
    out["isDeploy"] = config.isDeploy;
    out["isProductionTimeline"] = config.isProductionTimeline;
    out["allowOrigin"] = config.allowOrigin;
    out["authTokens"] = sortedTokens(config.authTokens);
    out["adminTokens"] = sortedTokens(config.adminTokens);
    out["codexBaseUrl"] = config.codexBaseUrl;
    out["codexAuthJsonB64"] = config.codexAuthJsonB64;
    out["runtimeGitSha"] = uos::config::runtimeGitSha();
    out["runtimeDeploymentId"] = uos::config::runtimeDeploymentId();
    out["RELEASE_GIT_SHA"] = std::string(uos::release::RELEASE_GIT_SHA);
    return out;
  }

  // How this binary was started (the SDK loader, its arguments and the binary).
  std::vector<std::string> selfCommand() {
#if defined(__APPLE__)
    return {uos::rt::process::execPath()};
#else
    const std::vector<std::uint8_t> bytes = rt::fs::readFileSync("/proc/self/cmdline");
    std::vector<std::string> argv;
    std::string current;
    for (const std::uint8_t byte : bytes) {
      if (byte == 0) {
        argv.push_back(current);
        current.clear();
      } else {
        current.push_back(static_cast<char>(byte));
      }
    }
    const bool viaLoader = !argv.empty() && rt::includes(argv[0], "ld-linux");
    const std::size_t keep = viaLoader ? 4 : 1;
    argv.resize(std::min(argv.size(), keep));
    return argv;
#endif
  }
}

TEST("config differential child") {
  if (!rt::env::has(CHILD_MARKER)) return;
  std::printf("CONFIG %s\n", rt::jsonStringify(snapshot()).c_str());
}

ASYNC_TEST("differential: config and runtime identity load like Bun") {
  const json::value scenarios = rt::jsonParse(SCENARIOS);
  std::vector<std::string> command = selfCommand();
  command.emplace_back(CHILD_TEST);
  for (const json::value& scenario : scenarios.as_array()) {
    std::vector<std::pair<std::string, std::string>> environment;
    for (const auto& [name, value] : scenario.as_array()[0].as_object()) environment.emplace_back(std::string(name), std::string(value.as_string()));
    environment.emplace_back(std::string(CHILD_MARKER), "1");
    rt::process::SpawnOptions options;
    options.env = environment;
    options.stdout = rt::process::Stdio::pipe;
    options.stderr = rt::process::Stdio::inherit;
    const rt::process::Subprocess child = rt::process::spawn(command, options);
    rt::Response response{rt::BodyInit(child.stdout())};
    auto reading = response.text();
    const std::string text = co_await std::move(reading);
    auto exited = child.exited();
    const int code = co_await exited.wait();
    EXPECT_EQ(code, 0);
    std::optional<json::value> printed;
    for (const std::string& line : rt::split(text, "\n"))
      if (rt::startsWith(line, "CONFIG ")) printed = rt::jsonParse(rt::slice(line, 7));
    ASSERT(printed.has_value());
    if (!rt::deepEqual(*printed, scenario.as_array()[1])) {
      EXPECT_EQ(*printed, scenario.as_array()[1]);
      std::fprintf(stderr, "      environment: %s\n", rt::jsonStringify(scenario.as_array()[0]).c_str());
    }
  }
}

TEST("runtime identity follows later environment changes and config mutation") {
  rt::env::ScopedOverride scope({{"DENO_DEPLOY_BUILD_ID", std::nullopt}, {"DENO_DEPLOYMENT_ID", std::nullopt}});
  EXPECT_EQ(uos::config::runtimeGitSha(), "unknown");
  EXPECT_EQ(uos::config::runtimeDeploymentId(), "unknown");
  {
    rt::env::ScopedOverride build({{"DENO_DEPLOY_BUILD_ID", " \t build-9 \n"}});
    EXPECT_EQ(uos::config::runtimeDeploymentId(), "build-9");
  }
  const bool previous = config.isDeploy;
  config.isDeploy = true;
  EXPECT_EQ(uos::config::runtimeDeploymentId(), "unknown");
  config.isDeploy = previous;
  config.adminTokens.insert("health-route-admin-token");
  EXPECT(config.adminTokens.count("health-route-admin-token") == 1);
  config.adminTokens.erase("health-route-admin-token");
}
