// Existing RPC/DTO assertions are retained. Direct module traces compare complete SQLite state with Bun.
// Original assembly/RPC/private-control integration traces compare real SQLite state and revisions with Bun.
// Links: src/release.cpp src/bounded_response_body.cpp tests/native_authority_test.cpp src/authority/native_authority_rpc.cpp src/authority/native_authority_contracts.cpp
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
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

#include <limits>
#include <memory>
#include <string>
#include <vector>

#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  namespace rt = uos::rt; namespace json = uos::json; namespace rpc = uos::authority::native_authority_rpc; namespace contracts = uos::authority::native_authority_contracts; namespace runtime = uos::authority::native_runtime_contracts;
  const json::value& vectors() { static const auto value = rt::jsonParse(
#include "tests/native_authority_vectors.inc"
  ); return value; }
  json::value command(const std::string& method, json::object args = {}) { return json::object{{"epoch", "e"}, {"operationId", "o"}, {"method", method}, {"args", args}}; }
}
TEST("native authority RPC differential: every method and closed argument shape") {
  std::size_t count = 0;
  for (const auto& row : *rt::getArray(vectors(), "commands")) {
    const auto& input = *rt::get(row, "input"); const bool control = rt::getBool(row, "control").value_or(false); json::value actualError = nullptr, parsed;
    try { parsed = control ? json::value_from(rpc::parseNativeControlCommand(input)) : json::value_from(rpc::parseNativeCommand(input)); }
    catch (const contracts::NativeAuthorityError& error) { actualError = json::object{{"code", error.code}, {"status", rt::makeNumber(error.status)}}; }
    const auto* expectedError = rt::get(row, "error");
    if (expectedError) { if (!rt::deepEqual(actualError, *expectedError)) std::fprintf(stderr, "      vector %zu: actual=%s expected=%s\n", count, rt::jsonStringify(actualError).c_str(), rt::jsonStringify(*expectedError).c_str()); EXPECT(rt::deepEqual(actualError, *expectedError)); }
    else { EXPECT(actualError.is_null()); EXPECT(rt::deepEqual(parsed, *rt::get(row, "value"))); EXPECT_EQ(rpc::nativeCommandBytes(parsed), std::string(*rt::getString(row, "bytes"))); }
    ++count;
  }
  EXPECT_EQ(count, std::size_t(321)); EXPECT(rt::deepEqual(rpc::NATIVE_METHOD_ROLES, *rt::get(vectors(), "roles")));
}
TEST("native authority RPC canonical bytes use JS property order and locale sorting") {
  for (const auto& row : *rt::getArray(vectors(), "bytes")) EXPECT_EQ(rpc::nativeCommandBytes(*rt::get(row, "input")), std::string(*rt::getString(row, "bytes")));
  const json::value nan(rt::makeNumber(std::numeric_limits<double>::quiet_NaN())); EXPECT_THROWS_MATCHING(rpc::nativeCommandBytes(nan), "invalid_command_value"); EXPECT_THROWS_MATCHING(rpc::nativeCommandBytes(static_cast<const json::value*>(nullptr)), "invalid_command_value");
}
ASYNC_TEST("native authority service: closed RPC rejects generic operations, extra trust fields and cross-principal arguments") {
  const rpc::NativeOperationRunner runner{[](runtime::NativeRuntimeBinding) {
    rpc::NativeBoundRuntimePort port;
    port.execute = [](contracts::NativeCommand value) { if (value.epoch != "e") throw contracts::NativeAuthorityError("authority_epoch_mismatch", 403); return rt::fulfilledTask(contracts::NativeReply{{"kind", "opened"}, {"extra", "preserved"}}); };
    port.bindTrustedRoutingDecision = [](std::string handle, std::string) { return rt::fulfilledTask(contracts::NativeReply{{"kind", "bound"}, {"handle", handle}}); }; return port;
  }};
  const runtime::NativeRuntimeBinding binding{"g", "c", nullptr}; rpc::NativeAuthorityRpcServer server(runner, binding);
  const std::vector<std::string> methods{"get", "set", "list", "compareAndSwap", "seed", "snapshot", "publishAccess"};
  for (const auto& method : methods) { AWAIT(reply, server.execute(command(method))); const auto* failure = std::get_if<rpc::NativeRpcResultError>(&reply); ASSERT(failure); EXPECT_EQ(failure->error.code, "invalid_authority_operation"); EXPECT_EQ(failure->error.status, 400); }
  const json::object args{{"requestId", "bad"}, {"route", "/v1/responses"}, {"path", "/v1/responses"}};
  const std::vector<std::string> fields{"namespace", "policyVersion", "policyCheck", "account", "credentialVersion", "providerUrl", "tokenHash", "principalId"};
  for (const auto& field : fields) { auto forged = args; forged[field] = "forged"; AWAIT(reply, server.execute(command("openAuthenticatedRequest", forged))); const auto* failure = std::get_if<rpc::NativeRpcResultError>(&reply); ASSERT(failure); EXPECT_EQ(failure->error.code, "invalid_authority_arguments"); }
  auto wrong = command("openAuthenticatedRequest", args); wrong.as_object()["epoch"] = "wrong"; AWAIT(reply, server.execute(wrong)); EXPECT_EQ(std::get<rpc::NativeRpcResultError>(reply).error.code, "authority_epoch_mismatch");
  rpc::NativeAuthorityClient client(server.transport(), "e"); AWAIT(opened, client.call("openAuthenticatedRequest", args, "operation")); EXPECT_EQ(rt::getString(opened, "extra"), std::optional<std::string_view>("preserved"));
  AWAIT(bound, client.bindTrustedRoutingDecision("h", "op")); EXPECT_EQ(rt::getString(bound, "kind"), std::optional<std::string_view>("bound")); AWAIT(invalid, server.bindTrustedRoutingDecision("", "op")); EXPECT_EQ(std::get<rpc::NativeRpcResultError>(invalid).error.code, "invalid_private_routing_binding");
}
ASYNC_TEST("native authority clients preserve error mapping and reject malformed ordinary replies") {
  const rpc::NativeRpcTransport malformed{[](json::value) { return rt::fulfilledTask<rpc::NativeRpcResult>(rpc::NativeRpcResultResult{nullptr}); }, {}};
  rpc::NativeAuthorityClient client(malformed, "e"); const json::object args;
  auto call = client.call("beginPaidKeyDeletion", args, "operation"); EXPECT_REJECTS_MESSAGE(std::move(call), contracts::NativeAuthorityError, "invalid_authority_reply");
  auto unavailable = client.bindTrustedRoutingDecision("h", "op"); EXPECT_REJECTS_MESSAGE(std::move(unavailable), contracts::NativeAuthorityError, "private_routing_port_unavailable");
  const rpc::NativeRpcTransport failed{[](json::value) { return rt::fulfilledTask<rpc::NativeRpcResult>(rpc::NativeRpcResultError{{"denied", 403}}); }, [](std::string, std::string) { return rt::fulfilledTask<rpc::NativeRpcResult>(rpc::NativeRpcResultResult{nullptr}); }};
  rpc::NativeAuthorityClient failedClient(failed, "e"); auto denied = failedClient.call("beginPaidKeyDeletion", args, "operation"); EXPECT_REJECTS_MESSAGE(std::move(denied), contracts::NativeAuthorityError, "denied"); AWAIT(unchecked, failedClient.bindTrustedRoutingDecision("h", "op")); EXPECT(unchecked.is_null());
}
ASYNC_TEST("private retained-history construction binds a distinct channel without a public mode") {
  auto grant = std::make_shared<std::string>(); const rpc::NativeRetainedHistoryRunner runner{[grant](runtime::NativeRetainedHistoryBinding binding) { *grant = binding.grantId; return rpc::NativeRetainedHistoryPort{[](contracts::NativeCommand value) { return rt::fulfilledTask(contracts::NativeReply{{"kind", "retained"}, {"method", value.method}}); }}; }};
  const runtime::NativeRetainedHistoryBinding binding{"retained_history", "grant", "context", nullptr}; rpc::NativeRetainedHistoryServer server(runner, binding); EXPECT_EQ(*grant, "grant"); EXPECT(!server.transport().bindTrustedRoutingDecision); AWAIT(reply, server.execute(command("releaseApiUndispatched", json::object{{"handle", "h"}, {"reason", "failed"}}))); EXPECT_EQ(rt::getString(std::get<rpc::NativeRpcResultResult>(reply).result, "kind"), std::optional<std::string_view>("retained"));
}
#include "tests/helpers/native_authority_harness.hpp"
namespace { const bool nativeAuthorityScenarioRegistration = uos::tests::helpers::native_authority_harness::registerScenarios("authority"); }
TEST("native authority nativeKey preserves complete Bun numeric, bytes, bigint and Unicode vectors") {
  const auto& metadata = uos::tests::helpers::native_authority_harness::vectors().as_array().back();
  std::size_t count = 0;
  for (const auto& vector : *rt::getArray(metadata, "keyVectors")) {
    const auto& input = *rt::get(vector, "input"); const auto kind = rt::getString(input, "kind"); rt::KvKeyPart part(false);
    if (kind == "bytes") { rt::KvBytes bytes; for (const auto& value : *rt::getArray(input, "value")) bytes.push_back(static_cast<std::uint8_t>(rt::toNumber(value))); part = rt::KvKeyPart(bytes); }
    else if (kind == "bigint") part = rt::KvKeyPart(rt::BigInt::fromString(*rt::getString(input, "value")));
    else if (kind == "number") part = rt::KvKeyPart(rt::stringToNumber(*rt::getString(input, "value")));
    else if (kind == "boolean") part = rt::KvKeyPart(*rt::getBool(input, "value"));
    else part = rt::KvKeyPart(std::string(*rt::getString(input, "value")));
    const rt::KvKey key{"fixture", part};
    if (const auto* expected = rt::get(vector, "error")) { bool rejected = false; try { uos::authority::native_authority_sql::nativeKey(key); } catch (const contracts::NativeAuthorityError& error) { rejected = true; EXPECT_EQ(error.code, std::string(*rt::getString(*expected, "code"))); EXPECT_EQ(error.status, *rt::getNumber(*expected, "status")); } EXPECT(rejected); }
    else EXPECT_EQ(uos::authority::native_authority_sql::nativeKey(key), std::string(*rt::getString(vector, "result")));
    ++count;
  }
  EXPECT_EQ(count, std::size_t(24));
}
TEST("native authority Codex account and blocked-account DTOs retain required null probe fields") {
  const auto& metadata = uos::tests::helpers::native_authority_harness::vectors().as_array().back();
  const json::value authorization(json::object{{"account", *rt::get(metadata, "account")}, {"owner", *rt::get(metadata, "owner")}});
  const auto restored = json::value_to<contracts::NativeCodexAuthorization>(authorization); const auto roundtrip = json::value_from(restored);
  EXPECT(rt::deepEqual(authorization, roundtrip));
  const auto* account = rt::get(roundtrip, "account"); ASSERT(account); ASSERT(rt::get(*account, "probeGeneration")); EXPECT(rt::get(*account, "probeGeneration")->is_null()); ASSERT(rt::get(*account, "probeToken")); EXPECT(rt::get(*account, "probeToken")->is_null());
  const auto blocked = json::value_to<contracts::NativeRequest>(*rt::get(metadata, "blockedRequest")); const auto blockedJson = json::value_from(blocked); EXPECT(rt::deepEqual(blockedJson, *rt::get(metadata, "blockedRequest")));
  auto nonNullSnapshots = *rt::get(metadata, "blockedRequest"); auto& selection = nonNullSnapshots.as_object().at("selection").as_object(); selection["poolSnapshotJson"] = "pool-snapshot"; selection["capacitySnapshotJson"] = "capacity-snapshot";
  EXPECT(rt::deepEqual(json::value_from(json::value_to<contracts::NativeRequest>(nonNullSnapshots)), nonNullSnapshots));
  auto absentSnapshot = nonNullSnapshots; rt::objectDelete(absentSnapshot.as_object().at("selection").as_object(), "poolSnapshotJson");
  EXPECT_THROWS_MATCHING(json::value_to<contracts::NativeRequest>(absentSnapshot), "poolSnapshotJson is undefined, not string or null");
  const json::value absent(json::object{{"account", *rt::get(metadata, "absent")}, {"owner", *rt::get(metadata, "owner")}});
  EXPECT_THROWS_MATCHING(json::value_to<contracts::NativeCodexAuthorization>(absent), "probeGeneration is undefined, not number or null");
  auto absentBlocked = *rt::get(metadata, "blockedRequest"); auto& raw = absentBlocked.as_object().at("selection").as_object().at("blockedAccounts").as_array().front().as_object(); rt::objectDelete(raw, "probeToken");
  EXPECT_THROWS_MATCHING(json::value_to<contracts::NativeRequest>(absentBlocked), "probeToken is undefined, not string or null");
}

