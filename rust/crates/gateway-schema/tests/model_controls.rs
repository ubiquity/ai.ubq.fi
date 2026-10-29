//! Model/effort controls and pinned Bun ordering evidence; not a host Intl or runtime substitute.
use gateway_catalog_input::catalog::{
    CatalogInput, request_model_metadata, validate_model_available,
};
use gateway_contracts::model::ModelId;
use gateway_core::source_control;
use gateway_model_metadata::reasoning::{ModelReasoning, effort_for_codex, resolve_reasoning};
use gateway_model_selection::{
    aliases::SourceStringCollator,
    selection::{AccountModelCatalog, AccountModelsStore, ModelWhitelist},
};
use gateway_model_snapshot::snapshot::{ModelSnapshot, SnapshotOverrides, normalize_snapshot};
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use serde::Deserialize;
use std::cmp::Ordering;

fn raw(text: &str) -> SourceJson {
    SourceJson::parse(text).unwrap()
}
fn string(value: &SourceJson, key: &str) -> JsString16 {
    source::string(&source::member(value, key).unwrap()).unwrap()
}
fn canonical(value: &SourceJson) -> SourceJson {
    source::canonical(value).unwrap()
}
fn model_row(model: &ModelId) -> SourceJson {
    source::object(vec![("slug", model.to_source_json().unwrap())]).unwrap()
}

#[derive(Deserialize)]
struct OrderingFixture {
    bun: String,
    icu: String,
    oracle_ref: String,
    collation_groups: Vec<Vec<JsString16>>,
    verified_comparison_count: usize,
    cases: Vec<OrderingCase>,
}
#[derive(Deserialize)]
struct OrderingCase {
    name: String,
    input: SourceJson,
    model: JsString16,
    at: i64,
    expected: SourceJson,
    #[serde(rename = "expectedKeys")]
    expected_keys: Vec<JsString16>,
}
struct CapturedCollator(Vec<Vec<JsString16>>);
impl SourceStringCollator for CapturedCollator {
    fn compare(&self, left: &JsString16, right: &JsString16) -> Result<Ordering, GatewayError> {
        let rank = |value| self.0.iter().position(|group| group.contains(value));
        Ok(rank(left)
            .expect("comparison must exist in the captured Bun oracle")
            .cmp(&rank(right).expect("comparison must exist in the captured Bun oracle")))
    }
}

#[test]
fn unsupported_survivor_order_matches_pinned_bun_including_equal_collation_and_numeric_keys() {
    let fixture: OrderingFixture = raw(include_str!("fixtures/model-controls-source-v1.json"))
        .deserialize()
        .unwrap();
    assert_eq!(fixture.bun, "1.4.0");
    assert_eq!(fixture.icu, "78.1");
    assert!(
        fixture
            .oracle_ref
            .ends_with("/af9107c1-1ccd-4f48-8833-a5c498098ee7")
    );
    assert_eq!(fixture.verified_comparison_count, 1936);
    assert_eq!(fixture.cases.len(), 4);
    let collator = CapturedCollator(fixture.collation_groups);
    for case in fixture.cases {
        let store = AccountModelsStore::parse(&case.input).unwrap();
        let updated = store
            .record_unsupported(
                "account",
                &ModelId::from_source(case.model),
                case.at,
                &collator,
            )
            .unwrap()
            .to_source_json()
            .unwrap();
        assert_eq!(
            canonical(&updated),
            canonical(&case.expected),
            "{}",
            case.name
        );
        let account =
            source::member(&source::member(&updated, "unsupported").unwrap(), "account").unwrap();
        let keys: Vec<_> = source::entries(&account)
            .unwrap()
            .into_iter()
            .map(|(key, _)| key)
            .collect();
        assert_eq!(keys, case.expected_keys, "{}", case.name);
    }
}

