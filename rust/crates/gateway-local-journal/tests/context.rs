use gateway_contracts::identity::{ApiKeyId, PrincipalId, RequestContext, RequestId};
use gateway_local_journal::context::{decode_journal_context, principal_has_non_whitespace};
use gateway_source::{js_string::JsString16, source_json::SourceJson};
use serde::{Deserialize, Serialize};
type TestResult = Result<(), Box<dyn std::error::Error>>;

#[derive(Deserialize, Serialize)]
struct SourceField {
    #[serde(with = "gateway_local_journal::context::source_field")]
    context: RequestContext,
}
#[derive(Serialize)]
struct LegacyField<'a> {
    context: &'a RequestContext,
}
fn context(principal: PrincipalId) -> RequestContext {
    RequestContext {
        request_id: RequestId("request".into()),
        principal,
        key_id: Some(ApiKeyId("key".into())),
        route: "responses".into(),
        started_at_ms: 7,
    }
}
fn journal(
    version: &str,
    context: &SourceJson,
) -> Result<SourceJson, gateway_source::error::GatewayError> {
    SourceJson::parse(&format!(
        r#"{{"v":{version},"context":{}}}"#,
        context.as_str()
    ))
}

#[test]
fn scalar_field_and_original_admission_preimage_keep_historical_bytes() -> TestResult {
    for text in [
        "",
        "a\0b",
        "\u{8}\t\n\u{c}\r\u{1f}\"\\",
        "passport-user:space ",
        "😀",
        "\u{85}\u{feff}",
    ] {
        for key in [None, Some(ApiKeyId("key".into()))] {
            let mut context = context(PrincipalId::from_scalar(text.into()));
            context.key_id = key;
            let old = serde_json::to_vec(&LegacyField { context: &context })?;
            let encoded = SourceJson::from_serializable(&SourceField {
                context: context.clone(),
            })?;
            assert_eq!(encoded.as_bytes(), old);
            let decoded: SourceField = encoded.deserialize()?;
            assert_eq!(decoded.context, context);
            let api: Option<String> = None;
            let old = serde_json::to_vec(&(&context, "original-digest", &api, "attempt"))?;
            let source = context.to_source_json()?;
            let new = serde_json::to_vec(&(&source, "original-digest", &api, "attempt"))?;
            assert_eq!(new, old, "compare actual preimage bytes before hashing");
        }
    }
    Ok(())
}

#[test]
fn actual_logical_version_selects_source_context_without_legacy_fallback() -> TestResult {
    let scalar = context(PrincipalId::from_scalar("principal".into()));
    let source = context(PrincipalId::from_source(JsString16::from_units(vec![
        0xd800, 0, 0xdfff,
    ])));
    for version in ["1", "2", "3"] {
        assert_eq!(
            decode_journal_context(&journal(version, &scalar.to_source_json()?)?)?,
            scalar
        );
        let raw = journal(version, &source.to_source_json()?)?;
        if version == "3" {
            assert_eq!(decode_journal_context(&raw)?, source);
        } else {
            assert!(decode_journal_context(&raw).is_err());
        }
    }
    assert!(serde_json::to_vec(&source).is_err());
    assert!(serde_json::from_str::<RequestContext>(source.to_source_json()?.as_str()).is_err());
    for version in ["0", "4", "1.0", "2.0", "3.0", "null", "\"3\""] {
        assert!(decode_journal_context(&journal(version, &scalar.to_source_json()?)?).is_err());
    }
    assert!(decode_journal_context(&SourceJson::parse(r#"{"context":{}}"#)?).is_err());
    Ok(())
}

#[test]
fn source_field_keeps_other_context_domains_and_required_types() -> TestResult {
    for raw in [
        r#"{"v":3,"context":null}"#,
        r#"{"v":3,"context":{"request_id":"r","principal":null,"key_id":null,"route":"responses","started_at_ms":7}}"#,
        r#"{"v":3,"context":{"request_id":"\ud800","principal":"p","key_id":null,"route":"responses","started_at_ms":7}}"#,
        r#"{"v":3,"context":{"request_id":"r","principal":"p","key_id":"\ud800","route":"responses","started_at_ms":7}}"#,
        r#"{"v":3,"context":{"request_id":"r","principal":"p","key_id":null,"route":"\ud800","started_at_ms":7}}"#,
        r#"{"v":3,"context":{"request_id":"r","principal":"p","key_id":null,"route":"responses","started_at_ms":1.5}}"#,
        r#"{"v":3,"context":{"request_id":"r","key_id":null,"route":"responses","started_at_ms":7}}"#,
    ] {
        assert!(decode_journal_context(&SourceJson::parse(raw)?).is_err());
    }
    Ok(())
}

#[test]
fn non_whitespace_observation_keeps_rust_scalar_trim_and_lone_units() {
    for text in [
        "",
        " \t\n",
        "\u{85}",
        "\u{feff}",
        "\0",
        "\u{2000}\u{2028}\u{3000}",
        "😀",
        " � ",
    ] {
        assert_eq!(
            principal_has_non_whitespace(&PrincipalId::from_scalar(text.into())),
            !text.trim().is_empty()
        );
    }
    for units in [
        vec![0xd800],
        vec![0xdfff],
        vec![32, 0xd800, 32],
        vec![0xd800, 0xdc00],
    ] {
        assert!(principal_has_non_whitespace(&PrincipalId::from_source(
            JsString16::from_units(units)
        )));
    }
    assert_ne!(
        PrincipalId::from_source(JsString16::from_units(vec![0xd800])),
        PrincipalId::from_scalar("�".into())
    );
}
