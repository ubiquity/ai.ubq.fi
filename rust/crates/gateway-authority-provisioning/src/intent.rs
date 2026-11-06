//! One-time operator intent. The Worker holds only the SHA-256 of a token the
//! operator minted for exactly one scope; the token itself travels in one
//! request header of one operator command and is never stored by the Worker.
use crate::model::{Authorization, Purpose};
use gateway_core::error::GatewayError;

pub const INTENT_HEADER: &str = "x-uos-state-transfer-intent";
pub const INTENT_SETTING: &str = "UOS_STATE_TRANSFER_INTENT_SHA256";
const PREFIX: &str = "uos-state-transfer-v1";

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Scope {
    /// Stage exactly this content-addressed capture into an empty target.
    Import { capture_id: String },
    /// Read the current canonical records for a latest inverse.
    Export,
    Control {
        purpose: Purpose,
        authorization_digest: String,
    },
}

#[derive(Debug)]
pub struct Intent {
    scope: Scope,
    configured_digest: String,
}

pub fn hex(value: &str, length: usize) -> bool {
    value.len() == length
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn refused() -> GatewayError {
    GatewayError::new(
        403,
        "The state-transfer operator intent is missing, malformed or not the configured one",
        Some("state_transfer_intent_refused"),
    )
}

/// Compare every byte so a mismatch position is not observable by timing.
fn same(left: &str, right: &str) -> bool {
    left.len() == right.len()
        && left
            .bytes()
            .zip(right.bytes())
            .fold(0u8, |difference, (a, b)| difference | (a ^ b))
            == 0
}

/// The canonical token text for a scope and a 32-byte random nonce in hex.
pub fn token(scope: &Scope, nonce: &str) -> Result<String, GatewayError> {
    if !hex(nonce, 64) {
        return Err(refused());
    }
    Ok(match scope {
        Scope::Import { capture_id } if hex(capture_id, 64) => {
            format!("{PREFIX}.import.{capture_id}.{nonce}")
        }
        Scope::Import { .. } => return Err(refused()),
        Scope::Export => format!("{PREFIX}.export.{nonce}"),
        Scope::Control {
            purpose,
            authorization_digest,
        } if hex(authorization_digest, 64) => format!(
            "uos-state-transfer-v2.{}.{authorization_digest}.{nonce}",
            purpose_name(*purpose)
        ),
        Scope::Control { .. } => return Err(refused()),
    })
}

impl Intent {
    /// `configured` is the Worker setting; `None` keeps every route closed.
    pub fn verify(presented: Option<&str>, configured: Option<&str>) -> Result<Self, GatewayError> {
        let configured = configured.map(str::trim).ok_or_else(|| {
            GatewayError::new(
                404,
                "State transfer is not open on this Worker",
                Some("state_transfer_closed"),
            )
        })?;
        let presented = presented.ok_or_else(refused)?;
        if !hex(configured, 64)
            || !same(
                &gateway_auth::crypto::sha256_hex(presented.as_bytes()),
                configured,
            )
        {
            return Err(refused());
        }
        let parts: Vec<&str> = presented.split('.').collect();
        let scope = match parts.as_slice() {
            [PREFIX, "import", capture_id, nonce] if hex(capture_id, 64) && hex(nonce, 64) => {
                Scope::Import {
                    capture_id: (*capture_id).to_owned(),
                }
            }
            [PREFIX, "export", nonce] if hex(nonce, 64) => Scope::Export,
            ["uos-state-transfer-v2", purpose, digest, nonce]
                if hex(digest, 64) && hex(nonce, 64) =>
            {
                Scope::Control {
                    purpose: parse_purpose(purpose)?,
                    authorization_digest: (*digest).into(),
                }
            }
            _ => return Err(refused()),
        };
        Ok(Self {
            scope,
            configured_digest: configured.into(),
        })
    }
    pub fn scope(&self) -> &Scope {
        &self.scope
    }
    pub fn import_capture(&self) -> Result<&str, GatewayError> {
        match &self.scope {
            Scope::Import { capture_id } => Ok(capture_id),
            _ => Err(refused()),
        }
    }
    pub fn require_export(&self) -> Result<(), GatewayError> {
        match self.scope {
            Scope::Export => Ok(()),
            _ => Err(refused()),
        }
    }
}

pub fn purpose_name(value: Purpose) -> &'static str {
    match value {
        Purpose::FinalizeImport => "finalize_import",
        Purpose::Drain => "drain",
        Purpose::Activate => "activate",
    }
}
fn parse_purpose(value: &str) -> Result<Purpose, GatewayError> {
    match value {
        "finalize_import" => Ok(Purpose::FinalizeImport),
        "drain" => Ok(Purpose::Drain),
        "activate" => Ok(Purpose::Activate),
        _ => Err(refused()),
    }
}
pub fn authorization_digest(value: &Authorization) -> Result<String, GatewayError> {
    let value = serde_json::to_value(value).map_err(|_| refused())?;
    Ok(gateway_auth::crypto::sha256_hex(
        format!(
            "uos-state-transfer-authorization-v2\0{}",
            canonical(&value)?
        )
        .as_bytes(),
    ))
}
fn canonical(value: &serde_json::Value) -> Result<String, GatewayError> {
    match value {
        serde_json::Value::Array(values) => Ok(format!(
            "[{}]",
            values
                .iter()
                .map(canonical)
                .collect::<Result<Vec<_>, _>>()?
                .join(",")
        )),
        serde_json::Value::Object(values) => {
            let mut keys: Vec<_> = values.keys().collect();
            keys.sort_by(|a, b| a.encode_utf16().cmp(b.encode_utf16()));
            Ok(format!(
                "{{{}}}",
                keys.into_iter()
                    .map(|key| Ok(format!(
                        "{}:{}",
                        serde_json::to_string(key).map_err(|_| refused())?,
                        canonical(&values[key])?
                    )))
                    .collect::<Result<Vec<_>, GatewayError>>()?
                    .join(",")
            ))
        }
        _ => serde_json::to_string(value).map_err(|_| refused()),
    }
}
impl Intent {
    pub fn configured_digest(&self) -> &str {
        &self.configured_digest
    }
    pub fn require_control(&self, value: &Authorization) -> Result<&str, GatewayError> {
        let expected = match value {
            Authorization::FinalizeImport(_) => Purpose::FinalizeImport,
            Authorization::Drain(_) => Purpose::Drain,
            Authorization::Activate(_) => Purpose::Activate,
        };
        match &self.scope {
            Scope::Control {
                purpose,
                authorization_digest: digest,
            } if *purpose == expected && same(digest, &authorization_digest(value)?) => {
                Ok(&self.configured_digest)
            }
            _ => Err(refused()),
        }
    }
}
