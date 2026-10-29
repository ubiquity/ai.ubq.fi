use gateway_model_selection::selection as model_selection;
use gateway_source::{
    error::GatewayError, source_json::SourceJson, source_serialization as source,
};
use serde_json::{Value, json};
use std::collections::BTreeMap;

const PRESENTATION: &[(&str, &str, &str, &str, &str)] = &[
    (
        "codex",
        "Codex",
        "subscription",
        "ChatGPT subscription capacity. The waterfall always tries it first.",
        "codex",
    ),
    (
        "claude_code",
        "Claude Code",
        "subscription",
        "Claude subscription capacity. Tried before the paid waterfall for Claude models when a credential, the principal allowlist, and a workspace are configured.",
        "claude_code",
    ),
    (
        "surplus",
        "Metered 1",
        "paid",
        "Surplus Intelligence. Second tier of the paid waterfall.",
        "surplus",
    ),
    (
        "openlux",
        "Metered 2",
        "paid",
        "OpenLux. Last tier of the paid waterfall.",
        "metered",
    ),
    (
        "deepseek",
        "DeepSeek",
        "direct",
        "Official DeepSeek key, served on Chat Completions upstream and Responses through the gateway's translation.",
        "deepseek",
    ),
    (
        "cerebras",
        "Cerebras",
        "direct",
        "GPT-OSS 120B, served on Chat Completions only.",
        "cerebras",
    ),
    (
        "lithos",
        "LithosAI",
        "direct",
        "LithosAI key, served on Chat Completions upstream and Responses through the gateway's translation.",
        "lithos",
    ),
    (
        "openrouter",
        "OpenRouter",
        "direct",
        "OpenRouter's full served catalogue on both OpenAI wires, plus System One decisions (Typesafe Jev) for the selector-repair design step.",
        "openrouter",
    ),
    (
        "ubiquity",
        "Ubiquity",
        "gateway",
        "Gateway-owned, always-current DeepSeek Flash waterfall: official DeepSeek, OpenRouter, LithosAI, Surplus, and OpenLux when they advertise the current release.",
        "ubiquity",
    ),
];

pub fn project(
    catalog: &SourceJson,
    subscriptions: Vec<Value>,
    secrets: &BTreeMap<String, String>,
) -> Result<Value, GatewayError> {
    let models = source::member(catalog, "models")
        .as_ref()
        .and_then(source::array)
        .ok_or_else(|| {
            GatewayError::unavailable(
                "Provider catalog is invalid.",
                "provider_picker_unavailable",
            )
        })?;
    let mut providers = Vec::new();
    for (id, label, tier, detail, health) in PRESENTATION {
        let provider_source = source::member(catalog, "sources")
            .as_ref()
            .and_then(|sources| source::member(sources, id));
        let count = models
            .iter()
            .filter(|model| {
                source::member(model, "providers")
                    .as_ref()
                    .and_then(source::array)
                    .is_some_and(|rows| {
                        rows.iter().any(|row| {
                            source::member(row, "id")
                                .as_ref()
                                .and_then(source::string)
                                .is_some_and(|value| {
                                    value.as_units().iter().copied().eq(id.encode_utf16())
                                })
                        })
                    })
            })
            .count();
        let gated = match *id {
            "openrouter" => Some(secrets.contains_key(*id)),
            "ubiquity" => Some(
                ["deepseek", "openrouter", "lithos", "surplus", "openlux"]
                    .iter()
                    .any(|id| secrets.contains_key(*id)),
            ),
            _ => None,
        };
        // Generated source-status/bool fields are scalar; model IDs never pass through Value.
        let status = gated
            .map(|yes| if yes { "available" } else { "unavailable" }.to_owned())
            .unwrap_or_else(|| {
                provider_source
                    .as_ref()
                    .and_then(|row| source::member(row, "status"))
                    .and_then(|value| value.deserialize::<String>().ok())
                    .unwrap_or_else(|| "unavailable".into())
            });
        let configured = gated.unwrap_or_else(|| {
            provider_source
                .as_ref()
                .and_then(|row| source::member(row, "configured"))
                .and_then(|value| value.deserialize::<bool>().ok())
                .unwrap_or(status == "available")
        });
        let endpoints = match *id {
            "cerebras" => json!(["/v1/chat/completions"]),
            "ubiquity" => json!(["/v1/responses"]),
            "openrouter" => json!(["/v1/chat/completions", "/v1/responses", "/v1/systemone"]),
            _ => json!(["/v1/responses", "/v1/chat/completions"]),
        };
        let tier_label = match *tier {
            "subscription" => "Subscription",
            "paid" => "Paid fallback",
            "gateway" => "Gateway",
            _ => "Direct",
        };
        let mut row = json!({"id":id,"label":label,"tier":tier,"tier_label":tier_label,"detail":detail,"endpoints":endpoints,"health_key":health,"model_count":count,"status":status,"configured":configured});
        if *id == "codex" {
            row["subscriptions"] = json!(subscriptions);
        }
        providers.push(row);
    }
    Ok(
        json!({"providers":providers,"tiers":[{"id":"subscription","label":"Subscription"},{"id":"paid","label":"Paid fallback"},{"id":"direct","label":"Direct"},{"id":"gateway","label":"Gateway"}]}),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn picker_keeps_roster_and_unknown_sources_without_claiming_gateway_capacity() {
        let result = project(
            &SourceJson::from_serializable(
                &json!({"models":[{"providers":[{"id":"codex"}]}],"sources":{}}),
            )
            .unwrap(),
            vec![],
            &BTreeMap::from([("cerebras".into(), "fixture".into())]),
        )
        .unwrap();
        let providers = result["providers"].as_array().unwrap();
        assert_eq!(providers.len(), model_selection::PROVIDERS.len());
        assert_eq!(providers[0]["model_count"], 1);
        assert_eq!(providers[0]["configured"], false);
        assert_eq!(providers.last().unwrap()["configured"], false);
    }
}

#[cfg(test)]
mod source_tests {
    #[test]
    fn picker_counts_source_models_without_scalarizing_their_labels() {
        let document = gateway_source::source_json::SourceJson::parse(r#"{"models":[{"id":"model\ud800","providers":[{"id":"codex"}]}],"sources":{"codex":{"status":"available","configured":true}}}"#).unwrap();
        let body = super::project(&document, Vec::new(), &Default::default()).unwrap();
        let codex = body["providers"]
            .as_array()
            .unwrap()
            .iter()
            .find(|row| row["id"] == "codex")
            .unwrap();
        assert_eq!(codex["model_count"], 1);
        assert_eq!(codex["status"], "available");
        assert_eq!(codex["configured"], true);
    }
}
