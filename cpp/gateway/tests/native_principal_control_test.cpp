// Existing RPC/DTO assertions are retained. Direct module traces compare complete SQLite state with Bun.
// Original assembly/RPC/private-control integration traces compare real SQLite state and revisions with Bun.
// Links: src/release.cpp src/bounded_response_body.cpp tests/native_principal_control_test.cpp src/authority/native_authority_rpc.cpp src/authority/native_authority_contracts.cpp
// src/authority/native_principal_contracts.cpp src/authority/native_runtime_contracts.cpp src/authority/native_api_key_audit.cpp
// src/authority/native_contracts.cpp src/authority/native_kernel_contracts.cpp src/authority/native_codex_contracts.cpp
// src/authority/native_paid_contracts.cpp src/types.cpp src/utils.cpp
// src/api_keys.cpp src/config.cpp src/authority/native_authority_sql.cpp
// src/authority/native_authority_api.cpp src/authority/native_authority_paid.cpp src/authority/native_authority_runtime.cpp
// src/authority/native_authority_codex.cpp src/authority/native_authority_kernel.cpp src/authority/native_authority_principals.cpp
// src/authority/native_api_key.cpp src/authority/native_api_key_policy.cpp src/authority/native_paid.cpp
// src/authority/native_paid_policy.cpp src/authority/native_kernel.cpp src/authority/native_codex.cpp
// src/authority/native_codex_capacity.cpp src/authority/native_codex_mutations.cpp src/authority/native_codex_evaluation.cpp
// src/authority/native_codex_429.cpp
// src/authority/native_authority.cpp
#include "uos/authority/native_authority_rpc.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <string>
#include <vector>
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace { namespace rt = uos::rt; namespace json = uos::json; namespace rpc = uos::authority::native_authority_rpc; namespace principal = uos::authority::native_principal_contracts; namespace contracts = uos::authority::native_authority_contracts; }
TEST("private control accepts exactly six intents and refuses caller identity or scope assertions") {
  const json::object command{{"epoch", "synthetic-epoch"}, {"operationId", "operation"}, {"method", "resolvePrincipal"}, {"args", json::object()}};
  EXPECT(rt::deepEqual(json::value_from(rpc::parseNativeControlCommand(command)), command));
  const std::vector<std::string> assertions{"principalId", "keyId", "targetKeyId", "tokenHash", "actor", "role", "verified", "now", "revision", "grantId"};
  for (const auto& assertion : assertions) { auto forged = command; forged["args"] = json::object{{assertion, "caller"}}; EXPECT_THROWS_MATCHING(rpc::parseNativeControlCommand(forged), "invalid_control_arguments"); }
  EXPECT_THROWS_MATCHING(rpc::parseNativeCommand(command), "invalid_authority_operation");
  const std::vector<std::string> createAssertions{"id", "token", "hash", "policy", "clock", "actor", "release"}; for (const auto& assertion : createAssertions) { auto forged = command; forged["method"] = "createKey"; forged["args"] = json::object{{"input", json::object{{"name", "fixture"}, {assertion, "caller"}}}}; EXPECT_THROWS_MATCHING(rpc::parseNativeControlCommand(forged), "invalid_control_arguments"); }
}
ASYNC_TEST("private control RPC construction clones its binding and preserves server errors") {
  auto bindings = std::make_shared<std::vector<principal::NativeControlBinding>>();
  const principal::NativePrincipalControlRunner runner{[bindings](principal::NativeControlBinding binding, principal::NativeControlCommand command) { bindings->push_back(binding); if (command.epoch != "e") throw contracts::NativeAuthorityError("authority_epoch_mismatch", 403); return rt::fulfilledTask<principal::NativeControlReply>(principal::NativeControlOutcomeApplied{"key", json::object{{"source", "native"}}}); }};
  principal::NativeControlBinding binding{"grant", "context", json::object{{"bound", "original"}}}; rpc::NativePrincipalControlServer server(runner, binding); binding.privateContext.as_object()["bound"] = "changed";
  rpc::NativePrincipalControlClient client(server.transport(), "e"); const json::object args; AWAIT(reply, client.call("resolvePrincipal", args, "op")); EXPECT_EQ(rt::getString(reply, "kind"), std::optional<std::string_view>("applied")); ASSERT_EQ(bindings->size(), std::size_t(1)); EXPECT_EQ(rt::getString(bindings->at(0).privateContext, "bound"), std::optional<std::string_view>("original"));
  const json::object wrong{{"epoch", "wrong"}, {"operationId", "op"}, {"method", "resolvePrincipal"}, {"args", args}}; AWAIT(failed, server.control(wrong)); EXPECT_EQ(std::get<rpc::NativeRpcResultError>(failed).error.code, "authority_epoch_mismatch");
}
ASYNC_TEST("private control client preserves an intentionally unvalidated reply") {
  const rpc::NativeControlRpcTransport transport{[](json::value) { return rt::fulfilledTask<rpc::NativeRpcResult>(rpc::NativeRpcResultResult{nullptr}); }}; rpc::NativePrincipalControlClient client(transport, "e"); const json::object args; AWAIT(reply, client.call("resolvePrincipal", args, "op")); EXPECT(reply.is_null());
}
#include "tests/helpers/native_authority_harness.hpp"
namespace { const bool nativeAuthorityScenarioRegistration = uos::tests::helpers::native_authority_harness::registerScenarios("principal"); }

