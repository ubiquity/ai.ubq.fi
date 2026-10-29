use super::NativeImportInputs;
use super::prepare_flow::VerifiedImportPublication;
use gateway_auth_native_import_data::native_import::authority::completion::ImportCompletionModelsData;
use gateway_authority::native_control::import::remote::RemoteImportAuthority;
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_native_import_source::result::{NativeImportCompletion, NativeImportResult};
use gateway_native_import_source::{catalog::CatalogValidation, display::expiry_display_ms};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde::Serialize;
use std::collections::BTreeMap;

#[derive(Serialize)]
struct SourceCompletion<'a> {
    stored: bool,
    refreshed: bool,
    account_id: &'a str,
    account_count: u64,
    account_ids: &'a [String],
    access_token_expires_at_ms: Option<f64>,
    updated_at_ms: f64,
    upstream_status: u16,
    upstream_content_type: Option<&'a str>,
    models: &'a ImportCompletionModelsData,
    catalog_seeded: bool,
    normalized_snapshot_updated: bool,
}
impl VerifiedImportPublication<'_> {
    pub(super) fn source_result<C: NativeRenewalCancellation>(
        &self,
        authority: &RemoteImportAuthority<'_, C>,
        context: &NativeImportInputs<'_>,
    ) -> Result<NativeImportResult, GatewayError> {
        context.check()?;
        let candidate = self.candidate();
        let metadata = self.terminal.completion();
        if metadata
            .account_ids
            .iter()
            .filter(|id| **id == candidate.credentials.account_id)
            .count()
            != 1
        {
            return Err(super::unavailable());
        }
        let normalized = matches!(candidate.catalog, CatalogValidation::Normalized { .. });
        // The genuine guarded publication is already complete. An actual optional
        // seed error, including frame overflow, cannot undo that primary result.
        let catalog_seeded = if normalized {
            candidate
                .raw_models_body
                .as_deref()
                .map(|raw| {
                    authority
                        .seed_catalog(
                            &self.prepared.claim,
                            &self.terminal,
                            raw,
                            candidate.etag.as_deref(),
                            candidate.content_type.as_deref(),
                        )
                        .unwrap_or(false)
                })
                .unwrap_or(false)
        } else {
            false
        };
        context.check()?;
        let upstream_status = match candidate.catalog {
            CatalogValidation::Normalized {
                upstream_status, ..
            } => upstream_status,
            CatalogValidation::AuthenticatedLimited => 429,
        };
        let body = SourceJson::from_serializable(&SourceCompletion {
            stored: true,
            refreshed: normalized && candidate.refreshed,
            account_id: &candidate.credentials.account_id,
            account_count: metadata.account_count,
            account_ids: &metadata.account_ids,
            access_token_expires_at_ms: expiry_display_ms(
                candidate.credentials.access_token.expose(),
            ),
            // This is the exact persisted producer projection, not a fabricated
            // native response clock or a claimed earlier validated-auth timestamp.
            updated_at_ms: metadata.updated_at_ms,
            upstream_status,
            upstream_content_type: normalized
                .then_some(candidate.content_type.as_deref())
                .flatten(),
            models: &metadata.models,
            catalog_seeded,
            normalized_snapshot_updated: normalized,
        })?;
        let publication_revision = self.publication_revision().to_owned();
        let native_receipt_ref = self.native_receipt_ref().to_owned();
        let completion = match self.disposition() {
            gateway_auth_native_protocol::native_import::publication::NativeImportPublicationDisposition::Applied => NativeImportCompletion::Published { publication_revision, native_receipt_ref },
            gateway_auth_native_protocol::native_import::publication::NativeImportPublicationDisposition::AlreadyCurrent => NativeImportCompletion::AlreadyCurrent { publication_revision, native_receipt_ref },
        };
        Ok(NativeImportResult {
            status: 200,
            headers: BTreeMap::from([("x-uos-upstream".into(), "chatgpt_codex".into())]),
            body,
            completion,
        })
    }
}
