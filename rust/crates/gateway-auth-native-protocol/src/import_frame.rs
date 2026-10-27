use gateway_auth_native_contracts::native_import::{NativeImportKind, NativeImportMetadata};
use gateway_auth_primitives::crypto::{base64url, decode_base64url, sha256_hex};
use gateway_core::error::GatewayError;
pub fn signature_input(
    kind: NativeImportKind,
    metadata: &NativeImportMetadata,
    body_sha256: &str,
    key: &str,
    time: i64,
    nonce: &str,
) -> Result<Vec<u8>, GatewayError> {
    metadata.validate()?;
    if key.is_empty()
        || key.len() > 64
        || !key
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'-'))
        || !(0..=gateway_core::identity::MAX_SAFE_INTEGER).contains(&time)
        || !digest(body_sha256)
    {
        return Err(invalid());
    }
    let decoded = decode_base64url(nonce).map_err(|_| invalid())?;
    if decoded.len() != 32 || base64url(&decoded) != nonce {
        return Err(invalid());
    }
    let canonical = serde_json::to_vec(metadata).map_err(|_| invalid())?;
    Ok(format!(
        "uos-native-import-v1\0POST\0{}\0{key}\0{time}\0{nonce}\0{}\0{body_sha256}",
        kind.path(),
        sha256_hex(&canonical)
    )
    .into_bytes())
}
pub(crate) fn digest(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
fn invalid() -> GatewayError {
    GatewayError::new(
        400,
        "Invalid native import metadata",
        Some("native_import_invalid"),
    )
}
