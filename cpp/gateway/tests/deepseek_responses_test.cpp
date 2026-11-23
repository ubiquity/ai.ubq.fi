// Links: tests/deepseek_responses_test.cpp
// src/auth/relay.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp
// src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp
// src/authority/native_codex_mutations.cpp src/bounded_response_body.cpp src/cache/prompt_analytics_core.cpp
// src/cache/prompt_analytics_read.cpp src/codex/account_routing.cpp src/codex/auth.cpp
// src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp
// src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp
// src/codex/external_auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp
// src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp
// src/codex/responses_error.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp
// src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp
// src/deepseek/index.cpp src/deepseek/responses.cpp src/defaults.cpp
// src/http.cpp src/kv.cpp src/metered_quota.cpp
// src/models/codex_models.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp
// src/openai_telemetry.cpp src/paid_fallback/ledger_stats.cpp src/provider/anthropic_chat_responses.cpp
// src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp
// src/provider/anthropic_responses.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
// src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp
// src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp src/provider/cerebras.cpp
// src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp
// src/provider/claude_code.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
// src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp
// src/provider/health.cpp src/provider/lithos.cpp src/provider/lithos_rate_limits.cpp
// src/provider/metered.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp
// src/provider/surplus.cpp src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp
// src/release.cpp src/request_policy.cpp src/responses_failover_stream.cpp
// src/responses_stream.cpp src/runtime/websocket.cpp src/runtime_config.cpp
// src/storage/kv.cpp src/storage/native_kv.cpp
// src/storage/sqlite_readonly.cpp src/types.cpp src/utils.cpp
// src/cache/scope_targets.cpp src/deepseek/forwarded_payload_policy.cpp src/deepseek/responses_payload.cpp
// src/deepseek/chat_projection.cpp src/deepseek/responses_stream.cpp src/deepseek/flash_aliases.cpp
// src/provider/lithos_models.cpp src/lib/jev_compaction/state.cpp
// src/models/openrouter_models.cpp src/lib/jev_compaction/types.cpp
// Owned API parity from the original Bun tests. Upstream/handler assertions remain separate.
#include "helpers/deepseek_port_replay.hpp"
namespace {
#include "deepseek_responses_vectors.inc"
  const bool recorded = uos::tests::helpers::deepseek_port_replay::registerReplay(recordedVectors);
}
