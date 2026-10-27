//! Current Claude subscription block projection, retaining source property order and UTF16 leaves.
use gateway_auth::types::AuthResponseBody;
use gateway_core::{error::GatewayError, source_json::SourceJson};
use serde_json::Value;
pub fn subscription_block(body: AuthResponseBody) -> Result<SourceJson, GatewayError> {
    match body {
        AuthResponseBody::Json(value) => {
            SourceJson::from_serializable(value.get("claude_code").unwrap_or(&Value::Null))
        }
        AuthResponseBody::Empty => SourceJson::parse("null"),
        AuthResponseBody::SourceJson(value) => Ok(value
            .object_entries()?
            .into_iter()
            .rev()
            .find(|(key, _)| {
                key.as_units()
                    .iter()
                    .copied()
                    .eq("claude_code".encode_utf16())
            })
            .map(|(_, value)| value)
            .unwrap_or(SourceJson::parse("null")?)),
    }
}

#[cfg(test)]
mod tests {
    use super::subscription_block;
    use gateway_auth::types::{AuthResponse, AuthResponseBody};
    use gateway_core::source_json::SourceJson;
    use serde_json::{Value, json};
    #[test]
    fn admin_readback_preserves_both_unpaired_utf16_directions_in_raw_quota() {
        let raw = r#"{"claude_code":{"accounts":[{"quota":{"last_error":{"message":"high\ud800"}}}],"quota":{"last_error":{"message":"low\udfff"}}}}"#;
        assert!(serde_json::from_str::<Value>(raw).is_err());
        let block = subscription_block(AuthResponseBody::SourceJson(
            SourceJson::parse(raw).unwrap(),
        ))
        .unwrap();
        let body =
            SourceJson::from_serializable(&std::collections::BTreeMap::from([("data", block)]))
                .unwrap();
        let response = AuthResponse::source_json(200, body);
        assert_eq!(response.headers["cache-control"], "no-store");
        let bytes = response.body.to_bytes().unwrap();
        let text = String::from_utf8(bytes).unwrap();
        assert!(text.contains(r#""message":"high\ud800""#));
        assert!(text.contains(r#""message":"low\udfff""#));
        assert!(!text.contains(r#"\\ud800"#));
    }

    #[test]
    fn state_readback_uses_the_last_source_property_and_missing_block_is_null() {
        let body = SourceJson::parse(
            r#"{"claude_code":{"configured":false},"claude_code":{"configured":true}}"#,
        )
        .unwrap();
        assert_eq!(
            subscription_block(AuthResponseBody::SourceJson(body))
                .unwrap()
                .as_str(),
            r#"{"configured":true}"#
        );
        assert_eq!(
            subscription_block(AuthResponseBody::Json(json!({})))
                .unwrap()
                .as_str(),
            "null"
        );
    }
}
