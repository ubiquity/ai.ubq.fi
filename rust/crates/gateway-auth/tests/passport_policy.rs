use gateway_auth_passport::passport::*;
use gateway_auth_passport_contracts::passport::*;
use gateway_auth_primitives::crypto::base64url;
use std::collections::BTreeMap;

#[test]
fn preview_config_never_defaults_to_the_production_client() {
    assert_eq!(PassportConfig::from_values(&BTreeMap::new()), None);
    let mut values = BTreeMap::from([
        ("UOS_AI_PASSPORT_CLIENT_ID".to_owned(), "preview".to_owned()),
        (
            "UOS_AI_PASSPORT_ORIGIN".to_owned(),
            "https://preview.example".to_owned(),
        ),
    ]);
    assert_eq!(PassportConfig::from_values(&values), None);
    values.insert(
        "UOS_AI_PASSPORT_REDIRECT_URI".to_owned(),
        "https://preview.example/auth/callback".to_owned(),
    );
    assert_eq!(
        PassportConfig::from_values(&values),
        Some(PassportConfig {
            client_id: "preview".into(),
            audience: "ai".into(),
            origin: "https://preview.example".into(),
            redirect_uri: "https://preview.example/auth/callback".into(),
        })
    );
    values.insert(
        "UOS_AI_PASSPORT_ORIGIN".to_owned(),
        "http://preview.example".to_owned(),
    );
    assert_eq!(PassportConfig::from_values(&values), None);
}

#[test]
fn jwt_shape_requires_three_parts_and_an_algorithm_header() {
    let header = base64url(br#"{"alg":"RS256"}"#);
    assert!(jwt_shaped(&format!("{header}.payload.signature")));
    assert!(!jwt_shaped("opaque"));
    assert!(!jwt_shaped("a.b"));
    assert!(!jwt_shaped(".."));
}
