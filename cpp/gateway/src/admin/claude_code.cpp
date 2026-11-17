#include "uos/admin/claude_code.hpp"

/**
 * Admin surface for the Claude subscription tier, the counterpart of the Codex
 * auth upload: read the credential/workspace/quota state, store or rotate a
 * credential (optionally pinned to a workspace), clear a stored credential,
 * and drop exhaustion guards so the next request retries immediately. Every
 * response reports the credential only as its Console-style partial hint.
 */

#include "uos/health.hpp"
#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/provider/claude_code.hpp"
#include "uos/provider/claude_code_access.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/request.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"
#include "uos/utils.hpp"

#include <memory>
#include <string>
#include <utility>
#include <variant>

namespace uos::admin::claude_code {
  namespace {
    namespace cc = provider::claude_code;
    namespace credentials = provider::claude_code_credentials;
    namespace accounts = provider::claude_code_accounts;
    namespace models = provider::claude_code_models;
    namespace quota = provider::claude_code_quota;
    namespace access = provider::claude_code_access;
    constexpr double CREDENTIAL_LABEL_MAX_LENGTH = 64;

    std::optional<std::string> getString(const json::value* value) {
      return value && value->is_string() ? std::optional<std::string>(std::string(value->as_string())) : std::nullopt;
    }

    struct CredentialValidation {
      bool ok;
      std::optional<double> status;
      std::optional<std::string> detail;
      std::optional<models::ClaudeCodeModelsSnapshot> snapshot{};
    };

    /**
     * Validate one submitted credential against the models listing, the same
     * cheap endpoint the health probe uses. A 429 is treated as authenticated but
     * limited (exactly like the Codex upload), transport failures leave the
     * credential unvalidated rather than rejected, and only an explicit
     * authentication or permission rejection refuses the upload.
     */
    rt::Task<CredentialValidation> validateClaudeCodeCredential(std::string secret, std::optional<std::string> workspaceId) {
      std::optional<double> status;
      try {
        cc::ClaudeCodeAuthHeaders credential{cc::claudeCodeCredentialKind(secret), secret};
        rt::Promise<models::ClaudeCodeModelsSnapshot> discovery(models::discoverClaudeCodeModels(credential, workspaceId));
        const auto snapshot = co_await discovery.wait();
        const CredentialValidation result{true, 200, std::nullopt, snapshot};
        co_return result;
      } catch (const models::ClaudeCodeModelsError& error) {
        status = error.status;
      } catch (...) {}
      if (status == 429) {
        const CredentialValidation result{true, status, std::nullopt};
        co_return result;
      }
      if (status == 401 || status == 403) {
        const CredentialValidation result{false, status, status == 401 ? "The credential was rejected as unauthenticated." : "The credential is not permitted for this endpoint."};
        co_return result;
      }
      const CredentialValidation result{true, status, "Stored unvalidated: the models listing could not be read."};
      co_return result;
    }

    /** The exact `claude_code` block the passive provider snapshot reports. */
    rt::Task<json::value> claudeSubscriptionBlock() {
      rt::Promise<json::object> reading(health::getPassiveProviderHealthSnapshot());
      const auto snapshot = co_await reading.wait();
      const auto* block = rt::get(snapshot, "claude_code");
      co_return block && rt::truthy(block) && (block->is_object() || block->is_array()) ? *block : json::value(nullptr);
    }

    struct CredentialParseValue { std::optional<std::string> secret; std::optional<std::string> label; };
    using CredentialParse = std::variant<CredentialParseValue, rt::Response>;
    CredentialParse parseCredentialSubmission(const json::value& body) {
      auto rawSecret = getString(rt::get(body, "credential"));
      if (!rawSecret) rawSecret = getString(rt::get(body, "api_key"));
      const auto secret = rawSecret ? std::optional<std::string>(rt::trim(*rawSecret)) : std::nullopt;
      if (secret && secret->empty()) return http::openaiError(400, "credential must not be blank", "invalid_request_error");
      if (secret && static_cast<double>(rt::length(*secret)) > credentials::CLAUDE_CODE_CREDENTIAL_MAX_LENGTH) return http::openaiError(400, "credential is too long", "invalid_request_error");
      if (secret && !rt::startsWith(*secret, "sk-ant-")) return http::openaiError(400, "credential must be an Anthropic API key or Claude Code OAuth token", "invalid_request_error");
      const auto rawLabel = getString(rt::get(body, "label"));
      const auto label = rawLabel ? std::optional<std::string>(rt::trim(*rawLabel)) : std::nullopt;
      if (label && static_cast<double>(rt::length(*label)) > CREDENTIAL_LABEL_MAX_LENGTH) return http::openaiError(400, "label is too long", "invalid_request_error");
      return CredentialParseValue{secret, label};
    }

