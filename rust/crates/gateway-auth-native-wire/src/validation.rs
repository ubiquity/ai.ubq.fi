//! Exact source scalar/nullable/error domain shared by closed native wire DATA.
use gateway_source::error::GatewayError;
use serde::Deserialize;

pub(crate) fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native import preparation is unavailable, changed, expired, or unsupported.",
        "native_import_authority_unavailable",
    )
}
pub(crate) fn required_nullable<'de, T: Deserialize<'de>, D: serde::Deserializer<'de>>(
    d: D,
) -> Result<Option<T>, D::Error> {
    Option::<T>::deserialize(d)
}
pub(crate) fn text(value: &str) -> Result<(), GatewayError> {
    if !value.trim().is_empty() && value.len() <= 4096 {
        Ok(())
    } else {
        Err(GatewayError::unavailable(
            "Missing or oversized authority identity.",
            "authority_state_invalid",
        ))
    }
}
pub(crate) fn hash(value: &str) -> Result<(), GatewayError> {
    if digest(value) {
        Ok(())
    } else {
        Err(GatewayError::unavailable(
            "Native renewal authority is missing, stale, or unavailable.",
            "native_renewal_authority_unavailable",
        ))
    }
}
// Exact existing schema parser's Number/trim domain, now co-located with this
// pure wire DTO. The schema-dependent normalizer remains in gateway-schema.
pub fn valid_client_version(raw: &str) -> bool {
    let whitespace = |c: char| matches!(c,'\u{0009}'..='\u{000d}'|'\u{0020}'|'\u{00a0}'|'\u{1680}'|'\u{2000}'..='\u{200a}'|'\u{2028}'|'\u{2029}'|'\u{202f}'|'\u{205f}'|'\u{3000}'|'\u{feff}');
    let parts = raw.trim_matches(whitespace).split('.').collect::<Vec<_>>();
    parts.len() == 3
        && parts.iter().all(|part| {
            !part.is_empty()
                && part.bytes().all(|b| b.is_ascii_digit())
                && part
                    .parse::<u64>()
                    .ok()
                    .is_some_and(|n| n <= 9_007_199_254_740_991)
        })
}

fn digest(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

pub(crate) fn invalid(message: &str) -> GatewayError {
    GatewayError::new(400, message, Some("invalid_request_error"))
}
