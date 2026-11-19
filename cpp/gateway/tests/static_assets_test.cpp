// Port of tests/static-assets.test.ts. Source scans read explicit repository paths; the real static module
// reads the same assets through task-owned symlinks, retaining the isolated wrapper working directory.
// Links: tests/static_assets_test.cpp
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
// src/utils.cpp
// Five original public/admin delivery cases use real handler/index; static asset/source scans stay separate.
#include "uos/http.hpp"
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
#include "uos/kv.hpp"
#include "uos/static.hpp"

#include "uos/rt/fs.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/url.hpp"


#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using uos::static_::handleRoot;
  using uos::static_::handleStaticAsset;
  using uos::static_::hasStaticAsset;

  // `new URL("../", import.meta.url)` of the test: the repository root.
  const std::string ROOT = rt::path::resolve(rt::path::dirname(__FILE__), "../../..");
  [[maybe_unused]] const bool ROOT_ASSETS_READY = [] { (void)t2PublicAssets(); return true; }();

  // `readFileSync(new URL("../<path>", import.meta.url), "utf8")`
  std::string readText(const std::string& path) {
    std::ifstream file(ROOT + "/" + path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
  }

  const std::string readmeText = readText("README.md");
  const std::string adminCss = readText("static/admin.css");
  const std::string adminCacheScript = readText("static/admin-cache.js");
  const std::string adminHtml = readText("static/admin.html");
  const std::string aboutHtml = readText("static/about.html");
  const std::string adminScript = readText("static/admin.js");
  const std::string authScript = readText("static/auth.js");
  const std::string chatCss = readText("static/chat.css");
  const std::string chatHtml = readText("static/chat.html");
  const std::string chatScript = readText("static/chat.js");
  const std::string companyLogoSvg = readText("static/company-logo.svg");
  const std::string contactHtml = readText("static/contact.html");
  const std::string developersHtml = readText("static/developers.html");
  const std::string docsHtml = readText("static/docs.html");
  const std::string indexHtml = readText("static/index.html");
  const std::string llmsText = readText("static/llms.txt");
  const std::string llmsFullText = readText("static/docs/llms-agents.md");
  const std::string modelsCss = readText("static/models.css");
  const std::string modelsHtml = readText("static/models.html");
  const std::string modelsScript = readText("static/models.js");
  const std::string openApiText = readText("static/openapi.json");
  const std::string privacyHtml = readText("static/privacy.html");
  const std::string styleCss = readText("static/style.css");

  // assert.match / assert.doesNotMatch
  void expectMatch(const std::string& text, std::string_view pattern, std::string_view flags, bool expected, const char* file, int line) {
    if (rt::Regex(pattern, flags).test(text) != expected) {
      rt::test::report(file, line, std::string(expected ? "expected a match of /" : "expected no match of /") + std::string(pattern) + "/" + std::string(flags), false);
    }
  }
#define EXPECT_MATCH(text, pattern, flags) expectMatch((text), (pattern), (flags), true, __FILE__, __LINE__)
#define EXPECT_NO_MATCH(text, pattern, flags) expectMatch((text), (pattern), (flags), false, __FILE__, __LINE__)

  // `regex.exec(text)?.[group]`
  std::optional<std::string> execGroup(std::string_view pattern, std::string_view flags, const std::string& text, std::size_t group) {
    const rt::Regex regex(pattern, flags);
    const std::optional<rt::RegexMatch> match = regex.exec(text);
    if (!match) return std::nullopt;
    return (*match)[group];
  }

  // `[...text.matchAll(regex)].map((match) => match[group])`
  std::vector<std::string> matchAllGroup(std::string_view pattern, std::string_view flags, const std::string& text, std::size_t group) {
    const rt::Regex regex(pattern, flags);
    std::vector<std::string> out;
    for (const rt::RegexMatch& match : rt::matchAll(text, regex)) out.push_back(match[group].value_or(""));
    return out;
  }

  // `(text.match(regex) ?? []).length` for a global regex.
  std::size_t countMatches(std::string_view pattern, std::string_view flags, const std::string& text) {
    const std::optional<std::vector<std::string>> matches = rt::matchGlobal(text, rt::Regex(pattern, flags));
    return matches ? matches->size() : 0;
  }

  /**
   * Linear equivalent of `html.replace(/<[^>]+>/g, " ")`: every run from a "<" to the next ">" that has
   * at least one character between them becomes a single space. A regex here is super-linearly
   * backtracking (sonarjs/super-linear-regex), and every candidate rewrite changes which runs match.
   */
  std::string stripHtmlTags(const std::string& html) {
    std::string text;
    std::size_t cursor = 0;
    std::size_t search = 0;
    while (search < html.size()) {
      const std::size_t open = html.find('<', search);
      if (open == std::string::npos) break;
      const std::size_t close = html.find('>', open + 1);
      if (close == std::string::npos) break;
      if (close == open + 1) {
        // "<>" has no tag content, so the regex cannot match here: keep the "<" and keep scanning after it.
        search = open + 1;
        continue;
      }
      text += html.substr(cursor, open - cursor) + " ";
      cursor = close + 1;
      search = cursor;
    }
    return text + html.substr(cursor);
  }

  // `.replace(/\s+/g, " ")`
  std::string collapseWhitespace(const std::string& text) {
    static const rt::Regex WHITESPACE("\\s+", "g");
    return rt::replace(text, WHITESPACE, " ");
  }

  // Property access on parsed JSON (`a.b["c"][0]`): nullptr is undefined; numeric keys index arrays.
  const json::value* at(const json::value& root, std::initializer_list<std::string_view> path) {
    const json::value* current = &root;
    for (const std::string_view key : path) {
      if (const json::array* array = current->if_array()) {
        const double index = rt::stringToNumber(key);
        current = rt::get(*array, static_cast<std::size_t>(index));
      } else {
        current = rt::get(*current, key);
      }
      if (!current) return nullptr;
    }
    return current;
  }
  // assert.equal(value, expected) / assert.deepEqual(value, expected) with the expectation as JSON text.
  void expectJson(const json::value* value, std::string_view expected, const char* file, int line) {
    if (!value || !rt::deepEqual(*value, rt::jsonParse(expected))) {
      rt::test::report(file, line, "expected " + std::string(expected) + ", got " + (value ? rt::jsonStringify(*value) : std::string("undefined")), false);
    }
  }
#define EXPECT_JSON(value, expected) expectJson((value), (expected), __FILE__, __LINE__)
#define EXPECT_UNDEFINED(value) EXPECT((value) == nullptr)
#define EXPECT_TRUTHY(value) EXPECT(rt::truthy(value))

  rt::Request rootRequest(std::string accept) {
    rt::RequestInit init;
    init.headers = rt::Headers{{"Accept", std::move(accept)}};
    return rt::Request("https://ai.ubq.fi/", std::move(init));
  }

  // The JSON root representation reads the process KV; the TS test runs with no KV fixture.
  struct NoKv {
    NoKv() { uos::kv::setKvForTest(nullptr); }
    ~NoKv() { uos::kv::setKvForTest(nullptr); }
    NoKv(const NoKv&) = delete;
    NoKv& operator=(const NoKv&) = delete;
  };
}

TEST("static assets register frontend module dependencies") {
  for (const std::string_view path : {"/admin.js", "/admin.css", "/auth.js", "/auth-relay.js", "/chat-stats.js", "/foreground-refresh.js", "/network.js", "/models.js",
                                      "/models.css", "/reasoning-select.js", "/admin-cache.js", "/toast.js"}) {
    if (!hasStaticAsset(std::string(path))) rt::test::report(__FILE__, __LINE__, std::string(path) + " should be registered", false);
  }
  EXPECT_MATCH(chatHtml, R"re(<script type="module" src="\/chat\.js\?v=passport-design-20260922"><\/script>)re", "");
  EXPECT_MATCH(chatScript, R"re(from "\.\/chat-stats\.js\?v=20260827-response-stats-v4";)re", "");
  EXPECT_MATCH(chatScript, R"re(from "\.\/toast\.js\?v=passport-design-20260922";)re", "");
}

TEST("chat uses the branded Ubiquity Passport sign-in button") {
  EXPECT_MATCH(chatHtml, R"re(id="passport-login"[\s\S]*?data-passport-signin[\s\S]*?Sign in with Ubiquity)re", "");
  EXPECT_MATCH(chatHtml, R"re(viewBox="0 0 96 96")re", "");
}

TEST("chat response stats use one conversation bar below the composer") {
  EXPECT_EQ(countMatches(R"re(\bdata-chat-stats\b)re", "g", chatHtml), 1u);
  EXPECT_MATCH(chatHtml, R"re(<\/form>\s*<div data-chat-stats role="status" aria-live="polite" aria-atomic="true" hidden><\/div>)re", "");
  EXPECT_NO_MATCH(chatHtml, R"re(data-message-stats)re", "");
  EXPECT_NO_MATCH(chatScript, R"re(appendChatMessageStats|data-message-stats)re", "");
  EXPECT_MATCH(chatScript, R"re(recordCompletedChatResponse\(chatStats, sample\))re", "");
  EXPECT_MATCH(chatScript, R"re(decodeMs: firstTokenAt === null \? undefined : completedAt - firstTokenAt)re", "");
  EXPECT_MATCH(chatScript, R"re(decodeTokens: readChatCompletionDecodeTokens\(responseUsage, reasoningEffort\))re", "");
  EXPECT_MATCH(chatScript, R"re(if \(!assistantText\.trim\(\)\) \{[\s\S]*?return;[\s\S]*?recordAndRenderChatStats\()re", "");
  EXPECT_NO_MATCH(chatScript, R"re(firstStreamEventAt)re", "");
  EXPECT_MATCH(chatCss, R"re(\[data-chat-stats\]\s*\{[^}]*overflow-wrap:\s*anywhere;[^}]*white-space:\s*normal;)re", "s");
  EXPECT_NO_MATCH(chatCss, R"re(\[data-chat-stats\]\s*\{[^}]*(?:overflow:\s*hidden|white-space:\s*nowrap))re", "s");
  EXPECT_NO_MATCH(chatCss, R"re(\[data-message-stats\])re", "");
}

