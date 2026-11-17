// Differential cases for src/openai-schema.ts: the allowlists, in order, as Bun
// prints them (JSON.stringify of each export).
#include "uos/openai_schema.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

#include <string>

namespace {
  template <class Keys>
  std::string stringify(const Keys& keys) {
    uos::json::array out;
    for (const auto key : keys) out.emplace_back(uos::json::string(key));
    return uos::rt::jsonStringify(uos::json::value(std::move(out)));
  }
}

TEST("differential: CHAT_COMPLETIONS_REQUEST_KEYS matches Bun") {
  EXPECT_EQ(stringify(uos::openai_schema::CHAT_COMPLETIONS_REQUEST_KEYS),
            R"(["audio","frequency_penalty","function_call","functions","logit_bias","logprobs","max_completion_tokens","max_tokens","messages","metadata","modalities","model","moderation","n","parallel_tool_calls","prediction","presence_penalty","prompt_cache_key","prompt_cache_options","prompt_cache_retention","reasoning_effort","response_format","safety_identifier","seed","service_tier","stop","store","stream","stream_options","temperature","tool_choice","tools","top_logprobs","top_p","user","verbosity","web_search_options"])");
}

TEST("differential: RESPONSES_REQUEST_KEYS matches Bun") {
  EXPECT_EQ(stringify(uos::openai_schema::RESPONSES_REQUEST_KEYS),
            R"(["background","context_management","conversation","include","input","instructions","max_output_tokens","max_tool_calls","metadata","model","moderation","parallel_tool_calls","previous_response_id","prompt","prompt_cache_key","prompt_cache_options","prompt_cache_retention","reasoning","safety_identifier","service_tier","store","stream","stream_options","temperature","text","tool_choice","tools","top_logprobs","top_p","truncation","user"])");
}
