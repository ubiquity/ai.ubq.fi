use gateway_admin::audit;
use gateway_auth::{
    crypto::Secret,
    types::{AuthMethod, AuthPrincipal},
};

#[test]
fn audit_identifies_passport_session_without_persisting_credentials() {
    let claims = serde_json::from_value(serde_json::json!({
        "iss": "https://passport.ubq.fi",
        "sub": "passport-subject",
        "handle": "passport-handle",
        "isAdmin": true,
        "aud": "ai",
        "clientId": "ai",
        "auth": "sso",
        "iat": 1_800_000_000,
        "exp": 1_900_000_000,
        "jti": "passport-jti"
    }))
    .unwrap();
    let principal = AuthPrincipal {
        token: Some(Secret::new("passport-jwt-must-not-persist".into())),
        method: AuthMethod::PassportSession { claims },
        is_admin: true,
        is_super_admin: false,
    };
    let identified = audit::actor(&principal);
    assert_eq!(identified.kind, "passkey_user");
    assert_eq!(identified.principal_id.as_deref(), Some("passport-subject"));
    assert_eq!(
        identified.principal_label.as_deref(),
        Some("passport-handle")
    );
}
