use gateway_catalog_input::{catalog::*, runtime_config::*};
use gateway_contracts::model::ModelId;
use gateway_model_selection::selection::ProviderSelection;
use gateway_model_snapshot::snapshot::ModelSnapshot;
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};
type TestResult = Result<(), Box<dyn std::error::Error>>;

#[test]
fn model_rows_keep_utf16_wide_created_and_unrestricted_fallback() -> TestResult {
    for (created, expected) in [
        ("1e100", 1e100),
        ("12.9", 12.0),
        ("-2", -7.0),
        ("null", -7.0),
    ] {
        let record = SourceJson::parse(&format!(
            r#"{{"id":"\ud800","slug":"ignored","owned_by":"\udc00","created":{created}}}"#
        ))?;
        let row = model_list_row(&record, -7)?.ok_or("row")?;
        let fields = source::entries(&row).ok_or("fields")?;
        let keys: Vec<_> = fields.iter().map(|(name, _)| name.clone()).collect();
        assert_eq!(
            keys,
            ["id", "object", "created", "owned_by"].map(JsString16::from_utf8)
        );
        assert_eq!(
            source::member(&row, "id"),
            Some(SourceJson::parse(r#""\ud800""#)?)
        );
        assert_eq!(
            source::member(&row, "owned_by"),
            Some(SourceJson::parse(r#""\udc00""#)?)
        );
        assert_eq!(
            source::member(&row, "created")
                .as_ref()
                .and_then(source::number),
            Some(expected)
        );
    }
    Ok(())
}
#[test]
fn claude_factory_keeps_raw_id_duplicate_tiers_and_boolean_only_default() -> TestResult {
    for disabled in [false, true] {
        let raw = r#"{"id":"\ud800","supported_reasoning_levels":["null","provider-tier","provider-tier"],"supports_disabled_thinking":BOOL}"#;
        let row = SourceJson::parse(&raw.replace("BOOL", if disabled { "true" } else { "false" }))?;
        let input = CatalogInput {
            providers: [(
                "claude_code".into(),
                ProviderCatalog {
                    models: vec![row.clone()],
                    ..ProviderCatalog::default()
                },
            )]
            .into_iter()
            .collect(),
            selection: Some(ProviderSelection {
                provider_ids: vec!["claude_code".into()],
                updated_at_ms: 1,
            }),
            ..CatalogInput::default()
        };
        let id = ModelId::from_source(JsString16::from_units(vec![0xd800]));
        let metadata = request_model_metadata(&input, &id, true, 1)?;
        let record = metadata.record.ok_or("record")?;
        assert_eq!(source::member(&record, "slug"), source::member(&row, "id"));
        let expected = if disabled {
            r#"["none","null","provider-tier","provider-tier"]"#
        } else {
            r#"["null","provider-tier","provider-tier"]"#
        };
        assert_eq!(
            source::member(&record, "supported_reasoning_levels"),
            Some(SourceJson::parse(expected)?)
        );
        assert_eq!(
            source::member(&record, "default_reasoning_level"),
            if disabled {
                Some(SourceJson::parse(r#""none""#)?)
            } else {
                None
            }
        );
        assert_eq!(
            metadata.supported_endpoints,
            Some(vec!["openai".into(), "openai-response".into()])
        );
    }
    Ok(())
}
#[test]
fn provider_serde_raw_domains_and_legitimate_absence_survive_checked_codecs() -> TestResult {
    for raw in [
        "null",
        "[]",
        "17",
        r#""\ud800""#,
        r#"{ "2":null,"opaque":"\udc00" }"#,
    ] {
        let provider = ProviderCatalog {
            models: vec![SourceJson::parse(raw)?],
            ..ProviderCatalog::default()
        };
        let decoded: ProviderCatalog = SourceJson::from_serializable(&provider)?.deserialize()?;
        assert_eq!(decoded, provider);
    }
    assert!(model_list_row(&SourceJson::parse(r#"{"id":null}"#)?, -7)?.is_none());
    assert!(normalize_model_list(&SourceJson::parse("[]")?)?.is_none());
    let empty = normalize_model_list(&SourceJson::parse(r#"{"data":[]}"#)?)?.ok_or("empty list")?;
    assert_eq!(
        source::member(&empty, "data"),
        Some(SourceJson::parse("[]")?)
    );
    let invalid = ModelSnapshot {
        models: Vec::new(),
        source: "source".into(),
        updated_at_ms: 1,
        client_version: None,
    };
    assert!(build_optional_runtime_config(&invalid, None, None, 1)?.is_none());
    assert!(build_runtime_config(&invalid, None, None, 1).is_err());
    Ok(())
}
