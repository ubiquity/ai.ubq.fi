// Auth-owned native MAC framing and exact typed reply-bound selection.
use gateway_auth_native_wire::native_read::{
    NATIVE_CONTROL_PATH, NATIVE_FOLLOW_PATH, NATIVE_MAX_BODY_BYTES, NATIVE_PUBLICATION_PATH,
    NATIVE_RENEWAL_AUTHORITY_PATH, NATIVE_RPC_PATH,
};
use gateway_auth_primitives::{
    crypto::{base64url, decode_base64url, sha256_hex},
    error::invalid,
};
use gateway_core::error::GatewayError;

/// HMAC-SHA256 authenticates these exact bytes with a dedicated constructor key.
/// Platforms supply real HMAC primitives; body bytes are never reserialized.
pub fn signature_input(
    path: &str,
    key_id: &str,
    time_ms: i64,
    nonce: &str,
    body: &[u8],
) -> Result<Vec<u8>, GatewayError> {
    if body.len() > NATIVE_MAX_BODY_BYTES {
        return Err(invalid("Invalid native authentication frame"));
    }
    let authority_limit = if path == NATIVE_RENEWAL_AUTHORITY_PATH {
        gateway_auth_native_import_data::native_import::authority::new_request_limit(body)?
            .unwrap_or(gateway_auth_native_wire::control::NATIVE_AUTHORITY_MAX_BYTES)
    } else {
        gateway_auth_native_wire::control::NATIVE_AUTHORITY_MAX_BYTES
    };
    if !matches!(
        path,
        NATIVE_RPC_PATH
            | NATIVE_FOLLOW_PATH
            | NATIVE_PUBLICATION_PATH
            | NATIVE_CONTROL_PATH
            | NATIVE_RENEWAL_AUTHORITY_PATH
    ) || key_id.is_empty()
        || key_id.len() > 64
        || !key_id
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'))
        || !(0..=gateway_core::identity::MAX_SAFE_INTEGER).contains(&time_ms)
        || body.len() > NATIVE_MAX_BODY_BYTES
        || path == NATIVE_CONTROL_PATH
            && body.len() > gateway_auth_native_wire::control::NATIVE_CONTROL_MAX_BYTES
        || path == NATIVE_RENEWAL_AUTHORITY_PATH && body.len() > authority_limit
    {
        return Err(invalid("Invalid native authentication frame"));
    }
    let decoded =
        decode_base64url(nonce).map_err(|_| invalid("Invalid native authentication nonce"))?;
    if decoded.len() != 32 || base64url(&decoded) != nonce {
        return Err(invalid("Invalid native authentication nonce"));
    }
    Ok(format!(
        "uos-native-http-v1\0POST\0{path}\0{key_id}\0{time_ms}\0{nonce}\0{}",
        sha256_hex(body)
    )
    .into_bytes())
}

pub fn publication_response_input(
    key_id: &str,
    request_nonce: &str,
    request_body: &[u8],
    status: u16,
    response_body: &[u8],
) -> Result<Vec<u8>, GatewayError> {
    signature_input(
        NATIVE_PUBLICATION_PATH,
        key_id,
        0,
        request_nonce,
        request_body,
    )?;
    let response_limit =
        crate::native_import::publication::new_reply_limit(request_body, status, response_body)?
            .unwrap_or(65_536);
    if !(200..=599).contains(&status) || response_body.len() > response_limit {
        return Err(invalid("Invalid native publication response frame"));
    }
    Ok(format!("uos-native-publication-response-v1\0POST\0{NATIVE_PUBLICATION_PATH}\0{key_id}\0{request_nonce}\0{}\0{status}\0{}",sha256_hex(request_body),sha256_hex(response_body)).into_bytes())
}
