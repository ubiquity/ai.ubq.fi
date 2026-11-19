// Public assets come from the repository root. Route literals come from the native source tree.
// Links: tests/openapi_runtime_contract_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
// src/openai_schema.cpp src/agent_messages.cpp src/request.cpp src/auth/index.cpp src/kernel/quota_reservations.cpp src/authority/native_kernel_contracts.cpp src/openai.cpp src/utils.cpp
// src/authority/native_kernel.cpp src/provider/metered.cpp src/api_key_policy.cpp src/models/codex_models.cpp src/authority/native_api_key.cpp src/kv.cpp src/auth/passport.cpp src/inference_deadline.cpp
// src/provider/surplus_claude_request.cpp src/kernel/quota_v2.cpp src/kernel/attestation.cpp src/api_keys.cpp src/authority/native_api_key_policy.cpp src/models/catalog.cpp src/config.cpp src/http.cpp
// src/storage/kv.cpp src/defaults.cpp src/kernel/policy_queue.cpp src/input_normalization.cpp src/provider/lithos.cpp src/request_policy.cpp src/provider/claude_code_models.cpp src/storage/native_kv.cpp
// src/models/metadata.cpp src/models/codex_models_fetch.cpp src/provider/surplus.cpp src/responses_request.cpp src/handler/index.cpp src/bounded_response_body.cpp src/types.cpp src/provider/cerebras.cpp
// src/auth/passkeys.cpp src/admin/kernel.cpp src/codex/auth_refresh.cpp src/admin/api_keys.cpp src/admin/api_key_audit.cpp src/models/openrouter_models.cpp src/codex/dispatch.cpp
// src/admin/codex.cpp src/auth/local_admin.cpp src/handler/terminal_route.cpp src/admin/claude_code.cpp src/release.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_backfill.cpp src/provider/selection.cpp
// src/storage/sqlite_readonly.cpp src/provider/capacity_parse.cpp src/codex/reset_settings.cpp src/kernel/usage.cpp src/codex/auth.cpp src/codex/banked_reset_pool.cpp src/authority/native_paid_contracts.cpp src/handler/admission.cpp
// src/admin/api_key_mutations.cpp src/codex/banked_reset_claim.cpp src/paid_fallback/ledger_stats.cpp src/provider/claude_code_credentials.cpp src/codex/supervisor_brief.cpp src/paid_fallback/ledger_settlement.cpp src/provider/claude_code.cpp src/codex/banked_reset.cpp
// src/authority/native_paid.cpp src/provider/capacity_events.cpp src/codex/overage_settings.cpp src/codex/banked_reset_provider.cpp src/provider/capacity_contract.cpp src/sentinel/replay_observation.cpp src/health.cpp src/kernel/quota_write.cpp
// src/provider/claude_code_quota.cpp src/provider/capacity.cpp src/auth/relay.cpp src/openai_telemetry.cpp src/inference_admission.cpp src/paid_fallback/health.cpp src/provider/capacity_history.cpp src/provider/lithos_models.cpp
// src/responses_failover_stream.cpp src/metered_quota.cpp src/handler/terminal_log.cpp src/provider/claude_code_accounts.cpp src/systemone/handlers.cpp src/deepseek/flash_aliases.cpp src/admin/codex_reset.cpp src/codex/routing_mutations.cpp
// src/cache/prompt_analytics_read.cpp src/codex/capacity_routing.cpp src/static.cpp src/handler/http.cpp src/decisions/handlers.cpp src/responses_native_headers.cpp src/cache/prompt_analytics_queue.cpp src/catalog/store.cpp
// src/provider/openrouter.cpp src/admin/error_log.cpp src/sentinel/replay_store.cpp src/runtime_config.cpp src/authority/native_codex_capacity.cpp src/sentinel/replay_read.cpp src/sentinel/replay_envelope.cpp src/provider/surplus_claude_chat_request.cpp
// src/provider/claude_code_access.cpp src/provider/capacity_rollups.cpp src/admin/defaults.cpp src/cache/scope_experiment_targets.cpp src/authority/native_codex_evaluation.cpp src/provider/health.cpp src/codex/external_auth.cpp src/sentinel/replay_retention.cpp
// src/models/codex_models_availability.cpp src/authority/native_codex_mutations.cpp src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_model.cpp src/sentinel/incident_outbox.cpp src/codex/supervisor_config.cpp src/quota_projection.cpp src/cache/kv_migration_validation.cpp
// src/models/codex_models_whitelist.cpp src/cache/prompt_analytics_core.cpp src/provider/anthropic_replay.cpp src/embeddings/jobs.cpp src/auth/passport_sessions.cpp src/paid_fallback/index.cpp src/codex/supervisor.cpp src/sentinel/replay_retention_metadata.cpp
// src/authority/native_paid_policy.cpp src/embeddings/handlers.cpp src/authority/native_codex_contracts.cpp src/sentinel/upstream_capture.cpp src/codex/banked_reset_submission.cpp src/cache/kv_migration_usage.cpp src/catalog/types.cpp src/paid_fallback/rollups.cpp
// src/sentinel/replay_retention_schema.cpp src/cache/kv_migration_paid_fallback.cpp src/provider/cerebras_rate_limits.cpp src/embeddings/ledger.cpp src/cache/scope_experiment_model.cpp src/optional_telemetry_queue.cpp src/live/handler.cpp src/analytics.cpp
// src/provider/anthropic_chat_responses.cpp src/codex/experiment.cpp src/codex/supervisor_secret.cpp src/authority/native_api_key_audit.cpp src/upstream_wire.cpp src/catalog/index.cpp src/codex/account_routing.cpp src/codex/routing_state.cpp
// src/embeddings/voyage.cpp src/sentinel/replay_admin.cpp src/codex/manual_reset_state.cpp src/provider/claude_capacity_history.cpp src/admin/sentinel_incident.cpp src/runtime/http_tasks.cpp src/auth/local_development_key.cpp src/provider/anthropic_response_wire.cpp
// src/provider/claude_capacity_rollups.cpp src/cache/kv_migration_base.cpp src/debug_routing.cpp src/responses_stream.cpp src/codex/native_auth.cpp src/codex/manual_reset.cpp src/catalog/models.cpp src/provider/claude_code_usage_schema.cpp
// src/deepseek/index.cpp src/deepseek/waterfall.cpp src/kernel/quota_reserve.cpp src/codex/routing_evaluation.cpp src/codex/rate_limit_429.cpp src/codex/supervisor_follow.cpp src/codex/routing_serial.cpp src/provider/removed_provider_telemetry.cpp
// src/provider/presentation.cpp src/live/sideband.cpp src/provider/capacity_analytics.cpp src/paid_fallback/ledger_admission.cpp src/kv_conversion/profile_json.cpp src/sentinel/replay_limits.cpp src/images.cpp src/live/routes.cpp
// src/recent_model_context.cpp src/responses_handler.cpp src/authority/native_codex.cpp src/codex/supervisor_inventory.cpp src/provider/lithos_rate_limits.cpp src/paid_fallback/removed_provider.cpp src/provider/cerebras_responses.cpp src/cache/scope_targets.cpp
// src/responses_attempts.cpp src/cache/scope_experiment.cpp src/admin/capacity_observation_sync.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_responses.cpp src/provider/openrouter_request_controls.cpp src/responses_buffered.cpp src/live/upstream.cpp
// src/cache/telemetry_gate.cpp src/deepseek/context_admission_failure.cpp src/admin/chart_source_proxy.cpp src/runtime/http_early_sse.cpp src/codex/supervisor_transport.cpp src/provider/removed_provider_circuit.cpp src/codex/responses_error.cpp src/codex/native_auth_transport.cpp
// src/provider/cerebras_handlers.cpp src/provider/openrouter_handlers.cpp src/authority/native_codex_429.cpp src/codex/supervisor_log.cpp src/deepseek/waterfall_handler.cpp src/chat/envelope.cpp src/provider/lithos_handlers.cpp src/catalog/promotion.cpp
// src/provider/lithos_streams.cpp src/chat/stream_translation.cpp src/codex/quota.cpp src/runtime/websocket.cpp src/chat/completions.cpp src/deepseek/chat_projection.cpp src/paid_fallback/routing.cpp src/provider/stream_relay.cpp
// src/live/calls.cpp src/deepseek/responses.cpp src/deepseek/forwarded_payload_policy.cpp src/provider/openrouter_streams.cpp src/deepseek/handlers.cpp src/paid_fallback/claude_code_route.cpp src/deepseek/responses_stream.cpp src/lib/jev_compaction/state.cpp
// src/deepseek/responses_payload.cpp src/provider/openrouter_web_tools.cpp src/deepseek/stream.cpp src/lib/jev_compaction/codex_items.cpp src/lib/jev_compaction/types.cpp src/codex/index.cpp src/lib/jev_compaction/compact.cpp src/codex/responses_operations.cpp
// src/lib/jev_compaction/request.cpp src/codex/responses_runners.cpp src/codex/responses_state.cpp
#include "uos/embeddings/ledger.hpp"
#include "uos/openai.hpp"
#include "uos/openai_schema.hpp"
#include "uos/responses_request.hpp"
#include "uos/handler/index.hpp"
#include "uos/handler/terminal_route.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/kv.hpp"
#include "uos/config.hpp"
#include "tests/helpers/responses_conformance_client.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/provider/openrouter_request_controls.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace ledger = uos::embeddings::ledger;

  const std::string ROOT = rt::path::resolve(rt::path::dirname(__FILE__), "../../..");
  const std::string GATEWAY = rt::path::join(ROOT, "cpp/gateway");

  const json::object& document() {
    static const json::object parsed = rt::jsonParse(rt::fs::readFileSync(rt::path::join(ROOT, "static/openapi.json"), "utf8")).as_object();
    return parsed;
  }

  constexpr std::array<std::string_view, 5> HTTP_METHODS{"get", "post", "put", "patch", "delete"};

  // `readdirSync(SOURCE_ROOT, { recursive: true })` (relative paths of every entry; rt::fs has no
  // recursive readdir).
  void readdirRecursive(const std::string& directory, const std::string& prefix, std::vector<std::string>& out) {
    for (const rt::fs::Dirent& entry : rt::fs::readdirSync(directory, rt::fs::withFileTypes)) {
      const std::string relative = prefix.empty() ? entry.name : prefix + "/" + entry.name;
      out.push_back(relative);
      if (entry.isDirectory()) readdirRecursive(rt::path::join(directory, entry.name), relative, out);
    }
  }

  std::vector<std::string> sorted(std::vector<std::string> values) {
    rt::sort(values, [](const std::string& left, const std::string& right) { return rt::localeCompare(left, right); });
    return values;
  }

  const std::vector<std::string>& SOURCE_FILES() {
    static const std::vector<std::string> files = [] {
      std::vector<std::string> entries;
      for (const char* directory : {"src", "include", "runtime", "entrypoints"}) {
        readdirRecursive(rt::path::join(GATEWAY, directory), directory, entries);
      }
      std::vector<std::string> sources;
      for (const std::string& file : entries)
        if (file.ends_with(".cpp") || file.ends_with(".hpp")) sources.push_back(file);
      for (const char* required : {"src/handler/index.cpp", "src/handler/terminal_route.cpp"}) {
        if (std::find(sources.begin(), sources.end(), required) == sources.end()) {
          throw rt::JsError(std::string("Missing native dispatch source: ") + required);
        }
      }
      return sorted(std::move(sources));
    }();
    return files;
  }

  std::string sourceText(const std::string& file) { return rt::fs::readFileSync(rt::path::join(GATEWAY, file), "utf8"); }

  const json::object& schemaNamed(std::string_view name) {
    const json::object& schemas = document().at("components").at("schemas").as_object();
    if (!rt::hasOwn(schemas, name)) throw rt::JsError("OpenAPI schema " + std::string(name) + " is missing");
    return schemas.at(name).as_object();
  }

  // `schema.required ?? []`
  std::vector<std::string> requiredOf(const json::object& schema) {
    std::vector<std::string> out;
    if (const json::value* required = rt::get(schema, "required"); required && required->is_array())
      for (const json::value& name : required->as_array()) out.push_back(std::string(name.as_string()));
    return out;
  }

  /** Retain the literal-dispatch safety net for code introduced outside the registered route tables. */
  std::vector<std::string> dispatchedUosOperations(const std::string& text) {
    static const std::string method = R"re(\b(?:req\.method\(\)|method)\s*==\s*"([A-Z]+)")re";
    static const std::string path = R"re(\bpath\s*==\s*"(/uos/[^"\s]+)")re";
    static const std::string prefix = R"re(\b(?:path\.starts_with\(\s*|rt::startsWith\(\s*path\s*,\s*)"(/uos/[^"\s]*/)"\s*\))re";
    static const std::string conjunction = R"(\s*&&\s*)";
    static const rt::Regex METHOD_THEN_PATH(method + conjunction + path, "g");
    static const rt::Regex PATH_THEN_METHOD(path + conjunction + method, "g");
    static const rt::Regex METHOD_THEN_PREFIX(method + conjunction + prefix, "g");
    static const rt::Regex PREFIX_THEN_METHOD(prefix + conjunction + method, "g");
    rt::OrderedSet<std::string> operations;
    for (const rt::RegexMatch& match : rt::matchAll(text, METHOD_THEN_PATH)) operations.add(*match[1] + " " + *match[2]);
    for (const rt::RegexMatch& match : rt::matchAll(text, PATH_THEN_METHOD)) operations.add(*match[2] + " " + *match[1]);
    for (const rt::RegexMatch& match : rt::matchAll(text, METHOD_THEN_PREFIX)) operations.add(*match[1] + " " + *match[2] + "{id}");
    for (const rt::RegexMatch& match : rt::matchAll(text, PREFIX_THEN_METHOD)) operations.add(*match[2] + " " + *match[1] + "{id}");
    return sorted(rt::arrayFrom(operations));
  }

  // `Readonly<{ method: string; path: string }>`
  struct Route {
    std::string method;
    std::string path;
  };

  /** Read the dispatch tables regardless of source spelling (prefix constants, joins and templates all work). */
  std::vector<std::string> runtimeUosOperations(const std::vector<Route>& routes, const std::vector<std::string>& sourceTexts) {
    rt::OrderedSet<std::string> operations;
    for (const Route& route : routes) operations.add(route.method + " " + route.path);
    for (const std::string& text : sourceTexts)
      for (const std::string& operation : dispatchedUosOperations(text)) operations.add(operation);
    return sorted(rt::arrayFrom(operations));
  }

  // Scoped to the native file and literal, with a required reason for each non-route reference.
  const rt::OrderedMap<std::string, std::string> UOS_NON_ROUTE_LITERALS{};

  std::vector<std::string> uosPathLiterals(const std::string& text) {
    static const rt::Regex LITERAL(R"re("(/uos/[^"\s]*)")re", "g");
    std::vector<std::string> out;
    for (const rt::RegexMatch& match : rt::matchAll(text, LITERAL)) out.push_back(*match[1]);
    return out;
  }

  std::vector<std::string> unpublishedUosLiterals(const std::string& file, const std::string& text, const rt::OrderedSet<std::string>& publishedPaths,
                                                  const rt::OrderedMap<std::string, std::string>& allowlist = UOS_NON_ROUTE_LITERALS) {
    std::vector<std::string> out;
    for (const std::string& literal : uosPathLiterals(text)) {
      const std::string key = file + ": " + literal;
      if (allowlist.has(key) && !rt::trim(*allowlist.get(key)).empty()) continue;
      const bool published = publishedPaths.has(literal) || (rt::endsWith(literal, "/") && publishedPaths.has(literal + "{id}"));
      if (!published) out.push_back(key);
    }
    return out;
  }

  // `Readonly<{ method: HttpMethod; path: string; operation: Operation }>`
  struct PublishedOperation {
    std::string method;
    std::string path;
    const json::object* operation;
  };

  const std::vector<PublishedOperation>& PUBLISHED_OPERATIONS() {
    static const std::vector<PublishedOperation> operations = [] {
      std::vector<PublishedOperation> out;
      for (const auto& entry : document().at("paths").as_object()) {
        for (const std::string_view method : HTTP_METHODS) {
          const json::value* operation = rt::get(entry.value(), method);
          if (rt::truthy(operation)) out.push_back(PublishedOperation{std::string(method), std::string(entry.key()), &operation->as_object()});
        }
      }
      return out;
    }();
    return operations;
  }

  std::vector<std::string> strings(std::initializer_list<const char*> values) {
    std::vector<std::string> out;
    for (const char* value : values) out.emplace_back(value);
    return out;
  }
}

namespace {
  std::vector<std::string> officialProperties(const json::object& schema) { std::vector<std::string> keys;for(const auto& property:schema.at("properties").as_object())if(!rt::hasOwn(property.value().as_object(),"x-uos-extension"))keys.emplace_back(property.key());return sorted(keys); }
  json::object extensionProperties(const json::object& schema) { json::object extensions;for(const auto& property:schema.at("properties").as_object())if(const auto* kind=rt::get(property.value(),"x-uos-extension"))extensions[property.key()]=*kind;return extensions; }
  std::vector<std::string> runtimeUosOperations() {
    std::vector<Route> routes;for(const auto& route:uos::handler::index::UOS_ROUTES)if(rt::startsWith(route.path,"/uos/"))for(const auto& method:route.methods)routes.push_back({method,route.path});for(const auto& route:uos::handler::terminal_route::UOS_TERMINAL_ROUTES)routes.push_back({std::string(route.method),std::string(route.path)+(route.prefix?"{id}":"")});std::vector<std::string> sourceTexts;for(const auto& file:SOURCE_FILES())sourceTexts.push_back(sourceText(file));return runtimeUosOperations(routes,sourceTexts);
  }
  std::vector<std::string> publishedUosOperations() { std::vector<std::string> result;for(const auto& operation:PUBLISHED_OPERATIONS())if(rt::startsWith(operation.path,"/uos/"))result.push_back(rt::toUpperCase(operation.method)+" "+operation.path);return sorted(result); }
}
TEST("published Chat and Responses request properties are exactly the runtime allowlists plus named extensions") {
  for(const auto* name:{"ChatCompletionRequest","ResponseRequest"}) {
    const bool chat=std::string(name)=="ChatCompletionRequest";const auto& schema=schemaNamed(name);EXPECT_EQ(rt::getBool(schema,"additionalProperties"),false);EXPECT_EQ(rt::getString(schema,"x-uos-official-schema"),chat?"CreateChatCompletionRequest":"CreateResponse");
    std::vector<std::string> allowlist;if(chat){for(const auto key:uos::openai_schema::CHAT_COMPLETIONS_REQUEST_KEYS)allowlist.emplace_back(key);}else{for(const auto key:uos::openai_schema::RESPONSES_REQUEST_KEYS)allowlist.emplace_back(key);}EXPECT_EQ(officialProperties(schema),sorted(allowlist));
    json::object extensions;const auto& specific=chat?uos::openai::DEEPSEEK_CHAT_EXTENSION_KEYS:uos::openai::CODEX_RESPONSES_EXTENSION_KEYS;for(const auto& key:specific)extensions[key]=chat?"deepseek":"codex-cli";for(const auto& key:uos::provider::openrouter_request_controls::OPENROUTER_CLAUDE_REQUEST_EXTENSION_KEYS)extensions[key]="openrouter-claude";EXPECT(rt::deepEqual(extensionProperties(schema),extensions));
    for(const auto& property:extensions){const auto description=rt::getString(schema.at("properties").at(property.key()),"description").value_or("");EXPECT(rt::includes(description,"not an official OpenAI field"));}EXPECT(!rt::includes(rt::getString(schema,"description").value_or(""),"Additional documented OpenAI fields may be accepted"));
  }
  const auto& chat=schemaNamed("ChatCompletionRequest").at("properties").as_object();for(const auto& key:uos::openai::CODEX_RESPONSES_EXTENSION_KEYS)EXPECT(!rt::hasOwn(chat,key));
}
TEST("the Responses validator accepts every published property and rejects an unpublished one") {
  json::object record;for(const auto& property:schemaNamed("ResponseRequest").at("properties").as_object())if(rt::getString(property.value(),"x-uos-extension")!="openrouter-claude")record[property.key()]=nullptr;
  // JSON has no present undefined carrier. Null preserves the source's inert values except the presence-validated boolean, which uses its simplest valid value.
  record["parallel_tool_calls"]=false;record["prompt_cache_key"]="contract";record["prompt_cache_options"]=json::object{};record["prompt_cache_retention"]="in_memory";record["client_metadata"]=json::object{};
  EXPECT(!uos::responses_request::validateResponsesRequestFields(record,record));auto unpublished=record;unpublished["contract_probe_unpublished"]=true;const auto rejected=uos::responses_request::validateResponsesRequestFields(unpublished,unpublished);ASSERT(rejected);EXPECT_EQ(rejected->status(),400);
}
TEST("published UOS embeddings request properties are exactly the runtime allowlists") {
  const std::vector<std::pair<std::string, const rt::OrderedSet<std::string>*>> contracts{
    {"EmbeddingsRequest", &ledger::UOS_SYNC_EMBEDDINGS_ALLOWED_KEYS},
    {"EmbeddingJobRequest", &ledger::UOS_EMBEDDINGS_JOB_ALLOWED_KEYS},
  };
  for (const auto& [name, allowlist] : contracts) {
    const json::object& schema = schemaNamed(name);
    // `${name} must reject undocumented properties`
    const json::value* additionalProperties = rt::get(schema, "additionalProperties");
    EXPECT(additionalProperties && additionalProperties->is_bool() && !additionalProperties->get_bool());
    // `${name} must equal the runtime allowlist`
    EXPECT_EQ(sorted(rt::objectKeys(schema.at("properties").as_object())), sorted(rt::arrayFrom(*allowlist)));
  }
  EXPECT_EQ(sorted(requiredOf(schemaNamed("EmbeddingJobRequest"))), strings({"input", "input_type", "model"}));
  EXPECT_EQ(sorted(requiredOf(schemaNamed("EmbeddingsRequest"))), strings({"input", "model"}));
}

TEST("OpenAPI publishes exactly the UOS routes the runtime dispatches and the agent contract lists") {
  const auto runtime=runtimeUosOperations();EXPECT_EQ(publishedUosOperations(),runtime);const auto text=rt::fs::readFileSync(rt::path::join(ROOT,"static/docs/llms-agents.md"),"utf8");const rt::Regex section(R"(UOS gateway endpoints:\n\n((?:- `[^`\n]+`[^\n]*\n)+))");const auto match=section.exec(text);ASSERT(match&&(*match)[1]);std::vector<std::string> operations;const rt::Regex operation(R"(- `([A-Z]+ \/uos\/[^`]+)`)","g");for(const auto& item:rt::matchAll(*(*match)[1],operation))operations.push_back(*item[1]);EXPECT_EQ(sorted(operations),runtime);
  for(const auto& entry:PUBLISHED_OPERATIONS()){if(!rt::startsWith(entry.path,"/uos/"))continue;EXPECT(rt::getString(*entry.operation,"operationId"));EXPECT(rt::length(rt::getString(*entry.operation,"description").value_or(""))>80);}
}
TEST("every /uos/ literal in native source is published or explicitly documented as a non-route") {
  rt::OrderedSet<std::string> publishedPaths;
  for (const PublishedOperation& operation : PUBLISHED_OPERATIONS()) publishedPaths.add(operation.path);
  std::vector<std::string> unpublished;
  rt::OrderedSet<std::string> seen;
  for (const std::string& file : SOURCE_FILES()) {
    const std::string text = sourceText(file);
    for (const std::string& literal : uosPathLiterals(text)) seen.add(file + ": " + literal);
    for (std::string& key : unpublishedUosLiterals(file, text, publishedPaths)) unpublished.push_back(std::move(key));
  }
  for (const auto& [key, reason] : UOS_NON_ROUTE_LITERALS) {
    // `${key} needs a non-route explanation`
    EXPECT(!rt::trim(reason).empty());
    // `${key} is a stale non-route allowlist entry`
    EXPECT(seen.has(key));
  }
  // "a UOS path in the runtime source must be published in static/openapi.json or explained as a non-route"
  EXPECT_EQ(unpublished, std::vector<std::string>{});
}

TEST("UOS table parity includes paths constructed from a slashless prefix or joined segments") {
  const std::string UOS = "/uos";
  EXPECT_EQ(runtimeUosOperations(
              {
                Route{"GET", UOS + "/probe"},
                Route{"POST", rt::join(strings({"", "uos", "joined-probe"}), "/")},
              },
              {}),
            strings({"GET /uos/probe", "POST /uos/joined-probe"}));
}

TEST("literal dispatch outside the UOS tables still participates in operation parity") {
  EXPECT_EQ(runtimeUosOperations({}, strings({
    R"(if (req.method() == "POST" && path == "/uos/auth") return probe();)",
    R"(if (path == "/uos/auth" && method == "PATCH") return probe();)",
    R"(if (method == "GET" && rt::startsWith(path, "/uos/probe/")) return probe();)",
    R"(if (path.starts_with("/uos/probe/") && req.method() == "DELETE") return probe();)",
  })), strings({"DELETE /uos/probe/{id}", "GET /uos/probe/{id}", "PATCH /uos/auth", "POST /uos/auth"}));
}

TEST("non-route UOS literal exceptions are file-scoped and do not mask other unpublished paths") {
  const std::string source = R"(const auto doc = "/uos/probe"; const auto route = "/uos/other";)";
  const rt::OrderedMap<std::string, std::string> allowlist{{"src/example.cpp: /uos/probe", "Documentation link"}};
  EXPECT_EQ(unpublishedUosLiterals("src/example.cpp", source, {}, allowlist), strings({"src/example.cpp: /uos/other"}));
  const rt::OrderedMap<std::string, std::string> blankReason{{"src/example.cpp: /uos/probe", " "}};
  EXPECT_EQ(unpublishedUosLiterals("src/example.cpp", "\"/uos/probe\"", {}, blankReason), strings({"src/example.cpp: /uos/probe"}));
  EXPECT_EQ(unpublishedUosLiterals("src/other.cpp", source, {}, allowlist), strings({"src/other.cpp: /uos/probe", "src/other.cpp: /uos/other"}));
}

TEST("agent-messages publishes the kernel attestation header as required and the repository headers as optional cross-checks") {
  // At runtime the kernel token alone authenticates the request and names the repository; X-GitHub-Owner and
  // X-GitHub-Repo are compared with the attestation only when both are sent (tests/kernel-coverage.test.ts).
  const json::object& parameters = document().at("components").at("parameters").as_object();
  const auto requiredHeaders = [&parameters](const json::object& operation) {
    json::object headers;
    if (const json::value* references = rt::get(operation, "parameters"); references && references->is_array()) {
      for (const json::value& reference : references->as_array()) {
        std::optional<std::string> name;
        if (const std::optional<std::string_view> ref = rt::getString(reference, "$ref")) name = rt::split(*ref, "/").back();
        if (name && !rt::hasOwn(parameters, *name)) throw rt::JsError("OpenAPI parameter " + *name + " is missing");
        const json::value& parameter = name ? parameters.at(*name) : reference;
        const std::optional<std::string_view> headerName = rt::getString(parameter, "name");
        if (rt::getString(parameter, "in") == std::string_view("header") && rt::truthy(headerName)) {
          const json::value* required = rt::get(parameter, "required");
          rt::objectSet(headers, std::string(*headerName), json::value(required && required->is_bool() && required->get_bool()));
        }
      }
    }
    return headers;
  };
  json::object expected;
  expected["X-GitHub-Owner"] = false;
  expected["X-GitHub-Repo"] = false;
  expected["X-Ubiquity-Kernel-Token"] = true;
  expected["X-GitHub-Installation-Id"] = false;
  for (const char* method : {"get", "post"}) {
    const json::value* operation = rt::get(document().at("paths").at("/uos/agent-messages"), method);
    // `${METHOD} /uos/agent-messages must be published`
    ASSERT(rt::truthy(operation));
    EXPECT(rt::deepEqual(json::value(requiredHeaders(operation->as_object())), json::value(expected)));
  }
}
ASYNC_TEST("every published route reaches its runtime handler with the published authentication") {
  uos::tests::helpers::responses_conformance_client::stageGatewayAssets();
  const auto tmp=rt::env::get("TMPDIR").value_or("/tmp");const rt::env::ScopedEnvironment clean(std::vector<std::pair<std::string,std::string>>{{"HOME",tmp+"/h"},{"TMPDIR",tmp}});const auto auth=uos::config::config.codexAuthJsonB64;const bool deploy=uos::config::config.isDeploy;uos::config::config.codexAuthJsonB64.clear();uos::config::config.isDeploy=true;
  const auto cleanup=rt::scopeExit([auth,deploy]{uos::kv::setKvForTest(nullptr);uos::config::config.codexAuthJsonB64=auth;uos::config::config.isDeploy=deploy;uos::provider::metered::resetMeteredModelsCacheForTest();uos::models::openrouter_models::resetOpenRouterModelsCacheForTest();uos::provider::surplus::resetSurplusModelsCacheForTest();});
  rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo,rt::RequestInit)->rt::Response{throw rt::JsError("contract probes must not reach the network");}));uos::kv::setKvForTest(nullptr);
  for(const auto& entry:PUBLISHED_OPERATIONS()) { rt::RequestInit init;init.method=rt::toUpperCase(entry.method);if(entry.method!="get"){init.body="{}";init.headers=rt::Headers{{"Content-Type","application/json"}};}const rt::Request request("https://ai.ubq.fi"+rt::replaceAll(entry.path,"{id}","contract-probe"),init);auto handling=uos::handler::index::handler(request);auto response=co_await std::move(handling);if(auto body=response.body()){auto cancel=body->cancel();co_await std::move(cancel);}const auto* security=rt::get(*entry.operation,"security");const bool isPublic=security&&security->is_array()&&security->as_array().empty();EXPECT_EQ(response.status(),isPublic?200:401); }
  const rt::Request request("https://ai.ubq.fi/uos/contract-probe-unpublished");auto handling=uos::handler::index::handler(request);auto unpublished=co_await std::move(handling);if(auto body=unpublished.body()){auto cancel=body->cancel();co_await std::move(cancel);}EXPECT_EQ(unpublished.status(),404);
}

TEST("published Chat and Responses request properties name exactly the OpenRouter Claude extension") {
  const auto& extensions = uos::provider::openrouter_request_controls::OPENROUTER_CLAUDE_REQUEST_EXTENSION_KEYS;
  for (const auto* name : {"ChatCompletionRequest", "ResponseRequest"}) {
    const auto& properties = schemaNamed(name).at("properties").as_object();
    rt::OrderedSet<std::string> published;
    for (const auto& key : rt::objectKeys(properties)) {
      const auto& property = properties.at(key);
      if (rt::getString(property, "x-uos-extension") != "openrouter-claude") continue;
      published.add(key);
      const auto description = rt::getString(property, "description");
      EXPECT(description && rt::includes(*description, "not an official OpenAI field"));
    }
    EXPECT_EQ(rt::arrayFrom(published), rt::arrayFrom(extensions));
  }
}

