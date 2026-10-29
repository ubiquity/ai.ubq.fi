//! Source snapshot normalization stays authority-owned; the exact wire DTOs
//! and finite validators are shared with authentication without a reverse edge.
use super::unavailable;
use gateway_auth_native_wire::{
    publication_command::{ImportCatalogData, SNAPSHOT_MAX_BYTES},
    publication_data::ImportCatalogDisposition,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};

pub fn prepare_import_catalog(
    snapshot: &SourceJson,
    client_version: &str,
    upstream_status: u16,
    observed_at_ms: i64,
) -> Result<ImportCatalogData, GatewayError> {
    ImportCatalogDisposition::Normalized {
        client_version: client_version.into(),
        upstream_status,
    }
    .validate()?;
    crate::validate_time(observed_at_ms)?;
    let snapshot = gateway_model_snapshot::snapshot::normalize_snapshot(
        snapshot,
        &gateway_model_snapshot::snapshot::SnapshotOverrides {
            source: Some("chatgpt_codex".into()),
            client_version: Some(client_version.into()),
            updated_at_ms: Some(observed_at_ms),
        },
        observed_at_ms,
    )?
    .ok_or_else(unavailable)?;
    let snapshot = gateway_source::source_serialization::canonical(
        &SourceJson::from_serializable(&snapshot)?,
    )?;
    if snapshot.as_str().len() > SNAPSHOT_MAX_BYTES {
        return Err(GatewayError::new(
            413,
            format!(
                "models snapshot too large ({} bytes; max 65536).",
                snapshot.as_str().len()
            ),
            Some("invalid_request_error"),
        ));
    }
    Ok(ImportCatalogData::Normalized {
        snapshot,
        client_version: client_version.into(),
        upstream_status,
    })
}
