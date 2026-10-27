use gateway_auth_contracts::types::AuthMethod;
use gateway_auth_passport_contracts::{
    actor_data::{PassportAppSessionBinding, PassportAppSessionData, PassportSessionData},
    session::PassportSessionIdentity,
};
use gateway_source::js_string::JsString16;

#[test]
fn historical_scalar_auth_method_keeps_exact_kind_claims_and_mandatory_fields()
-> Result<(), Box<dyn std::error::Error>> {
    let original = r#"{"kind":"passport_session","claims":{"iss":"https://passport.ubq.fi","sub":"subject","handle":"","isAdmin":false,"scopes":[],"aud":"ai","clientId":"ai","auth":"sso","iat":1,"exp":2,"jti":"jti"}}"#;
    let method: AuthMethod = serde_json::from_str(original)?;
    assert_eq!(serde_json::to_string(&method)?, original);
    assert_eq!(
        serde_json::from_value::<AuthMethod>(serde_json::from_str(original)?)?,
        method
    );
    for field in [",\"exp\":2", ",\"jti\":\"jti\""] {
        assert!(serde_json::from_str::<AuthMethod>(&original.replace(field, "")).is_err());
    }
    Ok(())
}
#[test]
fn source_principal_keeps_lone_units_and_app_data_refuses_old_jwt_serde()
-> Result<(), Box<dyn std::error::Error>> {
    let identity = PassportSessionIdentity {
        subject: JsString16::from_units(vec![0xd800, 0]),
        handle: "".into(),
        is_admin: false,
        scopes: vec![],
    };
    let binding = PassportAppSessionBinding {
        session_digest: "a".repeat(64),
        row_revision: "revision".into(),
        subject: identity.subject.clone(),
        created_at: "10/27/2026".into(),
        app_token_digest: "b".repeat(64),
        client_id: "ai".into(),
        audience: "ai".into(),
        origin: "https://ai.ubq.fi".into(),
        redirect_uri: "https://ai.ubq.fi/auth/callback".into(),
    };
    let method = AuthMethod::PassportSession {
        identity: PassportSessionData::App(PassportAppSessionData { identity, binding }),
    };
    let mut expected = "passport-user:".encode_utf16().collect::<Vec<_>>();
    expected.extend([0xd800, 0]);
    assert_eq!(method.principal_id(None).as_source().as_units(), expected);
    assert!(serde_json::to_string(&method).is_err());
    Ok(())
}
