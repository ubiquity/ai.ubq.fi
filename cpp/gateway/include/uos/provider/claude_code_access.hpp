#pragma once
// Port of src/provider/claude-code-access.ts.
#include "uos/provider/claude_code_fwd.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::provider::claude_code_access {
  inline const rt::KvKey CLAUDE_CODE_ACCESS_KEY{"uos_ai", "claude_code_access", "v1"};
  struct ClaudeCodeAccessPolicy {
    double v;
    std::string source;
    std::vector<std::string> allowed_key_ids;
    double updated_at_ms;
    friend bool operator==(const ClaudeCodeAccessPolicy&, const ClaudeCodeAccessPolicy&) = default;
  };
  std::optional<std::vector<std::string>> parseClaudeCodeAccessIds(const json::value* value);
  std::optional<std::vector<std::string>> parseClaudeCodeAccessIds(const json::value& value);
  rt::Task<ClaudeCodeAccessPolicy> loadClaudeCodeAccessPolicy(std::shared_ptr<rt::Kv> kv);
  rt::Task<ClaudeCodeAccessPolicy> loadClaudeCodeAccessPolicy(std::shared_ptr<rt::Kv> kv, claude_code::ClaudeCodeEnvLookup env);
  rt::Task<ClaudeCodeAccessPolicy> storeClaudeCodeAccessPolicy(rt::Kv& kv, const std::vector<std::string>& allowedKeyIds);
  rt::Task<ClaudeCodeAccessPolicy> storeClaudeCodeAccessPolicy(rt::Kv& kv, const json::value& allowedKeyIds);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeAccessPolicy& value);
  ClaudeCodeAccessPolicy tag_invoke(const json::value_to_tag<ClaudeCodeAccessPolicy>&, const json::value& value);
}
