// Auth-owned response MAC domains; parsed wire DATA remains non-authoritative.
use crate::native::signature_input;
use gateway_auth_native_wire::{
    control::{NATIVE_CONTROL_ERROR_MAX_BYTES, NATIVE_CONTROL_MAX_BYTES},
    native_read::{NATIVE_CONTROL_PATH, NATIVE_RENEWAL_AUTHORITY_PATH},
};
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_core::error::GatewayError;

pub fn control_response_input(
    key_id: &str,
    nonce: &str,
    request: &[u8],
    status: u16,
    response: &[u8],
) -> Result<Vec<u8>, GatewayError> {
    signature_input(NATIVE_CONTROL_PATH, key_id, 0, nonce, request)?;
    if !(200..=599).contains(&status)
        || response.len() > NATIVE_CONTROL_MAX_BYTES
        || status >= 400 && response.len() > NATIVE_CONTROL_ERROR_MAX_BYTES
    {
        return Err(invalid());
    }
    Ok(format!(
        "uos-native-control-response-v1\0POST\0{NATIVE_CONTROL_PATH}\0{key_id}\0{nonce}\0{}\0{status}\0{}",
        sha256_hex(request), sha256_hex(response)
    ).into_bytes())
}

pub fn authority_response_input(
    key_id: &str,
    nonce: &str,
    request: &[u8],
    status: u16,
    response: &[u8],
) -> Result<Vec<u8>, GatewayError> {
    signature_input(NATIVE_RENEWAL_AUTHORITY_PATH, key_id, 0, nonce, request)?;
    let response_limit =
        gateway_auth_native_import_data::native_import::authority::new_response_limit(
            request, status, response,
        )?
        .unwrap_or(NATIVE_CONTROL_MAX_BYTES);
    if !(200..=599).contains(&status)
        || response.len() > response_limit
        || status >= 400 && response.len() > NATIVE_CONTROL_ERROR_MAX_BYTES
    {
        return Err(invalid());
    }
    Ok(format!("uos-native-renewal-authority-response-v1\0POST\0{NATIVE_RENEWAL_AUTHORITY_PATH}\0{key_id}\0{nonce}\0{}\0{status}\0{}",sha256_hex(request),sha256_hex(response)).into_bytes())
}

fn invalid() -> GatewayError {
    GatewayError::new(
        400,
        "Invalid native control request",
        Some("native_control_invalid"),
    )
}
