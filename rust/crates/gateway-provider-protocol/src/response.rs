use crate::types::DecodePlan;
use crate::types::Headers;
use crate::types::ResponseDecode;
use crate::types::header;
use gateway_core::provider::UpstreamProvider;

/// Exact source Chat-only Responses producer spelling, not an upstream identity validator.
pub fn translated_response_id(id: &str) -> String {
    let clean = id
        .chars()
        .filter(char::is_ascii_alphanumeric)
        .take(40)
        .collect::<String>();
    format!("resp_{clean}")
}



pub fn valid_provider_request_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 256 && id.bytes().all(|byte| (0x21..=0x7e).contains(&byte))
}



fn first_header<'a>(headers: &'a Headers, names: &[&str]) -> Option<&'a str> {
    names.iter().find_map(|name| header(headers, name))
}



pub fn translated_provider_request_id(provider: UpstreamProvider, headers: &Headers) -> Option<&str> {
    // Source selects the first PRESENT header, then validates it. A malformed
    // preferred value must not silently select a later header.
    let names: &[&str] = match provider {
        UpstreamProvider::Deepseek => &["x-request-id", "x-ds-request-id", "x-deepseek-request-id"],
        UpstreamProvider::Cerebras => {
            &["x-request-id", "x-api-request-id", "x-cerebras-request-id"]
        }
        // Lithos deliberately has no documented response request-ID header.
        _ => return None,
    };
    first_header(headers, names).filter(|id| valid_provider_request_id(id))
}



/// Bind actual source header identity once, before both buffer and stream translation.
/// Native Responses and ordinary Chat retain the upstream's own body/event IDs.
pub fn response_plan(plan: &DecodePlan, headers: &Headers) -> DecodePlan {
    let mut result = plan.clone();
    let source_id = match plan.decode {
        ResponseDecode::ChatToResponses(_) => {
            translated_provider_request_id(plan.provider, headers)
        }
        ResponseDecode::AnthropicChatToResponses => first_header(
            headers,
            &["x-request-id", "x-api-request-id", "x-oneapi-request-id"],
        )
        .map(gateway_schema::util::js_trim)
        .filter(|id| !id.is_empty()),
        ResponseDecode::AnthropicToResponses => match plan.provider {
            UpstreamProvider::Metered => {
                ["x-api-request-id", "x-oneapi-request-id", "x-request-id"]
                    .into_iter()
                    .filter_map(|name| header(headers, name).map(gateway_schema::util::js_trim))
                    .find(|id| !id.is_empty())
            }
            UpstreamProvider::ClaudeCode => first_header(headers, &["request-id", "x-request-id"])
                .map(gateway_schema::util::js_trim)
                .filter(|id| !id.is_empty()),
            _ => None,
        },
        _ => None,
    };
    if let Some(id) = source_id {
        result.response_id = if plan.decode == ResponseDecode::AnthropicToResponses {
            native_response_id(id, &plan.response_id)
        } else {
            translated_response_id(id)
        };
    }
    result
}



pub fn native_response_id(id: &str, fallback: &str) -> String {
    let id = translated_response_id(id);
    if id == "resp_" { fallback.into() } else { id }
}
