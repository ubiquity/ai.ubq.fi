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
  struct GeminiModelDefinition {
    std::string_view id;
    std::string_view display_name;
    std::string_view reasoning_effort;
    double context_window_tokens;
  };

  // Antigravity documents Gemini 4 Neon as "Up to 512K tokens":
  // https://antigravity.google/docs/models/#gemini-4-neon
  // Model catalogs express that binary token count as 512 * 1024.
  inline constexpr double GEMINI_NEON_CONTEXT_WINDOW_TOKENS = 512 * 1024;

  inline constexpr std::array<GeminiModelDefinition, 7> GEMINI_MODELS{{
    {"gemini-4-neon-low", "Gemini 4 Neon (Low)", "low", GEMINI_NEON_CONTEXT_WINDOW_TOKENS},
    {"gemini-4-neon-medium", "Gemini 4 Neon (Medium)", "medium", GEMINI_NEON_CONTEXT_WINDOW_TOKENS},
    {"gemini-4-neon-high", "Gemini 4 Neon (High)", "high", GEMINI_NEON_CONTEXT_WINDOW_TOKENS},
    {"gemini-4-neon-xhigh", "Gemini 4 Neon (XHigh)", "xhigh", GEMINI_NEON_CONTEXT_WINDOW_TOKENS},
    {"gemini-3.8-flash-high", "Gemini 3.8 Flash (High)", "high", 1'048'576},
    {"gemini-3.8-flash-medium", "Gemini 3.8 Flash (Medium)", "medium", 1'048'576},
    {"gemini-3.8-flash-low", "Gemini 3.8 Flash (Low)", "low", 1'048'576},
  }};
  inline constexpr std::string_view GEMINI_PROVIDER_ID = "gemini";
  inline constexpr std::string_view GEMINI_OPT_IN_ENV = "UOS_GATEWAY_ENABLE_ANTIGRAVITY";

  inline const GeminiModelDefinition* geminiModelDefinition(std::string_view model) {
    for (const auto& definition : GEMINI_MODELS) if (definition.id == model) return &definition;
    return nullptr;
  }

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