    using WorkspaceParse = std::variant<rt::Nullable<std::string>, rt::Response>;
    WorkspaceParse parseWorkspaceSubmission(const json::value& body) {
      if (!body.is_object() || !rt::hasOwn(body.as_object(), "workspace_id")) return rt::Nullable<std::string>();
      const auto* value = rt::get(body, "workspace_id");
      if (value->is_null() || (value->is_string() && value->as_string().empty())) return rt::Nullable<std::string>(nullptr);
      const auto workspaceId = credentials::parseClaudeCodeWorkspaceId(value);
      if (workspaceId) return rt::Nullable<std::string>(*workspaceId);
      return http::openaiError(400, "workspace_id must be a wrkspc_ workspace id", "invalid_request_error");
    }

    rt::Task<std::optional<accounts::ClaudeCodeAccountBinding>> submissionTarget(std::shared_ptr<rt::Kv> store, json::value body, cc::ClaudeCodeEnvLookup env) {
      const auto* accountId = rt::get(body, "account_id");
      const auto* createNew = rt::get(body, "create_new");
      const auto* activate = rt::get(body, "activate");
      static const rt::Regex ACCOUNT_ID("^[a-f0-9-]{36}$");
      if (accountId && (!accountId->is_string() || !ACCOUNT_ID.test(std::string(accountId->as_string())))) throw rt::JsError("account_id is invalid");
      if (createNew && !createNew->is_bool()) throw rt::JsError("create_new must be boolean");
      if (activate && !activate->is_bool()) throw rt::JsError("activate must be boolean");
      const bool creating = createNew && createNew->as_bool();
      if (creating && accountId) throw rt::JsError("create_new cannot target an existing account");
      std::optional<accounts::ClaudeCodeAccountBinding> target;
      if (!creating) {
        accounts::ResolveClaudeCodeAccountOptions options{getString(accountId)};
        rt::Promise<std::optional<accounts::ClaudeCodeAccountBinding>> resolving(accounts::resolveClaudeCodeAccount(store, env, options));
        target = co_await resolving.wait();
      }
      if (rt::truthy(accountId) && !target) throw rt::JsError("Claude account was not found");
      co_return target;
    }

    struct ValidatedSubmission {
      json::value body;
      std::optional<accounts::ClaudeCodeAccountBinding> target;
      CredentialParseValue credential;
      std::string secret;
      std::optional<std::string> workspaceId;
      rt::Nullable<std::string> requestedWorkspace;
      CredentialValidation validation;
      cc::ClaudeCodeEnvLookup env;
    };
    std::optional<credentials::ClaudeCodeWorkspaceState> submittedWorkspace(const ValidatedSubmission& input) {
      if (!input.workspaceId || input.workspaceId->empty()) return std::nullopt;
      const auto previous = input.target ? input.target->workspace : std::nullopt;
      credentials::MergeClaudeCodeWorkspaceStateInput patch;
      patch.id = *input.workspaceId;
      patch.name = previous && previous->id == *input.workspaceId ? previous->name : std::nullopt;
      patch.source = input.requestedWorkspace == rt::undefined ? (previous ? previous->source : "kv") : "kv";
      patch.verified = input.validation.status == 200;
      return credentials::mergeClaudeCodeWorkspaceState(previous, patch);
    }
    rt::Task<std::variant<accounts::ClaudeCodeAccountBinding, rt::Response>> persistSubmission(std::shared_ptr<rt::Kv> store, ValidatedSubmission input) {
      std::optional<credentials::ClaudeCodeResolvedCredential> credential;
      if (input.credential.secret) credential = credentials::ClaudeCodeResolvedCredential{cc::claudeCodeCredentialKind(input.secret), input.secret, credentials::partialClaudeCodeCredentialHint(input.secret), input.credential.label, "kv", rt::dateNow()};
      rt::Reason failure;
      try {
        accounts::AccountUpdate patch;
        if (input.target) patch.accountId = input.target->account_id;
        patch.createNew = rt::getBool(input.body, "create_new").value_or(false);
        patch.activate = rt::getBool(input.body, "activate").value_or(false);
        patch.expected = input.target;
        patch.credential = credential;
        patch.workspace = rt::orNull(submittedWorkspace(input));
        rt::Promise<accounts::ClaudeCodeAccountBinding> updating(accounts::updateClaudeCodeAccount(store, patch, input.env));
        const auto account = co_await updating.wait();
        if (input.validation.snapshot) {
          rt::Promise<void> caching(models::cacheValidatedClaudeCodeModels(store, input.secret, input.workspaceId, *input.validation.snapshot, input.env, account));
          co_await caching.wait();
        }
        co_return account;
      } catch (...) {
        failure = std::current_exception();
      }
      co_return http::openaiError(409, rt::isError(failure) ? rt::errorMessage(failure).value_or("") : "Claude account changed before persistence", "claude_code_account_changed");
    }

