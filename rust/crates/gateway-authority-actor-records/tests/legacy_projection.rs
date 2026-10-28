//! Historical projection DATA boundaries; this does not manufacture authentication or a live actor.
use gateway_auth_passport_contracts::{
    actor_data::{PassportAppSessionBinding, PassportAppSessionData, PassportSessionData},
    passport::PassportClaims,
    session::{PassportSessionIdentity, PassportSourceClaims},
};
use gateway_authority_actor_records::legacy::passport_source;
use gateway_source::js_string::JsString16;

type TestResult = Result<(), Box<dyn std::error::Error>>;

fn claims() -> Result<PassportSourceClaims, serde_json::Error> {
    let legacy: PassportClaims = serde_json::from_str(
        r#"{"iss":"https://passport.ubq.fi","sub":"subject","handle":"handle","isAdmin":true,"scopes":["scope"],"aud":"ai","clientId":"ai","auth":"passkey","iat":1,"exp":2000,"jti":"jti"}"#,
    )?;
    Ok(PassportSourceClaims::from_legacy(legacy))
}

#[test]
fn scalar_jwt_projection_retains_original_v1_bytes_and_mandatory_expiry() -> TestResult {
    let projected = passport_source(&PassportSessionData::Jwt(claims()?))?;
    assert_eq!(
        serde_json::to_vec(&projected)?,
        br#"{"kind":"passport","sub":"subject","jti":"jti","expires_at_ms":2000000}"#
    );
    let mut overflow = claims()?;
    overflow.exp = i64::MAX;
    assert!(passport_source(&PassportSessionData::Jwt(overflow)).is_err());
    Ok(())
}

#[test]
fn legacy_projection_refuses_every_original_nonscalar_claim_domain_and_agent() -> TestResult {
    for field in ["sub", "jti", "handle", "scope", "auth"] {
        let mut value = claims()?;
        let lone = JsString16::from_units(vec![0xd800]);
        match field {
            "sub" => value.sub = lone,
            "jti" => value.jti = lone,
            "handle" => value.handle = lone,
            "scope" => value.scopes = vec![lone],
            "auth" => value.auth = lone,
            _ => return Err("unknown fixture field".into()),
        }
        assert!(passport_source(&PassportSessionData::Jwt(value)).is_err());
    }
    let mut agent = claims()?;
    agent.agent_token_id = Some(JsString16::from_utf8("agent"));
    assert!(passport_source(&PassportSessionData::Jwt(agent)).is_err());
    let mut nonadmin = claims()?;
    nonadmin.is_admin = false;
    assert!(passport_source(&PassportSessionData::Jwt(nonadmin)).is_err());
    Ok(())
}

#[test]
fn permanent_app_data_never_acquires_a_legacy_jti_or_expiry() {
    let subject = JsString16::from_utf8("subject");
    let app = PassportAppSessionData {
        identity: PassportSessionIdentity {
            subject: subject.clone(),
            handle: JsString16::default(),
            is_admin: true,
            scopes: vec![],
        },
        binding: PassportAppSessionBinding {
            session_digest: "a".repeat(64),
            row_revision: "row".into(),
            subject,
            created_at: JsString16::from_utf8("10/27/2026"),
            app_token_digest: "b".repeat(64),
            client_id: "ai".into(),
            audience: "ai".into(),
            origin: "https://ai.ubq.fi".into(),
            redirect_uri: "https://ai.ubq.fi/auth/callback".into(),
        },
    };
    assert!(passport_source(&PassportSessionData::App(app)).is_err());
}
