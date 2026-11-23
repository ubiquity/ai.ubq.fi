// Links: tests/admin_models_catalog_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
// src/admin/api_key_audit.cpp src/admin/api_key_mutations.cpp src/admin/api_keys.cpp src/admin/capacity_observation_sync.cpp src/admin/chart_source_proxy.cpp src/admin/claude_code.cpp src/admin/codex.cpp src/admin/codex_reset.cpp src/admin/defaults.cpp src/admin/error_log.cpp
// src/admin/kernel.cpp src/admin/sentinel_incident.cpp src/agent_messages.cpp src/analytics.cpp src/api_key_policy.cpp src/api_keys.cpp src/auth/index.cpp src/auth/local_admin.cpp src/auth/local_development_key.cpp src/auth/passkeys.cpp
// src/auth/passport.cpp src/auth/passport_sessions.cpp src/auth/relay.cpp src/authority/native_api_key.cpp src/authority/native_api_key_audit.cpp src/authority/native_api_key_policy.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp
// src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_kernel.cpp src/authority/native_kernel_contracts.cpp src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/authority/native_paid_policy.cpp src/bounded_response_body.cpp src/cache/kv_migration_base.cpp src/cache/kv_migration_paid_fallback.cpp
// src/cache/kv_migration_usage.cpp src/cache/kv_migration_validation.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/scope_experiment.cpp src/cache/scope_experiment_model.cpp src/cache/scope_experiment_targets.cpp src/cache/scope_targets.cpp src/cache/telemetry_gate.cpp
// src/catalog/index.cpp src/catalog/models.cpp src/catalog/promotion.cpp src/catalog/store.cpp src/catalog/types.cpp src/chat/completions.cpp src/chat/envelope.cpp src/chat/stream_translation.cpp src/codex/account_routing.cpp src/codex/auth.cpp
// src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_claim.cpp src/codex/banked_reset_pool.cpp src/codex/banked_reset_provider.cpp src/codex/banked_reset_submission.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp
// src/codex/index.cpp src/codex/manual_reset.cpp src/codex/manual_reset_state.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/quota.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/responses_error.cpp
// src/codex/responses_operations.cpp src/codex/responses_runners.cpp src/codex/responses_state.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/codex/supervisor.cpp src/codex/supervisor_brief.cpp src/codex/supervisor_config.cpp
// src/codex/supervisor_follow.cpp src/codex/supervisor_inventory.cpp src/codex/supervisor_log.cpp src/codex/supervisor_secret.cpp src/codex/supervisor_transport.cpp src/config.cpp src/debug_routing.cpp src/decisions/handlers.cpp src/deepseek/chat_projection.cpp src/deepseek/context_admission_failure.cpp
// src/deepseek/flash_aliases.cpp src/deepseek/forwarded_payload_policy.cpp src/deepseek/handlers.cpp src/deepseek/index.cpp src/deepseek/responses.cpp src/deepseek/responses_payload.cpp src/deepseek/responses_stream.cpp src/deepseek/stream.cpp src/deepseek/waterfall.cpp src/deepseek/waterfall_handler.cpp
// src/defaults.cpp src/embeddings/handlers.cpp src/embeddings/jobs.cpp src/embeddings/ledger.cpp src/embeddings/voyage.cpp src/handler/admission.cpp src/handler/http.cpp src/handler/index.cpp src/handler/terminal_log.cpp src/handler/terminal_route.cpp
// src/health.cpp src/http.cpp src/images.cpp src/inference_admission.cpp src/inference_deadline.cpp src/input_normalization.cpp src/kernel/attestation.cpp src/kernel/policy_queue.cpp src/kernel/quota_reservations.cpp src/kernel/quota_reserve.cpp
// src/kernel/quota_v2.cpp src/kernel/quota_write.cpp src/kernel/usage.cpp src/kv.cpp src/kv_conversion/profile_json.cpp src/lib/jev_compaction/codex_items.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/request.cpp src/lib/jev_compaction/state.cpp src/lib/jev_compaction/types.cpp
// src/live/calls.cpp src/live/handler.cpp src/live/routes.cpp src/live/sideband.cpp src/live/upstream.cpp src/metered_quota.cpp src/models/catalog.cpp src/models/codex_models.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp
// src/models/codex_models_whitelist.cpp src/models/metadata.cpp src/models/openrouter_models.cpp src/openai.cpp src/openai_schema.cpp src/openai_telemetry.cpp src/optional_telemetry_queue.cpp src/paid_fallback/claude_code_route.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp
// src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/removed_provider.cpp src/paid_fallback/rollups.cpp src/paid_fallback/routing.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp
// src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
// src/provider/cerebras.cpp src/provider/cerebras_handlers.cpp src/provider/cerebras_rate_limits.cpp src/provider/cerebras_responses.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code.cpp src/provider/claude_code_access.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
// src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/lithos.cpp src/provider/lithos_handlers.cpp src/provider/lithos_models.cpp src/provider/lithos_rate_limits.cpp src/provider/lithos_streams.cpp src/provider/metered.cpp
// src/provider/openrouter.cpp src/provider/openrouter_handlers.cpp src/provider/openrouter_request_controls.cpp src/provider/openrouter_streams.cpp src/provider/openrouter_web_tools.cpp src/provider/presentation.cpp src/provider/removed_provider_circuit.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/stream_relay.cpp
// src/provider/surplus.cpp src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/quota_projection.cpp src/recent_model_context.cpp src/release.cpp src/request.cpp src/request_policy.cpp src/responses_attempts.cpp src/responses_buffered.cpp
// src/responses_failover_stream.cpp src/responses_handler.cpp src/responses_native_headers.cpp src/responses_request.cpp src/responses_stream.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/websocket.cpp src/runtime_config.cpp src/sentinel/incident_outbox.cpp
// src/sentinel/replay_admin.cpp src/sentinel/replay_envelope.cpp src/sentinel/replay_limits.cpp src/sentinel/replay_model.cpp src/sentinel/replay_observation.cpp src/sentinel/replay_read.cpp src/sentinel/replay_retention.cpp src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_retention_metadata.cpp src/sentinel/replay_retention_schema.cpp
// src/sentinel/replay_store.cpp src/sentinel/upstream_capture.cpp src/static.cpp src/storage/kv.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/systemone/handlers.cpp src/types.cpp src/upstream_wire.cpp
// src/utils.cpp tests/helpers/codex_diff_kv.cpp
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include <array>
#include "uos/handler/index.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/test_async.hpp"

namespace {
  struct T2PublicAssets {
    std::vector<std::string> createdPaths;
    T2PublicAssets() {
      const auto temporary = uos::rt::env::get("TMPDIR");
      const auto directory = uos::rt::process::cwd();
      if (!temporary || !directory.starts_with(uos::rt::path::dirname(*temporary) + "/"))
        throw uos::rt::JsError("T2 public assets require the wrapper task workdir");
      const auto root = uos::rt::path::resolve(uos::rt::path::dirname(__FILE__), "../../..");
      const std::array<std::string, 3> names{"static", "favicon.png", "favicon-32.png"};
      for (const auto& name : names) {
        const auto target = uos::rt::path::join(root, name);
        const auto assetPath = uos::rt::path::join(directory, name);
        if (uos::rt::fs::existsSync(assetPath)) {
          if (uos::rt::fs::realpathSync(assetPath) != uos::rt::fs::realpathSync(target))
            throw uos::rt::JsError("T2 public asset fixture refuses an unexpected existing path");
        } else {
          uos::rt::fs::symlinkSync(target, assetPath);
          createdPaths.push_back(assetPath);
        }
      }
    }
    ~T2PublicAssets() {
      for (const auto& assetPath : createdPaths) uos::rt::fs::rmSync(assetPath);
    }
  };
  T2PublicAssets& t2PublicAssets() {
    static T2PublicAssets assets;
    return assets;
  }
  struct T2IngressScope {
    uos::config::Config savedConfig = uos::config::config;
    uos::rt::env::ScopedOverride credentials{{"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    uos::rt::GlobalFetchOverride blocked{uos::rt::Fetch([](uos::rt::RequestInfo, uos::rt::RequestInit) -> uos::rt::Response {
      throw uos::rt::JsError("T2 ingress fixture forbids an unstubbed upstream call");
    })};
    explicit T2IngressScope(std::shared_ptr<uos::rt::Kv> database = nullptr) {
      (void)t2PublicAssets();
      uos::config::config.codexAuthJsonB64.clear();
      uos::config::config.authTokens.clear();
      uos::config::config.adminTokens.clear();
      if (!database) database = std::make_shared<uos::rt::MemoryKv>(uos::rt::dateNow);
      uos::kv::setKvForTest(std::move(database));
    }
    ~T2IngressScope() {
      uos::kv::setKvForTest(nullptr);
      uos::config::config = savedConfig;
    }
  };
}
#include "uos/models/codex_models_whitelist.hpp"
#include "uos/deepseek/waterfall.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace w = uos::models::codex_models_whitelist;
  const w::CodexModelsWhitelist saved(std::vector<std::string> ids) { return {std::move(ids), 1}; }
  json::array ids(const json::array& rows) { json::array result; for (const auto& row : rows) result.push_back(*rt::get(row, "id")); return result; }