#[test]
fn unavailable_catalog_branch_keeps_exact_model_and_source_error_units() {
    let ids = [
        JsString16::from_units(vec![0xd800]),
        JsString16::from_units(vec![0xdc00]),
        "�".into(),
        "🛰".into(),
        "unknown-model".into(),
        JsString16::from_utf8(&"x".repeat(4097)),
    ];
    for id in ids {
        let model = ModelId::from_source(id.clone());
        for models in [None, Some(Vec::new()), Some(vec![model_row(&model)])] {
            let input = CatalogInput {
                snapshot: models.map(|models| ModelSnapshot {
                    models,
                    source: "codex_cli".into(),
                    updated_at_ms: 1,
                    client_version: None,
                }),
                ..Default::default()
            };
            let metadata = request_model_metadata(&input, &model, true, 1).unwrap();
            validate_model_available(&model, true, &metadata).unwrap();
        }
        let input = CatalogInput {
            snapshot: Some(ModelSnapshot {
                models: vec![model_row(&ModelId::from_scalar("other"))],
                source: "codex_cli".into(),
                updated_at_ms: 1,
                client_version: None,
            }),
            ..Default::default()
        };
        let metadata = request_model_metadata(&input, &model, true, 1).unwrap();
        let error = validate_model_available(&model, true, &metadata).unwrap_err();
        assert_eq!(error.status, 404);
        let wire = SourceJson::parse(error.body().as_str()).unwrap();
        let body = source::member(&wire, "error").unwrap();
        assert_eq!(string(&body, "code"), "model_not_found".into());
        assert_eq!(string(&body, "param"), "model".into());
        let mut message = "The model '".encode_utf16().collect::<Vec<_>>();
        message.extend_from_slice(id.as_units());
        message.extend("' does not exist or is not available through this gateway. Use /v1/models for supported models.".encode_utf16());
        assert_eq!(string(&body, "message").as_units(), message);
    }
}

