// Existing RPC/DTO assertions are retained. Direct module traces compare complete SQLite state with Bun.
// Original assembly/RPC/private-control integration traces compare real SQLite state and revisions with Bun.
// Links: src/authority/native_authority_rpc.cpp src/release.cpp src/bounded_response_body.cpp tests/native_runtime_binding_test.cpp src/authority/native_authority_contracts.cpp src/authority/native_runtime_contracts.cpp
// src/authority/native_principal_contracts.cpp src/authority/native_api_key_audit.cpp src/authority/native_contracts.cpp
// src/authority/native_kernel_contracts.cpp src/authority/native_codex_contracts.cpp src/authority/native_paid_contracts.cpp
// src/types.cpp src/utils.cpp src/api_keys.cpp
// src/config.cpp src/authority/native_authority_sql.cpp src/authority/native_authority_api.cpp
// src/authority/native_authority_paid.cpp src/authority/native_authority_runtime.cpp src/authority/native_authority_codex.cpp
// src/authority/native_authority_kernel.cpp src/authority/native_authority_principals.cpp src/authority/native_api_key.cpp
// src/authority/native_api_key_policy.cpp src/authority/native_paid.cpp src/authority/native_paid_policy.cpp
// src/authority/native_kernel.cpp src/authority/native_codex.cpp src/authority/native_codex_capacity.cpp
// src/authority/native_codex_mutations.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_429.cpp
// src/authority/native_authority.cpp
#include "uos/authority/native_runtime_contracts.hpp"
#include "uos/authority/native_authority_contracts.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

namespace { namespace rt = uos::rt; namespace json = uos::json; namespace runtime = uos::authority::native_runtime_contracts; namespace authority = uos::authority::native_authority_contracts; }
TEST("native runtime revision and retained cleanup method surface preserve source contracts") {
  EXPECT_EQ(runtime::NATIVE_RUNTIME_REVISION, 3); EXPECT_EQ(runtime::NATIVE_RETAINED_METHODS.size(), std::size_t(7)); EXPECT_EQ(runtime::NATIVE_RETAINED_METHODS.front(), "releaseApiUndispatched"); EXPECT_EQ(runtime::NATIVE_RETAINED_METHODS.back(), "finishPaidReconciliation");
  const runtime::NativeRuntimeGrant grant{"id", 3, "epoch", "subject", "context", {"inference", "transport"}, 1000, false}; const auto encoded = json::value_from(grant); EXPECT(rt::deepEqual(json::value_from(json::value_to<runtime::NativeRuntimeGrant>(encoded)), encoded));
  const runtime::NativeRetainedHistoryScope scope{"ns", "binding", "principal", "key", {"handle"}}; const auto scopeJson = json::value_from(scope); EXPECT_EQ(rt::getString(scopeJson, "namespace"), std::optional<std::string_view>("ns")); EXPECT(!rt::get(scopeJson, "namespace_"));
}
TEST("native runtime identity unions preserve every source authentication family") {
  const std::vector<runtime::NativeRuntimeIdentity> identities{runtime::NativeRuntimeIdentityKvApiKey{"k"}, runtime::NativeRuntimeIdentityLocalDevelopmentKey{"k"}, runtime::NativeRuntimeIdentityGithubToken{"owner", "repo"}, runtime::NativeRuntimeIdentityPasskeySession{"u"}, runtime::NativeRuntimeIdentityAuthTokensAllowlist{}, runtime::NativeRuntimeIdentityAdminAllowlist{}, runtime::NativeRuntimeIdentityDenoDeployToken{}, runtime::NativeRuntimeIdentityDisabledLocal{std::nullopt}};
  for (const auto& identity : identities) { const auto encoded = json::value_from(identity); const auto restored = json::value_to<runtime::NativeRuntimeIdentity>(encoded); EXPECT(rt::deepEqual(json::value_from(restored), encoded)); }
}
TEST("native authority helpers refuse absent API and kernel scope") {
  authority::NativeCapability capability{"id", "e", "principal", std::nullopt, std::nullopt, std::nullopt, 3, "grant", "context", "subject", std::nullopt, std::nullopt, {"inference"}, false};
  EXPECT_THROWS_MATCHING(authority::nativeApiKeyId(capability), "api_scope_not_applicable"); EXPECT_THROWS_MATCHING(authority::nativeIdentity("repo", capability), "kernel_scope_not_applicable"); capability.keyId = "key"; capability.kernel = authority::NativeKernelBinding{"owner", "repo"}; EXPECT_EQ(authority::nativeApiKeyId(capability), "key"); const auto org = authority::nativeIdentity("org", capability), repo = authority::nativeIdentity("repo", capability); EXPECT(!org.repo); EXPECT_EQ(repo.repo, std::optional<std::string>("repo")); EXPECT_EQ(org.owner, "owner");
}
#include "tests/helpers/native_authority_harness.hpp"
namespace { const bool nativeAuthorityScenarioRegistration = uos::tests::helpers::native_authority_harness::registerScenarios("runtime"); }

#include "tests/helpers/native_authority_assembly_harness.hpp"
namespace { const bool nativeAuthorityAssemblyRegistration = uos::tests::helpers::native_authority_assembly_harness::registerScenarios("runtime"); }