#include "codex_models_whitelist_vectors.inc"
}
// Catalog and admin behavior with browser asset drift guards.
// Real handler/index ingress covers the picker gate and rejected-optional-snapshot authentication assertion.
#include "uos/admin/codex.hpp"
#include "uos/catalog/types.hpp"
#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/models/catalog.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "tests/helpers/codex_diff_kv.hpp"
#include <algorithm>
#include <set>
namespace {
  namespace admin = uos::admin::codex;
  namespace catalog = uos::models::catalog;
  namespace mf = uos::models::codex_models_fetch;
  namespace orouter = uos::models::openrouter_models;
  namespace selection = uos::provider::selection;
  namespace metered = uos::provider::metered;
  namespace surplus = uos::provider::surplus;
  namespace diff = uos::tests::helpers::codex_diff_kv;
  const std::string WATERFALL(uos::deepseek::waterfall::DEEPSEEK_WATERFALL_MODEL_ID);
#define AWAIT(name, ...) auto name##Task=(__VA_ARGS__); auto name=co_await std::move(name##Task)
  class PickerKv final : public diff::DiffKv {
  public:
    std::set<std::string> rejectedKeys;
    struct Read {std::string key;std::optional<rt::KvConsistencyLevel> consistency;};
    std::vector<Read> reads;
    void put(const rt::KvKey& key,const rt::KvValue& value){rows.set(diff::keyOf(key),Row{key,value,stamp()});}
    using rt::Kv::get;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key,rt::KvGetOptions options={}) override {const auto encoded=diff::keyOf(key);reads.push_back({encoded,options.consistency});co_await rt::microtask();if(rejectedKeys.contains(encoded))throw rt::JsError("fixture KV read unavailable");co_return entryOf(key);}
    rt::Task<rt::KvCommitResult> set(rt::KvKey key,rt::KvValue value,rt::KvSetOptions={}) override {put(key,value);rt::KvCommitResult result{true,*entryOf(key).versionstamp};co_await rt::microtask();co_return result;}
  };
  struct PickerFixture {
    rt::env::ScopedEnvironment env{std::vector<std::pair<std::string,std::string>>{{"HOME",rt::env::get("HOME").value_or("/nonexistent/w07c4-picker")},{"TMPDIR",rt::env::get("TMPDIR").value_or("/tmp")}}};
    std::string auth=uos::config::config.codexAuthJsonB64;
    std::shared_ptr<PickerKv> kv=std::make_shared<PickerKv>();
    rt::GlobalFetchOverride reject{rt::Fetch([](rt::RequestInfo,rt::RequestInit)->rt::Response{throw rt::JsError("Provider discovery requires an explicit local fixture");})};
    PickerFixture(){uos::config::config.codexAuthJsonB64.clear();reset();uos::kv::setKvForTest(kv);uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const auto&){});}
    static void reset(){uos::codex::auth::resetCodexAuthCacheForTest();uos::runtime_config::resetRuntimeConfigCacheForTest();selection::resetProviderSelectionCacheForTest();orouter::resetOpenRouterModelsCacheForTest();metered::resetMeteredModelsCacheForTest();surplus::resetSurplusModelsCacheForTest();uos::provider::claude_code_models::resetClaudeCodeModelsCacheForTest();uos::provider::health::resetProviderHealthThrottleForTest();}
    ~PickerFixture(){uos::kv::setKvForTest(nullptr);reset();metered::setMeteredModelsFetchForTest({});orouter::setOpenRouterModelsFetchForTest({});uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest({});uos::config::config.codexAuthJsonB64=auth;}
  };
  rt::Request post(const std::string& body){rt::RequestInit init;init.method="POST";init.body=body;return rt::Request("https://ai.ubq.fi/admin/models/whitelist",init);}
  rt::Request versionRequest(const std::string& version="0.203.0"){return rt::Request("https://ai.ubq.fi/v1/models?client_version="+version);}
  std::string requestUrl(const rt::RequestInfo& input){return std::visit([](const auto& value)->std::string{using T=std::decay_t<decltype(value)>;if constexpr(std::is_same_v<T,std::string>)return value;else if constexpr(std::is_same_v<T,rt::URL>)return value.href();else return value.url();},input);}
  rt::Fetch payloadFetcher(json::value payload){return rt::Fetch([payload](rt::RequestInfo,rt::RequestInit){return rt::Response::json(payload);});}
  rt::Task<void> enrich(json::array rows){orouter::FetchOpenRouterModelsOptions options;options.force=true;options.fetcher=payloadFetcher(json::object{{"data",rows}});AWAIT(snapshot,orouter::fetchOpenRouterModels(options));(void)snapshot;}
  json::array providerIds(const catalog::PublicModelCatalogEntry& row){json::array result;for(const auto& provider:row.providers)result.push_back(json::value(provider.id));return result;}
  json::array rowIds(const std::vector<catalog::PublicModelCatalogEntry>& rows){json::array result;for(const auto& row:rows)result.push_back(json::value(row.id));return result;}
  json::array jsonFields(const json::array& rows,std::string_view field){json::array result;for(const auto& row:rows){const auto* item=rt::get(row,field);result.push_back(item?*item:json::value(nullptr));}return result;}
  const catalog::PublicModelCatalogEntry* rowById(const catalog::ModelCatalogSnapshot& snapshot,std::string_view id){for(const auto& row:snapshot.models)if(row.id==id)return &row;return nullptr;}
  const json::value* jsonRow(const json::array& rows,std::string_view id){for(const auto& row:rows)if(rt::getString(row,"id")==id)return &row;return nullptr;}
  catalog::ModelCatalogSnapshot catalogFixture(){
    return json::value_to<catalog::ModelCatalogSnapshot>(rt::jsonParse(R"({"models":[{"id":"gpt-5.6-sol","providers":[{"id":"codex","owned_by":"openai","supported_endpoints":["/v1/responses"]}],"created":1800000000},{"id":"gpt-5.6-terra","providers":[{"id":"codex","owned_by":"openai","supported_endpoints":["/v1/responses"]}],"created":1700000000},{"id":"kimi-k2","providers":[{"id":"surplus","owned_by":"moonshot","supported_endpoints":["/v1/chat/completions"]}]},{"id":"deepseek-v4-flash","providers":[{"id":"deepseek","owned_by":"deepseek","supported_endpoints":["/v1/chat/completions","/v1/responses"]}]}],"sources":{"claude_code":{"status":"unavailable","count":0,"updated_at_ms":null,"configured":false},"codex":{"status":"available","count":2,"updated_at_ms":1},"openlux":{"status":"unavailable","count":0,"updated_at_ms":null},"surplus":{"status":"available","count":1,"updated_at_ms":2},"deepseek":{"status":"available","count":1,"updated_at_ms":null,"configured":true},"cerebras":{"status":"unavailable","count":0,"updated_at_ms":null,"configured":false},"lithos":{"status":"unavailable","count":0,"updated_at_ms":null,"configured":false},"openrouter":{"status":"unavailable","count":0,"updated_at_ms":null}}})"));
  }
  std::string asset(std::string_view name){return rt::fs::readFileSync(rt::path::resolve(rt::path::dirname(__FILE__),"../../..",name),"utf8");}
  std::string capture(const std::string& source,const std::string& pattern){const auto match=rt::Regex(pattern).exec(source);return match?(*match)[1].value_or(""):"";}
  bool matches(const std::string& source,const std::string& pattern,const std::string& flags=""){return rt::Regex(pattern,flags).test(source);}
}
ASYNC_TEST("admin model picker lists every discovered model alongside the saved selection") {
  PickerFixture fixture;fixture.kv->put(w::CODEX_MODELS_WHITELIST_KV_KEY,json::object{{"model_ids",json::array{"gpt-5.6-sol"}},{"updated_at_ms",1700000000000.0}});int builds=0;admin::CatalogDependencies dependencies;dependencies.buildCatalog=[&builds]{++builds;return rt::fulfilledTask(catalogFixture());};AWAIT(response,admin::handleAdminModelsCatalogGet(dependencies));EXPECT_EQ(response.status(),200);EXPECT_EQ(response.headers().get("cache-control"),"no-store");EXPECT_EQ(builds,1);AWAIT(body,response.json());const auto& data=body.at("data");EXPECT(rt::deepEqual(jsonFields(data.at("models").as_array(),"id"),json::array{"gpt-5.6-sol","gpt-5.6-terra","kimi-k2","deepseek-v4-flash"}));EXPECT(rt::deepEqual(data.at("whitelist").at("model_ids"),json::array{"gpt-5.6-sol"}));EXPECT_EQ(rt::getNumber(data.at("whitelist"),"updated_at_ms"),1700000000000.0);EXPECT_EQ(rt::getBool(data,"filter_active"),true);const auto expected=json::value_from(catalogFixture());EXPECT(rt::deepEqual(data.at("sources"),expected.at("sources")));
}
ASYNC_TEST("an empty stored whitelist is reported as no filter, not as nothing selected") {
  PickerFixture fixture;admin::CatalogDependencies dependencies;dependencies.buildCatalog=[]{return rt::fulfilledTask(catalogFixture());};AWAIT(empty,admin::handleAdminModelsCatalogGet(dependencies));AWAIT(emptyBody,empty.json());EXPECT(rt::deepEqual(emptyBody.at("data").at("whitelist"),json::object{{"model_ids",json::array{}},{"updated_at_ms",0}}));EXPECT_EQ(rt::getBool(emptyBody.at("data"),"filter_active"),false);fixture.kv->put(w::CODEX_MODELS_WHITELIST_KV_KEY,json::object{{"model_ids",json::array{}},{"updated_at_ms",1700000000000.0}});AWAIT(stored,admin::handleAdminModelsCatalogGet(dependencies));AWAIT(storedBody,stored.json());EXPECT_EQ(rt::getBool(storedBody.at("data"),"filter_active"),false);
}
ASYNC_TEST("the official DeepSeek ids are cataloged as their own provider category") {
  PickerFixture fixture;rt::env::set("DEEPSEEK_API_KEY","fixture-deepseek-key");AWAIT(snapshot,catalog::buildModelCatalogSnapshot());std::vector<catalog::PublicModelCatalogEntry> rows;for(const auto& row:snapshot.models)if(row.id!=WATERFALL&&std::any_of(row.providers.begin(),row.providers.end(),[](const auto& provider){return provider.id=="deepseek";}))rows.push_back(row);EXPECT(rt::deepEqual(rowIds(rows),json::array{"deepseek-flash","deepseek-v4-flash","deepseek-v4-pro"}));for(const auto& row:rows){EXPECT(rt::deepEqual(providerIds(row),json::array{"deepseek"}));EXPECT_EQ(row.providers[0].supported_endpoints,(std::vector<std::string>{"/v1/chat/completions","/v1/responses"}));EXPECT_EQ(row.context_source,"provider_discovery");EXPECT_EQ(row.context_window_tokens,1000000);}EXPECT(rt::deepEqual(json::value_from(*snapshot.sources.get("deepseek")),json::object{{"status","available"},{"count",3},{"updated_at_ms",nullptr},{"configured",true}}));EXPECT(rt::deepEqual(json::value_from(*snapshot.sources.get("cerebras")),json::object{{"status","unavailable"},{"count",0},{"updated_at_ms",nullptr},{"configured",false}}));
}
ASYNC_TEST("openai: model capabilities are exposed outside /v1 model objects") {
  PickerFixture fixture;
  const auto snapshot = rt::jsonParse(R"({
    "source":"chatgpt_codex","client_version":"0.125.0","updated_at_ms":1800000000000,
    "models":[{
      "slug":"gpt-5-fixture-default","display_name":"GPT-5 Fixture Default",
      "context_window":272000,"max_context_window":1000000,"auto_compact_token_limit":null,
      "default_reasoning_level":"medium","supported_reasoning_levels":["none","low","medium","high","xhigh","max","ultra"],
      "reasoning_effort_wire_map":{"ultra":"max"}
    }]
  })");
  fixture.kv->put(uos::codex::auth::CODEX_MODELS_KV_KEY, snapshot);
  AWAIT(response, catalog::handleModelCapabilities());
  EXPECT_EQ(response.status(), 200);
  AWAIT(body, response.json());
  EXPECT_EQ(rt::getString(body, "object"), "list");
  const auto* row = jsonRow(body.at("data").as_array(), "gpt-5-fixture-default");
  ASSERT(row);
  EXPECT_EQ(rt::getString(*row, "object"), "uos.model_capabilities");
  EXPECT_EQ(rt::getString(*row, "upstream_provider"), "codex_chatgpt");
  EXPECT(rt::deepEqual(row->at("supported_reasoning_levels"), json::array{"none", "low", "medium", "high", "xhigh", "max", "ultra"}));
  EXPECT_EQ(rt::getString(*row, "default_reasoning_effort"), "medium");
  EXPECT(rt::deepEqual(row->at("reasoning_effort_wire_map"), json::object{{"ultra", "max"}}));
  EXPECT_EQ(rt::getNumber(*row, "context_window_tokens"), 272'000);
  EXPECT_EQ(rt::getNumber(*row, "max_context_window_tokens"), 1'000'000);
  EXPECT_EQ(rt::getNumber(*row, "auto_compact_token_limit_tokens"), 222'000);
  const auto& endpoints = row->at("supported_endpoints").as_array();
  EXPECT(rt::includes(endpoints, json::value("/v1/chat/completions")));
  EXPECT(rt::includes(endpoints, json::value("/v1/responses")));
}
ASYNC_TEST("openai: configured DeepSeek official models replace a row from the Codex snapshot") {
  PickerFixture fixture;
  rt::env::set("DEEPSEEK_API_KEY", "deepseek-test-key");
  const auto snapshot = rt::jsonParse(R"({
    "source":"chatgpt_codex","client_version":"0.125.0","updated_at_ms":1800000000000,
    "models":[{"slug":"gpt-5-fixture-default"},{"slug":"deepseek-v4-flash","display_name":"DeepSeek V4 Flash"}]
  })");
  fixture.kv->put(uos::codex::auth::CODEX_MODELS_KV_KEY, snapshot);
  fixture.kv->put(uos::runtime_config::RUNTIME_CONFIG_V2_KEY, json::value_from(uos::runtime_config::buildRuntimeConfig(snapshot.as_object())));
  AWAIT(models, catalog::handleModels());
  EXPECT_EQ(models.status(), 200);
  AWAIT(modelList, models.json());
  AWAIT(capabilities, catalog::handleModelCapabilities());
  EXPECT_EQ(capabilities.status(), 200);
  AWAIT(capabilityList, capabilities.json());
  for (const std::string_view id : {"deepseek-flash", "deepseek-v4-flash"}) {
    const auto matchesId = [id](const json::value& row) { return rt::getString(row, "id") == id; };
    const auto& modelRows = modelList.at("data").as_array();
    EXPECT_EQ(std::count_if(modelRows.begin(), modelRows.end(), matchesId), 1);
    const auto* model = jsonRow(modelRows, id);
    ASSERT(model);
    EXPECT(rt::deepEqual(*model, json::object{{"id", id}, {"object", "model"}, {"created", 0}, {"owned_by", "deepseek"}}));
    const auto& capabilityRows = capabilityList.at("data").as_array();
    EXPECT_EQ(std::count_if(capabilityRows.begin(), capabilityRows.end(), matchesId), 1);
    const auto* row = jsonRow(capabilityRows, id);
    ASSERT(row);
    const json::object expected{
      {"id", id}, {"object", "uos.model_capabilities"}, {"owned_by", "deepseek"},
      {"display_name", id == "deepseek-flash" ? "DeepSeek Flash" : "DeepSeek Flash (legacy id)"},
      {"upstream_provider", "deepseek"}, {"supported_endpoints", json::array{"/v1/chat/completions", "/v1/responses"}},
      {"supported_reasoning_levels", json::array{"none", "low", "high", "max"}}, {"default_reasoning_effort", "high"},
      {"reasoning_effort_wire_map", json::object{{"ultra", "max"}}}, {"context_window_tokens", 1'000'000},
      {"max_context_window_tokens", 1'000'000}, {"auto_compact_token_limit_tokens", 900'000},
      {"effective_context_window_percent", 95}, {"context_source", "provider_discovery"},
    };
    EXPECT(rt::deepEqual(*row, expected));
  }
}
ASYNC_TEST("a paid-discovered DeepSeek id receives the later official route metadata") {
  PickerFixture fixture;rt::env::set("SURPLUS_API_KEY","fixture-surplus-key");rt::env::set("DEEPSEEK_API_KEY","fixture-deepseek-key");const json::array rows{json::object{{"id","deepseek-v4-flash"},{"context_length",1048576},{"top_provider",json::object{{"context_length",1048576}}},{"reasoning",json::object{{"supported_efforts",json::array{"third-party"}},{"default_effort","third-party"}}}}};auto enrichment=enrich(rows);co_await std::move(enrichment);rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo input,rt::RequestInit)->rt::Response{if(requestUrl(input).find("surplusintelligence.ai/v1/models")!=std::string::npos)return rt::Response::json(json::object{{"data",json::array{json::object{{"id","deepseek-v4-flash"},{"created",1800000000},{"provider","surplus"}}}}});throw rt::JsError("Unexpected discovery request: "+requestUrl(input));}));AWAIT(snapshot,catalog::buildModelCatalogSnapshot());const auto* row=rowById(snapshot,"deepseek-v4-flash");ASSERT(row);EXPECT(rt::deepEqual(providerIds(*row),json::array{"surplus","deepseek"}));EXPECT_EQ(row->context_window_tokens,1000000);EXPECT_EQ(row->max_context_window_tokens,1000000);EXPECT_EQ(row->supported_reasoning_levels,(std::vector<std::string>{"none","low","high","max"}));EXPECT_EQ(row->default_reasoning_effort,"high");EXPECT_EQ(row->context_source,"provider_discovery");EXPECT_EQ(row->reasoning_source,"provider_discovery");
}
ASYNC_TEST("the gateway waterfall model is cataloged with one provider row per usable hop in cost-first order") {
  PickerFixture fixture;
  rt::env::set("OPENROUTER_API_KEY", "fixture-openrouter-key");
  rt::env::set("DEEPSEEK_API_KEY", "fixture-deepseek-key");
  const auto rows = rt::jsonParse(R"([{"id":"~deepseek/deepseek-flash-latest","context_length":1048576,"top_provider":{"context_length":1048576}}])").as_array();
  auto enrichment = enrich(rows);
  co_await std::move(enrichment);
  AWAIT(snapshot, catalog::buildModelCatalogSnapshot());
  const auto* row = rowById(snapshot, WATERFALL);
  ASSERT(row);
  EXPECT(rt::deepEqual(providerIds(*row), json::array{"ubiquity", "deepseek", "openrouter"}));
  EXPECT_EQ(row->providers[0].supported_endpoints, (std::vector<std::string>{"/v1/responses"}));
  EXPECT_EQ(row->providers[0].owned_by, "ubiquity");
  EXPECT_EQ(row->context_window_tokens, 800'000);
  EXPECT_EQ(row->max_context_window_tokens, 800'000);
  EXPECT_EQ(row->auto_compact_token_limit_tokens, 720'000);
  EXPECT_EQ(row->context_source, "provider_discovery");
  EXPECT_EQ(row->supported_reasoning_levels, (std::vector<std::string>{"low", "high", "max"}));
  EXPECT_EQ(row->default_reasoning_effort, "high");
}
ASYNC_TEST("the gateway waterfall model lists the paid hops, OpenLux last, when their credentials are configured") {
  PickerFixture fixture;
  rt::env::set("SURPLUS_API_KEY", "fixture-surplus-key");
  rt::env::set("METERED_API_KEY", "fixture-metered-key");
  AWAIT(snapshot, catalog::buildModelCatalogSnapshot());
  const auto* row = rowById(snapshot, WATERFALL);
  ASSERT(row);
  EXPECT(rt::deepEqual(providerIds(*row), json::array{"ubiquity", "surplus", "openlux"}));
  for (std::size_t i = 1; i < row->providers.size(); ++i) {
    EXPECT_EQ(row->providers[i].owned_by, "ubiquity");
    EXPECT_EQ(row->providers[i].supported_endpoints, (std::vector<std::string>{"/v1/responses"}));
  }
  EXPECT_EQ(row->context_window_tokens, 800'000);
  EXPECT_EQ(row->max_context_window_tokens, 800'000);
  EXPECT_EQ(row->auto_compact_token_limit_tokens, 720'000);
  EXPECT_EQ(row->supported_reasoning_levels, (std::vector<std::string>{"low", "high", "max"}));
  EXPECT_EQ(row->default_reasoning_effort, "high");
}
ASYNC_TEST("a credential-gated provider with no key is absent instead of unavailable") {
  PickerFixture fixture;AWAIT(snapshot,catalog::buildModelCatalogSnapshot());for(const auto& row:snapshot.models)for(const auto& provider:row.providers){EXPECT(provider.id!="deepseek");EXPECT(provider.id!="cerebras");}EXPECT_EQ(snapshot.sources.get("deepseek")->configured,false);EXPECT_EQ(snapshot.sources.get("deepseek")->status,"unavailable");EXPECT_EQ(snapshot.sources.get("cerebras")->configured,false);
}
ASYNC_TEST("Cerebras claims its id unless the Codex snapshot already owns it") {
  PickerFixture fixture;rt::env::set("CEREBRAS_API_KEY","fixture-cerebras-key");AWAIT(snapshot,catalog::buildModelCatalogSnapshot());const auto* row=rowById(snapshot,"gpt-oss-120b");ASSERT(row);EXPECT(rt::deepEqual(providerIds(*row),json::array{"cerebras"}));EXPECT_EQ(row->providers[0].supported_endpoints,(std::vector<std::string>{"/v1/chat/completions"}));const auto* qwen=rowById(snapshot,"qwen-3.8-27b");ASSERT(qwen);EXPECT(rt::deepEqual(providerIds(*qwen),json::array{"cerebras"}));EXPECT(rt::deepEqual(json::value_from(*snapshot.sources.get("cerebras")),json::object{{"status","available"},{"count",2},{"updated_at_ms",nullptr},{"configured",true}}));
}
ASYNC_TEST("the model picker refuses to render without KV") {
  PickerFixture fixture;uos::kv::setKvForTest(nullptr);admin::CatalogDependencies dependencies;dependencies.buildCatalog=[]()->rt::Task<catalog::ModelCatalogSnapshot>{throw rt::JsError("the catalog must not be built without KV");co_return catalog::ModelCatalogSnapshot{};};AWAIT(response,admin::handleAdminModelsCatalogGet(dependencies));EXPECT_EQ(response.status(),500);AWAIT(body,response.json());EXPECT_EQ(rt::getString(body.at("error"),"type"),"server_error");
}
ASYNC_TEST("catalog builder, capabilities, and public/admin handlers retain runtime ids and tiers when the optional full snapshot rejects") {
  PickerFixture fixture;fixture.kv->rejectedKeys.insert(diff::keyOf(uos::codex::auth::CODEX_MODELS_KV_KEY));const auto snapshot=rt::jsonParse(R"({"source":"runtime-fallback-fixture","updated_at_ms":1800000000000,"models":[{"slug":"runtime-fallback","supported_reasoning_levels":["low","ultra"],"default_reasoning_level":"ultra"},{"slug":"runtime-hidden","supported_reasoning_levels":["none"],"default_reasoning_level":"none"}]})").as_object();const auto runtime=uos::runtime_config::buildRuntimeConfig(snapshot);fixture.kv->put(uos::runtime_config::RUNTIME_CONFIG_V2_KEY,json::value_from(runtime));fixture.kv->put(w::CODEX_MODELS_WHITELIST_KV_KEY,json::object{{"model_ids",json::array{"runtime-fallback"}},{"updated_at_ms",1800000000001.0}});fixture.kv->put(selection::PROVIDER_SELECTION_KV_KEY,json::object{{"provider_ids",json::array{"codex"}},{"updated_at_ms",1800000000002.0}});
  AWAIT(built,catalog::buildModelCatalogSnapshot());std::vector<catalog::PublicModelCatalogEntry> codexRows;for(const auto& row:built.models)for(const auto& provider:row.providers)if(provider.id=="codex")codexRows.push_back(row);EXPECT(rt::deepEqual(rowIds(codexRows),json::array{"runtime-fallback","runtime-hidden"}));ASSERT_EQ(codexRows.size(),2u);EXPECT_EQ(codexRows[0].supported_reasoning_levels,(std::vector<std::string>{"low","ultra"}));EXPECT_EQ(codexRows[0].default_reasoning_effort,"ultra");EXPECT(rt::deepEqual(json::value_from(*built.sources.get("codex")),json::object{{"status","available"},{"count",2},{"updated_at_ms",1800000000000.0}}));
  AWAIT(publicResponse,catalog::handlePublicModelCatalog());EXPECT_EQ(publicResponse.status(),200);AWAIT(publicBody,publicResponse.json());EXPECT_EQ(rt::getString(publicBody,"object"),"uos.model_catalog");EXPECT(rt::deepEqual(jsonFields(publicBody.at("data").as_array(),"id"),json::array{"runtime-fallback"}));EXPECT(rt::deepEqual(publicBody.at("data").as_array()[0].at("supported_reasoning_levels"),json::array{"low","ultra"}));EXPECT_EQ(rt::getString(publicBody.at("data").as_array()[0],"default_reasoning_effort"),"ultra");
  AWAIT(capabilities,catalog::handleModelCapabilities());EXPECT_EQ(capabilities.status(),200);AWAIT(capabilitiesBody,capabilities.json());EXPECT(rt::deepEqual(jsonFields(capabilitiesBody.at("data").as_array(),"id"),json::array{"runtime-fallback"}));EXPECT(rt::deepEqual(capabilitiesBody.at("data").as_array()[0].at("supported_reasoning_levels"),json::array{"low","ultra"}));EXPECT_EQ(rt::getString(capabilitiesBody.at("data").as_array()[0],"default_reasoning_effort"),"ultra");
  AWAIT(adminResponse,admin::handleAdminModelsCatalogGet());EXPECT_EQ(adminResponse.status(),200);AWAIT(adminBody,adminResponse.json());json::array adminIds;for(const auto& row:adminBody.at("data").at("models").as_array())for(const auto& provider:row.at("providers").as_array())if(rt::getString(provider,"id")=="codex")adminIds.push_back(row.at("id"));EXPECT(rt::deepEqual(adminIds,json::array{"runtime-fallback","runtime-hidden"}));const auto* adminRow=jsonRow(adminBody.at("data").at("models").as_array(),"runtime-fallback");ASSERT(adminRow);EXPECT(rt::deepEqual(adminRow->at("supported_reasoning_levels"),json::array{"low","ultra"}));EXPECT_EQ(rt::getString(*adminRow,"default_reasoning_effort"),"ultra");EXPECT(rt::deepEqual(adminBody.at("data").at("whitelist").at("model_ids"),json::array{"runtime-fallback"}));EXPECT_EQ(rt::getBool(adminBody.at("data"),"filter_active"),true);std::size_t fullReads=0;bool runtimeStrong=false;for(const auto& read:fixture.kv->reads){if(read.key==diff::keyOf(uos::codex::auth::CODEX_MODELS_KV_KEY)&&read.consistency==rt::KvConsistencyLevel::strong)++fullReads;runtimeStrong=runtimeStrong||(read.key==diff::keyOf(uos::runtime_config::RUNTIME_CONFIG_V2_KEY)&&read.consistency==rt::KvConsistencyLevel::strong);}EXPECT_EQ(fullReads,4u);EXPECT(runtimeStrong);
}
ASYNC_TEST("optional snapshot failure containment preserves snapshot shape checks and critical control read failures") {
  PickerFixture fixture;const json::object valid{{"models",json::array{json::object{{"slug","valid-model"}}}},{"source","fixture"},{"updated_at_ms",1}};fixture.kv->put(uos::codex::auth::CODEX_MODELS_KV_KEY,valid);AWAIT(loaded,mf::loadFullCodexModelsSnapshot());ASSERT(loaded);EXPECT(rt::deepEqual(*loaded,valid));std::vector<json::object> invalid;auto row=valid;row["models"]=json::array{};invalid.push_back(row);row=valid;row["models"]=json::array{nullptr};invalid.push_back(row);row=valid;row["source"]=" ";invalid.push_back(row);row=valid;row["updated_at_ms"]=0;invalid.push_back(row);row["updated_at_ms"]=1.5;invalid.push_back(row);for(const auto& bad:invalid){fixture.kv->put(uos::codex::auth::CODEX_MODELS_KV_KEY,bad);AWAIT(rejected,mf::loadFullCodexModelsSnapshot());EXPECT(!rejected);}fixture.kv->rejectedKeys.insert(diff::keyOf(uos::codex::auth::CODEX_MODELS_KV_KEY));fixture.kv->rejectedKeys.insert(diff::keyOf(w::CODEX_MODELS_WHITELIST_KV_KEY));fixture.kv->rejectedKeys.insert(diff::keyOf(selection::PROVIDER_SELECTION_KV_KEY));AWAIT(failed,mf::loadFullCodexModelsSnapshot());EXPECT(!failed);EXPECT_REJECTS_MESSAGE(admin::handleAdminModelsCatalogGet(),rt::JsError,"fixture KV read unavailable");auto providerRead=selection::loadProviderSelection(fixture.kv);EXPECT_REJECTS_MESSAGE(std::move(providerRead),rt::JsError,"fixture KV read unavailable");
  T2IngressScope ingress(fixture.kv);const rt::Request request("https://ai.ubq.fi/admin/models/catalog");AWAIT(unauthenticated,uos::handler::index::handler(request));EXPECT_EQ(unauthenticated.status(),401);
}
ASYNC_TEST("versioned rich paid catalog survives rejected optional snapshots without invented enrichment or relaxed controls") {
  PickerFixture fixture;fixture.kv->rejectedKeys.insert(diff::keyOf(uos::catalog::types::CODEX_CATALOG_AUTH_GENERATION_KEY));fixture.kv->rejectedKeys.insert(diff::keyOf(uos::codex::auth::CODEX_MODELS_KV_KEY));fixture.kv->put(selection::PROVIDER_SELECTION_KV_KEY,json::object{{"provider_ids",json::array{"openlux"}},{"updated_at_ms",1800000000000.0}});fixture.kv->put(w::CODEX_MODELS_WHITELIST_KV_KEY,json::object{{"model_ids",json::array{"paid-fallback-fixture"}},{"updated_at_ms",1800000000001.0}});rt::env::set("METERED_API_KEY","fixture-paid-catalog-key");rt::env::set("SURPLUS_API_KEY","fixture-disabled-surplus-key");int paidFetches=0;int unexpected=0;metered::setMeteredModelsFetchForTest(rt::Fetch([&paidFetches](rt::RequestInfo input,rt::RequestInit){EXPECT_EQ(requestUrl(input),"https://api.openlux.ai/v1/models");++paidFetches;return rt::Response::json(json::object{{"data",json::array{json::object{{"id","paid-fallback-fixture"},{"description","Synthetic paid discovery"},{"owned_by","fixture"},{"supported_endpoint_types",json::array{"openai-response"}}},json::object{{"id","paid-not-whitelisted"},{"owned_by","fixture"},{"supported_endpoint_types",json::array{"openai-response"}}},json::object{{"id","paid-chat-only"},{"owned_by","fixture"},{"supported_endpoint_types",json::array{"openai"}}}}}});}));rt::GlobalFetchOverride fetch(rt::Fetch([&unexpected](rt::RequestInfo,rt::RequestInit)->rt::Response{++unexpected;throw rt::JsError("unexpected provider fetch");}));const rt::KvKey fullKey=uos::codex::auth::CODEX_MODELS_KV_KEY;const rt::KvGetOptions strong{.consistency=rt::KvConsistencyLevel::strong};auto direct=fixture.kv->get(fullKey,strong);EXPECT_REJECTS_MESSAGE(std::move(direct),rt::JsError,"fixture KV read unavailable");
  for(int rejected=1;rejected>=0;--rejected){if(rejected==0){fixture.kv->rejectedKeys.erase(diff::keyOf(fullKey));fixture.kv->put(fullKey,json::object{{"source"," "},{"updated_at_ms",1800000000000.0},{"models",json::array{json::object{{"slug","paid-fallback-fixture"},{"context_window",999999},{"supported_reasoning_levels",json::array{"high"}}}}}});}AWAIT(response,catalog::handleModels(versionRequest()));EXPECT_EQ(response.status(),200);EXPECT_EQ(response.headers().get("x-uos-upstream"),"metered");EXPECT(matches(response.headers().get("content-type").value_or(""),"application/json"));AWAIT(body,response.json());EXPECT(!rt::get(body,"data"));EXPECT(rt::deepEqual(jsonFields(body.at("models").as_array(),"slug"),json::array{"paid-fallback-fixture"}));const auto& model=body.at("models").as_array()[0];EXPECT_EQ(rt::getString(model,"description"),"Synthetic paid discovery");EXPECT(rt::deepEqual(model.at("supported_endpoint_types"),json::array{"openai-response"}));EXPECT(rt::deepEqual(model.at("supported_reasoning_levels"),json::array{json::object{{"effort","none"},{"description","No reasoning"}}}));EXPECT_EQ(rt::getString(model,"default_reasoning_level"),"none");EXPECT(!rt::get(model,"context_window"));EXPECT(!rt::get(model,"max_context_window"));EXPECT(!rt::get(model,"auto_compact_token_limit"));EXPECT_EQ(rt::getString(model,"visibility"),"list");EXPECT_EQ(rt::getBool(model,"supported_in_api"),true);}
  EXPECT_EQ(paidFetches,2);const auto reads=fixture.kv->reads.size();AWAIT(invalid,catalog::handleModels(versionRequest("invalid")));EXPECT_EQ(invalid.status(),400);AWAIT(error,invalid.json());EXPECT_EQ(rt::getString(error.at("error"),"code"),"invalid_client_version");EXPECT_EQ(fixture.kv->reads.size(),reads);EXPECT_EQ(paidFetches,2);fixture.kv->rejectedKeys.insert(diff::keyOf(fullKey));fixture.kv->rejectedKeys.insert(diff::keyOf(w::CODEX_MODELS_WHITELIST_KV_KEY));EXPECT_REJECTS_MESSAGE(catalog::handleModels(versionRequest()),rt::JsError,"fixture KV read unavailable");EXPECT_EQ(unexpected,0);for(const auto& read:fixture.kv->reads)if(read.key==diff::keyOf(fullKey))EXPECT(read.consistency==rt::KvConsistencyLevel::strong);
}
ASYNC_TEST("saving a selection trims, de-duplicates, and preserves the operator's order") {
  PickerFixture fixture;AWAIT(saved,admin::handleAdminCodexModelsWhitelistSet(post(R"({"model_ids":[" gpt-5.6-sol ","gpt-5.6-sol","","  ","gpt-5.6-terra"]})")));EXPECT_EQ(saved.status(),200);AWAIT(savedBody,saved.json());EXPECT(rt::deepEqual(savedBody.at("model_ids"),json::array{"gpt-5.6-sol","gpt-5.6-terra"}));EXPECT_EQ(rt::getBool(savedBody,"stored"),true);EXPECT(rt::deepEqual(fixture.kv->row(w::CODEX_MODELS_WHITELIST_KV_KEY).at("model_ids"),json::array{"gpt-5.6-sol","gpt-5.6-terra"}));AWAIT(read,admin::handleAdminCodexModelsWhitelistGet());AWAIT(readBody,read.json());EXPECT(rt::deepEqual(readBody.at("data").at("model_ids"),json::array{"gpt-5.6-sol","gpt-5.6-terra"}));AWAIT(cleared,admin::handleAdminCodexModelsWhitelistSet(post(R"({"model_ids":[]})")));EXPECT_EQ(cleared.status(),200);EXPECT(rt::deepEqual(fixture.kv->row(w::CODEX_MODELS_WHITELIST_KV_KEY).at("model_ids"),json::array{}));
}
ASYNC_TEST("the model picker rejects a selection it cannot store faithfully") {
  PickerFixture fixture;const std::vector<std::string> invalid{R"({"model_ids":"gpt-5.6-sol"})",R"({"model_ids":[42]})","not json","{}"};for(const auto& body:invalid){AWAIT(response,admin::handleAdminCodexModelsWhitelistSet(post(body)));EXPECT_EQ(response.status(),400);AWAIT(error,response.json());EXPECT_EQ(rt::getString(error.at("error"),"type"),"invalid_request_error");}EXPECT(fixture.kv->row(w::CODEX_MODELS_WHITELIST_KV_KEY).is_null());json::array ids;for(int index=0;index<4000;++index)ids.push_back(json::value("model-"+std::to_string(index)+"-"+std::string(40,'x')));const auto body=rt::jsonStringify(json::object{{"model_ids",ids}});AWAIT(oversized,admin::handleAdminCodexModelsWhitelistSet(post(body)));EXPECT_EQ(oversized.status(),413);EXPECT(fixture.kv->row(w::CODEX_MODELS_WHITELIST_KV_KEY).is_null());
}
TEST("normalizeWhitelistModelIds keeps the first spelling of every identifier") {
  EXPECT_EQ(w::normalizeWhitelistModelIds(json::array{" a ", "b", "a", "", "   ", 7, nullptr, "c"}), (std::vector<std::string>{"a", "b", "c"}));
  EXPECT(w::normalizeWhitelistModelIds(json::array{}).empty());
  const json::array legacy{uos::deepseek::waterfall::DEEPSEEK_WATERFALL_LEGACY_MODEL_ID};
  EXPECT_EQ(w::normalizeWhitelistModelIds(legacy), std::vector<std::string>{std::string(uos::deepseek::waterfall::DEEPSEEK_WATERFALL_MODEL_ID)});
}
TEST("whitelist filters hide unlisted models on every model surface, and an empty list hides none") {
  const json::array list{json::object{{"id", "a"}}, json::object{{"id", "b"}}, json::object{{"id", "c"}}};
  const json::array catalog{json::object{{"slug", "a"}}, json::object{{"id", "b"}}, json::object{{"name", "c"}}};
  EXPECT(rt::deepEqual(ids(w::filterWhitelistedModelList(list, std::nullopt)), json::array{"a", "b", "c"}));
  EXPECT(rt::deepEqual(ids(w::filterWhitelistedModelList(list, saved({}))), json::array{"a", "b", "c"}));
  EXPECT(rt::deepEqual(ids(w::filterWhitelistedModelList(list, saved({"b", "gone"}))), json::array{"b"}));
  EXPECT_EQ(w::filterWhitelistedCatalogModels(catalog, std::nullopt).size(), 3u);
  EXPECT_EQ(w::filterWhitelistedCatalogModels(catalog, saved({})).size(), 3u);
  EXPECT(rt::deepEqual(w::filterWhitelistedCatalogModels(catalog, saved({"a", "c"})), json::array{catalog[0], catalog[2]}));
  struct Entry { std::string id; }; const std::vector<Entry> entries{{"a"}, {"b"}, {"c"}};
  EXPECT_EQ(w::filterWhitelistedModelMap(entries, saved({})).size(), 3u);
  const auto filtered = w::filterWhitelistedModelMap(entries, saved({"c"})); ASSERT_EQ(filtered.size(), 1u); EXPECT_EQ(filtered[0].id, "c");
}
ASYNC_TEST("the model picker route is registered and stays behind admin auth") {
  PickerFixture fixture;
  T2IngressScope ingress(fixture.kv);
  const rt::Request catalogRequest("https://ai.ubq.fi/admin/models/catalog");
  AWAIT(unauthenticated, uos::handler::index::handler(catalogRequest));
  EXPECT_EQ(unauthenticated.status(), 401);
  const rt::Request unknownRequest("https://ai.ubq.fi/admin/models/catalog-unknown");
  AWAIT(unknown, uos::handler::index::handler(unknownRequest));
  EXPECT_EQ(unknown.status(), 404);
}

ASYNC_TEST("the public and admin catalogs render one shared snapshot with different filters") {
  int enrichmentFetches = 0;
  int surplusFetches = 0;
  PickerFixture fixture;
  T2IngressScope ingress(fixture.kv);
  rt::env::set("DEEPSEEK_API_KEY", "fixture-deepseek-key");
  fixture.kv->put(uos::codex::auth::CODEX_MODELS_KV_KEY, rt::jsonParse(R"({"source":"fixture","updated_at_ms":1,"models":[{"slug":"catalog-visible"},{"slug":"catalog-hidden"}]})"));
  fixture.kv->put(w::CODEX_MODELS_WHITELIST_KV_KEY, json::object{{"model_ids", json::array{"catalog-visible", "deepseek-v4-flash"}}, {"updated_at_ms", 2}});
  fixture.kv->put(selection::PROVIDER_SELECTION_KV_KEY, json::object{{"provider_ids", json::array{"codex"}}, {"updated_at_ms", 3}});

  const json::array enrichmentRows{json::object{{"id", "openai/catalog-visible"}}};
  auto warmEnrichment = enrich(enrichmentRows);
  co_await std::move(warmEnrichment);
  ASSERT(orouter::openRouterModelsSnapshot());
  orouter::setOpenRouterModelsFetchForTest(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) {
    ++enrichmentFetches;
    return rt::Response::json(json::object{{"data", enrichmentRows}});
  }));
  rt::GlobalFetchOverride paidDiscovery(rt::Fetch([&](rt::RequestInfo input, rt::RequestInit) {
    if (requestUrl(input).find("surplusintelligence.ai/v1/models") == std::string::npos) {
      throw rt::JsError("Unexpected catalog fixture request");
    }
    ++surplusFetches;
    return rt::Response::json(json::object{{"data", json::array{}}});
  }));
  AWAIT(snapshot, catalog::buildModelCatalogSnapshot());
  ASSERT(rowById(snapshot, "catalog-hidden"));
  ASSERT(rowById(snapshot, "deepseek-v4-flash"));
  EXPECT_EQ(surplusFetches, 1);

  AWAIT(adminResponse, admin::handleAdminModelsCatalogGet());
  EXPECT_EQ(adminResponse.status(), 200);
  AWAIT(adminBody, adminResponse.json());
  const auto expected = json::value_from(snapshot);
  const auto& adminData = adminBody.at("data");
  EXPECT(rt::deepEqual(adminData.at("models"), expected.at("models")));
  EXPECT(rt::deepEqual(adminData.at("sources"), expected.at("sources")));
  EXPECT(rt::deepEqual(adminData.at("whitelist").at("model_ids"), json::array{"catalog-visible", "deepseek-v4-flash"}));
  EXPECT_EQ(rt::getBool(adminData, "filter_active"), true);

  AWAIT(publicResponse, catalog::handlePublicModelCatalog());
  EXPECT_EQ(publicResponse.status(), 200);
  AWAIT(publicBody, publicResponse.json());
  EXPECT(rt::deepEqual(jsonFields(publicBody.at("data").as_array(), "id"), json::array{"catalog-visible"}));
  const auto* adminVisible = jsonRow(adminData.at("models").as_array(), "catalog-visible");
  ASSERT(adminVisible);
  EXPECT(rt::deepEqual(publicBody.at("data").as_array()[0], *adminVisible));
  EXPECT_EQ(rt::getBool(publicBody.at("sources").at("deepseek"), "disabled"), true);

  const rt::Request publicRoute("https://ai.ubq.fi/uos/models/catalog");
  AWAIT(routedResponse, uos::handler::index::handler(publicRoute));
  EXPECT_EQ(routedResponse.status(), 200);
  AWAIT(routedBody, routedResponse.json());
  EXPECT(rt::deepEqual(routedBody, publicBody));
  EXPECT_EQ(enrichmentFetches, 0);
  EXPECT_EQ(surplusFetches, 1);
}
TEST("the Models tab renders checkbox tools instead of a free-text whitelist") {
  const auto html=asset("static/admin.html");const auto script=asset("static/admin.js");const auto models=asset("static/models.js");EXPECT(matches(html,R"re(id="models-whitelist-list" data-model-picker)re"));EXPECT(!matches(html,"models-whitelist-input"));EXPECT(!matches(script,"modelsWhitelistInput"));
  const std::vector<std::string> controls{"models-whitelist-search","models-whitelist-sort","models-whitelist-only-selected","models-whitelist-check-all","models-whitelist-uncheck-all","models-whitelist-invert","models-whitelist-drop-missing","models-whitelist-discard","models-whitelist-reload","models-metadata-refresh","models-whitelist-save","models-whitelist-badge","models-whitelist-summary","models-whitelist-warning"};for(const auto& id:controls){EXPECT(matches(html,"id=\""+id+"\""));EXPECT(matches(script,"mustGet\\(\""+id+"\"\\)"));}
  EXPECT(matches(html,R"re(<div data-model-filters role="group" aria-label="Filter by provider"></div>)re"));EXPECT(!matches(html,"data-model-provider="));EXPECT(!matches(script,"MODEL_PROVIDER_(LABELS|IDS)"));EXPECT(matches(script,R"re(const renderModelProviderFilters = \(\) => \{)re"));EXPECT(matches(script,R"re(modelsProviderFilters\.addEventListener\("click")re"));EXPECT(matches(script,R"re(providerLabelFor\(provider\.id\))re"));EXPECT(matches(script,R"re(checkbox\.type = "checkbox")re"));EXPECT(matches(script,R"re(dataset\.modelToggle)re"));EXPECT(matches(script,R"re(fetch\(apiUrl\("/admin/models/catalog"\), \{)re"));EXPECT(matches(script,R"re(fetch\(apiUrl\("/admin/models/whitelist"\), \{)re"));EXPECT(matches(script,R"re(if \(modelsHasUnsavedChanges\(\) && options\.force !== true\))re"));
  EXPECT(matches(script,R"re(modelsCheckAllBtn\.textContent = scoped[\s\S]{0,80}`Check \$\{formatNumber\(visibleCount\)\} shown`[\s\S]{0,60}`Check all \$\{formatNumber\(visibleCount\)\}`)re"));EXPECT(matches(script,R"re(modelsUncheckAllBtn\.textContent = scoped[\s\S]{0,80}`Uncheck \$\{formatNumber\(visibleCount\)\} shown`[\s\S]{0,60}`Uncheck all \$\{formatNumber\(visibleCount\)\}`)re"));EXPECT(matches(script,R"re(const modelsMissingIds = \(\) => \{)re"));EXPECT(matches(script,R"re(modelsMissingWarning\(\))re"));EXPECT(matches(html,"Saving an empty selection removes the filter"));EXPECT(matches(script,"No models checked: the filter is off"));EXPECT(matches(script,R"re(source\.configured !== false)re"));EXPECT(matches(models,R"re(source\?\.configured !== false)re"));EXPECT(matches(models,R"re(deepseek: "DeepSeek")re"));EXPECT(matches(models,R"re(cerebras: "Cerebras")re"));
}
TEST("the Remove-missing control stays visible when an empty catalog leaves saved ids missing") {
  const auto status=capture(asset("static/admin.js"),R"re(const updateModelsStatus = \(\) => \{([\s\S]*?)\n\};\n\nconst renderModelsPicker)re");EXPECT(!status.empty());EXPECT(matches(status,R"re(const missing = modelsMissingIds\(\);)re"));EXPECT(matches(status,R"re(modelsDropMissingBtn\.disabled = saving \|\| missing\.length === 0;)re"));EXPECT(matches(status,R"re(modelsDropMissingBtn\.hidden = missing\.length === 0;)re"));
}
TEST("provider roster completion rerenders Models after normalizing its filter") {
  const auto completion=capture(asset("static/admin.js"),R"re(const loadProviderSelection = async \(options = \{\}\) => \{([\s\S]*?)\n\};\n\nconst saveProviderSelection)re");EXPECT(!completion.empty());const auto filterReset=completion.find("modelsProviderFilter = \"all\";");const auto modelsRender=completion.find("renderModelsPicker();");EXPECT(filterReset!=std::string::npos);EXPECT(modelsRender!=std::string::npos&&modelsRender>filterReset);
}
TEST("provider picker edits invalidate an in-flight roster refresh") {
  const auto script=asset("static/admin.js");EXPECT(matches(script,R"re(const markProviderSelectionEdited = \(\) => \{\s*providerSelectionLoadId \+= 1;\s*\};)re"));EXPECT(matches(script,R"re(const loadId = \+\+providerSelectionLoadId;[\s\S]*?if \(loadId !== providerSelectionLoadId\) return false;)re"));const auto loadStart=script.find("const loadProviderSelection = async (options = {}) => {");const auto staleGuard=script.find("if (loadId !== providerSelectionLoadId) return false;",loadStart);const auto serverSelection=script.find("providerSelection = new Set(providersSavedSelection);",loadStart);EXPECT(loadStart!=std::string::npos&&staleGuard!=std::string::npos&&serverSelection!=std::string::npos&&serverSelection>staleGuard);const auto applyStart=script.find("const applyProvidersSelection = (mutate) => {");const auto discardStart=script.find("const discardProviderChanges = () => {",applyStart);const auto changeStart=script.find("providersList.addEventListener(\"change\", (event) => {",discardStart);const auto dropMissingStart=script.find("providersDropMissingBtn.addEventListener(\"click\", () => {",changeStart);const bool found=applyStart!=std::string::npos&&discardStart!=std::string::npos&&changeStart!=std::string::npos&&dropMissingStart!=std::string::npos;ASSERT(found);EXPECT(discardStart>applyStart&&changeStart>discardStart&&dropMissingStart>changeStart);const std::string marker=R"re(markProviderSelectionEdited\(\);)re";EXPECT(matches(script.substr(applyStart,discardStart-applyStart),marker));EXPECT(matches(script.substr(discardStart,changeStart-discardStart),marker));EXPECT(matches(script.substr(changeStart,dropMissingStart-changeStart),marker));EXPECT(matches(script.substr(dropMissingStart),marker));
}
TEST("the Providers tab renders a provider picker next to the Analytics tab") {
  const auto html=asset("static/admin.html");const auto script=asset("static/admin.js");EXPECT(matches(html,R"re(id="providers-selection-list" data-provider-picker)re"));EXPECT(matches(html,R"re(id="view-analytics"[\s\S]*?id="provider-capacity-chart")re"));EXPECT(matches(html,R"re(id="view-providers"[\s\S]*?id="providers-selection-list")re"));EXPECT(matches(html,R"re(id="view-tab-analytics")re"));EXPECT(matches(script,"analytics: viewTabAnalytics"));EXPECT(matches(script,"analytics: viewAnalytics"));const std::vector<std::string> controls{"providers-selection-drop-missing","providers-selection-search","providers-selection-sort","providers-selection-only-active","providers-selection-check-all","providers-selection-uncheck-all","providers-selection-invert","providers-selection-discard","providers-selection-reload","providers-selection-save","providers-selection-badge","providers-selection-summary","providers-selection-warning"};for(const auto& id:controls){EXPECT(matches(html,"id=\""+id+"\""));EXPECT(matches(script,"mustGet\\(\""+id+"\"\\)"));}
  EXPECT(matches(html,R"re(<div data-provider-filters role="group" aria-label="Filter by tier"></div>)re"));EXPECT(!matches(html,"data-provider-tier="));EXPECT(!matches(script,"PROVIDER_(ROSTER|TIER_IDS|TIER_LABELS|HEALTH_KEYS|ALL_IDS)"));EXPECT(matches(script,R"re(const renderProviderTierFilters = \(\) => \{)re"));EXPECT(matches(script,R"re(providersTierFilters\.addEventListener\("click")re"));EXPECT(matches(script,R"re(providerHealthFor\(entry\))re"));EXPECT(matches(script,R"re(fetch\(apiUrl\("/admin/providers/selection"\), \{)re"));EXPECT(matches(script,R"re(method: "POST")re"));EXPECT(matches(script,R"re(dataset\.providerToggle)re"));EXPECT(matches(script,R"re(if \(providersHasUnsavedChanges\(\) && options\.force !== true\))re"));EXPECT(matches(html,"Saving an empty selection removes the filter"));EXPECT(matches(script,R"re(providersSelectionSave\.disabled = saving \|\| !unsaved \|\| !providersHasActiveTier\(\);)re"));EXPECT(matches(script,"providersEmptyWarning"));EXPECT(matches(html,"The waterfall order itself never changes"));
}
TEST("stale Codex subscriptions do not promise paid fallback") {const auto warning=capture(asset("static/admin.js"),R"re(const providersRoutingWarnings = \(\) => \{([\s\S]*?)\n\};)re");EXPECT(matches(warning,R"re(if \(!isProviderChecked\("codex"\) && missing\.length === 0\))re"));}
TEST("each Codex subscription is selectable under the Codex provider") {const auto html=asset("static/admin.html");const auto script=asset("static/admin.js");EXPECT(matches(html,"Each configured Codex subscription is selectable on its own"));EXPECT(matches(script,R"re(list\.dataset\.providerSubscriptions = "")re"));EXPECT(matches(script,R"re(dataset\.providerSubscriptionToggle = subscription\.id)re"));EXPECT(matches(script,R"re(dataset\.providerSubscriptionMeta)re"));EXPECT(matches(script,R"re(const setSubscriptionChecked = \(subscriptionId, checked\) => \{)re"));EXPECT(matches(script,R"re(providerSelection\.delete\("codex"\))re"));EXPECT(matches(script,R"re(providerSelection\.add\("codex"\))re"));EXPECT(matches(script,R"re(checkbox\.indeterminate = partial)re"));EXPECT(matches(script,"providersMissingSelectionIds"));EXPECT(matches(script,R"re(fetch\(apiUrl\("/admin/providers/selection"\), \{)re"));EXPECT(matches(script,R"re(subscriptions\.every\(\(subscription\) => providerSelection\.has\(subscription\.id\)\))re"));}
TEST("admin metadata refresh does not announce an unchanged OpenRouter snapshot as success") {const auto refresh=capture(asset("static/admin.js"),R"re(const refreshModelsMetadata = async \(\) => \{([\s\S]*?)\n\};)re");EXPECT(matches(refresh,R"re(if \(payload\?\.data\?\.openrouter\?\.refreshed === false\) \{[\s\S]*?toast\.error\("Metadata refresh failed", \{ description: "OpenRouter metadata was not refreshed" \}\);[\s\S]*?return;[\s\S]*?\}\s*toast\.success\("Model metadata refreshed")re"));}
ASYNC_TEST("admin metadata refresh forces every upstream and reports what is cached") {
  PickerFixture fixture;std::vector<std::string> calls;auto enrichment=std::make_shared<orouter::OpenRouterModelsSnapshot>();enrichment->updated_at_ms=1789000000000.0;for(const auto& id:std::vector<std::string>{"openai/gpt-5.6-sol","z-ai/glm-5.3","deepseek/deepseek-v4.1-flash-1017"}){orouter::OpenRouterModelMetadata row;row.id=id;row.context_window_tokens=id=="z-ai/glm-5.3"?1310720:id=="openai/gpt-5.6-sol"?1050000:1048576;row.max_context_window_tokens=row.context_window_tokens;enrichment->models.push_back(row);}admin::ModelsRefreshDependencies dependencies;
  dependencies.refreshClaude=[&calls](auto options){calls.push_back(options.force.value_or(false)?"claude:force":"claude:plain");uos::provider::claude_code_models::ClaudeCodeModelsSnapshot snapshot;uos::provider::claude_code_models::ClaudeCodeModel model;model.id="claude-observed";model.created=0;snapshot.models.push_back(model);snapshot.updated_at_ms=3;return rt::fulfilledTask(std::optional(snapshot));};dependencies.refreshEnrichment=[&calls,enrichment]{calls.push_back("openrouter");return rt::fulfilledTask(std::shared_ptr<const orouter::OpenRouterModelsSnapshot>(enrichment));};
  dependencies.refreshOpenlux=[&calls](auto options){calls.push_back(options.force.value_or(false)?"openlux:force":"openlux:plain");metered::MeteredModelsSnapshot snapshot{{metered::MeteredModel{.id="gpt-5.6-sol",.created=0,.owned_by="openlux",.supported_endpoint_types={"openai-response"}},metered::MeteredModel{.id="deepseek-v4.1-flash-1017",.created=0,.owned_by="openlux",.supported_endpoint_types={"openai-response"}}},1};return rt::fulfilledTask(std::optional(snapshot));};dependencies.refreshLithos=[&calls](auto options){calls.push_back(options.force.value_or(false)?"lithos:force":"lithos:plain");uos::provider::lithos_models::LithosModelsSnapshot snapshot{{{"deepseek-ai/DeepSeek-V4.1-Flash",4}},4};return rt::fulfilledTask(std::optional(snapshot));};
  dependencies.refreshSurplus=[&calls](auto options){calls.push_back(options.force.value_or(false)?"surplus:force":"surplus:plain");surplus::SurplusModelsSnapshot snapshot{{surplus::SurplusModel{.id="gpt-5.6-sol",.created=0,.owned_by="OpenAI",.supported_endpoint_types={"openai-response"}},surplus::SurplusModel{.id="hy3",.created=0,.owned_by="Tencent",.supported_endpoint_types={"openai-response"}},surplus::SurplusModel{.id="deepseek-v4.1-flash-1017",.created=0,.owned_by="DeepSeek",.supported_endpoint_types={"openai-response"}}},2};return rt::fulfilledTask(std::optional(snapshot));};
  AWAIT(response,admin::handleAdminModelsRefresh(dependencies));EXPECT_EQ(response.status(),200);EXPECT_EQ(response.headers().get("cache-control"),"no-store");AWAIT(body,response.json());rt::sort(calls,[](const auto& left,const auto& right){return rt::localeCompare(left,right);});EXPECT_EQ(calls,(std::vector<std::string>{"claude:force","lithos:force","openlux:force","openrouter","surplus:force"}));const auto expected=rt::jsonParse(R"({"openrouter":{"upstream_models":3,"updated_at_ms":1789000000000,"refreshed":true},"openlux":{"models":2,"updated_at_ms":1},"surplus":{"models":3,"updated_at_ms":2},"lithos":{"models":1,"updated_at_ms":4,"refreshed":true},"claude_code":{"models":1,"updated_at_ms":3,"status":"available","stale":false}})");EXPECT(rt::deepEqual(body.at("data"),expected));
}
ASYNC_TEST("admin metadata refresh is registered as an operator POST route") {
  int refreshes = 0;
  PickerFixture fixture;
  T2IngressScope ingress(fixture.kv);
  orouter::setOpenRouterModelsFetchForTest(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) {
    ++refreshes;
    return rt::Response::json(json::object{{"data", json::array{json::object{{"id", "openai/catalog-visible"}}}}});
  }));
  const std::string url = "https://ai.ubq.fi/admin/models/refresh";
  rt::RequestInit init;
  init.method = "POST";
  AWAIT(unauthenticated, uos::handler::index::handler(rt::Request(url, init)));
  EXPECT_EQ(unauthenticated.status(), 401);

  uos::config::config.adminTokens.insert("fixture-catalog-admin");
  init.headers = rt::Headers{{"Authorization", "Bearer fixture-catalog-admin"}};
  init.method = "GET";
  AWAIT(wrongMethod, uos::handler::index::handler(rt::Request(url, init)));
  EXPECT_EQ(wrongMethod.status(), 404);
  EXPECT_EQ(refreshes, 0);

  init.method = "POST";
  AWAIT(authorized, uos::handler::index::handler(rt::Request(url, init)));
  EXPECT_EQ(authorized.status(), 200);
  EXPECT_EQ(authorized.headers().get("cache-control"), "no-store");
  AWAIT(body, authorized.json());
  EXPECT_EQ(rt::getNumber(body.at("data").at("openrouter"), "upstream_models"), 1);
  EXPECT_EQ(refreshes, 1);
}
TEST("admin metadata refresh confirms before replacing a staged model selection") {const auto refresh=capture(asset("static/admin.js"),R"re(const refreshModelsMetadata = async \(\) => \{([\s\S]*?)\n\};)re");EXPECT(!refresh.empty());EXPECT(matches(refresh,R"re(if \(modelsHasUnsavedChanges\(\) && !globalThis\.confirm\("Discard the unsaved model selection and reload\?"\)\) return;\s+await loadModelsWhitelist\(\{ force: true \}\);)re"));}
TEST("whitelist guard and filters match25 isolated Bun observations") {
  const auto bytes = rt::gzip::gunzipSync(rt::fromBase64(whitelistVectors)); const auto vectors = rt::jsonParse(rt::decodeUtf8(bytes));
  EXPECT_EQ(vectors.as_array().size(), 25u);
  for (const auto& row : vectors.as_array()) {
    const auto name = rt::getString(row, "name").value(); const auto& args = row.at("args").as_array(); json::value actual;
    if (name == "normalizeCodexModelsWhitelist") { const auto result = w::normalizeCodexModelsWhitelist(args[0]); actual = result ? json::value_from(*result) : json::value(nullptr); }
    else if (name == "normalizeWhitelistModelIds") actual = json::value_from(w::normalizeWhitelistModelIds(args[0].as_array()));
    else {
      const auto whitelist = args[1].is_null() ? std::optional<w::CodexModelsWhitelist>{} : std::optional(json::value_to<w::CodexModelsWhitelist>(args[1]));
      actual = name == "filterWhitelistedModelList" ? w::filterWhitelistedModelList(args[0].as_array(), whitelist) : w::filterWhitelistedCatalogModels(args[0].as_array(), whitelist);
    }
    EXPECT(rt::deepEqual(actual, row.at("result")));
  }
}
ASYNC_TEST("whitelist KV storage trims deduplicates and canonicalizes while retaining order") {
  const auto store = std::make_shared<rt::MemoryKv>(rt::dateNow); rt::globalDateNow() = [] { return 123456.0; };
  const std::vector<std::string> requested{" b ", "a", "b", "", std::string(uos::deepseek::waterfall::DEEPSEEK_WATERFALL_LEGACY_MODEL_ID)};
  auto task = w::storeCodexModelsWhitelist(store, requested); const bool stored = co_await std::move(task); EXPECT(stored);
  const auto entry = co_await store->get(w::CODEX_MODELS_WHITELIST_KV_KEY);
  const json::object expected{{"model_ids", json::array{"b", "a", uos::deepseek::waterfall::DEEPSEEK_WATERFALL_MODEL_ID}}, {"updated_at_ms", 123456}};
  EXPECT(rt::deepEqual(entry.value.toJson(), expected));
  const auto loaded = co_await w::loadCodexModelsWhitelist(store); ASSERT(loaded); EXPECT(rt::deepEqual(json::value_from(*loaded), expected));
}
ASYNC_TEST("whitelist absence and invalid stored values remain null") {
  const auto store = std::make_shared<rt::MemoryKv>(rt::dateNow);
  auto noStore = co_await w::loadCodexModelsWhitelist(nullptr); EXPECT(!noStore);
  auto missing = co_await w::loadCodexModelsWhitelist(store); EXPECT(!missing);
  const rt::KvValue invalid(json::object{{"model_ids", json::array{"a"}}, {"updated_at_ms", 0}});
  const auto commit = co_await store->set(w::CODEX_MODELS_WHITELIST_KV_KEY, invalid); EXPECT(commit.ok);
  const auto result = co_await w::loadCodexModelsWhitelist(store); EXPECT(!result);
}
TEST("whitelist stored guards reject typed and undefined values without JSON coercion") {
  rt::KvObject value; value.set("model_ids", rt::KvValue(rt::KvArray{rt::KvValue(rt::BigInt(1))})); value.set("updated_at_ms", rt::KvValue(1));
  EXPECT(!w::normalizeCodexModelsWhitelist(rt::KvValue(value)));
  value.set("model_ids", rt::KvValue(rt::KvArray{rt::KvValue("a")})); value.set("updated_at_ms", rt::KvValue());
  EXPECT(!w::normalizeCodexModelsWhitelist(rt::KvValue(value)));
}
