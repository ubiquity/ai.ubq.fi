//! Existing pure source catalog/wire cases; public DATA API only, unchanged bodies/assertions.
use gateway_auth_native_wire::{
    native_read::NATIVE_MAX_BODY_BYTES,
    publication_command::{
        ACCESS_MAX_BYTES, ImportAccessDocument, ImportCatalogData, ImportPublicationCommandData,
        SNAPSHOT_MAX_BYTES, import_publication_max_bytes,
    },
    publication_data::ImportCatalogDisposition,
};
use gateway_authority::native_control::import::publication_wire::prepare_import_catalog;
use gateway_source::source_json::SourceJson;

fn command() -> ImportPublicationCommandData {
    ImportPublicationCommandData {
        operation: "codex_auth_import_publish".into(),
        v: 1,
        action_ref: "a".repeat(64),
        operation_id: "operation".into(),
        claim_ref: "claim".into(),
        native_owner_id: "owner".into(),
        owner_revision: "owner-r1".into(),
        source_revision: "source-r1".into(),
        normalized_revision: "pool-r1".into(),
        publication_revision: "publication-r1".into(),
        account_id_hash: "b".repeat(64),
        previous_native_source_generation: "c".repeat(64),
        native_source_generation: "d".repeat(64),
        access_generation: "e".repeat(64),
        native_receipt_ref: "receipt-r1".into(),
        document: ImportAccessDocument {
            account_id: "account".into(),
            access_token: "SYNTHETIC".into(),
            updated_at_ms: 1_000_000.0,
            native_refreshed_at: None,
        },
        catalog: ImportCatalogData::Normalized {
            snapshot: SourceJson::parse(r#"{"models":[{"slug":"model"}]}"#).unwrap(),
            client_version: "0.163.0".into(),
            upstream_status: 200,
        },
    }
}
#[test]
fn pure_safe_catalog_helper_retains_source_utf16_without_native_dependency() {
    let raw = SourceJson::parse(r#"{"models":[{"slug":"\ud800"}]}"#).unwrap();
    let catalog = prepare_import_catalog(&raw, "0.163.0", 200, 1_000_000).unwrap();
    let ImportCatalogData::Normalized { snapshot, .. } = &catalog else {
        panic!("normalized")
    };
    assert!(snapshot.as_str().contains("\\ud800"));
    let mut row = command();
    row.catalog = catalog;
    ImportPublicationCommandData::parse(&row.bytes().unwrap()).unwrap();
}
#[test]
fn exact_frame_and_component_boundaries_preserve_existing_thresholds() {
    let mut row = command();
    let initial = serde_json::to_vec(&row.document).unwrap().len();
    row.document
        .access_token
        .push_str(&"x".repeat(ACCESS_MAX_BYTES - initial));
    assert_eq!(
        serde_json::to_vec(&row.document).unwrap().len(),
        ACCESS_MAX_BYTES
    );
    let mut bytes = row.bytes().unwrap();
    bytes.resize(import_publication_max_bytes(), b' ');
    ImportPublicationCommandData::parse(&bytes).unwrap();
    bytes.push(b' ');
    assert!(ImportPublicationCommandData::parse(&bytes).is_err());
    row.document.access_token.push('x');
    assert!(row.bytes().is_err());
    assert!(import_publication_max_bytes() < NATIVE_MAX_BODY_BYTES);
}
#[test]
fn duplicated_version_and_escaped_authority_text_fit_computed_envelope() {
    let mut row = command();
    let maximum = "\u{1}".repeat(4096);
    row.native_owner_id = maximum.clone();
    row.owner_revision = maximum.clone();
    row.source_revision = maximum.clone();
    row.normalized_revision = maximum;
    let version = format!("{}.1.0", "0".repeat(30_000));
    let snapshot = SourceJson::from_serializable(
        &serde_json::json!({"models":[{"slug":"model"}],"client_version":version}),
    )
    .unwrap();
    assert!(snapshot.as_str().len() <= SNAPSHOT_MAX_BYTES);
    row.catalog = ImportCatalogData::Normalized {
        snapshot,
        client_version: version,
        upstream_status: 200,
    };
    assert!(serde_json::to_vec(&row.catalog).unwrap().len() > SNAPSHOT_MAX_BYTES);
    let bytes = row.bytes().unwrap();
    assert!(bytes.len() < import_publication_max_bytes());
    let parsed = ImportPublicationCommandData::parse(&bytes).unwrap();
    assert_eq!(parsed.owner_revision.len(), 4096);
    row.owner_revision.push('x');
    assert!(row.bytes().is_err());
}
#[test]
fn closed_status_nullability_and_secret_free_metadata_refuse_forgery() {
    let row = command();
    let bytes = row.bytes().unwrap();
    for field in [
        "refresh_token",
        "body_digest",
        "configuration_fingerprint",
        "mutations",
    ] {
        let mut value: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        value[field] = serde_json::json!("forbidden");
        assert!(ImportPublicationCommandData::parse(&serde_json::to_vec(&value).unwrap()).is_err());
    }
    let mut value: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
    value["document"]
        .as_object_mut()
        .unwrap()
        .remove("native_refreshed_at");
    assert!(ImportPublicationCommandData::parse(&serde_json::to_vec(&value).unwrap()).is_err());
    let mut limited = command();
    limited.catalog = ImportCatalogData::AuthenticatedLimited {
        upstream_status: 429,
    };
    limited.bytes().unwrap();
    limited.catalog = ImportCatalogData::AuthenticatedLimited {
        upstream_status: 200,
    };
    assert!(limited.bytes().is_err());
}
#[test]
fn promoted_wire_client_version_domain_matches_the_existing_schema_parser() {
    let large_zero = "0".repeat(59_994) + ".0.0";
    for raw in [
        "0.163.0",
        "\u{feff}0.163.0\u{a0}",
        "9007199254740991.0.0",
        "9007199254740992.0.0",
        "1.2",
        "1.2.3.4",
        "01.002.0003",
        "\u{1}0.1.0",
        large_zero.as_str(),
    ] {
        let disposition = ImportCatalogDisposition::Normalized {
            client_version: raw.into(),
            upstream_status: 200,
        };
        assert_eq!(
            disposition.validate().is_ok(),
            gateway_model_snapshot::snapshot::parse_client_version(raw).is_some(),
            "{raw}"
        );
    }
}