    using ClaudeUsageRefreshTarget = std::variant<rt::Response, accounts::ClaudeCodeAccountBinding>;
    rt::Task<ClaudeUsageRefreshTarget> resolveClaudeUsageRefreshTarget(rt::Request req, std::shared_ptr<rt::Kv> store, ClaudeUsageRefreshOptions options) {
      rt::Promise<json::value> reading(request::readJsonBody(req));
      const auto body = co_await reading.wait();
      const auto* accountId = rt::get(body, "account_id");
      if (!utils::isRecord(body) || !accountId || !accountId->is_string()) co_return http::openaiError(400, "account_id is required", "invalid_request_error");
      const auto* cohort = rt::get(body, "account_cohort_id");
      if (cohort && !cohort->is_string()) co_return http::openaiError(400, "account_cohort_id must be a string", "invalid_request_error");
      accounts::ResolveClaudeCodeAccountOptions resolveOptions{std::string(accountId->as_string())};
      rt::Promise<std::optional<accounts::ClaudeCodeAccountBinding>> resolving(accounts::resolveClaudeCodeAccount(store, options.env, resolveOptions));
      const auto account = co_await resolving.wait();
      if (!account) co_return http::openaiError(404, "Claude account was not found", "not_found");
      if (cohort && std::string(cohort->as_string()) != account->account_cohort_id) co_return http::openaiError(409, "Claude account generation changed", "claude_code_account_changed");
      co_return *account;
    }
  }

  rt::Task<rt::Response> handleAdminClaudeCodeAuthGet() {
    rt::Promise<json::value> reading(claudeSubscriptionBlock());
    const auto block = co_await reading.wait();
    const json::object body{{"data", block}};
    co_return http::json(200, body, rt::Headers{{"Cache-Control", "no-store"}});
  }

  rt::Task<rt::Response> handleAdminClaudeCodeAuthSet(rt::Request req) { return handleAdminClaudeCodeAuthSet(std::move(req), ClaudeCodeAuthOptions{}); }
  rt::Task<rt::Response> handleAdminClaudeCodeAuthSet(rt::Request req, ClaudeCodeAuthOptions options) {
    const auto store = co_await kv::getKv().wait();
    if (!store) co_return http::openaiError(500, "KV is not available; cannot persist the Claude credential", "server_error");
    rt::Promise<json::value> reading(request::readJsonBody(req));
    const auto body = co_await reading.wait();
    if (!utils::isRecord(body)) co_return http::openaiError(400, "Invalid JSON body", "invalid_request_error");
    const auto credentialResult = parseCredentialSubmission(body);
    if (const auto* error = std::get_if<rt::Response>(&credentialResult)) co_return *error;
    const auto credential = std::get<CredentialParseValue>(credentialResult);
    const auto workspaceResult = parseWorkspaceSubmission(body);
    if (const auto* error = std::get_if<rt::Response>(&workspaceResult)) co_return *error;
    const auto workspace = std::get<rt::Nullable<std::string>>(workspaceResult);
    if (!credential.secret && workspace == rt::undefined) co_return http::openaiError(400, "credential or workspace_id is required", "invalid_request_error");
    std::optional<accounts::ClaudeCodeAccountBinding> target;
    rt::Reason targetFailure;
    try {
      rt::Promise<std::optional<accounts::ClaudeCodeAccountBinding>> resolving(submissionTarget(store, body, options.env));
      target = co_await resolving.wait();
    } catch (...) {
      targetFailure = std::current_exception();
    }
    if (targetFailure) co_return http::openaiError(400, rt::isError(targetFailure) ? rt::errorMessage(targetFailure).value_or("") : "Invalid account target", "invalid_request_error");
    const auto secret = credential.secret ? credential.secret : target ? std::optional<std::string>(target->credential.secret) : std::nullopt;
    if (!secret || secret->empty()) co_return http::openaiError(409, "No Claude credential is configured to validate this workspace against", "credential_missing");
    const auto workspaceId = workspace == rt::undefined ? (target && target->workspace ? std::optional<std::string>(target->workspace->id) : std::nullopt) : workspace.hasValue() ? std::optional<std::string>(*workspace) : std::nullopt;
    rt::Promise<CredentialValidation> validating(validateClaudeCodeCredential(*secret, workspaceId));
    const auto validation = co_await validating.wait();
    if (!validation.ok) co_return http::openaiError(400, validation.detail.value_or("The credential was rejected upstream."), "invalid_api_key");
    ValidatedSubmission submission{body, target, credential, *secret, workspaceId, workspace, validation, options.env};
    rt::Promise<std::variant<accounts::ClaudeCodeAccountBinding, rt::Response>> persisting(persistSubmission(store, submission));
    const auto accountResult = co_await persisting.wait();
    if (const auto* response = std::get_if<rt::Response>(&accountResult)) co_return *response;
    const auto& account = std::get<accounts::ClaudeCodeAccountBinding>(accountResult);
    rt::Promise<json::value> status(claudeSubscriptionBlock());
    const auto block = co_await status.wait();
    const json::object response{{"data", block}, {"account_id", account.account_id}, {"validation", json::object{{"status", validation.status ? rt::makeNumber(*validation.status) : json::value(nullptr)}, {"detail", validation.detail ? json::value(*validation.detail) : json::value(nullptr)}}}, {"hint", credential.secret ? json::value(credentials::partialClaudeCodeCredentialHint(*secret)) : json::value(nullptr)}};
    co_return http::json(200, response);
  }

