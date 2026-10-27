use gateway_contracts::model::ModelId;
use gateway_native_authority_wire::context::NativeRenewalContext;
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};
#[test]
fn inference_raw_scalar_and_utf16_tokens_roundtrip_with_kind_anywhere() {
    for token in [
        r#""fixture-model""#,
        r#""\ud800""#,
        r#""\udc00""#,
        r#""�""#,
        r#""\ud83d\ude80""#,
        r#""\u0000\n\"\\""#,
        r#""\\ud800""#,
    ] {
        let raw=SourceJson::parse(&format!(r#"{{"model":{token},"slot":0,"kind":"inference","attempt_id":"attempt","original_admission_digest":"digest"}}"#)).unwrap();
        let expected: ModelId = source::member(&raw, "model")
            .unwrap()
            .deserialize()
            .unwrap();
        let context: NativeRenewalContext = raw.deserialize().unwrap();
        let NativeRenewalContext::Inference { model, .. } = &context else {
            panic!("inference")
        };
        assert_eq!(model, &expected);
        let encoded = SourceJson::from_serializable(&context).unwrap();
        let reopened: NativeRenewalContext = encoded.deserialize().unwrap();
        assert_eq!(reopened, context);
        assert_eq!(
            source::member(&encoded, "kind")
                .unwrap()
                .deserialize::<JsString16>()
                .unwrap(),
            JsString16::from_utf8("inference")
        );
    }
}
#[test]
fn every_variant_missing_unknown_duplicate_and_cross_variant_fields_refuse() {
    for raw in [
        r#"{"kind":"inference"}"#,
        r#"{"kind":"live_create"}"#,
        r#"{"kind":"live_mapped"}"#,
        r#"{"kind":"cache_experiment"}"#,
        r#"{"kind":"unknown"}"#,
        r#"{"kind":"live_mapped","call_id":"c","mapping_revision":"r","unexpected":1}"#,
        r#"{"kind":"live_mapped","call_id":"c","mapping_revision":"r","slot":0}"#,
        r#"{"kind":"live_mapped","call_id":"c","call_id":"c","mapping_revision":"r"}"#,
        r#"{"kind":"live_mapped","kind":"live_mapped","call_id":"c","mapping_revision":"r"}"#,
        r#"{"kind":"cache_experiment","unexpected":true}"#,
    ] {
        assert!(
            serde_json::from_str::<NativeRenewalContext>(raw).is_err(),
            "{raw}"
        );
    }
    let mapped = r#"{"kind":"live_mapped","call_id":"c","mapping_revision":"r"}"#;
    let value: NativeRenewalContext = serde_json::from_str(mapped).unwrap();
    assert_eq!(
        SourceJson::from_serializable(&value).unwrap().as_str(),
        mapped
    );
}
#[test]
fn live_create_keeps_required_nullable_model_units_and_exact_wire_shape() {
    let prefix = r#"{"kind":"live_create","binding":{"account_id_hash":"a","credential_version":"c","pool_versionstamp":"p","slot":0,"routing_generation":0},"active_generation":1"#;
    for token in ["null", "[55296,56320,57343]"] {
        let raw = format!("{prefix},\"model_utf16\":{token}}}");
        let value: NativeRenewalContext = serde_json::from_str(&raw).unwrap();
        let encoded = SourceJson::from_serializable(&value).unwrap();
        assert_eq!(
            source::member(&encoded, "model_utf16").unwrap().as_str(),
            token
        );
        assert_eq!(
            encoded.deserialize::<NativeRenewalContext>().unwrap(),
            value
        );
    }
    assert!(serde_json::from_str::<NativeRenewalContext>(&format!("{prefix}}}")).is_err());
}