#include "tests/helpers/native_authority_assembly_harness.hpp"
namespace { const bool nativeAuthorityAssemblyRegistration = uos::tests::helpers::native_authority_assembly_harness::registerScenarios("principal"); }
TEST("concurrent revision comparator rejects semantic, CAS, ordering and write-count defects") {
  namespace assembly = uos::tests::helpers::native_authority_assembly_harness;
  const auto state = [](const std::string& revision, double sequence = 50) -> json::value {
    const json::object row{{"row_key", "owned-row"}, {"revision", revision}, {"row_json", json::object{{"quota", 40}, {"state", "reserved"}, {"expectedRevision", revision}, {"requestId", "caller-supplied"}}}, {"expires_at_ms", 1000}, {"scope_key", "caller-principal"}, {"parent_key", "caller-request"}, {"due_at_ms", nullptr}};
    return json::object{{"1", json::object{{"meta", json::array{json::object{{"sequence", rt::makeNumber(sequence)}}}}, {"rows", json::object{{"native_fixture", json::array{row}}}}}}};
  };
  const auto expected = state("r00000000000000000010"), actual = state("r00000000000000000020");
  assembly::ConcurrentRevisionComparison comparator; comparator.compare(actual, expected);
  EXPECT(rt::deepEqual(comparator.normalize(actual), expected));
  const auto mutate = [](json::value input, const std::string& key, json::value value) { input.as_object().at("1").as_object().at("rows").as_object().at("native_fixture").as_array().front().as_object().at("row_json").as_object()[key] = std::move(value); return input; };
  EXPECT_THROWS_MATCHING(comparator.compare(mutate(actual, "quota", 41), expected), "semantic/CAS state differs");
  EXPECT_THROWS_MATCHING(comparator.compare(mutate(actual, "state", "settled"), expected), "semantic/CAS state differs");
  EXPECT_THROWS_MATCHING(comparator.compare(mutate(actual, "expectedRevision", "r00000000000000000021"), expected), "semantic/CAS state differs");
  EXPECT_THROWS_MATCHING(comparator.compare(mutate(actual, "requestId", "r00000000000000000020"), mutate(expected, "requestId", "r00000000000000000010")), "semantic/CAS state differs");
  EXPECT_THROWS_MATCHING(comparator.compare(state("r00000000000000000020", 49), expected), "write count/metadata differs");
  EXPECT_THROWS_MATCHING(comparator.compare(state("r00000000000000000020", 51), expected), "write count/metadata differs");
  EXPECT_THROWS_MATCHING(comparator.compare(state("r00000000000000000019"), state("r00000000000000000011")), "revision ordering differs");
  auto twoExpected = expected, twoActual = actual;
  auto otherExpected = twoExpected.as_object().at("1").as_object().at("rows").as_object().at("native_fixture").as_array().front(); otherExpected.as_object()["row_key"] = "other-row"; otherExpected.as_object()["revision"] = "r00000000000000000011";
  auto otherActual = twoActual.as_object().at("1").as_object().at("rows").as_object().at("native_fixture").as_array().front(); otherActual.as_object()["row_key"] = "other-row";
  twoExpected.as_object().at("1").as_object().at("rows").as_object().at("native_fixture").as_array().push_back(otherExpected);
  twoActual.as_object().at("1").as_object().at("rows").as_object().at("native_fixture").as_array().push_back(otherActual);
  EXPECT_THROWS_MATCHING(comparator.compare(actual, twoExpected), "missing/extra concurrent row write");
  EXPECT_THROWS_MATCHING(comparator.compare(twoActual, twoExpected), "noninjective generated revision mapping");
  comparator.compare(actual, expected);
}
