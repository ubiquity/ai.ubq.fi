#include "uos/provider/claude_code_models.hpp"
#include "uos/rt/memory.hpp"

#include "uos/kv.hpp"
#include "uos/provider/claude_code.hpp"
#include "uos/provider/surplus_claude_request.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"
#include "uos/rt/weak.hpp"
#include "uos/utils.hpp"

#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace uos::provider::claude_code_models {
  namespace {
    using claude_code_accounts::ClaudeCodeAccountBinding;
    using claude_code_credentials::ClaudeCodeResolvedCredential;
    using rt::record::Reader;
    using rt::record::Writer;

    struct Cache {
      std::string identity;
      std::optional<ClaudeCodeModelsSnapshot> snapshot;
      double expires;
      std::optional<rt::Promise<std::optional<ClaudeCodeModelsSnapshot>>> pending;
    };
    // `Map<string, Cache>`: Cache objects are compared by identity.
    struct Scoped : std::unordered_map<std::string, std::shared_ptr<Cache>> {
      rt::memory::ScopedCache memory{rt::memory::Cache::claude_models};

      void reportMemory() {
        std::size_t bytes = bucket_count() * sizeof(void*);
        for (const auto& [key, cached] : *this) {
          bytes += sizeof(Cache) + rt::memory::bytes(key) + rt::memory::bytes(cached->identity);
          if (cached->snapshot) bytes += rt::memory::bytes(json::value_from(*cached->snapshot));
        }
        memory.set(size(), bytes);
      }
    };

    constexpr double CACHE_TTL_MS = 5 * 60'000;
    constexpr double RETRY_MS = 30'000;
    constexpr int MAX_PAGES = 20;
    constexpr double MAX_PAGE_BYTES = 2 * 1024 * 1024;

    // `let caches = new WeakMap<object, Map<string, Cache>>()`, keyed by a Kv or by the module's
    // `environmentScope` object (its own slot here). Reassigned by resetClaudeCodeModelsCacheForTest.
    struct Caches {
      rt::WeakMap<rt::Kv, std::shared_ptr<Scoped>> byKv;
      std::shared_ptr<Scoped> environment;
    };
    std::shared_ptr<Caches>& caches() {
      static std::shared_ptr<Caches> slot = std::make_shared<Caches>();
      return slot;
    }
    // `caches.get(kv ?? environmentScope)`
    std::shared_ptr<Scoped> scopeOf(const std::shared_ptr<rt::Kv>& kv) {
      if (!kv) return caches()->environment;
      return caches()->byKv.get(*kv).value_or(nullptr);
    }
    void setScope(const std::shared_ptr<rt::Kv>& kv, std::shared_ptr<Scoped> scoped) {
      if (!kv) caches()->environment = std::move(scoped);
      else caches()->byKv.set(*kv, std::move(scoped));
    }
    void deleteScope(const std::shared_ptr<rt::Kv>& kv) {
      if (!kv) caches()->environment = nullptr;
      else caches()->byKv.delete_(*kv);
    }

    bool positiveInteger(const json::value* value) { return value != nullptr && rt::isNumber(*value) && rt::Number::isSafeInteger(*value) && rt::toNumber(*value) > 0; }

    // `parent && isRecord(parent[key]) ? parent[key] : null`, then `typeof flag.supported === "boolean" ? flag.supported : undefined`
    std::optional<bool> observedFlag(const json::value* parent, std::string_view key) {
      if (parent == nullptr) return std::nullopt;
      const json::value* flag = rt::get(*parent, key);
      if (!utils::isRecord(flag)) return std::nullopt;
      const json::value* supported = rt::get(*flag, "supported");
      if (supported == nullptr || !supported->is_bool()) return std::nullopt;
      return supported->get_bool();
    }

    std::vector<std::string> observedEfforts(const json::value* capabilities, const json::value* types) {
      const json::value* effort = capabilities != nullptr && utils::isRecord(rt::get(*capabilities, "effort")) ? rt::get(*capabilities, "effort") : nullptr;
      const json::value* supported = effort != nullptr ? rt::get(*effort, "supported") : nullptr;
      if (supported == nullptr || !supported->is_bool() || !supported->get_bool() || observedFlag(types, "adaptive") != std::optional<bool>(true)) return {};
      std::vector<std::string> levels;
      if (!effort->is_object()) return levels;
      for (const json::key_value_pair* entry : rt::objectEntries(effort->get_object())) {
        const std::string key(entry->key());
        if (key == "supported" || key == "none" || key.empty() || !utils::isRecord(entry->value())) continue;
        const json::value* flag = rt::get(entry->value(), "supported");
        if (flag != nullptr && flag->is_bool() && flag->get_bool()) levels.push_back(key);
      }
      return levels;
    }

    std::optional<ClaudeCodeModel> modelFromRecord(const json::value& value) {
      if (!utils::isRecord(value)) return std::nullopt;
      const json::value* id = rt::get(value, "id");
      if (id == nullptr || !id->is_string() || !surplus_claude_request::isSurplusClaudeModel(*id) || rt::length(std::string_view(id->get_string())) > 512)
        return std::nullopt;
      const json::value* createdAt = rt::get(value, "created_at");
      const double created =
        createdAt != nullptr && createdAt->is_string() ? rt::Math::floor(rt::Date::parse(std::string_view(createdAt->get_string())) / 1000) : 0;
      const json::value* displayName = rt::get(value, "display_name");
      const json::value* maxInputTokens = rt::get(value, "max_input_tokens");
      const json::value* maxTokens = rt::get(value, "max_tokens");
      const json::value* capabilities = utils::isRecord(rt::get(value, "capabilities")) ? rt::get(value, "capabilities") : nullptr;
      const json::value* thinking = capabilities != nullptr && utils::isRecord(rt::get(*capabilities, "thinking")) ? rt::get(*capabilities, "thinking") : nullptr;
      const json::value* types = thinking != nullptr && utils::isRecord(rt::get(*thinking, "types")) ? rt::get(*thinking, "types") : nullptr;
      return ClaudeCodeModel{
        .id = std::string(id->get_string()),
        .display_name = displayName != nullptr && displayName->is_string() ? std::optional<std::string>(std::string(displayName->get_string())) : std::nullopt,
        .created = rt::Number::isSafeInteger(created) && created > 0 ? created : 0,
        .context_window_tokens = positiveInteger(maxInputTokens) ? std::optional<double>(rt::toNumber(*maxInputTokens)) : std::nullopt,
        .max_output_tokens = positiveInteger(maxTokens) ? std::optional<double>(rt::toNumber(*maxTokens)) : std::nullopt,
        .supports_disabled_thinking = observedFlag(types, "disabled"),
        .supports_adaptive_thinking = observedFlag(types, "adaptive"),
        .supported_reasoning_levels = observedEfforts(capabilities, types),
        .supports_image_input = observedFlag(capabilities, "image_input"),
        .supports_pdf_input = observedFlag(capabilities, "pdf_input"),
      };
    }

    rt::Task<json::value> readPage(rt::Response response) {
      const std::optional<rt::ByteStream> body = response.body();
      if (!body) throw ClaudeCodeModelsError(response.status());
      const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = body->getReader();
      std::vector<rt::ByteChunk> chunks;
      double size = 0;
      rt::Reason failure;
      try {
        for (;;) {
          rt::ReadableStreamReadResult<rt::ByteChunk> result = co_await reader.read();
          if (result.done) break;
          size += static_cast<double>(result.value->size());
          if (size > MAX_PAGE_BYTES) throw ClaudeCodeModelsError(response.status());
          chunks.push_back(std::move(*result.value));
        }
      } catch (...) {
        failure = std::current_exception();
      }
      try {
        co_await reader.cancel();
      } catch (...) {
      }
      co_await rt::microtask();
      reader.releaseLock();
      if (failure) std::rethrow_exception(failure);
      std::vector<std::uint8_t> bytes;
      bytes.reserve(static_cast<std::size_t>(size));
      for (const rt::ByteChunk& chunk : chunks) bytes.insert(bytes.end(), chunk.begin(), chunk.end());
      json::value payload = rt::jsonParse(rt::decodeUtf8(bytes));
      const json::value* data = utils::isRecord(payload) ? rt::get(payload, "data") : nullptr;
      if (data == nullptr || !data->is_array()) throw ClaudeCodeModelsError(response.status());
      co_return payload;
    }

    struct ParsedPage {
      std::vector<ClaudeCodeModel> models;
      std::optional<std::string> next;  // string | null
    };

    rt::Task<ParsedPage> parsedPage(rt::Response response) {
      rt::Promise<json::value> reading(readPage(response));
      const json::value payload = co_await reading.wait();
      ParsedPage page;
      for (const json::value& row : rt::get(payload, "data")->get_array()) {
        const json::value* id = utils::isRecord(row) ? rt::get(row, "id") : nullptr;
        if (id == nullptr || !id->is_string() || rt::trim(std::string_view(id->get_string())).empty()) throw ClaudeCodeModelsError(response.status());
        if (std::optional<ClaudeCodeModel> model = modelFromRecord(row)) page.models.push_back(std::move(*model));
      }
      const json::value* hasMore = rt::get(payload, "has_more");
      if (hasMore == nullptr || !hasMore->is_bool()) throw ClaudeCodeModelsError(response.status());
      if (!hasMore->get_bool()) co_return page;
      const json::value* lastId = rt::get(payload, "last_id");
      if (lastId == nullptr || !lastId->is_string() || lastId->get_string().empty()) throw ClaudeCodeModelsError(response.status());
      page.next = std::string(lastId->get_string());
      co_return page;
    }

    struct Binding {
      ClaudeCodeResolvedCredential credential;
      std::optional<std::string> workspaceId;  // string | null
      std::string identity;
      double generation;
      std::string cacheKey;
    };

    rt::Task<std::optional<Binding>> binding(std::shared_ptr<rt::Kv> kv, claude_code::ClaudeCodeEnvLookup env, std::optional<ClaudeCodeAccountBinding> expectedAccount) {
      const double generation = claude_code_credentials::claudeCodeStateGeneration(kv);
      std::optional<ClaudeCodeAccountBinding> account;
      if (kv) {
        claude_code_accounts::ResolveClaudeCodeAccountOptions resolveOptions;
        if (expectedAccount) resolveOptions.accountId = expectedAccount->account_id;
        rt::Promise<std::optional<ClaudeCodeAccountBinding>> resolving(claude_code_accounts::resolveClaudeCodeAccount(kv, env, resolveOptions));
        account = co_await resolving.wait();
      }
      if (kv && !account) co_return std::nullopt;
      if (expectedAccount) {
        rt::Promise<bool> checking(claude_code_accounts::isClaudeCodeAccountCurrent(kv, *expectedAccount));
        const bool current = co_await checking.wait();
        if (!current) co_return std::nullopt;
      }
      std::optional<ClaudeCodeResolvedCredential> credential;
      if (account) {
        credential = account->credential;
      } else {
        rt::Promise<std::optional<ClaudeCodeResolvedCredential>> resolving(claude_code_credentials::resolveClaudeCodeCredential(nullptr, env));
        credential = co_await resolving.wait();
      }
      std::optional<std::string> workspaceId;
      if (account && account->workspace) {
        workspaceId = account->workspace->id;
      } else if (!kv) {
        rt::Promise<std::optional<claude_code_credentials::ClaudeCodeResolvedWorkspace>> resolving(claude_code_credentials::resolveClaudeCodeWorkspace(nullptr, env));
        const std::optional<claude_code_credentials::ClaudeCodeResolvedWorkspace> workspace = co_await resolving.wait();
        if (workspace) workspaceId = workspace->id;
      }
      if (!credential || generation != claude_code_credentials::claudeCodeStateGeneration(kv)) co_return std::nullopt;
      json::array parts;
      parts.emplace_back(account ? json::value(json::string(account->account_id)) : json::value(nullptr));
      parts.emplace_back(account ? json::value(json::string(account->account_cohort_id)) : json::value(nullptr));
      parts.emplace_back(json::string(credential->kind));
      parts.emplace_back(json::string(credential->secret));
      parts.emplace_back(workspaceId ? json::value(json::string(*workspaceId)) : json::value(nullptr));
      parts.emplace_back(rt::makeNumber(generation));
      rt::Promise<std::string> hashing(utils::sha256Hex(rt::jsonStringify(json::value(std::move(parts)))));
      const std::string identity = co_await hashing.wait();
      if (generation != claude_code_credentials::claudeCodeStateGeneration(kv)) co_return std::nullopt;
      co_return Binding{
        .credential = std::move(*credential),
        .workspaceId = std::move(workspaceId),
        .identity = identity,
        .generation = generation,
        .cacheKey = account ? account->account_id : std::string("environment"),
      };
    }

    // `binding(kv, env, expectedAccount).catch(() => null)`
    rt::Task<std::optional<Binding>> bindingOrNull(std::shared_ptr<rt::Kv> kv, claude_code::ClaudeCodeEnvLookup env,
                                                   std::optional<ClaudeCodeAccountBinding> expectedAccount) {
      std::optional<Binding> out;
      try {
        rt::Promise<std::optional<Binding>> resolving(binding(std::move(kv), std::move(env), std::move(expectedAccount)));
        out = co_await resolving.wait();
      } catch (...) {
        out = std::nullopt;
      }
      co_return out;
    }

    // The `(async () => { ... })()` refresh of fetchClaudeCodeModels.
    rt::Task<std::optional<ClaudeCodeModelsSnapshot>> refresh(std::shared_ptr<Cache> state, Binding current, std::shared_ptr<rt::Kv> kv, CatalogOptions options,
                                                              double now) {
      std::optional<ClaudeCodeModelsSnapshot> snapshot;
      bool failed = false;
      try {
        DiscoveryOptions discovery;
        discovery.fetcher = options.fetcher;
        discovery.now = options.now;
        rt::Promise<ClaudeCodeModelsSnapshot> discovering(
          discoverClaudeCodeModels(claude_code::ClaudeCodeAuthHeaders{.kind = current.credential.kind, .secret = current.credential.secret}, current.workspaceId,
                                   std::move(discovery)));
        snapshot = co_await discovering.wait();
      } catch (...) {
        failed = true;
      }
      if (failed) {
        if (state->snapshot) {
          ClaudeCodeModelsSnapshot stale = *state->snapshot;
          for (ClaudeCodeModel& model : stale.models) {
            model.supports_disabled_thinking = std::nullopt;
            model.supports_adaptive_thinking = std::nullopt;
            model.supported_reasoning_levels = std::nullopt;
          }
          stale.stale = true;
          snapshot = std::move(stale);
        } else {
          snapshot = std::nullopt;
        }
      }
      rt::Promise<std::optional<Binding>> relatest(bindingOrNull(kv, options.env, options.accountBinding));
      const std::optional<Binding> latest = co_await relatest.wait();
      const std::shared_ptr<Scoped> scoped = scopeOf(kv);
      const auto found = scoped ? scoped->find(current.cacheKey) : Scoped::iterator();
      const bool sameState = scoped && found != scoped->end() && found->second == state;
      if (!sameState || !latest || latest->identity != current.identity || current.generation != claude_code_credentials::claudeCodeStateGeneration(kv))
        co_return std::nullopt;
      state->snapshot = snapshot;
      scoped->reportMemory();
      state->expires = now + (snapshot && !snapshot->stale.value_or(false) ? CACHE_TTL_MS : RETRY_MS);
      co_return snapshot;
    }
  }

  std::vector<std::string> claudeCodeReasoningLevels(const ClaudeCodeModel& model) {
    std::vector<std::string> levels;
    if (model.supports_disabled_thinking == std::optional<bool>(true)) levels.emplace_back("none");
    if (model.supported_reasoning_levels) levels.insert(levels.end(), model.supported_reasoning_levels->begin(), model.supported_reasoning_levels->end());
    return levels;
  }

  rt::Task<ClaudeCodeModelsSnapshot> discoverClaudeCodeModels(claude_code::ClaudeCodeAuthHeaders credential, std::optional<std::string> workspaceId,
                                                              DiscoveryOptions options) {
    rt::OrderedMap<std::string, ClaudeCodeModel> models;
    std::unordered_set<std::string> cursors;
    const rt::AbortSignal signal = rt::AbortSignal::timeout(8'000);
    std::optional<std::string> cursor;
    for (int page = 0; page < MAX_PAGES; page++) {
      rt::URL url{std::string(claude_code::CLAUDE_CODE_MODELS_URL)};
      url.searchParams().set("limit", "100");
      if (cursor && !cursor->empty()) url.searchParams().set("after_id", *cursor);
      const claude_code::ClaudeCodeFetch fetcher = options.fetcher ? options.fetcher : rt::globalFetch();
      rt::RequestInit init;
      claude_code::ClaudeCodeRequestHeadersOptions headerOptions{.stream = false, .workspaceId = workspaceId};
      init.headers = claude_code::claudeCodeRequestHeaders(credential, headerOptions);
      init.redirect = "manual";
      init.signal = signal;
      rt::RequestInfo input = url;
      rt::Promise<rt::Response> fetching(fetcher(std::move(input), std::move(init)));
      const rt::Response response = co_await fetching.wait();
      if (!response.ok()) {
        if (const std::optional<rt::ByteStream> body = response.body()) {
          try {
            co_await body->cancel();
          } catch (...) {
          }
        }
        co_await rt::microtask();
        throw ClaudeCodeModelsError(response.status());
      }
      rt::Promise<ParsedPage> parsing(parsedPage(response));
      ParsedPage payload = co_await parsing.wait();
      for (ClaudeCodeModel& model : payload.models) {
        const std::string id = model.id;
        models.set(id, std::move(model));
      }
      if (!payload.next) {
        ClaudeCodeModelsSnapshot snapshot;
        for (const auto& [id, model] : models) snapshot.models.push_back(model);
        snapshot.updated_at_ms = options.now ? options.now() : rt::dateNow();
        co_return snapshot;
      }
      const std::string next = *payload.next;
      if (cursors.contains(next)) throw ClaudeCodeModelsError(response.status());
      cursors.insert(next);
      cursor = next;
    }
    throw ClaudeCodeModelsError(std::nullopt);
  }
  rt::Task<ClaudeCodeModelsSnapshot> discoverClaudeCodeModels(claude_code::ClaudeCodeAuthHeaders credential, std::optional<std::string> workspaceId) {
    return discoverClaudeCodeModels(std::move(credential), std::move(workspaceId), DiscoveryOptions());
  }

  rt::Task<std::optional<ClaudeCodeModelsSnapshot>> fetchClaudeCodeModels(CatalogOptions options) {
    std::shared_ptr<rt::Kv> kv;
    if (!options.kv) {
      const rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      kv = co_await opening.wait();
    } else {
      kv = *options.kv;
    }
    rt::Promise<std::optional<Binding>> resolving(bindingOrNull(kv, options.env, options.accountBinding));
    const std::optional<Binding> current = co_await resolving.wait();
    if (!current) {
      if (!options.accountBinding) deleteScope(kv);
      co_return std::nullopt;
    }
    if (const auto& expected = options.expectedBinding;
        expected && (current->credential.kind != expected->credential.kind || current->credential.secret != expected->credential.secret ||
                     current->workspaceId != expected->workspaceId))
      co_return std::nullopt;
    std::shared_ptr<Scoped> scoped = scopeOf(kv);
    if (!scoped) scoped = std::make_shared<Scoped>();
    setScope(kv, scoped);
    std::shared_ptr<Cache> cached;
    if (const auto found = scoped->find(current->cacheKey); found != scoped->end()) cached = found->second;
    if (!cached || cached->identity != current->identity) {
      cached = std::make_shared<Cache>(Cache{.identity = current->identity, .snapshot = std::nullopt, .expires = 0, .pending = std::nullopt});
      (*scoped)[current->cacheKey] = cached;
      scoped->reportMemory();
    }
    const std::shared_ptr<Cache> state = cached;
    const double now = options.now ? options.now() : rt::dateNow();
    if (!options.force.value_or(false) && state->expires > now) co_return state->snapshot;
    if (state->pending) {
      const rt::Promise<std::optional<ClaudeCodeModelsSnapshot>> shared = *state->pending;
      std::optional<ClaudeCodeModelsSnapshot> result = co_await shared.wait();
      co_return result;
    }
    const rt::Promise<std::optional<ClaudeCodeModelsSnapshot>> pending(refresh(state, *current, kv, options, now));
    state->pending = pending;
    rt::Reason failure;
    std::optional<ClaudeCodeModelsSnapshot> result;
    try {
      result = co_await pending.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    state->pending = std::nullopt;
    if (failure) std::rethrow_exception(failure);
    co_return result;
  }
  rt::Task<std::optional<ClaudeCodeModelsSnapshot>> fetchClaudeCodeModels() { return fetchClaudeCodeModels(CatalogOptions()); }

  rt::Task<void> cacheValidatedClaudeCodeModels(std::shared_ptr<rt::Kv> kv, std::string secret, std::optional<std::string> workspaceId,
                                                ClaudeCodeModelsSnapshot snapshot, claude_code::ClaudeCodeEnvLookup env,
                                                std::optional<ClaudeCodeAccountBinding> accountBinding) {
    rt::Promise<std::optional<Binding>> resolving(bindingOrNull(kv, std::move(env), std::move(accountBinding)));
    const std::optional<Binding> current = co_await resolving.wait();
    if (!current || current->credential.secret != secret || current->workspaceId != workspaceId ||
        current->generation != claude_code_credentials::claudeCodeStateGeneration(kv))
      co_return;
    std::shared_ptr<Scoped> scoped = caches()->byKv.get(*kv).value_or(nullptr);
    if (!scoped) scoped = std::make_shared<Scoped>();
    (*scoped)[current->cacheKey] =
      std::make_shared<Cache>(Cache{.identity = current->identity, .snapshot = std::move(snapshot), .expires = rt::dateNow() + CACHE_TTL_MS, .pending = std::nullopt});
    scoped->reportMemory();
    caches()->byKv.set(*kv, scoped);
  }
  rt::Task<void> cacheValidatedClaudeCodeModels(std::shared_ptr<rt::Kv> kv, std::string secret, std::optional<std::string> workspaceId,
                                                ClaudeCodeModelsSnapshot snapshot, claude_code::ClaudeCodeEnvLookup env) {
    return cacheValidatedClaudeCodeModels(std::move(kv), std::move(secret), std::move(workspaceId), std::move(snapshot), std::move(env), std::nullopt);
  }
  rt::Task<void> cacheValidatedClaudeCodeModels(std::shared_ptr<rt::Kv> kv, std::string secret, std::optional<std::string> workspaceId,
                                                ClaudeCodeModelsSnapshot snapshot) {
    return cacheValidatedClaudeCodeModels(std::move(kv), std::move(secret), std::move(workspaceId), std::move(snapshot), claude_code::ClaudeCodeEnvLookup(),
                                          std::nullopt);
  }

  void resetClaudeCodeModelsCacheForTest() { caches() = std::make_shared<Caches>(); }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeModel& value) {
    out = Writer()
            .required("id", value.id)
            .orUndefined("display_name", value.display_name)
            .required("created", value.created)
            .orUndefined("context_window_tokens", value.context_window_tokens)
            .orUndefined("max_output_tokens", value.max_output_tokens)
            .orUndefined("supports_disabled_thinking", value.supports_disabled_thinking)
            .orUndefined("supports_adaptive_thinking", value.supports_adaptive_thinking)
            .orUndefined("supported_reasoning_levels", value.supported_reasoning_levels)
            .orUndefined("supports_image_input", value.supports_image_input)
            .orUndefined("supports_pdf_input", value.supports_pdf_input)
            .done();
  }
  ClaudeCodeModel tag_invoke(const json::value_to_tag<ClaudeCodeModel>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeModel");
    return {
      .id = in.required<std::string>("id"),
      .display_name = in.orUndefined<std::string>("display_name"),
      .created = in.required<double>("created"),
      .context_window_tokens = in.orUndefined<double>("context_window_tokens"),
      .max_output_tokens = in.orUndefined<double>("max_output_tokens"),
      .supports_disabled_thinking = in.orUndefined<bool>("supports_disabled_thinking"),
      .supports_adaptive_thinking = in.orUndefined<bool>("supports_adaptive_thinking"),
      .supported_reasoning_levels = in.orUndefined<std::vector<std::string>>("supported_reasoning_levels"),
      .supports_image_input = in.orUndefined<bool>("supports_image_input"),
      .supports_pdf_input = in.orUndefined<bool>("supports_pdf_input"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeModelsSnapshot& value) {
    out = Writer().required("models", value.models).required("updated_at_ms", value.updated_at_ms).orUndefined("stale", value.stale).done();
  }
  ClaudeCodeModelsSnapshot tag_invoke(const json::value_to_tag<ClaudeCodeModelsSnapshot>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeModelsSnapshot");
    return {
      .models = in.required<std::vector<ClaudeCodeModel>>("models"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
      .stale = in.orUndefined<bool>("stale"),
    };
  }
}
