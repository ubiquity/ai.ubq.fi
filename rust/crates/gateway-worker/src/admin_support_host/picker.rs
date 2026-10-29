//! Credential-bound subscription reads and private email decoding remain request-owned.
use {
    gateway_schema_values::scalar as util,
    serde_json::{Value, json},
};

pub(super) async fn subscriptions(
    store: &gateway_worker_authority_binding::authority::RemoteAuthorityStore,
) -> Vec<Value> {
    let Ok(at) = gateway_worker_storage::store::now_ms() else {
        return Vec::new();
    };
    let Ok(Some(pool)) =
        gateway_auth_credentials::provider_credentials::read_codex_source_pool(store, at).await
    else {
        return Vec::new();
    };
    pool.pool.accounts.iter().enumerate().map(|(index, account)| {
        let email = account_email(account.access_token.expose());
        let slot = (index + 1) as u8;
        json!({"id":format!("codex:{}",gateway_auth_primitives::crypto::sha256_hex(account.account_id.as_bytes())),"label":gateway_capacity_projection::shapes::codex_label(slot,email.as_deref()),"slot":slot})
    }).collect()
}

fn account_email(token: &str) -> Option<String> {
    let encoded = token.split('.').nth(1)?.replace('-', "+").replace('_', "/");
    let bytes = gateway_auth_credentials::provider_credentials::decode_source_base64(&encoded)?;
    let payload: Value = serde_json::from_str(&String::from_utf8_lossy(&bytes)).ok()?;
    let normalize = |value: &Value| {
        let text = util::js_trim(value.as_str()?);
        (!text.is_empty()
            && text.encode_utf16().count() <= 320
            && text.contains('@')
            && !text.chars().any(|ch| ch < '\u{20}' || ch == '\u{7f}'))
        .then(|| text.to_owned())
    };
    normalize(&payload["https://api.openai.com/profile"]["email"])
        .or_else(|| normalize(&payload["email"]))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn picker_recovers_source_email_without_returning_token_material() {
        assert_eq!(
            account_email("header.eyJlbWFpbCI6ImZhbGxiYWNrQGV4YW1wbGUuY29tIn0.signature")
                .as_deref(),
            Some("fallback@example.com")
        );
        assert_eq!(account_email("invalid"), None);
    }
}