ASYNC_TEST("native SQL rejects both running Promise and lazy Task transaction callbacks") {
  const auto& scenario = uos::tests::helpers::native_authority_harness::vectors().as_array().front();
  uos::tests::helpers::native_authority_harness::Fixture fixture(scenario);
  EXPECT_THROWS_MATCHING(fixture.sql.transaction([] { return rt::Promise<int>::resolve(1); }), "async_transaction_forbidden");
  EXPECT_THROWS_MATCHING(fixture.sql.transaction([] { return rt::fulfilledTask(1); }), "async_transaction_forbidden");
  fixture.database.close();
  co_return;
}

#include "tests/helpers/native_authority_assembly_harness.hpp"
namespace { const bool nativeAuthorityAssemblyRegistration = uos::tests::helpers::native_authority_assembly_harness::registerScenarios("authority"); }
ASYNC_TEST("native authority real RPC clients share ordinary, principal and retained SQLite authority") {
  namespace base = uos::tests::helpers::native_authority_harness;
  namespace principal = uos::authority::native_principal_contracts;
  auto scenario = base::vectors().as_array().front(); scenario.as_object().at("options").as_object()["retained"] = true;
  base::Fixture fixture(scenario);
  uos::authority::native_authority::NativeAuthority authority(base::storage(fixture.database), fixture.initial, [&fixture] { return fixture.now; }, fixture.principalInitial, fixture.runtimeInitial);
  const principal::NativeControlBinding resolverBinding{"resolver", "resolver-context", json::object{{"kind", "principal_resolver"}}};
  rpc::NativePrincipalControlServer controlServer(authority, resolverBinding);
  rpc::NativePrincipalControlClient controlClient(controlServer.transport(), "e");
  const json::object noArgs;
  AWAIT(resolved, controlClient.call("resolvePrincipal", noArgs, "real-rpc-resolve")); EXPECT_EQ(rt::getString(resolved, "kind"), std::optional<std::string_view>("principal"));
  const runtime::NativeRuntimeBinding binding{"runtime", "context", json::object()};
  rpc::NativeAuthorityRpcServer server(authority, binding); rpc::NativeAuthorityClient client(server.transport(), "e");
  AWAIT(opened, client.call("openAuthenticatedRequest", json::object{{"requestId", "real-rpc"}, {"route", "/v1/responses"}, {"path", "/v1/responses"}}, "real-rpc-open"));
  EXPECT_EQ(rt::getString(opened, "kind"), std::optional<std::string_view>("opened")); const auto handle = std::string(*rt::getString(opened, "handle"));
  AWAIT(reserved, client.call("reserveApiUsage", json::object{{"handle", handle}}, "real-rpc-api")); EXPECT_EQ(rt::getString(reserved, "kind"), std::optional<std::string_view>("reserved"));
  AWAIT(metadata, client.call("bindValidatedRequestMetadata", json::object{{"handle", handle}, {"model", "fixture"}, {"stream", false}, {"reasoning", nullptr}}, "real-rpc-metadata")); EXPECT_EQ(rt::getString(metadata, "kind"), std::optional<std::string_view>("bound"));
  const auto raw = fixture.sql.read("request_lifetime", {handle}, fixture.now).value.value();
  auto countedStorage = base::storage(fixture.database); const auto originalExec = countedStorage.sql.exec; std::size_t lifetimeReads = 0, writes = 0;
  countedStorage.sql.exec = [&](std::string query, std::vector<contracts::NativeSqlStorage::Binding> bindings) {
    if (query.starts_with("SELECT row_json,revision,expires_at_ms FROM uos_native_request_lifetime ")) ++lifetimeReads;
    if (query.starts_with("UPDATE ") || query.starts_with("INSERT ") || query.starts_with("DELETE ")) ++writes;
    return originalExec(std::move(query), std::move(bindings));
  };
  uos::authority::native_authority_sql::NativeAuthoritySql countedSql(countedStorage, fixture.initial.namespace_, fixture.initial.epoch);
  uos::authority::native_authority_api::NativeAuthorityApi countedApi(countedSql); lifetimeReads = 0; writes = 0;
  const auto validated = countedApi.validated(fixture.cap, handle, fixture.now);
  EXPECT_EQ(lifetimeReads, std::size_t(1)); EXPECT_EQ(writes, std::size_t(0)); EXPECT(rt::deepEqual(validated.rawJson, raw));
  const auto encodedValidated = json::value_from(static_cast<const contracts::NativeRequest&>(validated)); EXPECT(!rt::hasOwn(encodedValidated.as_object(), "rawJson"));
  AWAIT(routing, client.bindTrustedRoutingDecision(handle, "real-rpc-routing")); EXPECT_EQ(rt::getString(routing, "kind"), std::optional<std::string_view>("bound"));
  AWAIT(paid, client.call("reservePaidAttempt", json::object{{"handle", handle}, {"hopId", "openlux-hop"}}, "real-rpc-paid")); EXPECT_EQ(rt::getString(paid, "kind"), std::optional<std::string_view>("reserved"));
  const auto attemptHandle = std::string(*rt::getString(paid, "attemptHandle"));
  const runtime::NativeRetainedHistoryGrant grant{"retained", 3, "e", "retainer", "retained-context", {"transport", "billing", "reconcile"}, fixture.now + 60000, false, fixture.initial.namespace_, "binding", "api-key:key", "key", {handle}};
  fixture.sql.transaction([&] { fixture.sql.write("retained_history_grant", {grant.id}, grant); });
  const runtime::NativeRetainedHistoryBinding retainedBinding{"retained_history", "retained", "retained-context", json::object{{"requestHandles", json::array{handle}}}};
  rpc::NativeRetainedHistoryServer retainedServer(authority, retainedBinding); rpc::NativeAuthorityClient retainedClient(retainedServer.transport(), "e");
  AWAIT(releasedPaid, retainedClient.call("releasePaidBeforeFetch", json::object{{"attemptHandle", attemptHandle}}, "real-rpc-retained-paid")); EXPECT_EQ(rt::getString(releasedPaid, "kind"), std::optional<std::string_view>("released"));
  AWAIT(releasedApi, retainedClient.call("releaseApiUndispatched", json::object{{"handle", handle}, {"reason", "private-fixture-cleanup"}}, "real-rpc-retained-api")); EXPECT_EQ(rt::getString(releasedApi, "kind"), std::optional<std::string_view>("released"));
  AWAIT(status, client.call("readRequestStatus", json::object{{"handle", handle}, {"limit", 10}}, "real-rpc-status")); EXPECT_EQ(rt::getString(status, "kind"), std::optional<std::string_view>("status"));
  AWAIT(replayed, client.call("openAuthenticatedRequest", json::object{{"requestId", "real-rpc"}, {"route", "/v1/responses"}, {"path", "/v1/responses"}}, "real-rpc-open")); EXPECT(rt::deepEqual(opened, replayed));
  fixture.database.close();
}