  rt::Task<rt::Response> handleAdminClaudeCodeQuotaRefresh(rt::Request req) { return handleAdminClaudeCodeQuotaRefresh(std::move(req), ClaudeUsageRefreshOptions{}); }
  /** Compatibility read for the former refresh action. This route never probes. */
  rt::Task<rt::Response> handleAdminClaudeCodeQuotaRefresh(rt::Request req, ClaudeUsageRefreshOptions options) {
    const auto store = co_await kv::getKv().wait();
    if (!store) co_return http::openaiError(503, "Claude quota storage is unavailable", "server_error");
    rt::Promise<ClaudeUsageRefreshTarget> resolving(resolveClaudeUsageRefreshTarget(req, store, options));
    const auto target = co_await resolving.wait();
    if (const auto* response = std::get_if<rt::Response>(&target)) co_return *response;
    const auto account = std::get<accounts::ClaudeCodeAccountBinding>(target);
    rt::Promise<std::optional<quota::ClaudeCodeQuotaState>> loading(quota::loadClaudeCodeQuotaState(store, account));
    const auto state = co_await loading.wait();
    rt::Promise<json::value> status(claudeSubscriptionBlock());
    const auto block = co_await status.wait();
    const auto observedAt = state && state->response_headers ? std::optional<double>(state->response_headers->observed_at_ms) : std::nullopt;
    const json::object response{{"data", block}, {"refreshed", false}, {"reason", "response_headers"}, {"observed_at_ms", observedAt ? rt::makeNumber(*observedAt) : json::value(nullptr)}, {"usage", nullptr}, {"message", "Quota updates from response headers on ordinary Claude requests."}};
    co_return http::json(200, response);
  }

  rt::Task<rt::Response> handleAdminClaudeCodeAuthDelete() { return handleAdminClaudeCodeAuthDelete(std::nullopt); }
  rt::Task<rt::Response> handleAdminClaudeCodeAuthDelete(std::optional<rt::Request> req) {
    const auto store = co_await kv::getKv().wait();
    if (!store) co_return http::openaiError(500, "KV is not available; cannot remove the Claude account", "server_error");
    const auto accountId = req ? rt::URL(req->url()).searchParams().get("account_id") : std::nullopt;
    const accounts::ResolveClaudeCodeAccountOptions options{accountId};
    rt::Promise<std::optional<accounts::ClaudeCodeAccountBinding>> resolving(accounts::resolveClaudeCodeAccount(store, cc::ClaudeCodeEnvLookup(), options));
    const auto account = co_await resolving.wait();
    if (!account && accountId && !accountId->empty()) co_return http::openaiError(404, "Claude account was not found", "not_found");
    if (account) {
      rt::Promise<bool> removing(accounts::removeClaudeCodeAccount(store, account->account_id));
      co_await removing.wait();
    }
    rt::Promise<json::value> status(claudeSubscriptionBlock());
    const auto block = co_await status.wait();
    const json::object response{{"data", block}};
    co_return http::json(200, response);
  }

