use gateway_source::{js_string::JsString16, source_json::SourceJson};
use serde::Deserialize;

#[derive(Deserialize)]
struct Case {
    units: Vec<u16>,
    json: String,
}

#[test]
fn public_encoder_matches_frozen_javascript_tokens_and_all_source_units()
-> Result<(), Box<dyn std::error::Error>> {
    let cases: Vec<Case> = serde_json::from_str(include_str!(
        "../../gateway-core/tests/fixtures/js-string-tokens.json"
    ))?;
    for case in cases {
        let source = JsString16::from_units(case.units);
        let token = source.json_token();
        assert_eq!(token, case.json);
        assert_eq!(
            SourceJson::parse(&token)?.deserialize::<JsString16>()?,
            source
        );
    }
    let source = JsString16::from_units((0..=u16::MAX).collect());
    let token = source.json_token();
    assert_eq!(
        token,
        include_str!("../../gateway-core/tests/fixtures/all-code-units-source.txt")
    );
    assert_eq!(
        SourceJson::parse(&token)?.deserialize::<JsString16>()?,
        source
    );
    assert_ne!(
        JsString16::from_utf8(r"\ud800").json_token(),
        JsString16::from_units(vec![0xd800]).json_token()
    );
    Ok(())
}

#[test]
fn exposing_the_token_encoder_keeps_open_json_construction_fallible() {
    struct Refused;
    impl serde::Serialize for Refused {
        fn serialize<S: serde::Serializer>(&self, _: S) -> Result<S::Ok, S::Error> {
            Err(serde::ser::Error::custom("synthetic refusal"))
        }
    }
    let map = std::collections::BTreeMap::from([(vec![1_u8], "value")]);
    assert!(SourceJson::from_serializable(&Refused).is_err());
    assert!(SourceJson::from_serializable(&map).is_err());
    assert!(SourceJson::parse("{not JSON}").is_err());
    assert!(SourceJson::parse_bytes(&[255]).is_err());
}
