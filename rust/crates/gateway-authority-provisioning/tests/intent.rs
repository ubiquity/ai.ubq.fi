use gateway_auth::crypto::sha256_hex;
use gateway_authority_provisioning::intent::{Intent, Scope, token};
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
