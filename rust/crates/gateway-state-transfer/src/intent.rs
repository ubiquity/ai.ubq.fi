//! One-time operator intent. The Worker holds only the SHA-256 of a token the
//! operator minted for exactly one scope; the token itself travels in one
//! request header of one operator command and is never stored by the Worker.
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
}

#[derive(Debug)]
pub struct Intent {
    scope: Scope,
}

pub(crate) fn hex(value: &str, length: usize) -> bool {
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
            _ => return Err(refused()),
        };
        Ok(Self { scope })
    }
    pub fn scope(&self) -> &Scope {
        &self.scope
    }
    pub fn import_capture(&self) -> Result<&str, GatewayError> {
        match &self.scope {
            Scope::Import { capture_id } => Ok(capture_id),
            Scope::Export => Err(refused()),
        }
    }
    pub fn require_export(&self) -> Result<(), GatewayError> {
        match self.scope {
            Scope::Export => Ok(()),
            Scope::Import { .. } => Err(refused()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{Intent, Scope, token};
    use gateway_auth::crypto::sha256_hex;
    use gateway_core::error::GatewayError;

    const NONCE: &str = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const CAPTURE: &str = "aa23456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    #[test]
    fn a_configured_import_token_verifies_its_exact_scope_only() -> Result<(), GatewayError> {
        let scope = Scope::Import {
            capture_id: CAPTURE.into(),
        };
        let presented = token(&scope, NONCE)?;
        let configured = sha256_hex(presented.as_bytes());
        let intent = Intent::verify(Some(&presented), Some(&configured))?;
        assert_eq!(intent.scope(), &scope);
        assert!(intent.require_export().is_err());
        assert_eq!(intent.import_capture()?, CAPTURE);
        Ok(())
    }

    #[test]
    fn closed_wrong_or_malformed_intents_refuse() -> Result<(), GatewayError> {
        let export = token(&Scope::Export, NONCE)?;
        let configured = sha256_hex(export.as_bytes());
        let status = |presented: Option<&str>, configured: Option<&str>| {
            Intent::verify(presented, configured)
                .err()
                .map(|e| e.status)
        };
        assert_eq!(status(Some(&export), None), Some(404));
        assert_eq!(status(None, Some(&configured)), Some(403));
        assert_eq!(status(Some("other"), Some(&configured)), Some(403));
        assert_eq!(status(Some(&export), Some("not-a-hash")), Some(403));
        let forged = format!("{export}x");
        let forged_hash = sha256_hex(forged.as_bytes());
        assert_eq!(status(Some(&forged), Some(&forged_hash)), Some(403));
        assert_eq!(status(Some(&export), Some(&configured)), None);
        assert!(token(&Scope::Export, "short").is_err());
        let bad = Scope::Import {
            capture_id: "x".into(),
        };
        assert!(token(&bad, NONCE).is_err());
        Ok(())
    }
}