  rt::Task<rt::Response> handleAdminClaudeCodeQuotaClear() { return handleAdminClaudeCodeQuotaClear(std::nullopt); }
  /** Clear only gateway guards for the targeted generation, never upstream quota or other accounts. */
  rt::Task<rt::Response> handleAdminClaudeCodeQuotaClear(std::optional<rt::Request> req) {
    const auto store = co_await kv::getKv().wait();
    if (!store) co_return http::openaiError(500, "KV is not available; cannot clear Claude guards", "server_error");
    json::value raw = json::object();
    if (req && req->body()) {
      rt::Promise<json::value> reading(request::readJsonBody(*req));
      raw = co_await reading.wait();
    }
    if (!utils::isRecord(raw)) co_return http::openaiError(400, "Invalid JSON body", "invalid_request_error");
    const auto* rawId = rt::get(raw, "account_id");
    if (rawId && !rawId->is_string()) co_return http::openaiError(400, "account_id must be a string", "invalid_request_error");
    const accounts::ResolveClaudeCodeAccountOptions options{getString(rawId)};
    rt::Promise<std::optional<accounts::ClaudeCodeAccountBinding>> resolving(accounts::resolveClaudeCodeAccount(store, cc::ClaudeCodeEnvLookup(), options));
    const auto account = co_await resolving.wait();
    if (!account && rt::truthy(rawId)) co_return http::openaiError(404, "Claude account was not found", "not_found");
    if (account) {
      const auto* cohort = rt::get(raw, "account_cohort_id");
      if (rt::truthy(rawId) && (!cohort || !cohort->is_string())) co_return http::openaiError(400, "account_cohort_id is required for a targeted clear", "invalid_request_error");
      if (cohort && (!cohort->is_string() || std::string(cohort->as_string()) != account->account_cohort_id)) co_return http::openaiError(409, "Claude account generation changed", "claude_code_account_changed");
      rt::Promise<std::optional<quota::ClaudeCodeQuotaState>> clearing(quota::clearClaudeCodeQuotaGuards(store, account));
      const auto cleared = co_await clearing.wait();
      if (!cleared) co_return http::openaiError(409, "Claude guards changed during clear", "claude_code_account_changed");
    }
    rt::Promise<json::value> status(claudeSubscriptionBlock());
    const auto block = co_await status.wait();
    const json::object response{{"data", block}, {"guards_cleared", account.has_value()}};
    co_return http::json(200, response);
  }

  rt::Task<rt::Response> handleAdminClaudeCodeAccess(rt::Request req) {
    const auto store = co_await kv::getKv().wait();
    if (!store) co_return http::openaiError(503, "Claude principal policy storage is unavailable", "server_error");
    access::ClaudeCodeAccessPolicy policy;
    if (req.method() == "GET") {
      rt::Promise<access::ClaudeCodeAccessPolicy> loading(access::loadClaudeCodeAccessPolicy(store));
      policy = co_await loading.wait();
    } else {
      rt::Promise<json::value> reading(request::readJsonBody(req));
      const auto body = co_await reading.wait();
      const auto ids = utils::isRecord(body) ? access::parseClaudeCodeAccessIds(rt::get(body, "allowed_key_ids")) : std::nullopt;
      if (!ids) co_return http::openaiError(400, "allowed_key_ids must be an explicit array of principal IDs", "invalid_request_error");
      rt::Promise<access::ClaudeCodeAccessPolicy> storing(access::storeClaudeCodeAccessPolicy(*store, *ids));
      policy = co_await storing.wait();
    }
    const json::object response{{"data", json::object{{"source", policy.source}, {"allowed_key_ids", json::value_from(policy.allowed_key_ids)}, {"updated_at_ms", rt::makeNumber(policy.updated_at_ms)}}}};
    co_return http::json(200, response);
  }

  rt::Task<rt::Response> handleAdminClaudeCodeAccountActive(rt::Request req) {
    const auto store = co_await kv::getKv().wait();
    if (!store) co_return http::openaiError(503, "Claude account storage is unavailable", "server_error");
    rt::Promise<json::value> reading(request::readJsonBody(req));
    const auto body = co_await reading.wait();
    const auto accountId = getString(rt::get(body, "account_id"));
    if (!utils::isRecord(body) || !accountId) co_return http::openaiError(400, "account_id is required", "invalid_request_error");
    rt::Promise<bool> selecting(accounts::selectClaudeCodeAccount(store, *accountId));
    const auto selected = co_await selecting.wait();
    if (!selected) co_return http::openaiError(404, "Claude account was not found", "not_found");
    rt::Promise<json::value> status(claudeSubscriptionBlock());
    const auto block = co_await status.wait();
    const json::object response{{"data", block}};
    co_return http::json(200, response);
  }
}
