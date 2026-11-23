// Links: tests/input_normalization_logprobs_test.cpp src/api_key_policy.cpp src/api_keys.cpp src/auth/relay.cpp src/authority/native_api_key_policy.cpp src/authority/native_api_key.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_codex.cpp src/authority/native_paid_policy.cpp src/authority/native_paid.cpp src/bounded_response_body.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/prompt_analytics.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp src/codex/auth_refresh.cpp src/codex/auth.cpp src/codex/banked_reset_claim.cpp src/codex/banked_reset_pool.cpp src/codex/banked_reset_provider.cpp src/codex/banked_reset_submission.cpp src/codex/banked_reset.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/index.cpp src/codex/native_auth_transport.cpp src/codex/native_auth.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/responses_error.cpp src/codex/responses_operations.cpp src/codex/responses_runners.cpp src/codex/responses_state.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp src/deepseek/flash_aliases.cpp src/deepseek/index.cpp src/deepseek/waterfall.cpp src/defaults.cpp src/http.cpp src/inference_deadline.cpp src/input_normalization.cpp src/kv.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp src/models/codex_models.cpp src/models/metadata.cpp src/models/openrouter_models.cpp src/openai_schema.cpp src/openai_telemetry.cpp src/openai.cpp src/optional_telemetry_queue.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/rollups.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp src/provider/capacity.cpp src/provider/cerebras_rate_limits.cpp src/provider/cerebras.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/claude_code.cpp src/provider/health.cpp src/provider/lithos_models.cpp src/provider/lithos_rate_limits.cpp src/provider/lithos.cpp src/provider/metered.cpp src/provider/openrouter_request_controls.cpp src/provider/openrouter.cpp src/provider/presentation.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/provider/surplus.cpp src/recent_model_context.cpp src/release.cpp src/request_policy.cpp src/request.cpp src/responses_failover_stream.cpp src/responses_request.cpp src/responses_stream.cpp src/runtime_config.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/runtime/http.cpp src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/upstream_wire.cpp src/utils.cpp src/types.cpp src/authority/native_paid_contracts.cpp src/lib/jev_compaction/types.cpp
// Original direct normalizer assertions plus request parsing, body assembly and isolated paid transport assertions.
#include "uos/input_normalization.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/responses_request.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/rt/fetch.hpp"
namespace {
  namespace n = uos::input_normalization; namespace rt = uos::rt; namespace json = uos::json;
  json::value encoded(const n::NormalizationResult<uos::types::ResponseMessageItem>& result) {
    if (const auto* error = std::get_if<n::NormalizationResultNotOk>(&result)) return json::object{{"ok", false}, {"param", error->param}, {"message", error->message}};
    return json::object{{"ok", true}, {"value", json::value_from(std::get<n::NormalizationResultOk<uos::types::ResponseMessageItem>>(result).value)}};
  }
}
TEST("Responses replay accepts and drops empty and populated output_text logprobs [message normalizer]") {
  const json::array samples{json::array{}, json::array{json::object{{"token", "reply"}, {"bytes", json::array{114, 101, 112, 108, 121}}, {"logprob", -0.01}, {"top_logprobs", json::array{}}}}};
  for (const auto& logprobs : samples) {
    const json::value message = json::object{{"type", "message"}, {"role", "assistant"}, {"content", json::array{json::object{{"type", "output_text"}, {"text", "reply"}, {"annotations", json::array{}}, {"logprobs", logprobs}}}}};
    const auto before = rt::jsonStringify(message); const auto result = n::normalizeResponseMessageItem(message, "input[0]");
    const json::object expected{{"ok", true}, {"value", json::object{{"type", "message"}, {"role", "assistant"}, {"content", json::array{json::object{{"type", "output_text"}, {"text", "reply"}}}}}}};
    EXPECT(rt::deepEqual(encoded(result), expected)); EXPECT_EQ(rt::jsonStringify(message), before);
  }
}
TEST("Responses replay rejects non-array logprobs at the exact nested field") {
  const json::array invalid{nullptr, "invalid", json::object{}, 0, true};
  for (const auto& logprobs : invalid) {
    const json::object message{{"role", "assistant"}, {"content", json::array{json::object{{"type", "output_text"}, {"text", "reply"}, {"logprobs", logprobs}}}}};
    const json::object expected{{"ok", false}, {"param", "input[9].content[0].logprobs"}, {"message", "input[9].content[0].logprobs must be an array"}};
    EXPECT(rt::deepEqual(encoded(n::normalizeResponseMessageItem(json::value(message), "input[9]")), expected));
  }
}
TEST("Responses replay keeps rejecting unknown output_text fields and logprobs on input_text") {
  const json::value output = json::object{{"type", "output_text"}, {"text", "reply"}, {"logprobs", json::array{}}, {"unknown_field", true}};
  const auto first = n::normalizeResponseContentItem(output, "input[9].content[0]", "assistant"); ASSERT(std::holds_alternative<n::NormalizationResultNotOk>(first));
  const n::NormalizationResultNotOk expectedFirst{"Unknown content field: unknown_field", "input[9].content[0].unknown_field"}; EXPECT_EQ(std::get<n::NormalizationResultNotOk>(first), expectedFirst);
  const json::value input = json::object{{"type", "input_text"}, {"text", "prompt"}, {"logprobs", json::array{}}};
  const auto second = n::normalizeResponseContentItem(input, "input[0].content[0]", "user"); ASSERT(std::holds_alternative<n::NormalizationResultNotOk>(second));
  const n::NormalizationResultNotOk expectedSecond{"Unknown content field: logprobs", "input[0].content[0].logprobs"}; EXPECT_EQ(std::get<n::NormalizationResultNotOk>(second), expectedSecond);
}

