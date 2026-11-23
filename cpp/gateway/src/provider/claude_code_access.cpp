#include "uos/provider/claude_code_access.hpp"
#include "uos/provider/claude_code.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uos::provider::claude_code_access {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    std::optional<ClaudeCodeAccessPolicy> parsePolicy(const rt::KvValue& stored) {
      const auto value = rt::storedJsonView(stored);
      if (!utils::isRecord(value) || rt::getNumber(value, "v") != 1) return std::nullopt;
      const auto source = rt::getString(value, "source");
      if (!source || (*source != "env" && *source != "kv")) return std::nullopt;
      const auto ids = parseClaudeCodeAccessIds(rt::get(value, "allowed_key_ids"));
      const auto updated = rt::getNumber(value, "updated_at_ms");
      if (!ids || !updated || !rt::Number::isSafeInteger(*updated) || *updated <= 0) return std::nullopt;
      return ClaudeCodeAccessPolicy{1, std::string(*source), *ids, *updated};
    }
    rt::Task<ClaudeCodeAccessPolicy> storePolicy(rt::Kv& kv, json::value allowedKeyIds) {
      const auto ids = parseClaudeCodeAccessIds(allowedKeyIds);
      if (!ids) throw rt::JsError("Claude principal IDs are invalid");
      const ClaudeCodeAccessPolicy policy{1, "kv", *ids, rt::dateNow()};
      const rt::KvValue stored(json::value_from(policy));
      rt::Promise<rt::KvCommitResult> writing(kv.set(CLAUDE_CODE_ACCESS_KEY, stored));
      auto ignored = co_await writing.wait();
      (void)ignored;
      co_return policy;
    }
  }
  std::optional<std::vector<std::string>> parseClaudeCodeAccessIds(const json::value* value) {
    if (!value || !value->is_array() || value->as_array().size() > 1'000) return std::nullopt;
    static const rt::Regex SEPARATOR(R"([\s,])");
    rt::OrderedSet<std::string> ids;
    for (const auto& id : value->as_array()) {
      if (!id.is_string()) return std::nullopt;
      const std::string text(id.as_string());
      if (rt::trim(text).empty() || rt::length(text) > 256 || SEPARATOR.test(text)) return std::nullopt;
      for (const auto character : rt::codePoints(text)) if (character < 32 || character == 127) return std::nullopt;
      ids.add(text);
    }
    auto result = rt::arrayFrom(ids.values());
    rt::sort(result, [](const auto& left, const auto& right) { return rt::localeCompare(left, right); });
    return result;
  }
  std::optional<std::vector<std::string>> parseClaudeCodeAccessIds(const json::value& value) { return parseClaudeCodeAccessIds(&value); }
  rt::Task<ClaudeCodeAccessPolicy> loadClaudeCodeAccessPolicy(std::shared_ptr<rt::Kv> kv) { return loadClaudeCodeAccessPolicy(std::move(kv), {}); }
  rt::Task<ClaudeCodeAccessPolicy> loadClaudeCodeAccessPolicy(std::shared_ptr<rt::Kv> kv, claude_code::ClaudeCodeEnvLookup env) {
    if (!kv) throw rt::JsError("Claude principal policy storage is unavailable");
    for (int attempt = 0; attempt < 8; ++attempt) {
      const rt::KvGetOptions options{.consistency = rt::KvConsistencyLevel::strong};
      rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(CLAUDE_CODE_ACCESS_KEY, options));
      const auto entry = co_await reading.wait();
      if (!entry.value.isNull()) {
        const auto policy = parsePolicy(entry.value);
        if (!policy) throw rt::JsError("Claude principal policy is invalid; refusing environment fallback");
        co_return *policy;
      }
      const auto ids = claude_code::readClaudeCodeAllowedKeyIds(env);
      auto allowed = rt::arrayFrom(ids.values());
      rt::sort(allowed, [](const auto& left, const auto& right) { return rt::localeCompare(left, right); });
      const ClaudeCodeAccessPolicy policy{1, "env", allowed, rt::dateNow()};
      auto operation = kv->atomic();
      operation->check(entry).set(CLAUDE_CODE_ACCESS_KEY, rt::KvValue(json::value_from(policy)));
      rt::Promise<rt::KvCommitResult> committing(operation->commit());
      const auto committed = co_await committing.wait();
      if (committed.ok) co_return policy;
    }
    throw rt::JsError("Claude principal policy changed repeatedly");
  }
  rt::Task<ClaudeCodeAccessPolicy> storeClaudeCodeAccessPolicy(rt::Kv& kv, const std::vector<std::string>& allowedKeyIds) {
    return storePolicy(kv, json::value_from(allowedKeyIds));
  }
  rt::Task<ClaudeCodeAccessPolicy> storeClaudeCodeAccessPolicy(rt::Kv& kv, const json::value& allowedKeyIds) { return storePolicy(kv, allowedKeyIds); }
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeAccessPolicy& value) {
    out = Writer().required("v", value.v).required("source", value.source).required("allowed_key_ids", value.allowed_key_ids).required("updated_at_ms", value.updated_at_ms).done();
  }
  ClaudeCodeAccessPolicy tag_invoke(const json::value_to_tag<ClaudeCodeAccessPolicy>&, const json::value& value) {
    const Reader in(value, "ClaudeCodeAccessPolicy");
    return {in.required<double>("v"), in.required<std::string>("source"), in.required<std::vector<std::string>>("allowed_key_ids"), in.required<double>("updated_at_ms")};
  }
}
