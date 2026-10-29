use gateway_catalog_input::catalog::{CatalogInput, ProviderCatalog};
use gateway_catalog_records::{native::*, provider::*};
use gateway_model_metadata::metadata::ResolvedMetadata;
use gateway_model_selection::selection::{ModelWhitelist, ProviderSelection};
use gateway_prompt_policy::declaration::FORWARDED_PAYLOAD_DECLARATION;
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};
type TestResult = Result<(), Box<dyn std::error::Error>>;
#[test]
fn paid_native_retains_raw_fragments_null_and_description_absence() -> TestResult {
    let model = SourceJson::parse(
        r#"{"id":"vendor/\ud800","owned_by":{"raw":"\udc00"},"supported_endpoint_types":[null,{"opaque":true}],"description":null}"#,
    )?;
    let row = paid_native_record(&model, &CatalogInput::default())?;
    for key in ["owned_by", "supported_endpoint_types", "description"] {
        assert_eq!(source::member(&row, key), source::member(&model, key));
    }
    let keys: Vec<_> = source::entries(&row)
        .ok_or("fields")?
        .into_iter()
        .take(5)
        .map(|(k, _)| k)
        .collect();
    assert_eq!(
        keys,
        [
            "slug",
            "display_name",
            "description",
            "owned_by",
            "supported_endpoint_types"
        ]
        .map(JsString16::from_utf8)
    );
    let missing = paid_native_record(
        &SourceJson::parse(r#"{"id":"x"}"#)?,
        &CatalogInput::default(),
    )?;
    assert!(source::member(&missing, "description").is_none());
    assert_eq!(
        source::member(&missing, "owned_by"),
        Some(SourceJson::parse("null")?)
    );
    Ok(())
}
#[test]
fn claude_native_keeps_arbitrary_id_and_duplicate_opaque_levels() -> TestResult {
    for id in ["null", "17", "[]", r#"{"opaque":"\ud800"}"#] {
        let model = SourceJson::parse(&format!(
            r#"{{"id":{id},"display_name":null,"supports_disabled_thinking":true,"supported_reasoning_levels":[null,{{"effort":"high"}},"null","provider-future","provider-future"]}}"#
        ))?;
        let row = claude_native_record(&model)?;
        assert_eq!(source::member(&row, "slug"), source::member(&model, "id"));
        assert_eq!(
            source::member(&row, "display_name"),
            Some(SourceJson::parse("null")?)
        );
        let levels = source::member(&row, "supported_reasoning_levels")
            .as_ref()
            .and_then(source::array)
            .ok_or("levels")?;
        let efforts: Vec<_> = levels
            .iter()
            .map(|level| source::member(level, "effort").ok_or("effort"))
            .collect::<Result<_, _>>()?;
        assert_eq!(
            efforts,
            ["none", "null", "provider-future", "provider-future"]
                .map(|value| SourceJson::from_serializable(value).unwrap())
        );
        assert_eq!(
            source::member(&row, "default_reasoning_level"),
            Some(SourceJson::parse(r#""none""#)?)
        );
    }
    Ok(())
}
#[test]
fn resolved_four_fields_preserve_numeric_key_order_duplicates_and_raw_members() -> TestResult {
    let resolved = ResolvedMetadata {
        context_window_tokens: Some(-7),
        max_context_window_tokens: None,
        auto_compact_token_limit_tokens: Some(200000),
        effective_context_window_percent: None,
        supported_reasoning_levels: None,
        default_reasoning_effort: None,
        context_source: "test".into(),
        reasoning_source: "test".into(),
    };
    for native in [false, true] {
        for nulls in [false, true] {
            let key = if native {
                "context_window"
            } else {
                "context_window_tokens"
            };
            let maximum = if native {
                "max_context_window"
            } else {
                "max_context_window_tokens"
            };
            let mut row = SourceJson::parse(&format!(
                r#"{{"2":"two","{key}":1,"{key}":2,"opaque":"\ud800","1":null}}"#
            ))?;
            apply_resolved_fields(&mut row, &resolved, native, nulls)?;
            let fields = source::entries(&row).ok_or("fields")?;
            assert_eq!(fields[0].0, JsString16::from_utf8("1"));
            assert_eq!(fields[1].0, JsString16::from_utf8("2"));
            assert_eq!(fields[2].0, JsString16::from_utf8(key));
            assert_eq!(source::member(&row, key), Some(SourceJson::parse("-7")?));
            assert_eq!(
                source::member(&row, "opaque"),
                Some(SourceJson::parse(r#""\ud800""#)?)
            );
            assert_eq!(
                source::member(&row, maximum),
                if nulls {
                    Some(SourceJson::parse("null")?)
                } else {
                    None
                }
            );
            assert!(source::member(&row, "effective_context_window_percent").is_none());
        }
    }
    Ok(())
}
#[test]
fn native_assembly_retains_unknown_source_and_current_three_field_forwarding() -> TestResult {
    let stored = SourceJson::parse(
        r#"{"future":"\ud800","models":[{"slug":"opaque/\udc00","supported_reasoning_levels":[{"effort":"future","opaque":null}],"auto_compact_token_limit":200000}]}"#,
    )?;
    let input = CatalogInput {
        providers: [(
            "deepseek".into(),
            ProviderCatalog {
                configured: true,
                ..ProviderCatalog::default()
            },
        )]
        .into_iter()
        .collect(),
        selection: Some(ProviderSelection {
            provider_ids: vec!["codex".into(), "deepseek".into()],
            updated_at_ms: 1,
        }),
        ..CatalogInput::default()
    };
    let catalog = native_catalog(Some(&stored), &input)?;
    assert_eq!(
        source::member(&catalog, "future"),
        source::member(&stored, "future")
    );
    let original = source::member(&stored, "models")
        .as_ref()
        .and_then(source::array)
        .ok_or("original")?;
    let rows = source::member(&catalog, "models")
        .as_ref()
        .and_then(source::array)
        .ok_or("rows")?;
    assert_eq!(rows[0], original[0]);
    let forwarding = rows
        .iter()
        .find_map(|row| source::member(row, "forwarding_policy"))
        .ok_or("forwarding")?;
    let keys: Vec<_> = source::entries(&forwarding)
        .ok_or("fields")?
        .into_iter()
        .map(|(k, _)| k)
        .collect();
    assert_eq!(
        keys,
        ["version", "mode", "per_message_limit"].map(JsString16::from_utf8)
    );
    assert_eq!(
        source::member(&forwarding, "version"),
        Some(SourceJson::from_serializable(
            FORWARDED_PAYLOAD_DECLARATION.version
        )?)
    );
    assert!(source::member(&forwarding, "summary_exemption").is_none());
    let hidden = CatalogInput {
        providers: [(
            "openlux".into(),
            ProviderCatalog {
                models: vec![SourceJson::parse(
                    r#"{"id":"paid","supported_endpoint_types":["openai-response"]}"#,
                )?],
                ..ProviderCatalog::default()
            },
        )]
        .into_iter()
        .collect(),
        selection: Some(ProviderSelection {
            provider_ids: vec!["openlux".into()],
            updated_at_ms: 1,
        }),
        whitelist: Some(
            SourceJson::parse(r#"{"model_ids":["other"],"updated_at_ms":1}"#)?
                .deserialize::<ModelWhitelist>()?,
        ),
        ..CatalogInput::default()
    };
    let filtered = native_catalog_fallback(&hidden)?.ok_or("assembled before filtering")?;
    assert_eq!(
        source::member(&filtered, "models"),
        Some(SourceJson::parse("[]")?)
    );
    Ok(())
}

#[test]
fn checked_metadata_encoding_failure_stays_separate_from_absent_codex_record() -> TestResult {
    let input = CatalogInput::default();
    let absent = SourceJson::parse("[]")?;
    assert!(normalized_codex_capability(&input, &absent)?.is_none());
    let mut row = absent.clone();
    let empty = ResolvedMetadata {
        context_window_tokens: None,
        max_context_window_tokens: None,
        auto_compact_token_limit_tokens: None,
        effective_context_window_percent: None,
        supported_reasoning_levels: None,
        default_reasoning_effort: None,
        context_source: "test".into(),
        reasoning_source: "test".into(),
    };
    apply_resolved_fields(&mut row, &empty, false, false)?;
    assert_eq!(row, absent);
    let populated = ResolvedMetadata {
        context_window_tokens: Some(17),
        ..empty
    };
    let error = apply_resolved_fields(&mut row, &populated, false, false)
        .expect_err("non-object metadata must fail checked encoding");
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("source_json_serialization_invalid")
    );
    assert_eq!(row, absent);
    Ok(())
}