ASYNC_TEST("Responses replay accepts and drops empty and populated output_text logprobs") {
  namespace rq = uos::responses_request;
  const json::array samples{json::array{},json::array{json::object{{"token","reply"},{"bytes",json::array{114,101,112,108,121}},{"logprob",-0.01},{"top_logprobs",json::array{}}}}};
  for(const auto& logprobs:samples){
    const json::value input=json::array{json::object{{"type","message"},{"role","assistant"},{"content",json::array{json::object{{"type","output_text"},{"text","reply"},{"annotations",json::array{}},{"logprobs",logprobs}}}}}};
    const auto normalized=rq::normalizeResponsesInput(input); ASSERT(std::holds_alternative<rq::ResponsesStepOk<std::vector<uos::types::ResponseInputItem>>>(normalized));
    const auto items=std::get<rq::ResponsesStepOk<std::vector<uos::types::ResponseInputItem>>>(normalized).value;
    const json::array expected{json::object{{"type","message"},{"role","assistant"},{"content",json::array{json::object{{"type","output_text"},{"text","reply"}}}}}};
    EXPECT(rt::deepEqual(json::value_from(items),expected)); const auto bodies=rq::buildResponsesUpstreamBodies("gpt-6.1-sol",items,std::nullopt,json::object{{"effort","low"}},{});
    EXPECT(rt::deepEqual(*rt::get(bodies.codexBody,"input"),expected)); EXPECT(rt::deepEqual(*rt::get(bodies.removedProviderBody,"input"),expected));
    for(int provider=0;provider<2;++provider){
      std::vector<std::string> serialized;
      rt::Fetch fetcher([&serialized](rt::RequestInfo,rt::RequestInit init){ ASSERT(init.body); const auto* body=std::get_if<std::string>(&*init.body); ASSERT(body); serialized.push_back(*body); return rt::Response("{}",{.headers=rt::Headers{{"Content-Type","application/json"}}}); });
      rt::Response response;
      const json::value body=bodies.codexBody;
      if(provider==0){uos::provider::surplus::SurplusAuthenticatedFetchOptions options;options.apiKey="fixture-key";options.fetcher=fetcher;auto task=uos::provider::surplus::fetchSurplusResponses(body,options);auto result=co_await std::move(task);response=result.response;}
      else{uos::provider::metered::MeteredAuthenticatedFetchOptions options;options.apiKey="fixture-key";options.fetcher=fetcher;auto task=uos::provider::metered::fetchMeteredResponses(body,options);auto result=co_await std::move(task);response=result.response;}
      EXPECT_EQ(serialized.size(),1U);const auto sent=rt::jsonParse(serialized[0]);EXPECT(rt::deepEqual(*rt::get(sent,"input"),expected));if(response.body())co_await response.body()->cancel();
    }
  }
}

