#pragma once
// Port of src/authority/native-authority-codex.ts.
#include "uos/authority/native_authority_api.hpp"
#include "uos/authority/native_codex.hpp"
#include "uos/authority/native_codex_429.hpp"

namespace uos::authority::native_authority_codex {
  class NativeAuthorityCodex {
    native_authority_sql::NativeAuthoritySql& _sql;
    native_authority_api::NativeAuthorityApi& _api;
    struct Rows {
      native_authority_contracts::NativePool pool;
      std::string poolRevision;
      native_authority_contracts::NativeRead<json::value> active;
      native_authority_contracts::NativeRead<json::value> routing;
      native_authority_contracts::NativeRead<json::value> capacity;
      std::optional<native_codex_contracts::CodexAccountRoutingState> parsed;
      native_codex_contracts::CodexAccountRoutingState normalized;
      native_codex_contracts::CodexRoutingRowRevisions revisions;
    };
    rt::OrderedMap<std::string, native_codex_contracts::CodexRoutingAccountPolicy> _policies(const native_authority_contracts::NativePool& pool, const std::string& model) const;
    Rows _rows(double now) const;
    native_authority_contracts::NativeOwner _owner(const native_codex_contracts::CodexRoutingAccount& account, double now) const;
  public:
    NativeAuthorityCodex(native_authority_sql::NativeAuthoritySql& sql, native_authority_api::NativeAuthorityApi& api) : _sql(sql), _api(api) {}
    native_authority_contracts::NativeCodexAuthorization authorization(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now) const;
    void fence(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now, const std::optional<native_authority_contracts::NativeCodexAuthorization>& authorization = std::nullopt) const;
    native_authority_contracts::NativeReply select(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now);
    native_authority_contracts::NativeReply signal(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::recordCodexAttemptSignal& args, const native_codex_429::Codex429Classification* parsed, double now);
    native_authority_contracts::NativeReply release(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now);
    native_authority_contracts::NativeReply refresh(const native_authority_contracts::NativeCapability& cap, const std::string& handle, double now);
    native_authority_contracts::NativeReply elect(const native_authority_contracts::NativeCapability& cap, const native_authority_contracts::NativeArguments::electCodexResetProbe& args, double now);
  };
}
