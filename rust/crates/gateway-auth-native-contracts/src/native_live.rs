use gateway_auth_primitives::crypto::{base64url, decode_base64url, sha256_hex};
use gateway_source::error::GatewayError;
pub const NATIVE_LIVE_PATH: &str = "/v1/native/live";
pub const NATIVE_LIVE_AUTHORITY_PATH: &str = "/uos/native/live-authority";
pub const NATIVE_LIVE_ACTION_HEADER: &str = "x-uos-native-live-action";
pub const NATIVE_LIVE_AUTHORITY_MAX_BYTES: usize = 65_536;

pub fn validate_reference(value: &str) -> Result<(), GatewayError> {
    if value.is_empty()
        || value.len() > 256
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
    {
        return Err(invalid());
    }
    Ok(())
}

fn frame_fields(key_id: &str, time_ms: i64, nonce: &str) -> Result<(), GatewayError> {
    if key_id.is_empty()
        || key_id.len() > 64
        || !key_id
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'))
        || !(0..=gateway_contracts::identity::MAX_SAFE_INTEGER).contains(&time_ms)
    {
        return Err(invalid());
    }
    let decoded = decode_base64url(nonce).map_err(|_| invalid())?;
    if decoded.len() != 32 || base64url(&decoded) != nonce {
        return Err(invalid());
    }
    Ok(())
}

pub fn request_signature_input(
    key_id: &str,
    time_ms: i64,
    nonce: &str,
    action_ref: &str,
) -> Result<Vec<u8>, GatewayError> {
    frame_fields(key_id, time_ms, nonce)?;
    validate_reference(action_ref)?;
    Ok(format!("uos-native-live-upgrade-v1\0GET\0{NATIVE_LIVE_PATH}\0{key_id}\0{time_ms}\0{nonce}\0{action_ref}").into_bytes())
}

pub fn authority_signature_input(
    key_id: &str,
    time_ms: i64,
    nonce: &str,
    body: &[u8],
) -> Result<Vec<u8>, GatewayError> {
    frame_fields(key_id, time_ms, nonce)?;
    if body.len() > NATIVE_LIVE_AUTHORITY_MAX_BYTES {
        return Err(invalid());
    }
    Ok(format!("uos-native-live-authority-v1\0POST\0{NATIVE_LIVE_AUTHORITY_PATH}\0{key_id}\0{time_ms}\0{nonce}\0{}", sha256_hex(body)).into_bytes())
}

pub fn authority_response_input(
    key_id: &str,
    nonce: &str,
    request: &[u8],
    status: u16,
    response: &[u8],
) -> Result<Vec<u8>, GatewayError> {
    authority_signature_input(key_id, 0, nonce, request)?;
    if !(100..=599).contains(&status) || response.len() > NATIVE_LIVE_AUTHORITY_MAX_BYTES {
        return Err(invalid());
    }
    Ok(format!("uos-native-live-authority-response-v1\0POST\0{NATIVE_LIVE_AUTHORITY_PATH}\0{key_id}\0{nonce}\0{}\0{status}\0{}", sha256_hex(request), sha256_hex(response)).into_bytes())
}

fn invalid() -> GatewayError {
    GatewayError::new(
        401,
        "Native live authentication failed",
        Some("native_live_auth_invalid"),
    )
}