ASYNC_TEST("captured Codex 0.163.0 request normalizes without changing client history") {
  namespace rq = uos::responses_request;
  const auto fixture=rt::jsonParse(R"FIXTURE({
  "model": "gpt-6.1-sol",
  "stream": true,
  "input": [
    {
      "type": "additional_tools",
      "id": "fixture_1",
      "role": "developer",
      "tools": [
        {
          "type": "namespace",
          "name": "functions",
          "description": "Synthetic fixture text.",
          "tools": [
            {
              "type": "custom",
              "name": "exec",
              "description": "Synthetic fixture text.",
              "format": {
                "type": "grammar",
                "syntax": "lark",
                "definition": "start: \"fixture\""
              }
            },
            {
              "type": "function",
              "name": "wait",
              "description": "Synthetic fixture text.",
              "strict": false,
              "parameters": {
                "type": "object",
                "properties": {
                  "cell_id": {
                    "type": "string",
                    "description": "Synthetic fixture text."
                  },
                  "max_tokens": {
                    "type": "number",
                    "description": "Synthetic fixture text."
                  },
                  "terminate": {
                    "type": "boolean",
                    "description": "Synthetic fixture text."
                  },
                  "yield_time_ms": {
                    "type": "number",
                    "description": "Synthetic fixture text."
                  }
                },
                "required": ["cell_id"],
                "additionalProperties": false
              }
            },
            {
              "type": "function",
              "name": "request_user_input",
              "description": "Synthetic fixture text.",
              "strict": false,
              "parameters": {
                "type": "object",
                "properties": {
                  "questions": {
                    "type": "array",
                    "description": "Synthetic fixture text.",
                    "items": {
                      "type": "object",
                      "properties": {
                        "header": {
                          "type": "string",
                          "description": "Synthetic fixture text."
                        },
                        "id": {
                          "type": "string",
                          "description": "Synthetic fixture text."
                        },
                        "options": {
                          "type": "array",
                          "description": "Synthetic fixture text.",
                          "items": {
                            "type": "object",
                            "properties": {
                              "description": {
                                "type": "string",
                                "description": "Synthetic fixture text."
                              },
                              "label": {
                                "type": "string",
                                "description": "Synthetic fixture text."
                              }
                            },
                            "required": ["label", "description"],
                            "additionalProperties": false
                          }
                        },
                        "question": {
                          "type": "string",
                          "description": "Synthetic fixture text."
                        }
                      },
                      "required": ["id", "header", "question", "options"],
                      "additionalProperties": false
                    }
                  }
                },
                "required": ["questions"],
                "additionalProperties": false
              }
            },
            {
              "type": "function",
              "name": "request_user_input_async",
              "description": "Synthetic fixture text.",
              "strict": false,
              "parameters": {
                "type": "object",
                "properties": {
                  "questions": {
                    "type": "array",
                    "description": "Synthetic fixture text.",
                    "items": {
                      "type": "object",
                      "properties": {
                        "options": {
                          "type": "array",
                          "description": "Synthetic fixture text.",
                          "items": {
                            "type": "string"
                          },
                          "minItems": 1
                        },
                        "title": {
                          "type": "string",
                          "description": "Synthetic fixture text."
                        }
                      },
                      "required": ["title"],
                      "additionalProperties": false
                    },
                    "minItems": 1
                  }
                },
                "required": ["questions"],
                "additionalProperties": false
              }
            }
          ]
        },
        {
          "type": "namespace",
          "name": "clock",
          "description": "Synthetic fixture text.",
          "tools": [
            {
              "type": "function",
              "name": "sleep",
              "description": "Synthetic fixture text.",
              "strict": false,
              "parameters": {
                "type": "object",
                "properties": {
                  "duration_ms": {
                    "type": "number",
                    "description": "Synthetic fixture text."
                  }
                },
                "required": ["duration_ms"],
                "additionalProperties": false
              }
            }
          ]
        },
        {
          "type": "namespace",
          "name": "collaboration",
          "description": "Synthetic fixture text.",
          "tools": [
            {
              "type": "function",
              "name": "followup_task",
              "description": "Synthetic fixture text.",
              "strict": false,
              "parameters": {
                "type": "object",
                "properties": {
                  "message": {
                    "type": "string",
                    "description": "Synthetic fixture text.",
                    "encrypted": true
                  },
                  "target": {
                    "type": "string",
                    "description": "Synthetic fixture text."
                  }
                },
                "required": ["target", "message"],
                "additionalProperties": false
              }
            },
            {
              "type": "function",
              "name": "interrupt_agent",
              "description": "Synthetic fixture text.",
              "strict": false,
              "parameters": {
                "type": "object",
                "properties": {
                  "target": {
                    "type": "string",
                    "description": "Synthetic fixture text."
                  }
                },
                "required": ["target"],
                "additionalProperties": false
              }
            },
            {
              "type": "function",
              "name": "list_agents",
              "description": "Synthetic fixture text.",
              "strict": false,
              "parameters": {
                "type": "object",
                "properties": {
                  "path_prefix": {
                    "type": "string",
                    "description": "Synthetic fixture text."
                  }
                },
                "additionalProperties": false
              }
            },
            {
              "type": "function",
              "name": "send_message",
              "description": "Synthetic fixture text.",
              "strict": false,
              "parameters": {
                "type": "object",
                "properties": {
                  "message": {
                    "type": "string",
                    "description": "Synthetic fixture text.",
                    "encrypted": true
                  },
                  "target": {
                    "type": "string",
                    "description": "Synthetic fixture text."
                  }
                },
                "required": ["target", "message"],
                "additionalProperties": false
              }
            },
            {
              "type": "function",
              "name": "spawn_agent",
              "description": "Synthetic fixture text.",
              "strict": false,
              "parameters": {
                "type": "object",
                "properties": {
                  "fork_turns": {
                    "type": "string",
                    "description": "Synthetic fixture text."
                  },
                  "message": {
                    "type": "string",
                    "description": "Synthetic fixture text.",
                    "encrypted": true
                  },
                  "model": {
                    "type": "string",
                    "description": "Synthetic fixture text."
                  },
                  "reasoning_effort": {
                    "type": "string",
                    "description": "Synthetic fixture text."
                  },
                  "task_name": {
                    "type": "string",
                    "description": "Synthetic fixture text."
                  }
                },
                "required": ["task_name", "message"],
                "additionalProperties": false
              }
            },
            {
              "type": "function",
              "name": "wait_agent",
              "description": "Synthetic fixture text.",
              "strict": false,
              "parameters": {
                "type": "object",
                "properties": {
                  "timeout_ms": {
                    "type": "number",
                    "description": "Synthetic fixture text."
                  }
                },
                "additionalProperties": false
              }
            }
          ]
        }
      ]
    },
    {
      "type": "message",
      "id": "msg_2",
      "role": "developer",
      "content": [
        {
          "type": "input_text",
          "text": "Synthetic fixture text."
        }
      ]
    },
    {
      "type": "message",
      "id": "msg_3",
      "role": "developer",
      "content": [
        {
          "type": "input_text",
          "text": "Synthetic fixture text."
        },
        {
          "type": "input_text",
          "text": "Synthetic fixture text."
        },
        {
          "type": "input_text",
          "text": "Synthetic fixture text."
        }
      ]
    },
    {
      "type": "message",
      "id": "msg_4",
      "role": "developer",
      "content": [
        {
          "type": "input_text",
          "text": "Synthetic fixture text."
        }
      ]
    },
    {
      "type": "message",
      "id": "msg_5",
      "role": "developer",
      "content": [
        {
          "type": "input_text",
          "text": "Synthetic fixture text."
        }
      ]
    },
    {
      "type": "message",
      "id": "msg_6",
      "role": "user",
      "content": [
        {
          "type": "input_text",
          "text": "Synthetic fixture text."
        }
      ]
    },
    {
      "type": "message",
      "id": "msg_7",
      "role": "user",
      "content": [
        {
          "type": "input_text",
          "text": "Synthetic fixture text."
        }
      ]
    },
    {
      "type": "message",
      "status": "completed",
      "id": "msg_9",
      "role": "assistant",
      "content": [
        {
          "type": "output_text",
          "text": "Synthetic fixture text.",
          "annotations": [],
          "logprobs": []
        },
        {
          "type": "output_text",
          "text": "Synthetic fixture text.",
          "annotations": [],
          "logprobs": [
            {
              "token": "S",
              "logprob": -0.1,
              "bytes": [83],
              "top_logprobs": [
                {
                  "token": "S",
                  "logprob": -0.1,
                  "bytes": [83]
                }
              ]
            }
          ]
        }
      ],
      "phase": "final_answer"
    },
    {
      "type": "message",
      "id": "msg_10",
      "role": "user",
      "content": [
        {
          "type": "input_text",
          "text": "Synthetic fixture text."
        }
      ]
    }
  ],
  "tool_choice": "auto",
  "parallel_tool_calls": false,
  "reasoning": {
    "effort": "low",
    "context": "Synthetic fixture text."
  },
  "store": false,
  "include": ["reasoning.encrypted_content"],
  "prompt_cache_key": "fixture_8",
  "text": {
    "verbosity": "low"
  },
  "client_metadata": {
    "session_id": "Synthetic fixture text.",
    "turn_id": "Synthetic fixture text.",
    "x-codex-installation-id": "Synthetic fixture text.",
    "thread_id": "Synthetic fixture text.",
    "root_turn_id": "Synthetic fixture text.",
    "x-codex-turn-metadata": "Synthetic fixture text.",
    "x-codex-window-id": "Synthetic fixture text."
  }
})FIXTURE");const auto original=fixture;
  const rt::Request request("http://fixture.invalid/v1/responses",{.method="POST"});auto task=rq::readResponsesRequest(request,&fixture);const auto parsed=co_await std::move(task);
  ASSERT(std::holds_alternative<rq::ResponsesStepOk<rq::ReadResponsesRequestValue>>(parsed));const auto value=std::get<rq::ResponsesStepOk<rq::ReadResponsesRequestValue>>(parsed).value;
  EXPECT(!rq::validateResponsesRequestFields(value.rawRecord,value.rawRecord));const auto settings=rq::resolveResponsesStreamSettings(value.rawRecord,value.rawRecord);ASSERT(std::holds_alternative<rq::ResponsesStepOk<rq::ResponsesStreamSettings>>(settings));EXPECT(std::get<rq::ResponsesStepOk<rq::ResponsesStreamSettings>>(settings).value.clientWantsStream);
  const auto normalized=rq::normalizeResponsesInput(rt::get(value.rawRecord,"input"));ASSERT(std::holds_alternative<rq::ResponsesStepOk<std::vector<uos::types::ResponseInputItem>>>(normalized));const auto items=std::get<rq::ResponsesStepOk<std::vector<uos::types::ResponseInputItem>>>(normalized).value;const auto actual=json::value_from(items);
  EXPECT_EQ(items.size(),9U);EXPECT(rt::deepEqual(actual.as_array()[0],rt::getArray(fixture,"input")->at(0)));
  const json::object expected{{"type","message"},{"role","assistant"},{"content",json::array{json::object{{"type","output_text"},{"text","Synthetic fixture text."}},json::object{{"type","output_text"},{"text","Synthetic fixture text."}}}}};EXPECT(rt::deepEqual(actual.as_array()[7],expected));
  const auto bodies=rq::buildResponsesUpstreamBodies(std::string(rt::getString(fixture,"model").value()),items,std::nullopt,*rt::getObject(fixture,"reasoning"),value.rawRecord);
  EXPECT(rt::deepEqual(*rt::get(bodies.codexBody,"input"),actual));EXPECT(rt::deepEqual(*rt::get(bodies.removedProviderBody,"input"),actual));const auto serialized=rt::jsonStringify(json::object{{"codexBody",bodies.codexBody},{"removedProviderBody",bodies.removedProviderBody}});EXPECT(serialized.find("\"logprobs\"")==std::string::npos);EXPECT(serialized.find("\"annotations\"")==std::string::npos);EXPECT(rt::deepEqual(fixture,original));
}
