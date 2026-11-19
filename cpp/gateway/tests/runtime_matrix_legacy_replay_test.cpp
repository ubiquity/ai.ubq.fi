// Links: tests/runtime_matrix_legacy_replay_test.cpp src/api_key_policy.cpp src/api_keys.cpp src/auth/relay.cpp src/authority/native_api_key_policy.cpp src/authority/native_api_key.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_codex.cpp src/authority/native_paid_policy.cpp src/authority/native_paid.cpp src/bounded_response_body.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/prompt_analytics.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp src/codex/auth_refresh.cpp src/codex/auth.cpp src/codex/banked_reset_claim.cpp src/codex/banked_reset_pool.cpp src/codex/banked_reset_provider.cpp src/codex/banked_reset_submission.cpp src/codex/banked_reset.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/index.cpp src/codex/native_auth_transport.cpp src/codex/native_auth.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/responses_error.cpp src/codex/responses_operations.cpp src/codex/responses_runners.cpp src/codex/responses_state.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp src/deepseek/flash_aliases.cpp src/deepseek/index.cpp src/deepseek/waterfall.cpp src/defaults.cpp src/http.cpp src/inference_deadline.cpp src/input_normalization.cpp src/kv.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp src/models/codex_models.cpp src/models/metadata.cpp src/models/openrouter_models.cpp src/openai_schema.cpp src/openai_telemetry.cpp src/openai.cpp src/optional_telemetry_queue.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/rollups.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp src/provider/capacity.cpp src/provider/cerebras_rate_limits.cpp src/provider/cerebras.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/claude_code.cpp src/provider/health.cpp src/provider/lithos_models.cpp src/provider/lithos_rate_limits.cpp src/provider/lithos.cpp src/provider/metered.cpp src/provider/openrouter_request_controls.cpp src/provider/openrouter.cpp src/provider/presentation.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/provider/surplus.cpp src/recent_model_context.cpp src/release.cpp src/request_policy.cpp src/request.cpp src/responses_failover_stream.cpp src/responses_request.cpp src/responses_stream.cpp src/runtime_config.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/runtime/http.cpp src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/upstream_wire.cpp src/utils.cpp src/types.cpp src/authority/native_paid_contracts.cpp src/lib/jev_compaction/types.cpp
// Original message-ID assertions plus the six source legacyReplay harness cases and exact eleven-item forwarding inventory.
// The bounded legacyReplay fixture is test-local; unrelated runtime-matrix operations are outside this test.
#include "uos/input_normalization.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"
namespace { namespace rt = uos::rt; namespace json = uos::json; namespace n = uos::input_normalization; }
TEST("actual source message normalization clears both observed message IDs without editing history") {
  const std::vector<std::string> ids{"msg_03375d708dca4424016abf51971e4887d1acd5ea8e0c400198", "resp_custom_item_0"};
  const std::vector<double> indices{7, 10};
  for (std::size_t index = 0; index < ids.size(); ++index) {
    const json::value original = json::object{{"id", ids[index]}, {"type", "message"}, {"role", "assistant"}, {"content", json::array{json::object{{"type", "output_text"}, {"text", "legacy answer"}}}}};
    const auto before = rt::jsonStringify(original);
    const auto normalized = n::normalizeResponseMessageItem(original, "input[" + rt::numberToString(indices[index]) + "]");
    ASSERT(std::holds_alternative<n::NormalizationResultOk<uos::types::ResponseMessageItem>>(normalized));
    const auto result = json::value_from(std::get<n::NormalizationResultOk<uos::types::ResponseMessageItem>>(normalized).value);
    EXPECT(!rt::hasOwn(result.as_object(), "id"));
    const json::object expected{{"type", "message"}, {"role", "assistant"}, {"content", json::array{json::object{{"type", "output_text"}, {"text", "legacy answer"}}}}};
    EXPECT(rt::deepEqual(result, expected)); EXPECT_EQ(rt::jsonStringify(original), before); EXPECT_EQ(rt::toString(rt::get(original, "id")), ids[index]);
  }
}
#include "uos/responses_request.hpp"
#include "uos/models/codex_models_fetch.hpp"
namespace {
  struct LegacyFixture { json::array original; json::array normalized; json::array forwarded; std::size_t requests = 0; std::vector<std::string> cases; };
  void strictLegacy(const json::array& input) {
    if (input.size()!=11U) throw rt::JsError("legacy inventory: expected 11, actual " + rt::numberToString(static_cast<double>(input.size())));
    json::array ids; for(const auto& item:input){const auto* id=rt::get(item,"id");ids.push_back(id?*id:json::value(nullptr));}
    const json::array expected{nullptr,nullptr,nullptr,nullptr,"rs_03375d708dca4424016abf51952afc87d1824eacab5be80344",nullptr,nullptr,nullptr,nullptr,"resp_"+std::string(41,'a')+"_rs_0",nullptr};
    if(!rt::deepEqual(ids,expected))throw rt::JsError("legacy item identities differ");
    const auto* content=rt::getArray(input[0],"content");
    if(!content || content->empty() || rt::getString((*content)[0],"text")!="legacy answer 😀")throw rt::JsError("legacy answer differs");
    if(rt::getString(input[1],"call_id")!="call_legacy" || rt::getString(input[2],"call_id")!="call_legacy")throw rt::JsError("legacy call identities differ");
    if(rt::getString(input[3],"encrypted_content")!="fixture-synthetic-opaque" || rt::getString(input[4],"encrypted_content")!="fixture-genuine-opaque" || rt::getString(input[7],"encrypted_content")!="fixture-forty-opaque")throw rt::JsError("legacy encrypted replay differs");
  }
  LegacyFixture legacyFixture(const std::function<void(json::array&)>& transform={}) {
    const std::string rid="resp_197c782c84e54851b12fbae3fc396d7e";
    const auto thought=[](const std::string& id,const std::string& text)->json::object{return {{"type","reasoning"},{"id",id},{"summary",json::array{json::object{{"type","summary_text"},{"text",text}}}}};};
    const auto message=[](const std::string& id,const std::string& role,const std::string& text)->json::object{return {{"type","message"},{"id",id},{"role",role},{"content",json::array{json::object{{"type",role=="user"?"input_text":"output_text"},{"text",text}}}}};};
    auto synthetic=thought("rs_"+rid+"_1","");synthetic["summary"]=json::array{};synthetic["encrypted_content"]="fixture-synthetic-opaque";
    auto empty=synthetic;empty["id"]="rs_"+rid+"_2";empty["encrypted_content"]="";
    auto genuine=thought("rs_03375d708dca4424016abf51952afc87d1824eacab5be80344","keep genuine");genuine["encrypted_content"]="fixture-genuine-opaque";
    auto forty=synthetic;forty["id"]="rs_resp_"+std::string(40,'a')+"_0";forty["encrypted_content"]="fixture-forty-opaque";
    auto longReason=thought("resp_"+std::string(41,'a')+"_rs_0","");longReason["summary"]=json::array{};
    auto last=message("","user","fixture:matrix:replay:legacy");rt::objectDelete(last,"id");
    LegacyFixture f;f.original=json::array{thought(rid+"_rs_0","drop local thought"),message(rid+"_msg_0","assistant","legacy answer 😀"),json::object{{"type","function_call"},{"id",rid+"_fc_0"},{"call_id","call_legacy"},{"name","lookup"},{"arguments","{\"query\":\"é😀\"}"}},json::object{{"type","function_call_output"},{"call_id","call_legacy"},{"output","legacy result 😀"}},synthetic,empty,genuine,message("msg_03375d708dca4424016abf51971e4887d1acd5ea8e0c400198","assistant","genuine answer é"),message("msg_resp_chatcmplABC123_0","assistant","nonhex synthetic answer"),forty,message("resp_custom_item_0","user","unrelated id"),longReason,last};
    const auto snapshot=rt::jsonStringify(f.original);const json::value raw=f.original;const auto result=uos::responses_request::normalizeResponsesInput(raw);
    const auto* normalized=std::get_if<uos::responses_request::ResponsesStepOk<std::vector<uos::types::ResponseInputItem>>>(&result);if(!normalized)throw rt::JsError("actual source normalization rejected legacy vector");
    f.normalized=json::value_from(normalized->value).as_array();auto codex=uos::models::codex_models_fetch::buildCodexRequest("gpt-6.1-sol",normalized->value);f.forwarded=*rt::getArray(codex,"input");++f.requests;
    if(transform){transform(f.forwarded);}if(rt::jsonStringify(f.original)!=snapshot)throw rt::JsError("client history changed");strictLegacy(f.forwarded);
    f.cases.push_back("matrix_exact_legacy_replay_drops_only_unencrypted_synthetic_reasoning_preserves_genuine_and_nonmatching_ids");return f;
  }
}
TEST("actual source message normalization precedes replay cleanup and clears both observed message IDs") {
  const auto f=legacyFixture();EXPECT_EQ(f.requests,1U);
  for(const std::size_t index:{7U,10U}){const auto normalized=n::normalizeResponseMessageItem(f.original[index],"input["+rt::numberToString(static_cast<double>(index))+"]");ASSERT(std::holds_alternative<n::NormalizationResultOk<uos::types::ResponseMessageItem>>(normalized));const auto value=json::value_from(std::get<n::NormalizationResultOk<uos::types::ResponseMessageItem>>(normalized).value);EXPECT(!rt::hasOwn(value.as_object(),"id"));EXPECT(rt::deepEqual(f.normalized[index],value));}
  EXPECT_EQ(rt::getString(f.original[7],"id").value(),"msg_03375d708dca4424016abf51971e4887d1acd5ea8e0c400198");EXPECT_EQ(rt::getString(f.original[10],"id").value(),"resp_custom_item_0");EXPECT_EQ(f.forwarded.size(),11U);EXPECT_EQ(f.cases,(std::vector<std::string>{"matrix_exact_legacy_replay_drops_only_unencrypted_synthetic_reasoning_preserves_genuine_and_nonmatching_ids"}));
}
TEST("echoing message IDs is rejected rather than accepted as an order or subset relaxation") {
  EXPECT_THROWS(legacyFixture([](json::array& input){input[5].as_object()["id"]="msg_03375d708dca4424016abf51971e4887d1acd5ea8e0c400198";input[8].as_object()["id"]="resp_custom_item_0";}),rt::JsError);
}
TEST("genuine and nonmatching long-prefix non-message reasoning IDs must still be preserved") {
  for(const std::size_t index:{4U,9U})EXPECT_THROWS(legacyFixture([index](json::array& input){rt::objectDelete(input[index].as_object(),"id");}),rt::JsError);
}
TEST("encrypted synthetic and genuine reasoning replay payload checks remain strict") {
  for(const std::size_t index:{3U,4U,7U})EXPECT_THROWS(legacyFixture([index](json::array& input){input[index].as_object()["encrypted_content"]="changed";}),rt::JsError);
}
TEST("original call identities and legacy message content assertions remain strict") {
  for(const std::size_t index:{1U,2U})EXPECT_THROWS(legacyFixture([index](json::array& input){input[index].as_object()["call_id"]="different-call";}),rt::JsError);
  EXPECT_THROWS(legacyFixture([](json::array& input){input[0].as_object()["content"]=json::array{json::object{{"type","output_text"},{"text","changed legacy answer"}}};}),rt::JsError);
}
TEST("the exact eleven-item inventory is not reduced to make the legacy assertion pass") {
  EXPECT_THROWS_MATCHING(legacyFixture([](json::array& input){input.pop_back();}),"expected 11, actual 10");
}
