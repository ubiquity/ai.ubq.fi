use gateway_admin::audit;
use gateway_auth::{
    crypto::Secret,
    types::{AuthMethod, AuthPrincipal},
};

#[test]
fn audit_identifies_passport_session_without_persisting_credentials() {
    let passport = AuthPrincipal {
        token: Some(Secret::new("passport-jwt-must-not-persist".into())),
        method: AuthMethod::PassportSession {
            claims: gateway_auth::passport::PassportClaims {
                iss: "https://auth.ubq.fi".into(),
                sub: "passport-subject".into(),
                handle: "passport-handle".into(),
                is_admin: true,
                scopes: Vec::new(),
                aud: "ai".into(),
                client_id: "ai".into(),
                auth: "sso".into(),
                iat: 1_800_000_000,
                exp: 1_900_000_000,
                jti: "passport-jti".into(),
                agent_token_id: None,
            },
        },
        is_admin: true,
        is_super_admin: false,
    };
    let identified = audit::actor(&passport);
    assert_eq!(identified.kind, "passkey_user");
    assert_eq!(identified.principal_id.as_deref(), Some("passport-subject"));
    assert_eq!(
        identified.principal_label.as_deref(),
        Some("passport-handle")
    );
}
