#pragma once
// Port of src/deepseek/chat-projection.ts.
#include "uos/deepseek/responses.hpp"
#include "uos/deepseek/forwarded_payload_policy.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include <optional>
#include <string>
#include <vector>
namespace uos::deepseek::chat_projection {
  responses::DeepSeekResponsesResult<json::array> toDeepSeekChatMessages(const json::value* input, const std::optional<std::string>& instructions,
    const forwarded_payload_policy::ForwardedPayloadReduction& reduction, std::vector<forwarded_payload_policy::ForwardedPayloadElision>& elisions,
    std::vector<forwarded_payload_policy::ForwardedPayloadExemption>& exemptions);
  responses::DeepSeekResponsesResult<json::array> toDeepSeekChatMessages(const json::value* input, const std::optional<std::string>& instructions,
    const forwarded_payload_policy::ForwardedPayloadReduction& reduction = "reduce");
  responses::DeepSeekResponsesResult<json::array> toDeepSeekChatMessages(const json::value& input, const std::optional<std::string>& instructions,
    const forwarded_payload_policy::ForwardedPayloadReduction& reduction = "reduce");
  struct DeepSeekResponsesChatBody {
    json::object body;
    rt::OrderedMap<std::string, responses::OriginalToolName> toolNames;
    rt::OrderedSet<std::string> customToolNames;
    std::vector<forwarded_payload_policy::ForwardedPayloadElision> elisions;
    std::vector<forwarded_payload_policy::ForwardedPayloadExemption> exemptions;
    forwarded_payload_policy::PromptAdmission promptAdmission;
    std::vector<std::string> warnings;
  };
  responses::DeepSeekResponsesResult<DeepSeekResponsesChatBody> toDeepSeekResponsesChatBody(const json::object& rawRecord, const std::string& requestedModel,
    bool clientWantsStream, const responses::ChatOnlyResponsesProfile& profile = responses::DEEPSEEK_RESPONSES_PROFILE,
    const forwarded_payload_policy::PromptAdmissionProjection& promptAdmissionProjection = {});
  void logForwardedPayloadElisions(const std::vector<forwarded_payload_policy::ForwardedPayloadElision>& elisions);
}