TEST("chat falls back to the loopback development principal without a token") {
  EXPECT_MATCH(chatScript, R"re(import \{\n(?:.|\n)*?isLocalDevelopmentOrigin,\n(?:.|\n)*?\} from "\.\/auth\.js";)re", "");
  EXPECT_MATCH(chatScript, R"re(let localDevelopmentAuth = false;)re", "");
  EXPECT_MATCH(chatScript, R"re(if \(!token && !localDevelopmentAuth\) \{)re", "");
  EXPECT_MATCH(chatScript, R"re(if \(auth\?\.mode !== "disabled" \|\| auth\?\.is_admin !== true\) return null;)re", "");
  EXPECT_MATCH(chatScript, R"re(const authHeaders = \(token = ""\) => \{)re", "");
}

TEST("public models page is registered") {
  EXPECT_EQ(hasStaticAsset("/models"), true);
  EXPECT_EQ(hasStaticAsset("/models.html"), true);
  EXPECT_MATCH(modelsHtml, R"re(<script type="module" src="\/models\.js\?v=decision-models-20261025"><\/script>)re", "");
  EXPECT_MATCH(modelsScript, R"re(Decisions · POST \/v1\/decisions)re", "");
}

TEST("public console pages share versioned styles, canonical navigation, and accurate active states") {
  const std::string sharedAssetVersion = "ubiquity-signin-20261025";
  const std::vector<std::pair<std::string, std::string>> canonicalLinks = {
    {"/models", "Models"}, {"/developers", "Developers"}, {"/docs", "Docs"}, {"/chat", "Chat"}, {"/admin", "Admin"},
  };
  struct Page {
    std::string name;
    const std::string& html;
    std::string pageCss;
    std::optional<std::string> activeHref;
  };
  const std::vector<Page> pages = {
    {"index", indexHtml, "home.css", std::nullopt},  {"docs", docsHtml, "docs.css", "/docs"},     {"developers", developersHtml, "docs.css", "/developers"},
    {"models", modelsHtml, "models.css", "/models"}, {"chat", chatHtml, "chat.css", "/chat"},     {"about", aboutHtml, "docs.css", std::nullopt},
    {"contact", contactHtml, "docs.css", std::nullopt}, {"privacy", privacyHtml, "docs.css", std::nullopt},
  };

  for (const Page& page : pages) {
    const std::vector<std::string> stylesheetHrefs = matchAllGroup(R"re(<link\b(?=[^>]*\brel="stylesheet")[^>]*\bhref="([^"]+)"[^>]*>)re", "g", page.html, 1);
    EXPECT_EQ(stylesheetHrefs.size(), 2u);
    EXPECT_EQ(stylesheetHrefs.empty() ? std::string() : stylesheetHrefs[0], "/style.css?v=" + sharedAssetVersion);
    static const rt::Regex DOT("\\.", "g");
    const std::string pagePattern = "^/" + rt::replace(page.pageCss, DOT, "\\.") + "\\?v=[\\w.-]+$";
    EXPECT_MATCH(stylesheetHrefs.size() > 1 ? stylesheetHrefs[1] : std::string(), pagePattern, "");
    for (const std::string& href : stylesheetHrefs) {
      if (!hasStaticAsset(rt::URL(href, "https://ai.ubq.fi").pathname())) rt::test::report(__FILE__, __LINE__, href + " should be registered", false);
    }

    EXPECT_EQ(countMatches(R"re(<header\b[^>]*\bdata-shared-header\b[^>]*>)re", "g", page.html), 1u);
    EXPECT_EQ(countMatches(R"re(<nav\b[^>]*\bdata-actions\b[^>]*>)re", "g", page.html), 1u);
    const std::vector<std::string> primaryNavMatches =
      matchAllGroup(R"re(<nav\b(?=[^>]*\bdata-actions\b)(?=[^>]*\baria-label="Primary")[^>]*>([\s\S]*?)<\/nav>)re", "g", page.html, 1);
    EXPECT_EQ(primaryNavMatches.size(), 1u);
    const std::string primaryNav = primaryNavMatches.empty() ? std::string() : primaryNavMatches[0];
    EXPECT_NO_MATCH(primaryNav, R"re(<a\b[^>]*\bdata-variant="primary")re", "");

    struct Link {
      std::string attributes;
      std::string href;
      std::string label;
    };
    std::vector<Link> links;
    const rt::Regex LINK(R"re(<a\b([^>]*)>([\s\S]*?)<\/a>)re", "g");
    for (const rt::RegexMatch& match : rt::matchAll(primaryNav, LINK)) {
      const std::string attributes = match[1].value_or("");
      links.push_back({attributes, execGroup(R"re(\bhref="([^"]+)")re", "", attributes, 1).value_or(""), rt::trim(collapseWhitespace(stripHtmlTags(match[2].value_or(""))))});
    }
    std::vector<std::pair<std::string, std::string>> linkPairs;
    for (const Link& link : links) linkPairs.emplace_back(link.href, link.label);
    if (linkPairs != canonicalLinks) rt::test::report(__FILE__, __LINE__, page.name + " should preserve the canonical primary navigation order and labels", false);
    for (const Link& link : links) EXPECT_MATCH(link.attributes, R"re(\bdata-button\b)re", "");

    std::vector<std::string> currentLinks;
    for (const Link& link : links) {
      if (rt::Regex(R"re(\baria-current="page")re").test(link.attributes)) currentLinks.push_back(link.href);
    }
    EXPECT_EQ(countMatches(R"re(\baria-current=)re", "g", primaryNav), page.activeHref ? 1u : 0u);
    EXPECT_EQ(currentLinks, page.activeHref ? std::vector<std::string>{*page.activeHref} : std::vector<std::string>{});
  }

  EXPECT_MATCH(adminHtml, "/style\\.css\\?v=" + sharedAssetVersion, "");
  EXPECT_MATCH(
    adminHtml,
    R"re(<nav data-actions aria-label="Primary">[\s\S]*href="\/models">Models<\/a>[\s\S]*href="\/developers">Developers<\/a>[\s\S]*href="\/docs">Docs<\/a>[\s\S]*href="\/chat">Chat<\/a>[\s\S]*href="\/admin">Admin<\/a>[\s\S]*<\/nav>)re",
    "");
}

TEST("public console styles share the light and dark design tokens without decorative Models blue") {
  const auto ruleBodies = [](const std::string& css, const std::string& selector) {
    static const rt::Regex SPECIAL(R"re([.*+?^${}()|[\]\\])re", "g");
    const std::string escapedSelector = rt::replace(selector, SPECIAL, "\\$&");
    return matchAllGroup(escapedSelector + "\\s*\\{([^}]*)\\}", "g", css, 1);
  };
  const auto ruleBodyContaining = [&ruleBodies](const std::string& css, const std::string& selector, const std::string& declaration) {
    for (const std::string& candidate : ruleBodies(css, selector)) {
      if (rt::includes(candidate, declaration)) return candidate;
    }
    rt::test::report(__FILE__, __LINE__, selector + " should have a CSS rule containing " + declaration, true);
    return std::string();
  };
  const auto tokenValue = [](const std::string& block, const std::string& name) {
    return rt::trim(execGroup("--" + name + ":\\s*([^;]+);", "", block, 1).value_or(""));
  };

  const std::string headerRule = ruleBodyContaining(styleCss, "header[data-shared-header]", "border-radius");
  EXPECT_MATCH(headerRule, R"re(border:\s*1px solid var\(--border\))re", "");
  EXPECT_MATCH(headerRule, R"re(background:\s*var\(--surface\))re", "");
  EXPECT_MATCH(headerRule, R"re(border-radius:\s*var\(--radius-lg\))re", "");

  const std::string navControlRule = ruleBodyContaining(styleCss, "[data-actions] a[data-button]", "min-height");
  EXPECT_MATCH(navControlRule, R"re(min-height:\s*var\(--control-height\))re", "");
  EXPECT_MATCH(navControlRule, R"re(border-radius:\s*var\(--radius-sm\))re", "");
  EXPECT_MATCH(navControlRule, R"re(background:\s*transparent)re", "");
  EXPECT_MATCH(navControlRule, R"re(color:\s*var\(--muted\))re", "");

  const std::string activeNavRule = ruleBodyContaining(styleCss, R"([data-actions] a[aria-current="page"])", "background");
  EXPECT_MATCH(activeNavRule, R"re(background:\s*var\(--selection\))re", "");
  EXPECT_MATCH(activeNavRule, R"re(color:\s*var\(--link\))re", "");

  // The OS picks the scheme; the light block is the base and dark overrides only the palette.
  const std::vector<std::string> rootRules = ruleBodies(styleCss, ":root");
  const std::string lightTokens = rootRules.empty() ? std::string() : rootRules[0];
  const std::string darkTokens = execGroup(R"re(@media \(prefers-color-scheme: dark\)\s*\{\s*:root\s*\{([\s\S]*?)\}\s*\})re", "", styleCss, 1).value_or("");
  EXPECT_MATCH(lightTokens, R"re(color-scheme:\s*light dark;)re", "");
  EXPECT_MATCH(darkTokens, R"re(color-scheme:\s*dark;)re", "");
  EXPECT_EQ(tokenValue(lightTokens, "bg"), "#f7f9fc");
  EXPECT_EQ(tokenValue(lightTokens, "surface"), "#ffffff");
  EXPECT_EQ(tokenValue(lightTokens, "surface-2"), "#f1f5f9");
  EXPECT_EQ(tokenValue(lightTokens, "text"), "#111827");
  EXPECT_EQ(tokenValue(lightTokens, "muted"), "#5f6b7a");
  EXPECT_EQ(tokenValue(lightTokens, "accent"), "#0063d1");
  EXPECT_EQ(tokenValue(lightTokens, "accent-ink"), "#ffffff");
  EXPECT_EQ(tokenValue(lightTokens, "control-height"), "44px");
  EXPECT_EQ(tokenValue(lightTokens, "radius-sm"), "10px");
  EXPECT_EQ(tokenValue(lightTokens, "radius-lg"), "14px");
  EXPECT_EQ(tokenValue(darkTokens, "bg"), "#09090b");
  EXPECT_EQ(tokenValue(darkTokens, "surface"), "#141418");
  EXPECT_EQ(tokenValue(darkTokens, "surface-2"), "#1d1d22");
  EXPECT_EQ(tokenValue(darkTokens, "text"), "#f5f5f7");
  EXPECT_EQ(tokenValue(darkTokens, "muted"), "#a1a1aa");
  EXPECT_EQ(tokenValue(darkTokens, "accent-ink"), "#ffffff");
  EXPECT_MATCH(styleCss, R"re(--font-family:\s*ui-sans-serif)re", "");
  EXPECT_MATCH(styleCss, R"re(body\s*\{[^}]*font-size:\s*14px)re", "");

  const std::vector<std::string> inputBlocks = matchAllGroup(R"re((?:^|\n)input,\n\s*textarea,\n\s*select\s*\{([^}]*)\})re", "g", styleCss, 1);
  std::string controlBlock;
  for (const std::string& block : inputBlocks) {
    if (rt::includes(block, "min-height")) {
      controlBlock = block;
      break;
    }
  }
  EXPECT(controlBlock != "");
  EXPECT_NO_MATCH(controlBlock, R"re(color-scheme)re", "");

  EXPECT_MATCH(styleCss, R"re(@media \(hover: hover\) and \(pointer: fine\))re", "");
  const std::string reducedMotion = execGroup(R"re(@media \(prefers-reduced-motion: reduce\)\s*\{([\s\S]*?)\n\})re", "", styleCss, 1).value_or("");
  EXPECT(reducedMotion != "");
  EXPECT_NO_MATCH(reducedMotion, R"re(transition:\s*none)re", "");

  const std::vector<std::string> cssColorLiterals = rt::matchGlobal(modelsCss, rt::Regex(R"re(#[\da-f]{3,8}\b|rgba?\([^)]*\))re", "gi")).value_or(std::vector<std::string>{});
  const auto colorChannels = [](const std::string& literal) -> std::optional<std::array<double, 3>> {
    if (rt::startsWith(literal, "#")) {
      const std::string hex = rt::slice(literal, 1);
      std::string rgb;
      if (rt::length(hex) == 3 || rt::length(hex) == 4) {
        for (const std::string& channel : rt::split(rt::slice(hex, 0, 3), "")) rgb += rt::repeat(channel, 2);
      } else {
        rgb = rt::slice(hex, 0, 6);
      }
      if (rt::length(rgb) != 6) return std::nullopt;
      return std::array<double, 3>{rt::parseInt(rt::slice(rgb, 0, 2), 16), rt::parseInt(rt::slice(rgb, 2, 4), 16), rt::parseInt(rt::slice(rgb, 4, 6), 16)};
    }
    const std::optional<rt::RegexMatch> match = rt::Regex(R"re(^rgba?\(\s*(\d+(?:\.\d+)?)\s*,\s*(\d+(?:\.\d+)?)\s*,\s*(\d+(?:\.\d+)?))re", "i").exec(literal);
    if (!match) return std::nullopt;
    return std::array<double, 3>{rt::stringToNumber(*(*match)[1]), rt::stringToNumber(*(*match)[2]), rt::stringToNumber(*(*match)[3])};
  };
  std::vector<std::string> decorativeBlues;
  for (const std::string& literal : cssColorLiterals) {
    const std::optional<std::array<double, 3>> channels = colorChannels(literal);
    if (!channels) continue;
    const auto [red, green, blue] = *channels;
    if (blue - red >= 24 && blue - green >= 12) decorativeBlues.push_back(literal);
  }
  EXPECT_EQ(decorativeBlues, std::vector<std::string>{});
}

TEST("published guidance documents endpoint-specific output caps and repository-local auth uploads") {
  for (const std::string* text : {&readmeText, &llmsFullText}) {
    const std::string& publishedText = *text;
    EXPECT_MATCH(publishedText, R"re(`max_completion_tokens` is the OpenAI Chat Completions cap)re", "");
    EXPECT_MATCH(publishedText, R"re(`max_output_tokens` is the OpenAI Responses cap)re", "");
    EXPECT_MATCH(publishedText, R"re(Chat Completions to Codex[\s\S]*translated to the Codex Responses field `max_output_tokens`)re", "");
    EXPECT_MATCH(publishedText, R"re(Responses to Codex[\s\S]*`max_output_tokens` is forwarded as `max_output_tokens`)re", "");
    EXPECT_MATCH(publishedText, R"re(Chat Completions to Cerebras \(`gpt-oss-120b`\)[\s\S]*forwarded unchanged)re", "");
    EXPECT_MATCH(publishedText, R"re(Chat Completions to DeepSeek official[\s\S]*translated to DeepSeek's documented `max_tokens`)re", "");
    EXPECT_MATCH(publishedText, R"re(DeepSeek official[\s\S]*https:\/\/api\.deepseek\.com\/chat\/completions)re", "");
    EXPECT_MATCH(publishedText, R"re(DeepSeek official[\s\S]*deepseek_api_key_missing)re", "");
    EXPECT_MATCH(publishedText, R"re(Responses to DeepSeek official[\s\S]*`max_output_tokens` is translated to DeepSeek's documented `max_tokens`)re", "");
    EXPECT_MATCH(publishedText, R"re(Responses API[\s\S]*tool_calls)re", "");
    EXPECT_MATCH(publishedText, R"re(Paid fallback \(Metered or Surplus\)[\s\S]*Chat `max_completion_tokens` arrives as `max_output_tokens`)re", "");
    EXPECT_MATCH(publishedText, R"re(Do not swap these fields between endpoints)re", "");
  }

  EXPECT_NO_MATCH(readmeText, R"re(cd lib\/ai\.ubq\.fi)re", "");
  EXPECT_MATCH(readmeText, R"re(git rev-parse --show-toplevel)re", "");
  EXPECT_MATCH(llmsFullText, R"re(scripts\/upload-codex-auth\.ts)re", "");
  EXPECT_MATCH(llmsFullText, R"re(bun run upload:auth --url https:\/\/ai\.ubq\.fi --auth-json ~\/\.codex\/auth\.json)re", "");
}

TEST("admin console every rendered tab declares an access requirement") {
  // `canAccessView` returns false for a view with no `VIEW_REQUIREMENTS` entry,
  // so a tab added to the markup without one is dead for every role.
  const std::string requirements = execGroup(R"re(const VIEW_REQUIREMENTS = \{([\s\S]*?)\n\};)re", "", adminScript, 1).value_or("");
  EXPECT(requirements != "");
  const std::vector<std::string> declaredList = matchAllGroup(R"re(^\s{2}([a-z-]+):)re", "gm", requirements, 1);
  const std::set<std::string> declared(declaredList.begin(), declaredList.end());
  const std::vector<std::string> tabs = matchAllGroup(R"re(id="view-tab-([a-z-]+)")re", "g", adminHtml, 1);
  EXPECT(!tabs.empty());
  for (const std::string& tab : tabs) {
    // `session` is the signed-out shell, handled before the requirement lookup.
    if (tab == "session") continue;
    if (!declared.contains(tab)) rt::test::report(__FILE__, __LINE__, "admin tab '" + tab + "' declares no access requirement", false);
  }
}

TEST("admin console tabs follow the approved operator order") {
  // The strip renders in DOM order and arrow-key traversal follows it, so this pins the approved operator order.
  const std::vector<std::string> tabs = matchAllGroup(R"re(id="view-tab-([a-z-]+)")re", "g", adminHtml, 1);
  EXPECT_EQ(tabs, (std::vector<std::string>{"analytics", "providers", "models", "defaults", "errors", "keys", "users", "kernel", "pubkeys", "supervisor"}));
}

TEST("models page labels provider counts as catalog entries, not inference availability") {
  EXPECT_MATCH(modelsHtml, R"re(Model catalog)re", "");
  EXPECT_MATCH(modelsHtml, R"re(Availability and quota vary by\s+provider)re", "");
  EXPECT_MATCH(modelsHtml, R"re(Cataloged text models and the providers that list\s+them)re", "");
  EXPECT_MATCH(modelsHtml, R"re(does not guarantee\s+inference availability or remaining quota)re", "");
  EXPECT_NO_MATCH(modelsHtml, R"re(Live upstream catalog)re", "");
  EXPECT_MATCH(modelsScript, R"re(cataloged model)re", "");
  EXPECT_MATCH(modelsScript, R"re(cataloged models)re", "");
  EXPECT_NO_MATCH(modelsScript, R"re(available models)re", "");
}

ASYNC_TEST("public brand logos are inline and inherit the page foreground") {
  EXPECT_MATCH(styleCss, R"re(\[data-logo\]\s*\{[^}]*color: inherit;)re", "");
  const std::optional<std::string> sourcePath = execGroup(R"re(<path\b[\s\S]*?\bd="([^"]+)")re", "", companyLogoSvg, 1);
  ASSERT(sourcePath.has_value());

  const std::vector<std::pair<std::string, const std::string*>> pages = {
    {"/about", &aboutHtml}, {"/contact", &contactHtml}, {"/developers", &developersHtml}, {"/models", &modelsHtml}, {"/privacy", &privacyHtml},
  };
  for (const auto& [path, sourceHtml] : pages) {
    auto serving = handleStaticAsset(path);
    const std::optional<rt::Response> response = co_await std::move(serving);
    EXPECT(response.has_value() && response->status() == 200);
    ASSERT(response.has_value());
    auto reading = response->text();
    const std::string html = co_await std::move(reading);
    EXPECT(html == *sourceHtml);
    EXPECT_NO_MATCH(html, R"re(<img\b[^>]*(?:data-logo|data-models-logo)[^>]*>)re", "");
    EXPECT_MATCH(html, R"re(<svg\b(?=[^>]*\bdata-logo\b)[^>]*>[\s\S]*?<path\b[\s\S]*?fill="currentColor")re", "");
    const std::optional<std::string> inlinePath = execGroup(R"re(<svg\b(?=[^>]*\bdata-logo\b)[^>]*>[\s\S]*?<path\b[\s\S]*?\bd="([^"]+)")re", "", html, 1);
    EXPECT_EQ(inlinePath, sourcePath);
  }
}

TEST("public agent-readiness pages and crawl artifacts are registered") {
  for (const std::string_view path : {"/developers", "/developers.html", "/about", "/about.html", "/contact", "/contact.html", "/privacy", "/privacy.html", "/robots.txt",
                                      "/sitemap.xml"}) {
    if (!hasStaticAsset(std::string(path))) rt::test::report(__FILE__, __LINE__, std::string(path) + " should be registered", false);
  }
}

TEST("admin analytics view places capacity history before current providers") {
  const std::int64_t listIndex = rt::indexOf(adminHtml, R"(id="provider-capacity-list")");
  const std::int64_t chartIndex = rt::indexOf(adminHtml, R"(id="provider-capacity-chart")");
  EXPECT(chartIndex >= 0);
  EXPECT(listIndex > chartIndex);

  EXPECT_MATCH(adminHtml, R"re(id="card-provider-capacity">Provider analytics)re", "");
  EXPECT_MATCH(adminHtml, R"re(id="view-tab-analytics"[\s\S]*?>\s*Analytics\s*<)re", "");
  EXPECT_NO_MATCH(adminHtml, R"re(Fifteen-minute capacity, cached-input, and cache-write history)re", "");
  EXPECT_MATCH(adminHtml, R"re(admin\.css\?v=claude-usage-refresh-20261025)re", "");
  EXPECT_MATCH(adminHtml, R"re(admin\.js\?v=gemini-inference-20261118)re", "");
  EXPECT_MATCH(adminScript, R"re(endpoints\.has\("\/v1\/decisions"\))re", "");
  EXPECT_MATCH(adminScript, R"re(toLocaleString\(undefined,)re", "");
  EXPECT_MATCH(adminScript, R"re(timeZoneName: "short")re", "");
  EXPECT_MATCH(adminScript, R"re(creditExpiries)re", "");
  // ECMA-402 forbids combining `dateStyle`/`timeStyle` with `timeZoneName`; V8
  // throws `TypeError: Invalid option : option`, which aborted the capacity
  // render and blanked the analytics panel.
  const std::string expiryFormatter =
    rt::slice(adminScript, static_cast<double>(rt::indexOf(adminScript, "const formatCreditExpiry")), static_cast<double>(rt::indexOf(adminScript, "const creditExpiryHint")));
  EXPECT_MATCH(expiryFormatter, R"re(timeZoneName: "short")re", "");
  EXPECT_NO_MATCH(expiryFormatter, R"re(dateStyle|timeStyle)re", "");
  EXPECT_MATCH(adminHtml, R"re(admin-supervisor\.css\?v=admin-permissions-20261005)re", "");
  EXPECT_MATCH(adminScript, R"re(from "\.\/admin-supervisor\.js\?v=20261001-supervisor-v4";)re", "");
  EXPECT_NO_MATCH(adminScript, R"re(super-admin)re", "");
  EXPECT_NO_MATCH(adminHtml, R"re(removed_provider-failover|debug-routing)re", "");
  EXPECT_NO_MATCH(adminScript, R"re(RemovedProviderFailover|refresh=live)re", "");
  EXPECT_MATCH(adminScript, R"re(fetch\(apiUrl\("\/admin\/providers\/capacity"\))re", "");
  EXPECT_MATCH(adminScript, R"re(Cached input share)re", "");
  EXPECT_MATCH(adminScript, R"re(written to cache)re", "");
  EXPECT_MATCH(adminScript, R"re(cache_write_input_tokens)re", "");
  EXPECT_MATCH(adminScript, R"re(cacheWriteFieldsMissing)re", "");
  EXPECT_MATCH(adminScript, R"re(snapshot\?\.prompt_cache\?\.buckets)re", "");
  EXPECT_MATCH(adminScript, R"re(snapshot\?\.prompt_cache\?\.status !== "ready")re", "");
  EXPECT_MATCH(adminScript, R"re(cache analytics unavailable)re", "");
  EXPECT_MATCH(adminScript, R"re(const loadId = \+\+providersLoadId)re", "");
  EXPECT_MATCH(adminScript, R"re(if \(loadId !== providersLoadId\) return)re", "");
  EXPECT_MATCH(adminScript, R"re(cache: "no-store")re", "");
  EXPECT_MATCH(adminScript, R"re(Authorization: `Bearer \$\{token\}`)re", "");
  EXPECT_MATCH(adminScript, R"re(auth\.js\?v=passkey-relay-20260823-v5)re", "");
  EXPECT_MATCH(adminScript, R"re(credentials: "include")re", "");
  EXPECT_MATCH(adminScript, R"re(if \(!getAdminToken\(\) \|\| \(isRemoteRelayOrigin\(\) && relaySessionActive\)\) headers\.delete\("Authorization"\))re", "");
  const std::int64_t cookieFirstIndex = rt::indexOf(adminScript, "authResult = await requestAuth({});");
  const std::int64_t bearerFallbackIndex = rt::indexOf(adminScript, "authResult = await requestAuth({ Authorization: `Bearer ${token}` });");
  EXPECT(cookieFirstIndex >= 0);
  EXPECT(bearerFallbackIndex > cookieFirstIndex);
  EXPECT_MATCH(adminScript, R"re(relaySessionActive = false;\s+const authenticated = await testAdminToken\(\{ allowBearerFallback: false \}\))re", "");
  EXPECT_MATCH(adminScript, R"re(testAdminToken\(\{ allowBearerFallback: false \}\))re", "");
  EXPECT_MATCH(adminScript, R"re(authResult\.data\?\.auth\?\.method\?\.kind === "passkey_session")re", "");
  EXPECT_MATCH(adminScript, R"re(if \(relaySessionActive\) return "")re", "");
  EXPECT_MATCH(adminScript, R"re(token: isRemoteRelayOrigin\(\) && relaySessionActive \? "" : token)re", "");
  EXPECT_MATCH(adminScript, R"re(if \(isRemoteRelayOrigin\(\) && relaySessionActive\) \{)re", "");
  EXPECT_MATCH(adminScript, R"re(authenticated: true)re", "");
  EXPECT_NO_MATCH(adminScript, R"re(token: result\.token)re", "");
  EXPECT_NO_MATCH(adminScript, R"re(applySignedInToken\(relay\.token)re", "");
  EXPECT_MATCH(adminScript, R"re(!adminAccessState\.isAdmin \|\| !hasAdminCredential\(\))re", "");
  EXPECT_MATCH(authScript, R"re(relay_session !== true)re", "");
  EXPECT_MATCH(authScript, R"re(credentials: init\?\.credentials \?\? "include")re", "");
}

TEST("admin error tab opens its view") {
  EXPECT_MATCH(adminScript, R"re(viewTabErrors\.addEventListener\("click", \(\) => setAdminView\("errors", \{ hashMode: "push", focusAuth: true \}\)\))re", "");
  EXPECT_MATCH(adminScript, R"re(const loadId = \+\+errorsLoadId)re", "");
  EXPECT_MATCH(adminScript, R"re(if \(loadId !== errorsLoadId\) return)re", "");
  EXPECT_MATCH(adminScript, R"re(invalidateAdminErrors\("Sign in to load gateway errors\."\))re", "");
  EXPECT_MATCH(adminScript, R"re(invalidateAdminErrors\("Target changed\. Sign in to load gateway errors\."\))re", "");
}

TEST("admin responses use a scoped IndexedDB stale cache") {
  EXPECT_MATCH(adminCacheScript, R"re(const DATABASE_NAME = "uos_ai\.admin-cache";)re", "");
  EXPECT_MATCH(adminCacheScript, R"re(store\.createIndex\(SCOPE_INDEX, "scope", \{ unique: false \}\))re", "");
  EXPECT_MATCH(adminCacheScript, R"re(objectStore\(STORE_NAME\)\.get)re", "");
  EXPECT_NO_MATCH(adminCacheScript, R"re(localStorage|Authorization|Cookie)re", "");

  EXPECT_MATCH(adminScript, R"re(import \{ createAdminSnapshotCache \} from "\.\/admin-cache\.js\?v=admin-indexeddb-cache-20260830-v7";)re", "");
  EXPECT_MATCH(adminScript, R"re(const cacheFreshAdminRead = \(response, cacheKey, scope, epoch\) =>)re", "");
  EXPECT_MATCH(adminScript, R"re(if \(!response\.ok \|\| !cacheKey \|\| !scope)re", "");
  EXPECT_MATCH(adminScript, R"re(const hydrateAdminSnapshots = async \(\) =>)re", "");
  EXPECT_MATCH(adminScript,
               R"re(setAdminSnapshotScopeFromAuth\(data\.auth\);\s+setAdminAccessState\(\{ checked: true, isAdmin: true, isSuperAdmin \}\);\s+void hydrateAdminSnapshots\(\);)re",
               "");
  EXPECT_MATCH(adminScript, R"re(globalThis\.requestAnimationFrame\(\(\) => \{\s+globalThis\.setTimeout)re", "");
  EXPECT_NO_MATCH(adminScript, R"re(showPendingAdminViewAfterPrefetch)re", "");
  EXPECT_MATCH(
    adminScript,
    R"re(adminAccessState\.checked && adminAccessState\.isAdmin && currentAdminView === ADMIN_VIEW_DEFAULT\) \{\s+setAdminView\(ADMIN_VIEW_AUTHENTICATED_DEFAULT, \{ allowInaccessible: false \}\);)re",
    "");
  EXPECT_MATCH(adminScript, R"re("Cached · refreshing")re", "");
  EXPECT_MATCH(adminScript, R"re(setKeysBadge\("ok", view === "all" \? "No API keys" : `No \$\{view\} API keys`\);)re", "");
  EXPECT_MATCH(adminScript, R"re(const readCachedApiKeyRequestLogs = async \(keyId\) =>)re", "");
  EXPECT_MATCH(adminScript, R"re(renderApiKeyRequestLogs\(panel, list, summary, cached\.records, "Cached · refreshing"\);)re", "");
  EXPECT_MATCH(adminScript, R"re(const inFlightEntry = \{ scope, epoch, request: null \};)re", "");
  EXPECT_MATCH(adminScript, R"re(isCurrentApiKeyRequestLogCacheEntry\(cached, scope, epoch\)[\s\S]*now - cached\.fetchedAt < API_KEY_REQUEST_LOGS_TTL_MS)re", "");
  EXPECT_MATCH(adminScript, R"re(if \(isCurrentApiKeyRequestLogScope\(scope, epoch\)\) \{\s+apiKeyRequestLogCache\.set\(cacheKey, \{[\s\S]*scope,[\s\S]*epoch,)re", "");
  EXPECT_MATCH(adminScript, R"re(if \(apiKeyRequestLogPromises\.get\(cacheKey\) === inFlightEntry\) \{\s+apiKeyRequestLogPromises\.delete\(cacheKey\);)re", "");
  EXPECT_MATCH(adminScript,
               R"re(const scope = adminCacheScope;\s+const epoch = adminCacheEpoch;\s+const isCurrentPanel = \(\) =>[\s\S]*isCurrentApiKeyRequestLogScope\(scope, epoch\);)re",
               "");
  EXPECT_MATCH(adminScript, R"re(const cached = await readCachedApiKeyRequestLogs\(currentKeyId\);\s+if \(!isCurrentPanel\(\)\))re", "");
  EXPECT_MATCH(adminScript, R"re(const response = await refresh;\s+if \(!isCurrentPanel\(\)\))re", "");
  EXPECT_MATCH(adminScript, R"re(clearAdminSnapshotScope\(\);)re", "");
}

TEST("admin boot probes server auth mode without requiring a browser token") {
  EXPECT_NO_MATCH(adminScript, R"re(if \(!token && !relaySessionActive && !isRemoteRelayOrigin\(\)\))re", "");
  EXPECT_MATCH(adminScript, R"re(requestAuth\(token \? \{ Authorization: `Bearer \$\{token\}` \} : \{\}\))re", "");
  EXPECT_MATCH(adminScript, R"re(resetAdminPrefetchState\("Checking admin session\.\.\."\))re", "");
}

TEST("expanded auth widget keeps its mobile toggle right-aligned") {
  EXPECT_MATCH(adminCss, R"re(\[data-auth-widget\]\s*\{[^}]*grid-template-columns:\s*minmax\(0, 1fr\))re", "");
  EXPECT_MATCH(adminCss, R"re(#auth-widget-toggle\s*\{[^}]*grid-column:\s*1;[^}]*justify-self:\s*end)re", "");
  EXPECT_MATCH(adminCss, R"re(#auth-widget-panel\s*\{[^}]*grid-column:\s*1)re", "");
}

TEST("provider analytics graph reports inference 5xx buckets") {
  EXPECT_MATCH(adminScript, R"re(label: "Errors \(5xx\)")re", "");
  EXPECT_MATCH(adminScript, R"re(five_xx_buckets)re", "");
  EXPECT_MATCH(adminScript, R"re(marker\.dataset\.capacityInferenceError)re", "");
  EXPECT_NO_MATCH(adminScript, R"re(marker\.dataset\.capacityInference5xx)re", "");
  EXPECT_MATCH(adminCss, R"re(\[data-capacity-inference-error\] path)re", "");
  EXPECT_MATCH(adminCss, R"re(\[data-capacity-legend-item="inference-error"\])re", "");
  EXPECT_MATCH(adminScript, R"re(15-minute bucket starting)re", "");
  EXPECT_MATCH(adminScript, R"re(Failed inference buckets)re", "");
  EXPECT_MATCH(adminScript, R"re(marker\.setAttribute\("tabindex", "0"\))re", "");
}

TEST("provider analytics graph preserves its SVG text aspect ratio") {
  EXPECT_MATCH(adminScript, R"re(preserveAspectRatio: "xMinYMin meet")re", "");
  EXPECT_MATCH(adminScript, R"re(viewBox: `0 0 \$\{width\} \$\{height\}`,[\s\S]*?width,[\s\S]*?height,)re", "");
  EXPECT_MATCH(adminCss, R"re(\[data-capacity-chart-svg\]\s*\{[\s\S]*?height:\s*var\(--capacity-chart-height-px, 180px\))re", "");
  EXPECT_NO_MATCH(adminCss, R"re(\[data-capacity-chart-svg\]\s*\{[^}]*height:\s*clamp\()re", "");
}

TEST("provider analytics chart fills its column and follows the newest edge") {
  EXPECT_MATCH(adminCss, R"re(\[data-capacity-chart-body\]\s*\{[^}]*grid-template-columns:\s*minmax\(0, 1fr\);)re", "");
  EXPECT_NO_MATCH(adminCss, R"re(\[data-capacity-chart-body\][^}]*minmax\((180|210)px)re", "");
  EXPECT_MATCH(adminScript, R"re(let capacityChartFollowsLatest = true;)re", "");
  EXPECT_MATCH(adminScript, R"re(if \(scroll\.clientWidth <= 0\) return;)re", "");
  EXPECT_MATCH(adminScript, R"re(!capacityChartFollowsLatest && typeof state\?\.anchorAtMs === "number")re", "");
  EXPECT_MATCH(adminScript, R"re(view === "analytics"[\s\S]*?scheduleProviderCapacityChartResize\(\);)re", "");
}

TEST("static assets register autonomous agent discovery documents") {
  for (const std::string_view path : {"/llms.txt", "/llms-full.txt", "/docs/llms-agents.md", "/openapi.json"}) {
    if (!hasStaticAsset(std::string(path))) rt::test::report(__FILE__, __LINE__, std::string(path) + " should be registered", false);
  }
}

TEST("OpenAPI discovery contract describes the public inference API") {
  const json::value document = rt::jsonParse(openApiText);

  EXPECT_JSON(at(document, {"openapi"}), R"("3.1.0")");
  EXPECT_JSON(at(document, {"servers", "0", "url"}), R"("https://ai.ubq.fi")");
  EXPECT_TRUTHY(at(document, {"components", "securitySchemes", "bearerAuth"}));
  for (const auto& [path, method] : std::vector<std::pair<std::string_view, std::string_view>>{
         {"/health", "get"},
         {"/v1/models", "get"},
         {"/v1/chat/completions", "post"},
         {"/v1/responses", "post"},
         {"/v1/decisions", "post"},
         {"/v1/images/generations", "post"},
         {"/v1/images/edits", "post"},
         {"/uos/models/capabilities", "get"},
       }) {
    const json::value* operation = at(document, {"paths", path, method});
    ASSERT(operation != nullptr);
    if (!rt::get(*operation, "operationId") || !rt::get(*operation, "operationId")->is_string()) {
      rt::test::report(__FILE__, __LINE__, rt::toUpperCase(method) + " " + std::string(path) + " needs an operation id", false);
    }
    const std::optional<std::string_view> description = rt::getString(*operation, "description");
    if (!description || rt::length(*description) <= 80) {
      rt::test::report(__FILE__, __LINE__, rt::toUpperCase(method) + " " + std::string(path) + " needs a useful description", false);
    }
  }
  EXPECT_JSON(at(document, {"components", "schemas", "ChatCompletionRequest", "properties", "tools", "items", "$ref"}), R"("#/components/schemas/FunctionTool")");
  EXPECT_JSON(at(document, {"components", "schemas", "ChatCompletionRequest", "properties", "tool_choice", "$ref"}), R"("#/components/schemas/ToolChoice")");
  EXPECT_JSON(at(document, {"components", "schemas", "ResponseRequest", "properties", "tools", "items", "$ref"}), R"("#/components/schemas/ResponseFunctionTool")");
  EXPECT_JSON(at(document, {"components", "schemas", "ResponseRequest", "properties", "tool_choice", "$ref"}), R"("#/components/schemas/ResponseToolChoice")");
  EXPECT_UNDEFINED(at(document, {"components", "schemas", "ResponseRequest", "required"}));
  const json::value& responseFunctionTool = *at(document, {"components", "schemas", "ResponseFunctionTool"});
  EXPECT_JSON(at(responseFunctionTool, {"required"}), R"(["type","name"])");
  EXPECT_TRUTHY(at(responseFunctionTool, {"properties", "name"}));
  EXPECT_TRUTHY(at(responseFunctionTool, {"properties", "description"}));
  EXPECT_TRUTHY(at(responseFunctionTool, {"properties", "parameters"}));
  EXPECT_UNDEFINED(at(responseFunctionTool, {"properties", "function"}));
  const json::value& responseForcedToolChoice = *at(document, {"components", "schemas", "ResponseToolChoice", "oneOf", "1"});
  EXPECT_JSON(at(responseForcedToolChoice, {"required"}), R"(["type","name"])");
  EXPECT_TRUTHY(at(responseForcedToolChoice, {"properties", "name"}));
  EXPECT_UNDEFINED(at(responseForcedToolChoice, {"properties", "function"}));
  EXPECT_TRUTHY(at(document, {"components", "headers", "RateLimit"}));
  EXPECT_TRUTHY(at(document, {"components", "headers", "RateLimitPolicy"}));
  EXPECT_TRUTHY(at(document, {"components", "responses", "RateLimited", "headers", "Retry-After"}));
  EXPECT_TRUTHY(at(document, {"components", "responses", "RateLimited", "headers", "RateLimit"}));

  const json::value& generation = *at(document, {"paths", "/v1/images/generations", "post"});
  const json::value& edit = *at(document, {"paths", "/v1/images/edits", "post"});
  EXPECT_JSON(at(generation, {"operationId"}), R"("createImage")");
  EXPECT_JSON(at(edit, {"operationId"}), R"("createImageEdit")");
  EXPECT_EQ(rt::objectKeys(at(generation, {"requestBody", "content"})->as_object()), std::vector<std::string>{"application/json"});
  std::vector<std::string> editContent = rt::objectKeys(at(edit, {"requestBody", "content"})->as_object());
  rt::sort(editContent, [](const std::string& a, const std::string& b) { return static_cast<double>(rt::localeCompare(a, b)); });
  EXPECT_EQ(editContent, (std::vector<std::string>{"application/json", "multipart/form-data"}));
  EXPECT_JSON(at(generation, {"requestBody", "content", "application/json", "schema", "$ref"}), R"("#/components/schemas/ImageGenerationRequest")");
  EXPECT_JSON(at(edit, {"requestBody", "content", "application/json", "schema", "$ref"}), R"("#/components/schemas/ImageEditJsonRequest")");
  EXPECT_JSON(at(edit, {"requestBody", "content", "multipart/form-data", "schema", "$ref"}), R"("#/components/schemas/ImageEditMultipartRequest")");
  EXPECT_JSON(at(document, {"components", "schemas", "ImageGenerationRequest", "required"}), R"(["prompt"])");
  EXPECT_JSON(at(document, {"components", "schemas", "ImageEditJsonRequest", "required"}), R"(["images","prompt"])");
  EXPECT_JSON(at(document, {"components", "schemas", "ImageEditMultipartRequest", "required"}), R"(["prompt"])");
  EXPECT_JSON(at(document, {"components", "schemas", "ImageEditMultipartRequest", "anyOf"}), R"([{"required":["image"]},{"required":["image[]"]}])");
  const json::value* bracketOneOf = at(document, {"components", "schemas", "ImageEditMultipartRequest", "properties", "image[]", "oneOf"});
  const json::value* imageOneOf = at(document, {"components", "schemas", "ImageEditMultipartRequest", "properties", "image", "oneOf"});
  EXPECT(bracketOneOf && imageOneOf && rt::deepEqual(*bracketOneOf, *imageOneOf));
  EXPECT_JSON(at(document, {"components", "schemas", "ImageEditJsonRequest", "properties", "images", "items", "$ref"}), R"("#/components/schemas/ImageRef")");
  EXPECT_JSON(at(document, {"components", "schemas", "ImageEditJsonRequest", "properties", "mask", "$ref"}), R"("#/components/schemas/ImageMaskRef")");
  const json::value& imageRef = *at(document, {"components", "schemas", "ImageRef"});
  EXPECT_JSON(at(imageRef, {"required"}), R"(["image_url"])");
  EXPECT_UNDEFINED(at(imageRef, {"anyOf"}));
  EXPECT_UNDEFINED(at(imageRef, {"not"}));
  EXPECT_JSON(at(imageRef, {"additionalProperties"}), "false");
  EXPECT_JSON(at(imageRef, {"properties", "image_url", "format"}), R"("uri")");
  EXPECT_JSON(at(imageRef, {"properties", "image_url", "minLength"}), "1");
  EXPECT_JSON(at(imageRef, {"properties", "image_url", "maxLength"}), "20971520");
  EXPECT_JSON(
    at(imageRef, {"properties", "image_url", "pattern"}),
    R"j("^(?:[hH][tT][tT][pP][sS]?://|[dD][aA][tT][aA]:[iI][mM][aA][gG][eE]/(?:[pP][nN][gG]|[xX]-[pP][nN][gG]|[jJ][pP](?:[eE][gG]|[gG])|[wW][eE][bB][pP]);[bB][aA][sS][eE]64,)")j");
  EXPECT_UNDEFINED(at(imageRef, {"properties", "file_id"}));
  const json::value& imageMaskRef = *at(document, {"components", "schemas", "ImageMaskRef"});
  EXPECT_JSON(at(imageMaskRef, {"required"}), R"(["image_url"])");
  EXPECT_UNDEFINED(at(imageMaskRef, {"anyOf"}));
  EXPECT_UNDEFINED(at(imageMaskRef, {"not"}));
  EXPECT_JSON(at(imageMaskRef, {"additionalProperties"}), "false");
  EXPECT_JSON(at(imageMaskRef, {"properties", "image_url", "maxLength"}), "20971520");
  EXPECT_JSON(at(imageMaskRef, {"properties", "image_url", "pattern"}), R"("^[dD][aA][tT][aA]:[iI][mM][aA][gG][eE]/[pP][nN][gG];[bB][aA][sS][eE]64,")");
  EXPECT_UNDEFINED(at(imageMaskRef, {"properties", "file_id"}));
  const json::value& generationSchema = *at(document, {"components", "schemas", "ImageGenerationRequest"});
  const json::value& editJsonSchema = *at(document, {"components", "schemas", "ImageEditJsonRequest"});
  const json::value& editMultipartSchema = *at(document, {"components", "schemas", "ImageEditMultipartRequest"});
  EXPECT_JSON(at(generationSchema, {"properties", "n", "type"}), R"(["integer","null"])");
  EXPECT_JSON(at(generationSchema, {"properties", "model", "default"}), R"("gpt-image-1")");
  EXPECT_JSON(at(editJsonSchema, {"properties", "model", "default"}), R"("gpt-image-1.5")");
  EXPECT_JSON(at(editMultipartSchema, {"properties", "model", "default"}), R"("gpt-image-1.5")");
  EXPECT_MATCH(std::string(rt::getString(editMultipartSchema, "description").value_or("")), R"re(together they must total no more than 50 MiB)re", "");
  EXPECT_JSON(at(editJsonSchema, {"properties", "input_fidelity", "type"}), R"(["string","null"])");
  for (const json::value* schema : {&generationSchema, &editJsonSchema, &editMultipartSchema}) {
    EXPECT_JSON(at(*schema, {"additionalProperties"}), "false");
    EXPECT_JSON(at(*schema, {"properties", "prompt", "maxLength"}), "32000");
    EXPECT_JSON(at(*schema, {"properties", "user", "type"}), R"("string")");
    const json::value* userDescription = at(*schema, {"properties", "user", "description"});
    EXPECT_MATCH(std::string(userDescription ? rt::getString(*userDescription).value_or("") : ""), R"re(x-uos-warning: user_ignored)re", "");
    EXPECT_JSON(at(*schema, {"properties", "stream", "enum"}), "[false,null]");
    EXPECT_JSON(at(*schema, {"properties", "output_compression", "type"}), R"(["integer","null"])");
  }
  EXPECT_JSON(at(generationSchema, {"properties", "response_format", "enum"}), R"(["b64_json",null])");
  EXPECT_UNDEFINED(at(generationSchema, {"properties", "style"}));
  EXPECT_JSON(at(generationSchema, {"properties", "quality", "enum"}), R"(["low","medium","high","auto",null])");
  EXPECT_JSON(at(editJsonSchema, {"properties", "size", "type"}), R"(["string","null"])");
  EXPECT_JSON(at(editJsonSchema, {"properties", "size", "minLength"}), "1");
  EXPECT_UNDEFINED(at(editJsonSchema, {"properties", "size", "enum"}));
  EXPECT_JSON(at(editJsonSchema, {"properties", "quality", "enum"}), R"(["low","medium","high","auto",null])");
  EXPECT_JSON(at(editMultipartSchema, {"properties", "quality", "enum"}), R"(["low","medium","high","auto",null])");
  EXPECT_UNDEFINED(at(editMultipartSchema, {"properties", "moderation"}));
  EXPECT_JSON(at(editMultipartSchema, {"properties", "response_format", "enum"}), R"(["b64_json",null])");
  EXPECT_JSON(at(document, {"components", "schemas", "ImagesResponse", "properties", "data", "items", "$ref"}), R"("#/components/schemas/Image")");
  EXPECT_TRUTHY(at(document, {"components", "schemas", "ImagesResponse", "properties", "output_format"}));
  EXPECT_UNDEFINED(at(document, {"components", "schemas", "Image", "properties", "output_format"}));
}

TEST("published agent contracts distinguish bodyless catalog revalidation and conditional rate limits") {
  const json::value document = rt::jsonParse(openApiText);
  const std::string developerText = collapseWhitespace(stripHtmlTags(developersHtml));
  const json::value& modelResponses = *at(document, {"paths", "/v1/models", "get", "responses"});
  const json::value& modelList = *at(modelResponses, {"200"});
  const json::value& notModified = *at(document, {"paths", "/v1/models", "get", "responses", "304"});
  const auto text = [](const json::value* value) { return std::string(value ? rt::getString(*value).value_or("") : ""); };

  EXPECT_JSON(at(modelList, {"headers", "ETag", "$ref"}), R"("#/components/headers/ETag")");
  EXPECT_JSON(at(notModified, {"headers", "ETag", "$ref"}), R"("#/components/headers/ETag")");
  EXPECT_MATCH(text(at(document, {"components", "headers", "ETag", "description"})), R"re(versioned 200 and 304 responses)re", "i");
  EXPECT_MATCH(text(at(document, {"components", "headers", "ETag", "description"})), R"re(omitted from the unversioned)re", "i");
  EXPECT_UNDEFINED(at(notModified, {"content"}));
  EXPECT_MATCH(text(at(notModified, {"description"})), R"re(no response body; reuse the cached catalog)re", "i");
  EXPECT_MATCH(developerText, R"re(matching If-None-Match request to GET \/v1\/models\?client_version=X\.Y\.Z can return 304 Not Modified with no body)re", "i");
  EXPECT_MATCH(llmsFullText, R"re(matching `If-None-Match` requests receive `304 Not Modified`\s+with no body)re", "i");

  for (const std::string* published : {&openApiText, &llmsFullText}) {
    EXPECT_NO_MATCH(*published, R"re(RFC 9449)re", "");
    EXPECT_MATCH(*published, R"re(draft-ietf-httpapi-ratelimit-headers-11)re", "");
  }
  EXPECT_MATCH(text(at(document, {"components", "headers", "RetryAfter", "description"})), R"re(when present)re", "i");
  EXPECT_MATCH(text(at(document, {"components", "responses", "RateLimited", "description"})), R"re(Retry-After when present)re", "i");
  EXPECT_MATCH(developerText, R"re(For a 429 error, honor Retry-After when present)re", "i");
  EXPECT_MATCH(llmsText, R"re(honor `Retry-After` when present)re", "");
}

ASYNC_TEST("agent discovery documents are served with useful media types") {
  auto servingLlms = handleStaticAsset("/llms.txt");
  const std::optional<rt::Response> llms = co_await std::move(servingLlms);
  auto servingFull = handleStaticAsset("/llms-full.txt");
  const std::optional<rt::Response> llmsFull = co_await std::move(servingFull);
  auto servingMarkdown = handleStaticAsset("/docs/llms-agents.md");
  const std::optional<rt::Response> markdown = co_await std::move(servingMarkdown);
  auto servingOpenapi = handleStaticAsset("/openapi.json");
  const std::optional<rt::Response> openapi = co_await std::move(servingOpenapi);

  EXPECT(llms.has_value() && llms->status() == 200);
  ASSERT(llms.has_value());
  EXPECT_EQ(llms->headers().get("content-type"), std::optional<std::string>("text/plain; charset=utf-8"));
  auto reading = llms->text();
  const std::string llmsBody = co_await std::move(reading);
  EXPECT_MATCH(llmsBody, R"re(https:\/\/ai\.ubq\.fi\/openapi\.json)re", "");
  EXPECT(llmsFull.has_value() && llmsFull->headers().get("content-type") == std::optional<std::string>("text/plain; charset=utf-8"));
  EXPECT(markdown.has_value() && markdown->headers().get("content-type") == std::optional<std::string>("text/markdown; charset=utf-8"));
  EXPECT(openapi.has_value() && openapi->headers().get("content-type") == std::optional<std::string>("application/json; charset=utf-8"));
}

ASYNC_TEST("homepage content negotiation serves server-rendered HTML, Markdown, and explicit JSON") {
  const NoKv noKv;
  auto genericServing = handleRoot(rootRequest("*/*"));
  const rt::Response generic = co_await std::move(genericServing);
  auto htmlServing = handleRoot(rootRequest("text/html"));
  const rt::Response html = co_await std::move(htmlServing);
  auto xhtmlServing = handleRoot(rootRequest("application/xhtml+xml"));
  const rt::Response xhtml = co_await std::move(xhtmlServing);
  auto markdownServing = handleRoot(rootRequest("text/markdown"));
  const rt::Response markdown = co_await std::move(markdownServing);
  auto unacceptableServing = handleRoot(rootRequest("image/png"));
  const rt::Response unacceptable = co_await std::move(unacceptableServing);

  for (const rt::Response* response : {&generic, &html, &xhtml}) {
    EXPECT_EQ(response->headers().get("content-type"), std::optional<std::string>("text/html; charset=utf-8"));
    EXPECT_EQ(response->headers().get("vary"), std::optional<std::string>("Accept, Accept-Encoding"));
  }
  auto htmlReading = html.text();
  const std::string rawHtml = co_await std::move(htmlReading);
  EXPECT_MATCH(rawHtml, R"re(<h1>UbiquityOS AI Gateway<\/h1>)re", "");
  const std::string readableText = rt::trim(collapseWhitespace(stripHtmlTags(rawHtml)));
  EXPECT(rt::length(readableText) >= 500);
  EXPECT(static_cast<double>(rt::length(readableText)) / static_cast<double>(rt::length(rawHtml)) >= 0.05);

  EXPECT_EQ(markdown.headers().get("content-type"), std::optional<std::string>("text/markdown; charset=utf-8"));
  EXPECT_EQ(markdown.headers().get("vary"), std::optional<std::string>("Accept, Accept-Encoding"));
  auto markdownReading = markdown.text();
  const std::string markdownBody = co_await std::move(markdownReading);
  EXPECT_MATCH(markdownBody, R"re(^# UbiquityOS AI Gateway)re", "m");

  EXPECT_EQ(unacceptable.status(), 406);
  EXPECT_EQ(unacceptable.headers().get("content-type"), std::optional<std::string>("application/json"));
  auto unacceptableReading = unacceptable.json();
  const json::value unacceptableBody = co_await std::move(unacceptableReading);
  EXPECT_JSON(at(unacceptableBody, {"error", "code"}), R"("not_acceptable")");
}

ASYNC_TEST("public HEAD routes preserve GET metadata without a response body") {
  T2IngressScope ingress;
  const std::vector<std::pair<std::string, rt::Headers>> routes{
    {"/", rt::Headers{{"Accept", "text/html"}}}, {"/index.html", rt::Headers{}},
    {"/style.css", rt::Headers{}}, {"/docs/llms-agents.md", rt::Headers{}}, {"/openapi.json", rt::Headers{}},
  };
  for (const auto& [path, headers] : routes) {
    rt::RequestInit getInit; getInit.headers = headers;
    rt::RequestInit headInit; headInit.method = "HEAD"; headInit.headers = headers;
    const rt::Request getRequest("https://ai.ubq.fi" + path, getInit);
    const rt::Request headRequest("https://ai.ubq.fi" + path, headInit);
    auto getting = uos::handler::index::handler(getRequest); auto getResponse = co_await std::move(getting);
    auto heading = uos::handler::index::handler(headRequest); auto headResponse = co_await std::move(heading);
    EXPECT_EQ(headResponse.status(), getResponse.status());
    rt::Headers getHeaders = getResponse.headers(); getHeaders.delete_("x-uos-request-id");
    rt::Headers headHeaders = headResponse.headers(); headHeaders.delete_("x-uos-request-id");
    EXPECT_EQ(headHeaders.entries(), getHeaders.entries());
    auto reading = headResponse.text(); const auto text = co_await std::move(reading); EXPECT_EQ(text, "");
  }
}
ASYNC_TEST("admin routes and page aliases are never cacheable before authentication") {
  T2IngressScope ingress;
  const std::vector<std::pair<std::string, std::string>> routes{
    {"GET", "/admin"}, {"HEAD", "/admin"}, {"GET", "/admin.html"}, {"HEAD", "/admin.html"},
    {"OPTIONS", "/admin/defaults"}, {"GET", "/admin/not-a-real-route"},
    {"GET", "/admin/passkey-users"},
    {"PATCH", "/admin/passkey-users"},
    {"POST", "/admin/codex/auth"},
    {"GET", "/admin/providers/codex/banked-resets"},
    {"PATCH", "/admin/providers/codex/banked-resets"},
    {"GET", "/admin/providers/codex/overage-usage"},
    {"PATCH", "/admin/providers/codex/overage-usage"},
    {"GET", "/admin/providers/codex/banked-resets/shadow-decisions"},
    {"GET", "/admin/providers/codex/cache-scope-experiment"},
    {"POST", "/admin/providers/codex/cache-scope-experiment"},
    {"GET", "/admin/codex/models"},
    {"POST", "/admin/codex/models"},
    {"GET", "/admin/models/whitelist"},
    {"POST", "/admin/models/whitelist"},
    {"GET", "/admin/models/catalog"},
    {"POST", "/admin/models/refresh"},
    {"POST", "/admin/codex/prompts/purge"},
    {"POST", "/admin/kv-migration/import"},
    {"GET", "/admin/kv-migration/validate"},
    {"GET", "/admin/sentinel/replay-captures"},
    {"GET", "/admin/sentinel/incidents"},
    {"GET", "/admin/codex/supervisor/sessions"},
    {"GET", "/admin/codex/supervisor/output"},
    {"POST", "/admin/codex/supervisor/brief"},
    {"GET", "/admin/errors"},
    {"GET", "/admin/defaults"},
    {"POST", "/admin/defaults"},
    {"GET", "/admin/debug/routing"},
    {"POST", "/admin/debug/routing"},
    {"DELETE", "/admin/debug/routing"},
    {"GET", "/admin/providers"},
    {"GET", "/admin/providers/selection"},
    {"POST", "/admin/providers/selection"},
    {"GET", "/admin/providers/capacity"},
    {"POST", "/admin/providers/capacity/observations"},
    {"GET", "/admin/providers/capacity/rollups"},
    {"GET", "/admin/providers/claude-code/access"},
    {"POST", "/admin/providers/claude-code/access"},
    {"POST", "/admin/providers/claude-code/accounts/active"},
    {"GET", "/admin/providers/claude-code/auth"},
    {"POST", "/admin/providers/claude-code/auth"},
    {"DELETE", "/admin/providers/claude-code/auth"},
    {"POST", "/admin/providers/claude-code/quota/clear"},
    {"POST", "/admin/providers/claude-code/quota/refresh"},
    {"GET", "/admin/providers/quota-projection"},
    {"POST", "/admin/providers/quota-projection/backfill"},
    {"GET", "/admin/prompt-cache-analytics"},
    {"GET", "/admin/api-keys"},
    {"POST", "/admin/api-keys"},
    {"PATCH", "/admin/api-keys"},
    {"DELETE", "/admin/api-keys"},
    {"GET", "/admin/api-keys/changes"},
    {"POST", "/admin/api-keys/revoke"},
    {"POST", "/admin/api-keys/unrevoke"},
    {"GET", "/admin/kernel-usage"},
    {"POST", "/admin/kernel-usage"},
    {"DELETE", "/admin/kernel-usage"},
    {"GET", "/admin/kernel-policy-queue"},
    {"GET", "/admin/kernel-pubkeys"},
    {"POST", "/admin/kernel-pubkeys"},
    {"DELETE", "/admin/kernel-pubkeys"},
    {"POST", "/admin/providers/codex/1/recheck"},
    {"GET", "/admin/api-keys/example/paid-fallbacks"},
  };
  const std::vector<std::optional<std::string>> authorizations{std::nullopt, "Bearer definitely-wrong"};
  for (const auto& authorization : authorizations) {
    for (const auto& [method, path] : routes) {
      rt::RequestInit init; init.method = method;
      if (authorization) init.headers = rt::Headers{{"Authorization", *authorization}};
      const rt::Request request("https://ai.ubq.fi" + path, init);
      auto handling = uos::handler::index::handler(request); auto response = co_await std::move(handling);
      EXPECT_EQ(response.headers().get("cache-control"), "no-store");
      if (auto body = response.body()) co_await body->cancel();
    }
  }
}

TEST("admin API-key expired rows use the warning accent") {
  EXPECT_MATCH(styleCss, R"re(\[data-key\]\[data-state="expired"\][\s\S]*?--key-accent:\s*var\(--warning\))re", "");
}

TEST("admin defaults keep queued edits through reloads and debounce the follow-up") {
  EXPECT_MATCH(adminScript, R"re(if \(!defaultsLoaded\) \{\s+queueDefaultsSave\(\);\s+return;)re", "");
  EXPECT_MATCH(adminScript, R"re(const scheduleQueuedDefaultsSave = \(loadTarget\) =>)re", "");
  EXPECT_MATCH(adminScript, R"re(const clearDefaultsSaveQueue = \(\) => \{\s+cancelDefaultsSaveSchedule\(\);)re", "");
  EXPECT_MATCH(adminScript, R"re(defaultsEditRevision \+= 1;\s+clearDefaultsSaveQueue\(\);)re", "");
  EXPECT_MATCH(adminScript, R"re(else scheduleDefaultsSave\(\);)re", "");
}

ASYNC_TEST("unknown public HEAD routes remain 404") {
  T2IngressScope ingress;
  rt::RequestInit init; init.method = "HEAD";
  const rt::Request request("https://ai.ubq.fi/not-a-real-public-route", init);
  auto handling = uos::handler::index::handler(request); auto response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 404);
  auto reading = response.text(); const auto text = co_await std::move(reading); EXPECT_EQ(text, "");
}
ASYNC_TEST("credentialed root responses retain content-negotiation Vary tokens") {
  T2IngressScope ingress;
  rt::RequestInit init;
  init.headers = rt::Headers{{"Accept", "text/html"}, {"Origin", "https://agent-worker-4d2p9cx7m1ab.ubiquity-os.deno.net"}};
  const rt::Request request("https://ai.ubq.fi/", init);
  auto handling = uos::handler::index::handler(request); const auto response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(response.headers().get("vary"), "Accept, Accept-Encoding, Origin");
}

ASYNC_TEST("JSON root advertises autonomous agent discovery documents") {
  const NoKv noKv;
  auto serving = handleRoot(rootRequest("application/json"));
  const rt::Response response = co_await std::move(serving);
  auto reading = response.json();
  const json::value body = co_await std::move(reading);

  EXPECT_EQ(response.headers().get("vary"), std::optional<std::string>("Accept, Accept-Encoding"));
  EXPECT_JSON(at(body, {"name"}), R"("UbiquityOS AI Gateway")");
  EXPECT_JSON(at(body, {"discovery", "llms"}), R"("/llms.txt")");
  EXPECT_JSON(at(body, {"discovery", "llms_full"}), R"("/llms-full.txt")");
  EXPECT_JSON(at(body, {"discovery", "openapi"}), R"("/openapi.json")");
  EXPECT_JSON(at(body, {"discovery", "developers"}), R"("/developers")");
  EXPECT_JSON(at(body, {"discovery", "sitemap"}), R"("/sitemap.xml")");
}

TEST("homepage metadata and agent guidance expose the canonical service identity") {
  EXPECT_MATCH(indexHtml, R"re(<html lang="en">)re", "");
  EXPECT_MATCH(indexHtml, R"re(<link rel="canonical" href="https:\/\/ai\.ubq\.fi\/" \/>)re", "");
  EXPECT_MATCH(indexHtml, R"re(<meta property="og:type" content="website" \/>)re", "");
  EXPECT_MATCH(indexHtml, R"re(<meta property="og:image" content="https:\/\/ai\.ubq\.fi\/favicon\.png" \/>)re", "");
  for (const std::string_view path : {"/developers", "/openapi.json", "/llms.txt", "/llms-full.txt"}) {
    EXPECT_MATCH(indexHtml, "href=\"" + std::string(path) + "\"", "");
  }
  const std::optional<std::string> jsonLdMatch = execGroup(R"re(<script type="application\/ld\+json">([\s\S]*?)<\/script>)re", "", indexHtml, 1);
  const std::string jsonLdText = jsonLdMatch ? rt::trim(*jsonLdMatch) : std::string();
  ASSERT(!jsonLdText.empty());
  const json::value jsonLd = rt::jsonParse(jsonLdText);
  const auto hasType = [&jsonLd](std::string_view type) {
    return std::any_of(jsonLd.as_array().begin(), jsonLd.as_array().end(), [type](const json::value& entry) { return rt::getString(entry, "@type") == type; });
  };
  EXPECT(hasType("SoftwareApplication"));
  EXPECT(hasType("Organization"));
  EXPECT_MATCH(llmsText, R"re(## When to use this service)re", "");
  EXPECT_MATCH(llmsText, R"re(https:\/\/ai\.ubq\.fi\/developers)re", "");
}

ASYNC_TEST("trust pages and crawl artifacts are public, substantial, and well formed") {
  for (const std::string_view path : {"/developers", "/about", "/contact", "/privacy"}) {
    const std::string route(path);
    auto serving = handleStaticAsset(route);
    const std::optional<rt::Response> response = co_await std::move(serving);
    EXPECT(response.has_value() && response->status() == 200);
    ASSERT(response.has_value());
    EXPECT_EQ(response->headers().get("content-type"), std::optional<std::string>("text/html; charset=utf-8"));
    auto reading = response->text();
    const std::string body = co_await std::move(reading);
    if (rt::length(rt::trim(collapseWhitespace(stripHtmlTags(body)))) < 500) rt::test::report(__FILE__, __LINE__, route + " should have real text", false);
  }

  auto servingRobots = handleStaticAsset("/robots.txt");
  const std::optional<rt::Response> robots = co_await std::move(servingRobots);
  auto servingSitemap = handleStaticAsset("/sitemap.xml");
  const std::optional<rt::Response> sitemap = co_await std::move(servingSitemap);
  ASSERT(robots.has_value());
  ASSERT(sitemap.has_value());
  EXPECT_EQ(robots->headers().get("content-type"), std::optional<std::string>("text/plain; charset=utf-8"));
  auto robotsReading = robots->text();
  const std::string robotsText = co_await std::move(robotsReading);
  EXPECT_MATCH(robotsText, R"re(Sitemap: https:\/\/ai\.ubq\.fi\/sitemap\.xml)re", "");
  EXPECT_EQ(sitemap->headers().get("content-type"), std::optional<std::string>("application/xml; charset=utf-8"));
  auto sitemapReading = sitemap->text();
  const std::string sitemapText = co_await std::move(sitemapReading);
  EXPECT_MATCH(sitemapText, R"re(^<\?xml version="1\.0" encoding="UTF-8"\?>)re", "");
  for (const std::string_view path : {"", "/developers", "/docs", "/about", "/contact", "/privacy"}) {
    const std::string sitemapPath = path.empty() ? std::string("/") : std::string(path);
    EXPECT_MATCH(sitemapText, "<loc>https://ai\\.ubq\\.fi" + sitemapPath + "<\\/loc>", "");
  }
  EXPECT_MATCH(sitemapText, R"re(<lastmod>2026-08-21<\/lastmod>)re", "");
}

ASYNC_TEST("public delivery returns structured JSON errors and exposes rate-limit fields to browsers") {
  T2IngressScope ingress;
  const rt::Request request("https://ai.ubq.fi/not-a-real-public-route");
  auto handling = uos::handler::index::handler(request); auto response = co_await std::move(handling);
  EXPECT_EQ(response.status(), 404);
  EXPECT_EQ(response.headers().get("content-type"), "application/json");
  auto reading = response.json(); const auto body = co_await std::move(reading);
  EXPECT_EQ(rt::getString(body.at("error"), "code"), "not_found");
  EXPECT(rt::Regex("Not found").test(rt::getString(body.at("error"), "message").value_or("")));
  const auto exposed = uos::http::corsHeaders().get("Access-Control-Expose-Headers").value_or("");
  const std::vector<std::string> headers{"RateLimit", "RateLimit-Policy", "RateLimit-Limit", "RateLimit-Remaining", "RateLimit-Reset", "x-uos-attempted-providers"};
  for (const auto& header : headers) EXPECT(rt::Regex(header).test(exposed));
}

TEST("admin passkey sign-in uses discoverable credentials instead of a saved username") {
  const std::optional<std::string> loginCall = execGroup(R"re(const result = await signInWithPasskey\(\{([\s\S]*?)\}\);)re", "", adminScript, 1);
  ASSERT(loginCall.has_value());
  EXPECT_NO_MATCH(*loginCall, R"re(handle:|useHandle:)re", "");
  EXPECT_MATCH(*loginCall, R"re(baseUrl: getPasskeyBaseUrl\(\))re", "");
}

TEST("admin paid-provider wallet display never claims a balance the token endpoint cannot prove") {
  // OpenLux's only balance surface answers `unlimited_quota: true` for every
  // token and its granted/available/used totals do not track the real wallet
  // (a confirmed top-up moved them the wrong way), and inference responses carry
  // no balance header. The UI must therefore never render those figures, and
  // must never present the token-scope flag as an unlimited balance.
  EXPECT_NO_MATCH(adminScript, R"re(Unlimited quota)re", "");
  EXPECT_NO_MATCH(adminHtml, R"re(Unlimited quota)re", "");
  EXPECT_NO_MATCH(adminScript, R"re(formatQuotaTokens)re", "");
  EXPECT_NO_MATCH(adminScript, R"re("Available tokens"|"Granted tokens"|"Used tokens")re", "");
  EXPECT_NO_MATCH(adminHtml, R"re(Available tokens|Granted tokens|Used tokens)re", "");
  EXPECT_NO_MATCH(adminScript, R"re(meteredQuotaGranted|meteredQuotaTokenUsage)re", "");
  EXPECT_MATCH(adminHtml, R"re(<dt>Balance<\/dt>)re", "");
  // The Analytics Metered capacity row and quota forecast card that carried the
  // removed "Not reported" labels are gone (2026-10-03 trim); the surviving
  // Defaults metered-quota panel must still refuse to report an unprovable balance.
  EXPECT_MATCH(adminScript, R"re(meteredQuotaBalance\.textContent = "Not reported")re", "");
  // The real, event-derived provider signal and its timestamp survive.
  EXPECT_MATCH(adminScript, R"re(appendProviderFact\(facts, "Inference", status\.health \? providerStateLabel\(status\.health\) : "Not observed"\))re", "");
  EXPECT_MATCH(adminScript, R"re(appendProviderFact\(facts, "Last response", formatDate\(status\.health\?\.last_observed_at_ms\)\))re", "");
}
