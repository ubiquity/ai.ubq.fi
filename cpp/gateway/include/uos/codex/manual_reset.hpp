#pragma once
// Port of src/codex/manual-reset.ts.
#include "uos/codex/banked_reset.hpp"
#include "uos/codex/banked_reset_provider.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/types.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>
namespace uos::codex::manual_reset {
  class ManualResetError : public rt::JsError {
  public:
    double status;
    std::string code;
    std::optional<std::string> resetStatus;
    ManualResetError(double status_, std::string code_, std::string message_, std::optional<std::string> resetStatus_ = std::nullopt)
      : rt::JsError(std::move(message_)), status(status_), code(std::move(code_)), resetStatus(std::move(resetStatus_)) {}
  };
  struct ManualResetDependencies {
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    std::function<banked_reset::CodexBankedResetConfig()> reloadConfig{};
    rt::Fetch fetcher{};
    std::function<double()> now{};
    std::function<std::shared_ptr<banked_reset_provider::CodexUsageResetProvider>(::uos::types::CodexAuthState)> provider{};
  };
  struct ManualResetResult {
    std::string reset_status;
    std::string active_account_id_hash;
    friend bool operator==(const ManualResetResult&, const ManualResetResult&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const ManualResetResult& value);
  ManualResetResult tag_invoke(const json::value_to_tag<ManualResetResult>&, const json::value& value);
  rt::Task<ManualResetResult> consumeManualCodexReset(std::string hash);
  rt::Task<ManualResetResult> consumeManualCodexReset(std::string hash, ManualResetDependencies dependencies);
  rt::Task<ManualResetResult> consumeManualCodexReset(std::string hash, ManualResetDependencies dependencies, rt::AbortSignal signal);
}
