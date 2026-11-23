#pragma once
// Port of src/openai.ts.
#include "uos/codex/dispatch.hpp"
#include "uos/defaults.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/rt/ordered.hpp"
namespace uos::openai {
  std::optional<rt::Response> temporaryFreeSurplusCapabilityError(const std::string& model, const json::object& body);
  rt::Task<std::optional<std::string>> getDefaultModel();
  rt::AbortSignal downstreamSignalFor(const rt::Request& request, const openai_telemetry::UsageContext* context = nullptr);
  rt::AbortSignal inferenceSignal(const rt::Request& request, const openai_telemetry::UsageContext* context = nullptr);
  using CodexBankedResetOptionsForTest = codex::dispatch::CodexBankedResetOptions;
  extern std::optional<CodexBankedResetOptionsForTest> codexBankedResetOptionsForTest;
  void setCodexBankedResetOptionsForTest(std::optional<CodexBankedResetOptionsForTest> options);
  rt::Response defaultModelUnavailableError();
  rt::Task<defaults::ReasoningEffort> getDefaultReasoningEffort();
  extern const rt::OrderedSet<std::string> CHAT_COMPLETIONS_ALLOWED_KEYS;
  extern const rt::OrderedSet<std::string> RESPONSES_ALLOWED_KEYS;
  extern const rt::OrderedSet<std::string> CODEX_RESPONSES_EXTENSION_KEYS;
  extern const rt::OrderedSet<std::string> DEEPSEEK_CHAT_EXTENSION_KEYS;
  std::optional<std::string> findUnknownKey(const json::object& record, const rt::OrderedSet<std::string>& allowed,
    const rt::OrderedSet<std::string>* extensions = nullptr);
}