#[test]
fn open_reasoning_labels_and_wire_maps_never_become_membership_validation() {
    let record = raw(
        r#"{"supported_reasoning_levels":[null,"tier-\ud800",{"effort":"wire-\udc00"},"ultra"],"default_reasoning_level":"tier-\ud800","reasoning_effort_wire_map":{"wire-\udc00":"pass-\ud800","none":"not-none","ultra":"not-max"}}"#,
    );
    let reasoning = ModelReasoning::from_record(Some(&record));
    assert_eq!(reasoning.levels.len(), 4);
    assert_eq!(
        reasoning.default_level,
        Some(string(&record, "default_reasoning_level"))
    );
    assert_eq!(effort_for_codex(&"none".into(), &reasoning), "none".into());
    assert_eq!(effort_for_codex(&"ultra".into(), &reasoning), "max".into());
    for (input, expected) in [
        (r#"{"effort":"wire-\udc00"}"#, raw(r#""pass-\ud800""#)),
        (
            r#"{"effort":"unadvertised-\ud800"}"#,
            raw(r#""unadvertised-\ud800""#),
        ),
        (r#"{"effort":"none"}"#, raw(r#""none""#)),
        (r#"{"effort":"ultra"}"#, raw(r#""max""#)),
    ] {
        let input = raw(input);
        let resolved = resolve_reasoning(Some(&input), &reasoning, &"none".into()).unwrap();
        assert_eq!(resolved.label, string(&input, "effort"));
        assert_eq!(
            string(&resolved.value, "effort"),
            source::string(&expected).unwrap()
        );
    }
    let unknown = ModelReasoning::default();
    assert!(unknown.levels.is_empty());
    for units in [
        vec![0xd800],
        vec![0xdc00],
        vec![0xd83d, 0xde80],
        vec![0],
        "\\ud800".encode_utf16().collect(),
    ] {
        let effort = JsString16::from_units(units);
        let value = source::object(vec![(
            "effort",
            SourceJson::from_serializable(&effort).unwrap(),
        )])
        .unwrap();
        let resolved = resolve_reasoning(Some(&value), &unknown, &"none".into()).unwrap();
        assert_eq!(resolved.label, effort);
        assert_eq!(string(&resolved.value, "effort"), effort);
    }
}

#[test]
fn model_map_record_codec_preserves_distinct_lone_keys_and_source_revision_expiry() {
    let row = raw(
        r#"{"accounts":{"a":{"slugs":["\ud800","\udc00","�","🚀"],"client_version":"0.160.0","updated_at_ms":1700000000000}},"unsupported":{"a":{"\ud800":1700000000001,"\udc00":1700000000002,"�":1700000000003,"\\ud800":1700000000004}}}"#,
    );
    let store = AccountModelsStore::parse(&row).unwrap();
    assert_eq!(store.accounts["a"].slugs.len(), 4);
    assert_eq!(store.unsupported["a"].len(), 4);
    let document = store.to_source_json().unwrap();
    let encoded = source_control::encode_source_control_record(
        "same-source-revision",
        &document,
        Some(1_700_000_060_000),
    )
    .unwrap();
    assert_eq!(encoded.revision, "same-source-revision");
    assert_eq!(encoded.expires_at_ms, Some(1_700_000_060_000));
    let decoded = source_control::decode_source_control_record(&encoded).unwrap();
    assert_eq!(canonical(&decoded), canonical(&row));
    let restored: AccountModelsStore = decoded.deserialize().unwrap();
    assert_eq!(restored, store);
    assert_eq!(
        restored.unavailable(
            &ModelId::from_source(JsString16::from_units(vec![0xd800])),
            &["a".into()],
            1_700_000_000_010
        ),
        ["a"]
    );
    let scalar = raw(r#"{"accounts":{},"unsupported":{"a":{"ordinary":1700000000001}}}"#);
    let scalar_record =
        source_control::encode_source_control_record("scalar", &scalar, None).unwrap();
    assert!(scalar_record.json_value().is_ok());
    assert_eq!(
        canonical(&source_control::decode_source_control_record(&scalar_record).unwrap()),
        canonical(&scalar)
    );
}

#[test]
fn model_whitelist_and_same_version_catalog_evidence_compare_original_units() {
    let model = ModelId::from_source(JsString16::from_units(vec![0xd800]));
    let replacement = ModelId::from_scalar("�");
    let whitelist =
        ModelWhitelist::normalize(&raw(r#"{"model_ids":["\ud800"],"updated_at_ms":1}"#)).unwrap();
    assert!(ModelWhitelist::permits(Some(&whitelist), &model));
    assert!(!ModelWhitelist::permits(Some(&whitelist), &replacement));
    let store = AccountModelsStore::default().record_catalogs(&[
        (
            "a".into(),
            AccountModelCatalog {
                slugs: vec![replacement],
                client_version: "0.160.0".into(),
                updated_at_ms: 1,
            },
        ),
        (
            "b".into(),
            AccountModelCatalog {
                slugs: vec![model.clone()],
                client_version: "0.160.0".into(),
                updated_at_ms: 1,
            },
        ),
    ]);
    assert_eq!(
        store.unavailable(&model, &["a".into(), "b".into()], 1),
        ["a"]
    );
    let mut different_version = store.clone();
    different_version
        .accounts
        .get_mut("b")
        .unwrap()
        .client_version = "0.161.0".into();
    assert!(
        different_version
            .unavailable(&model, &["a".into(), "b".into()], 1)
            .is_empty()
    );
}

#[test]
fn uploaded_snapshot_source_label_is_opaque_and_scalar_default_is_unchanged() {
    for source_label in [
        r#""\ud800""#,
        r#""\udc00""#,
        r#""🚀""#,
        r#""""#,
        r#""plain-source""#,
    ] {
        let label = raw(source_label);
        let envelope = source::object(vec![
            ("models", raw(r#"[{"slug":"\ud800"}]"#)),
            ("source", label.clone()),
        ])
        .unwrap();
        let snapshot = normalize_snapshot(&envelope, &SnapshotOverrides::default(), 1)
            .unwrap()
            .unwrap();
        assert_eq!(snapshot.source, source::string(&label).unwrap());
        assert_eq!(
            snapshot.default_model(),
            Some(ModelId::from_source(JsString16::from_units(vec![0xd800])))
        );
        let wire = SourceJson::from_serializable(&snapshot).unwrap();
        assert_eq!(string(&wire, "source"), source::string(&label).unwrap());
    }
    for input in [
        r#"[{"slug":"ordinary"}]"#,
        r#"{"models":[{"slug":"ordinary"}],"source":null}"#,
    ] {
        let snapshot = normalize_snapshot(&raw(input), &SnapshotOverrides::default(), 1)
            .unwrap()
            .unwrap();
        assert_eq!(snapshot.source, "codex_cli".into());
    }
    let input = raw(r#"{"models":[{"slug":"ordinary"}],"source":"original"}"#);
    let blank_override = SnapshotOverrides {
        source: Some("".into()),
        ..Default::default()
    };
    assert_eq!(
        normalize_snapshot(&input, &blank_override, 1)
            .unwrap()
            .unwrap()
            .source,
        "original".into()
    );
    let opaque = JsString16::from_units(vec![0xdc00]);
    let override_source = SnapshotOverrides {
        source: Some(opaque.clone()),
        ..Default::default()
    };
    assert_eq!(
        normalize_snapshot(&input, &override_source, 1)
            .unwrap()
            .unwrap()
            .source,
        opaque
    );
}

#[test]
fn reasoning_wire_serde_retains_arbitrary_raw_shapes_and_byte_equality() {
    for wire in [
        "null",
        "17",
        r#""raw""#,
        r#"[null,{"effort":"custom"}]"#,
        r#"{"z":null,"\ud800":{"effort":" low "},"0":"opaque"}"#,
    ] {
        let model = raw(&format!(
            "{{\"levels\":[],\"default_level\":null,\"wire_efforts\":{wire}}}"
        ))
        .deserialize::<ModelReasoning>()
        .unwrap();
        let encoded = SourceJson::from_serializable(&model).unwrap();
        assert_eq!(
            source::member(&encoded, "wire_efforts").unwrap().as_str(),
            wire
        );
        assert_eq!(model, encoded.deserialize::<ModelReasoning>().unwrap());
    }
    let generated = ModelReasoning::from_record(Some(&raw(
        r#"{"reasoning_effort_wire_map":{" 2 ":" x ","1":"\ud800","\ud800":"\udc00"," 2":"last"}}"#,
    )));
    let encoded = SourceJson::from_serializable(&generated).unwrap();
    assert_eq!(
        source::member(&encoded, "wire_efforts").unwrap().as_str(),
        r#"{"1":"\ud800","2":"last","\ud800":"\udc00"}"#
    );
    assert_eq!(generated, encoded.deserialize::<ModelReasoning>().unwrap());
    let spaced = raw(r#"{"levels":[],"default_level":null,"wire_efforts":{ "1":"\ud800","2":"last","\ud800":"\udc00"}}"#).deserialize::<ModelReasoning>().unwrap();
    assert_ne!(generated, spaced);
}
#[test]
fn reasoning_hints_keep_raw_elements_and_total_provider_data() {
    use gateway_model_metadata::metadata::{MetadataHint, MetadataSources, resolve_metadata};
    let input =
        raw(r#"{"supported_reasoning_levels":[null,{"effort":" \ud800 "},false,["x"],"\udc00"]}"#);
    let hint = input.deserialize::<MetadataHint>().unwrap();
    let encoded = SourceJson::from_serializable(&hint).unwrap();
    assert_eq!(
        source::member(&encoded, "supported_reasoning_levels"),
        source::member(&input, "supported_reasoning_levels")
    );
    let resolved = resolve_metadata(&MetadataSources {
        provider: Some(hint),
        ..MetadataSources::default()
    });
    let levels = resolved.supported_reasoning_levels.unwrap();
    assert!(
        levels[0]
            .as_units()
            .iter()
            .copied()
            .eq("none".encode_utf16())
    );
    assert_eq!(levels[1].as_units(), &[0xd800]);
    assert_eq!(levels[2].as_units(), &[0xdc00]);
    let provider =
        MetadataHint::provider(-1, &[" custom ", "null", "\n", "nonstandard"], " default ");
    let decoded = SourceJson::from_serializable(&provider)
        .unwrap()
        .deserialize::<MetadataHint>()
        .unwrap();
    assert_eq!(provider, decoded);
    assert_eq!(
        resolve_metadata(&MetadataSources {
            provider: Some(provider),
            ..MetadataSources::default()
        }),
        resolve_metadata(&MetadataSources {
            provider: Some(decoded),
            ..MetadataSources::default()
        })
    );
    let invalid = ModelSnapshot {
        models: Vec::new(),
        source: "source".into(),
        updated_at_ms: 1,
        client_version: None,
    };
    assert!(
        gateway_catalog_input::runtime_config::build_runtime_config(&invalid, None, None, 1)
            .is_err()
    );
    assert_eq!(
        gateway_catalog_input::runtime_config::build_optional_runtime_config(
            &invalid, None, None, 1
        )
        .unwrap(),
        None
    );
}
