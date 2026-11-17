#pragma once
// Port of tests/helpers/manual-reset-harness.ts.
#include "tests/helpers/codex_banked_reset_harness.hpp"
#include "uos/codex/manual_reset.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/authority/native_codex_contracts.hpp"
#include "uos/config.hpp"
#include "uos/rt/env.hpp"
#include <memory>
#include <string>
#include <vector>
namespace uos::tests::helpers::manual_reset_harness {
  namespace banked = ::uos::tests::helpers::codex_banked_reset_harness;
  class ManualKv : public banked::MemoryKv {
  public:
    using banked::MemoryKv::getMany;
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions options = {}) override;
  };
  struct ManualFixture {
    std::shared_ptr<ManualKv> kv;
    double now;
    ::uos::types::CodexAuthPoolState pool;
    std::string targetHash;
    std::vector<codex::capacity_routing::CodexRoutingAccountIdentity> identities;
    json::object active;
    std::shared_ptr<banked::FakeCodexUsageResetProvider> provider;
    codex::manual_reset::ManualResetDependencies dependencies;
    std::shared_ptr<double> usageCallCount;
    std::shared_ptr<bool> restored;
    double usageCalls() const { return *usageCallCount; }
    void restore(bool value) { *restored = value; }
  };
  rt::Task<std::shared_ptr<ManualFixture>> manualFixture();
  void cleanupManualFixture();
  struct CredentialScope {
    rt::env::ScopedEnvironment environment{{{"HOME", "/nonexistent/w05c7-isolated"}}};
    std::string saved = ::uos::config::config.codexAuthJsonB64;
    CredentialScope() { ::uos::config::config.codexAuthJsonB64.clear(); }
    ~CredentialScope() { cleanupManualFixture(); ::uos::config::config.codexAuthJsonB64 = saved; }
  };
}
