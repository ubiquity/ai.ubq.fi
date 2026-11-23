// w04c1 phase A (PA3): every PA3 header of the cycle cluster in one translation unit, with one
// trivial test so the binary links and runs through tools/unit.sh.
// Links: tests/cluster_w04c1/PA3_headers_test.cpp (unit.sh adds src/rt/*.cpp).
#include "uos/codex/auth.hpp"
#include "uos/codex/auth_refresh.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/codex/rate_limit_429.hpp"
#include "uos/codex/routing_evaluation.hpp"
#include "uos/codex/routing_mutations.hpp"
#include "uos/codex/banked_reset.hpp"
#include "uos/codex/banked_reset_claim.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/codex/experiment.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/codex/account_routing.hpp"
#include "uos/codex/native_auth.hpp"
#include "uos/codex/routing_serial.hpp"
#include "uos/codex/banked_reset_pool.hpp"
#include "uos/codex/banked_reset_submission.hpp"
#include "uos/codex/dispatch.hpp"
#include "uos/codex/index.hpp"
#include "uos/codex/responses_state.hpp"
#include "uos/codex/responses_operations.hpp"
#include "uos/codex/responses_runners.hpp"

#include "uos/rt/test.hpp"

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>

namespace rt = uos::rt;
namespace codex = uos::codex;

TEST("w04c1 PA3 headers: inline declarations and default-constructed records") {
  const codex::auth::CodexError error("Codex auth missing.", "codex_auth_missing", 503);
  EXPECT_EQ(error.name, std::string("CodexError"));
  EXPECT_EQ(static_cast<const rt::JsError&>(error).code, std::optional<std::string>("codex_auth_missing"));
  EXPECT(codex::index::codexErrorOf(std::make_exception_ptr(error)) != nullptr);
  EXPECT(codex::auth::codexErrorOf(std::make_exception_ptr(codex::dispatch::CodexActiveAccountFenceError())) == nullptr);
  EXPECT_EQ(codex::experiment::CodexCacheScopeExperimentError("x").name, std::string("CodexCacheScopeExperimentError"));
  EXPECT(codex::auth::CODEX_AUTH_POOL_KV_KEY == (rt::KvKey{"ubq_ai", "codex_auth"}));

  const auto ctx = std::make_shared<codex::responses_state::CodexResponseContext>();
  EXPECT(!ctx->fetchAttempt && !ctx->advanceActiveReselection && !ctx->lastError);
  EXPECT(std::holds_alternative<std::monostate>(ctx->options.beforeDispatch));
  EXPECT(codex::banked_reset_claim::StateWithPatch{}.provider_receipt_id == rt::undefined);
}

// A string literal resolves to the array overloads (unevaluated: the module .cpps are not linked here).
static_assert(std::is_same_v<decltype(codex::auth::getJwtExpMs("a.b")), std::optional<double>>);
static_assert(std::is_same_v<decltype(codex::banked_reset_claim::receiptId("receipt-1")), std::optional<std::string>>);
static_assert(std::is_same_v<decltype(codex::banked_reset::isNonEmptyText("text", 128)), bool>);
