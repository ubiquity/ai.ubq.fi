use gateway_auth_passport_contracts::{
    actor_data::{PassportActorDataV2, PassportAppSessionBinding, PassportJwtBindingV2},
    passport::PassportConfig,
};
use gateway_source::{js_string::JsString16, source_json::SourceJson};

fn binding() -> PassportAppSessionBinding {
    let config = PassportConfig::production();
    PassportAppSessionBinding {
        session_digest: "a".repeat(64),
        row_revision: "opaque revision".into(),
        subject: JsString16::from_units(vec![0xd800, 0, 65]),
        created_at: "October 1, 2026".into(),
        app_token_digest: "b".repeat(64),
        client_id: config.client_id,
        audience: config.audience,
        origin: config.origin,
        redirect_uri: config.redirect_uri,
    }
}
#[test]
fn lossless_app_and_jwt_data_are_explicitly_versioned() -> Result<(), Box<dyn std::error::Error>> {
    for data in [
        PassportActorDataV2::PassportAppSession(binding()),
        PassportActorDataV2::PassportJwt(PassportJwtBindingV2 {
            sub: JsString16::from_units(vec![0xdfff, 0]),
            jti: JsString16::from_units(vec![0xd800]),
            expires_at_ms: 1234,
        }),
    ] {
        let source = data.to_source_json()?;
        assert_eq!(PassportActorDataV2::from_source_json(&source)?, data);
        let bytes = data.framed_bytes()?;
        assert!(bytes.starts_with(b"uos_ai:auth:passport_actor:v2\0"));
        assert!(bytes.ends_with(source.as_bytes()));
        assert!(!source.as_str().contains("is_admin"));
        assert!(!source.as_str().contains("appSessionToken"));
        assert!(
            PassportActorDataV2::from_source_json(&SourceJson::parse(
                &source.as_str().replace("\"version\":2", "\"version\":1")
            )?)
            .is_err()
        );
    }
    for source in [
        r#"{"version":2,"kind":"passport_jwt","data":{"sub":"a","jti":"b"}}"#,
        r#"{"version":2,"kind":"passport_jwt","data":{"sub":"a","jti":"b","expires_at_ms":null}}"#,
        r#"{"version":2,"kind":"passport_app_session","data":{},"authorized":true}"#,
    ] {
        assert!(PassportActorDataV2::from_source_json(&SourceJson::parse(source)?).is_err());
    }
    Ok(())
}
#[test]
fn data_guards_do_not_normalize_source_identity_or_weaken_fixed_digests() {
    let mut b = binding();
    assert!(b.has_valid_shape());
    b.subject = JsString16::from_units(vec![0xd800; 500]);
    assert!(b.has_valid_shape());
    b.subject = JsString16::from_units(vec![0xd800; 501]);
    assert!(!b.has_valid_shape());
    b.subject = "subject".into();
    b.session_digest = "A".repeat(64);
    assert!(!b.has_valid_shape());
    b.session_digest = "a".repeat(64);
    b.app_token_digest = "b".repeat(63);
    assert!(!b.has_valid_shape());
    b.app_token_digest = "b".repeat(64);
    b.origin = "https://attacker.example".into();
    assert!(!b.has_valid_shape());
}
