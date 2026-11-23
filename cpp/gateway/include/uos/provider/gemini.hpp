#pragma once
// Antigravity Gemini subscription adapter. The transport is the official CLI
// headless stream-json contract documented at
// https://antigravity.google/docs/cli/headless/. Credentials remain owned and
// refreshed by `agy`; the gateway never reads or forwards the OAuth token.

#include "uos/openai_telemetry.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/process.hpp"
#include "uos/upstream_auth.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::provider::gemini {
  inline constexpr std::array<std::string_view, 3> GEMINI_MODEL_IDS{
    "gemini-3.8-flash-high",
    "gemini-3.8-flash-medium",
    "gemini-3.8-flash-low",
  };
  inline constexpr std::string_view GEMINI_PROVIDER_ID = "gemini";
  inline constexpr std::string_view GEMINI_OPT_IN_ENV = "UOS_GATEWAY_ENABLE_ANTIGRAVITY";
  inline constexpr double GEMINI_CONTEXT_WINDOW_TOKENS = 1'048'576;

  std::optional<std::string> geminiUpstreamModelFor(std::string_view model);
  bool isGeminiModel(std::string_view model);
  inline bool geminiOperatorOptIn() { return !upstream_auth::followerMode() && rt::env::get(GEMINI_OPT_IN_ENV) == "1"; }
  inline bool geminiCliConfigured() {
    try {
      if (!geminiOperatorOptIn()) return false;
      const auto home = rt::env::get("HOME");
      if (!home || home->empty()) return false;
      const auto executable = rt::fs::lstatSync(rt::path::join(*home, ".local/bin/agy"));
      if (!executable.isFile() || executable.isSymbolicLink() || executable.uid != rt::process::getuid() ||
          executable.nlink != 1 || (executable.mode & 077) != 0 || (executable.mode & 0100) == 0) {
        return false;
      }
      const std::string stateDirectory = rt::path::join(*home, ".gemini/antigravity-cli");
      const auto directory = rt::fs::lstatSync(stateDirectory);
      if (!directory.isDirectory() || directory.isSymbolicLink() || directory.uid != rt::process::getuid() || (directory.mode & 077) != 0) {
        return false;
      }
      const auto credentials = rt::fs::lstatSync(rt::path::join(stateDirectory, "antigravity-oauth-token"));
      return credentials.isFile() && !credentials.isSymbolicLink() && credentials.uid == rt::process::getuid() &&
        credentials.nlink == 1 && (credentials.mode & 077) == 0;
    } catch (...) {
      return false;
    }
  }

  struct GeminiCliOptions {
    std::string model;
    std::string prompt;
    std::optional<rt::AbortSignal> signal{};
    std::optional<std::string> jsonSchema{};
    std::string mode = "plan";
    bool disableSlashCommands = true;
    bool streamJson = true;
    std::function<void()> onDispatch{};
    std::function<void()> onInit{};
    double timeoutMs = 120'000;
  };

  struct GeminiCliTurn {
    std::string response;
    std::optional<std::string> conversationId;
    json::object usage;
    json::object rawResult;
    std::vector<std::string> deniedActions;
  };

  // Test seam. An empty runner selects the real installed `agy` executable.
  using GeminiCliRunner = std::function<rt::Task<GeminiCliTurn>(const GeminiCliOptions&)>;

  // Set by the sealed Linux bootstrap before requests can start.
  void setGeminiChildConfinement(std::function<void(const std::string&)> confine);
  rt::Task<GeminiCliTurn> runGeminiCli(GeminiCliOptions options);
  void setGeminiCliRunnerForTest(GeminiCliRunner runner);
  rt::Task<GeminiCliTurn> runGeminiCli(GeminiCliOptions options, GeminiCliRunner runner);

  struct GeminiHandlerOptions {
    GeminiCliRunner runner{};
    bool refreshQuota = true;
  };

  rt::Task<rt::Response> handleGeminiChatCompletions(rt::Request req, json::object rawRecord, std::string modelRaw,
    const openai_telemetry::UsageContext* usageContext, GeminiHandlerOptions options = {});
  rt::Task<rt::Response> handleGeminiResponses(rt::Request req, json::object rawRecord, std::string modelRaw,
    const openai_telemetry::UsageContext* usageContext, GeminiHandlerOptions options = {});

  std::optional<openai_telemetry::UsageTokens> geminiUsageTokens(const json::object& usage);
}
