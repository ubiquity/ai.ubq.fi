//! Private live-authority v1. Request selectors never carry original auth or credentials.
use gateway_source::error::GatewayError;
use serde::{Deserialize, Serialize};

pub const NATIVE_LIVE_AUTHORITY_PATH: &str = "/uos/native/live-authority";
pub const NATIVE_LIVE_AUTHORITY_MAX_BYTES: usize = 65_536;

#[derive(Deserialize, Serialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
pub enum NativeLiveAuthorityRequest {
    Authorize {
        v: u8,
        action_ref: String,
        connection_id: String,
    },
    Recheck {
        v: u8,
        action_ref: String,
        connection_id: String,
        authorization_ref: String,
    },
    Retire {
        v: u8,
        action_ref: String,
        connection_id: String,
        authorization_ref: String,
        upstream_status: u16,
    },
}
impl NativeLiveAuthorityRequest {
    pub fn parse(bytes: &[u8]) -> Result<Self, GatewayError> {
        if bytes.len() > NATIVE_LIVE_AUTHORITY_MAX_BYTES {
            return Err(invalid());
        }
        let request: Self = serde_json::from_slice(bytes).map_err(|_| invalid())?;
        let (v, fields, status) = match &request {
            Self::Authorize {
                v,
                action_ref,
                connection_id,
            } => (*v, vec![action_ref, connection_id], None),
            Self::Recheck {
                v,
                action_ref,
                connection_id,
                authorization_ref,
            } => (*v, vec![action_ref, connection_id, authorization_ref], None),
            Self::Retire {
                v,
                action_ref,
                connection_id,
                authorization_ref,
                upstream_status,
            } => (
                *v,
                vec![action_ref, connection_id, authorization_ref],
                Some(*upstream_status),
            ),
        };
        if v != 1
            || fields.iter().any(|s| !reference(s))
            || status.is_some_and(|s| s != 404 && s != 410)
        {
            return Err(invalid());
        }
        Ok(request)
    }
}
pub fn reference(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 256
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b':' | b'.'))
}

fn invalid() -> GatewayError {
    GatewayError::new(
        400,
        "Invalid finite native live frame.",
        Some("native_live_wire_invalid"),
    )
}
