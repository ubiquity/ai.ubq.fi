// Links the sources of tests/prompt_cache_scope_experiment_part1_test.cpp.
// Additional real-TypeScript differentials pin CAS refusal, lease loss, drift and promotion terminal-write behavior.
#include "tests/helpers/prompt_cache_scope_experiment_harness.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
namespace h = uos::tests::helpers::prompt_cache_scope_experiment_harness;
namespace m = uos::cache::scope_experiment_model;
namespace rt = uos::rt;
namespace json = uos::json;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  void put(const rt::KvKey& key, const json::value& value) { h::kv->put(key, rt::KvValue(value)); }
  rt::Task<void> replay(std::string name) {
    h::HarnessScope scope; rt::globalDateNow() = [] { return 1'700'000'000'000.0; }; h::seed();
    const auto vectors = rt::jsonParse(
#include "prompt_cache_scope_experiment_failure_vectors.inc"
    );
    const json::value* expected = nullptr; for (const auto& row : vectors.as_array()) if (rt::getString(row, "name") == name) expected = &row; ASSERT(expected);
    h::ScopeTransport transport;
    transport.onInference = [name](std::size_t index, const auto&, const auto&) {
      if (name == "lost_cycle_lease" && index == 0) put(h::leaseKey(std::string(h::MODEL)), json::object{{"owner", "other"}, {"lease_until_ms", rt::makeNumber(rt::dateNow() + 60000)}});
      if (name == "state_changed" && index == 0) { auto state = h::kv->row(h::stateKey); state.as_object()["campaign_owner"] = "other"; put(h::stateKey, state); }
      if (name == "final_conflicts") return h::sseCompleted(0, 0);
      const auto step = static_cast<double>(index % 10); const bool changed = name == "cycle_disagreement" && index == 22;
      return h::sseCompleted(changed ? 2560 : h::cycleValue(h::CACHE_READ_CYCLE, step), changed ? 0 : h::cycleValue(h::CACHE_WRITE_CYCLE_ALTERNATING, step));
    };
    if (name == "refresh_unchanged") transport.onRefresh = [](auto) { return rt::Response::json(json::object{{"access_token", "access-one"}, {"refresh_token", "refresh-one"}}); };
    if (name == "intermediate_conflicts" || name == "final_conflicts" || (name == "promotion_final_conflicts" || name == "promotion_ready_resume")) h::kv->refuseAtomicCommit = [name](const auto& writes) {
      for (const auto& write : writes) {
        if (write.type != "set" || !write.value) continue;
        const auto& value = *write.value;
        if (name == "intermediate_conflicts" && value["next_cycle"].isNumber() && value["next_cycle"].asNumber() == 2) return true;
        if (name == "final_conflicts" && value["outcome"].isString() && (value["outcome"].asString() == "inconclusive" || value["outcome"].asString() == "failed")) return true;
        if ((name == "promotion_final_conflicts" || name == "promotion_ready_resume") && value["outcome"].isString() && value["outcome"].asString() == "completed") return true;
      }
      return false;
    };
    if (name == "promotion_conflict") h::kv->afterAtomicCommit = [](const auto& writes) {
      for (const auto& write : writes) if (write.type == "set" && write.value && (*write.value)["next_cycle"].isNumber() && (*write.value)["next_cycle"].asNumber() == 4) h::kv->put(h::CODEX_MODELS_KV_KEY, h::kv->entryOf(h::CODEX_MODELS_KV_KEY).value);
    };
    const rt::GlobalFetchOverride fetch(transport.fetcher()); json::array results;
    const int runs = name == "promotion_ready_resume" ? 4 : name == "cycle_disagreement" || name == "promotion_conflict" || name == "promotion_final_conflicts" ? 3 : name == "session_auth_drift" ? 2 : 1;
    for (int run = 0; run < runs; ++run) {
      if (name == "promotion_ready_resume" && run == 3) { h::kv->refuseAtomicCommit = {}; put(h::leaseKey(std::string(h::MODEL)), json::object{{"owner", "expired"}, {"lease_until_ms", rt::makeNumber(rt::dateNow() - 1)}}); }
      try {
        AWAIT(result, h::runExperiment()); json::object row{{"status", result.status}, {"completed_cycles", rt::makeNumber(result.completed_cycles)}};
        if (result.inconclusive_reason) row["inconclusive_reason"] = *result.inconclusive_reason;
        if (result.scope) row["scope"] = json::value_from(*result.scope);
        results.push_back(row);
      } catch (const rt::JsError& error) { results.push_back(json::object{{"error", json::object{{"name", error.name}, {"message", error.message}}}}); }
      if (name == "session_auth_drift" && run == 0) h::kv->put(h::CODEX_AUTH_POOL_KV_KEY, h::kv->entryOf(h::CODEX_AUTH_POOL_KV_KEY).value);
    }
    const auto state = h::kv->row(h::stateKey), evidence = h::kv->row(h::evidenceKey); const auto* nextCycle = rt::get(state, "next_cycle"), *outcome = rt::get(evidence, "outcome"); const auto* cycles = rt::getArray(evidence, "cycles");
    const json::object actual{{"name", name}, {"results", results}, {"inferenceCalls", rt::makeNumber(static_cast<double>(transport.requests.size()))}, {"refreshes", rt::makeNumber(static_cast<double>(transport.refreshes))}, {"atomicCalls", rt::makeNumber(h::kv->atomicCalls)},
      {"state_next_cycle", nextCycle ? *nextCycle : json::value(nullptr)}, {"evidence_outcome", outcome ? *outcome : json::value(nullptr)}, {"evidence_cycles", cycles ? rt::makeNumber(static_cast<double>(cycles->size())) : json::value(nullptr)}, {"published_scope", rt::includes(rt::jsonStringify(h::kv->row(h::CODEX_MODELS_KV_KEY)), "\"scope\"")}};
    for (const auto& key : rt::objectKeys(actual)) EXPECT(uos::tests::helpers::codex_diff_kv::sameVector(expected->as_object(), actual, key));
  }
}
ASYNC_TEST("scope failure differential: refresh_unchanged") { auto task = replay("refresh_unchanged"); co_await std::move(task); }
ASYNC_TEST("scope failure differential: cycle_disagreement") { auto task = replay("cycle_disagreement"); co_await std::move(task); }
ASYNC_TEST("scope failure differential: session_auth_drift") { auto task = replay("session_auth_drift"); co_await std::move(task); }
ASYNC_TEST("scope failure differential: lost_cycle_lease") { auto task = replay("lost_cycle_lease"); co_await std::move(task); }
ASYNC_TEST("scope failure differential: state_changed") { auto task = replay("state_changed"); co_await std::move(task); }
ASYNC_TEST("scope failure differential: intermediate_conflicts") { auto task = replay("intermediate_conflicts"); co_await std::move(task); }
ASYNC_TEST("scope failure differential: final_conflicts") { auto task = replay("final_conflicts"); co_await std::move(task); }
ASYNC_TEST("scope failure differential: promotion_conflict") { auto task = replay("promotion_conflict"); co_await std::move(task); }
ASYNC_TEST("scope failure differential: promotion_final_conflicts") { auto task = replay("promotion_final_conflicts"); co_await std::move(task); }
ASYNC_TEST("scope failure differential: promotion_ready_resume") { auto task = replay("promotion_ready_resume"); co_await std::move(task); }
