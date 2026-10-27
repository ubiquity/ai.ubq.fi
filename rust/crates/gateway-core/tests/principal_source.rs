use gateway_core::{
    identity::{PrincipalId, RequestContext, RequestId},
    source_control,
    tuple_key::{KeyPart, TupleKey},
    value::ValueEncoding,
};
use gateway_source::{js_string::JsString16, source_json::SourceJson};

#[test]
fn principal_units_keys_and_explicit_context_roundtrip() -> Result<(), Box<dyn std::error::Error>> {
    let value = JsString16::from_units(vec![112, 0, 0xd800, 0xdc00, 0xdfff]);
    let principal = PrincipalId::from_source(value.clone());
    assert_eq!(principal.as_source(), &value);
    assert!(principal.try_scalar().is_err());
    assert!(serde_json::to_string(&principal).is_err());
    assert_eq!(
        PrincipalId::from_source_json(&principal.to_source_json()?)?,
        principal
    );
    assert_ne!(
        principal,
        PrincipalId::from_source(JsString16::from_units(vec![112, 0, 0xd800, 0xdc00, 0xfffd]))
    );
    let context = RequestContext {
        request_id: RequestId("request".into()),
        principal: principal.clone(),
        key_id: None,
        route: "/v1/responses".into(),
        started_at_ms: 0,
    };
    let source = context.to_source_json()?;
    assert_eq!(RequestContext::from_source_json(&source)?, context);
    assert!(serde_json::from_str::<RequestContext>(source.as_str()).is_err());
    let record = source_control::encode_source_control_record("revision", &source, None)?;
    assert_eq!(record.value_encoding, ValueEncoding::ProfileV2);
    assert_eq!(
        RequestContext::from_source_json(&source_control::decode_source_control_record(&record)?)?,
        context
    );
    let part = KeyPart::source_string_utf16(value.as_units());
    assert_eq!(
        TupleKey::from_parts(&[part])?,
        TupleKey::from_parts(&[KeyPart::String(String::from_utf16_lossy(value.as_units()))])?
    );
    Ok(())
}
#[test]
fn historical_scalar_serde_and_other_id_domains_are_unchanged()
-> Result<(), Box<dyn std::error::Error>> {
    for text in ["", "a\0b", "passport-user:space ", "😀"] {
        let principal = PrincipalId::from_scalar(text.into());
        let legacy = serde_json::to_string(text)?;
        assert_eq!(serde_json::to_string(&principal)?, legacy);
        assert_eq!(serde_json::from_str::<PrincipalId>(&legacy)?, principal);
        assert_eq!(
            principal,
            PrincipalId::from_source(JsString16::from_utf8(text))
        );
    }
    assert!(serde_json::from_str::<RequestId>(r#""\ud800""#).is_err());
    assert!(PrincipalId::from_source_json(&SourceJson::parse("null")?).is_err());
    Ok(())
}
