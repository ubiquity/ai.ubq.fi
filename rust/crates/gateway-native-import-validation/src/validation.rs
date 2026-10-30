//! Fixed native import validation. Candidates are DATA, never mutation or publication authority.
use gateway_auth_credentials::native_credentials::NativeCredentials;
use gateway_native_import_context::context::{NativeImportInputs, RequestIdentity};
use gateway_native_import_source::{
    catalog::{
        CatalogValidation, invalid_catalog, model_error, normalized_catalog, selected_version,
        tokens_replaced,
    },
    documents as document,
};
use gateway_source::source_json::SourceJson;

// Only the fixed models/OAuth producer below may create this; callers cannot deserialize it.
pub struct ValidatedNativeImport<'a> {
    pub(crate) original: RequestIdentity<'a>,
    pub(crate) document: SourceJson,
    pub(crate) credentials: NativeCredentials,
    pub(crate) catalog: CatalogValidation,
    pub(crate) refreshed: bool,
    pub(crate) updated_at_ms: f64,
    pub(crate) content_type: Option<String>,
    pub(crate) raw_models_body: Option<String>,
    pub(crate) etag: Option<String>,
}

use crate::journal::RotationStore;
use gateway_authority::native_control::import::AuthenticatedNativeImport;
use gateway_native_custody_io::protocol::unavailable;
use gateway_native_import_http::{
    NativeProviderResponseData as PrivateResponse, ValidationClients,
};
use gateway_native_import_request::live::LiveImportCustody;
use gateway_source::{error::GatewayError, js_string::JsString16};
use std::{
    net::TcpStream,
    time::{Duration, Instant},
};

/// Constructor-preconnected fixed TLS origins, never a caller URL or a ProviderLive connector.
pub struct ImportValidation {
    clients: ValidationClients,
}
pub struct NeedsRefresh401<'a> {
    original: RequestIdentity<'a>,
    document: SourceJson,
    credentials: NativeCredentials,
    original_error: GatewayError,
    seed_updated_at_ms: f64,
}
pub enum ValidationOutcome<'a> {
    Valid(ValidatedNativeImport<'a>),
    Original401(NeedsRefresh401<'a>),
}
impl ImportValidation {
    pub fn from_clients(clients: ValidationClients) -> Self {
        Self { clients }
    }
    pub fn probe<'a>(
        &self,
        context: &NativeImportInputs<'a>,
    ) -> Result<ValidationOutcome<'a>, GatewayError> {
        let (identity, source) = context.validation_seed()?;
        let credentials = document::credentials(&source)?;
        let seed_updated_at_ms = context.now_ms()? as f64;
        if credentials.external_user_id.is_some() {
            return Err(unavailable());
        }
        context.require_credentials_target(&credentials)?;
        let response = self.models(&credentials, context)?;
        if response.status == 401 {
            return Ok(ValidationOutcome::Original401(NeedsRefresh401 {
                original: identity,
                original_error: model_error(response.status, &response.body, &credentials),
                document: source,
                credentials,
                seed_updated_at_ms,
            }));
        }
        self.finish(
            identity,
            (source, credentials),
            response,
            false,
            (seed_updated_at_ms, seed_updated_at_ms),
            context,
        )
        .map(ValidationOutcome::Valid)
    }
    fn models(
        &self,
        credentials: &NativeCredentials,
        context: &NativeImportInputs<'_>,
    ) -> Result<PrivateResponse, GatewayError> {
        context.check()?;
        let result = self
            .clients
            .probe_native_credentials(credentials, context.deadline())?;
        context.check()?;
        Ok(result)
    }
    fn finish<'a>(
        &self,
        original: RequestIdentity<'a>,
        document: (SourceJson, NativeCredentials),
        response: PrivateResponse,
        refreshed: bool,
        timestamps: (f64, f64),
        context: &NativeImportInputs<'_>,
    ) -> Result<ValidatedNativeImport<'a>, GatewayError> {
        let (source, credentials) = document;
        let (seed_updated_at_ms, validated_updated_at_ms) = timestamps;
        context.check()?;
        let content_type = if (200..300).contains(&response.status) {
            response
                .headers
                .iter()
                .find(|(name, _)| name.eq_ignore_ascii_case("content-type"))
                .map(|(_, value)| value.clone())
        } else {
            None
        };
        let catalog = if response.status == 429 {
            CatalogValidation::AuthenticatedLimited
        } else if (200..300).contains(&response.status) {
            let is_json = response.headers.iter().any(|(name, value)| {
                name.eq_ignore_ascii_case("content-type") && value.contains("application/json")
            });
            if !is_json {
                return Err(invalid_catalog());
            }
            let decoded = String::from_utf8_lossy(&response.body);
            let text = decoded.strip_prefix('\u{feff}').unwrap_or(&decoded);
            let body = SourceJson::parse(text).map_err(|_| invalid_catalog())?;
            self.clients
                .normalize_models_data(&body, response.status, context.now_ms()?)?
        } else {
            return Err(model_error(response.status, &response.body, &credentials));
        };
        let etag = if content_type.is_some() {
            response
                .headers
                .iter()
                .find(|(name, _)| name.eq_ignore_ascii_case("etag"))
                .map(|(_, value)| value.clone())
        } else {
            None
        };
        let raw_models_body = if content_type.is_some() {
            let decoded = String::from_utf8_lossy(&response.body);
            Some(
                decoded
                    .strip_prefix('\u{feff}')
                    .unwrap_or(&decoded)
                    .to_owned(),
            )
        } else {
            None
        };
        let updated_at_ms = if response.status == 429 {
            seed_updated_at_ms
        } else {
            validated_updated_at_ms
        };
        Ok(ValidatedNativeImport {
            original,
            document: source,
            credentials,
            catalog,
            refreshed,
            updated_at_ms,
            content_type,
            raw_models_body,
            etag,
        })
    }
    /// A genuine original401 plus consumed original Start/live custody is required before the one dispatch.
    pub fn refresh_once<'a>(
        &mut self,
        pending: NeedsRefresh401<'a>,
        custody: &mut LiveImportCustody<'_>,
        store: &RotationStore<'_>,
        context: &NativeImportInputs<'_>,
    ) -> Result<ValidatedNativeImport<'a>, GatewayError> {
        pending.original.require_same(context)?;
        custody.check_live(store.root(), context)?;
        let response = self
            .clients
            .dispatch_original_refresh(&pending.credentials, context.deadline());
        custody.check_live(store.root(), context)?;
        let response = response.map_err(|_| pending.original_error.clone())?;
        if !(200..300).contains(&response.status) {
            return Err(pending.original_error);
        }
        let rotated =
            SourceJson::parse_bytes(&response.body).map_err(|_| pending.original_error.clone())?;
        let tokens = document::member(&pending.document, "tokens")?.ok_or_else(unavailable)?;
        let mut fields = pending.document.object_entries()?;
        fields.retain(|(k, _)| {
            !["tokens", "last_refresh"]
                .iter()
                .any(|n| *k == JsString16::from_utf8(n))
        });
        fields.push((
            JsString16::from_utf8("tokens"),
            document::object(
                tokens_replaced(&tokens, &rotated, &pending.credentials)
                    .map_err(|_| pending.original_error.clone())?,
            )?,
        ));
        let timestamp = time::OffsetDateTime::from_unix_timestamp_nanos(
            i128::from(context.now_ms()?) * 1_000_000,
        )
        .map_err(|_| unavailable())?
        .format(&time::format_description::well_known::Rfc3339)
        .map_err(|_| unavailable())?;
        fields.push((
            JsString16::from_utf8("last_refresh"),
            SourceJson::from_serializable(&timestamp)?,
        ));
        let source = document::object(fields)?;
        let credentials = document::credentials(&source)?;
        let refreshed_updated_at_ms = context.now_ms()? as f64;
        // Rotated material is durable native-private before a later catalog retry can fail.
        store.retain_rotated(custody, context, &source)?;
        let result = self
            .models(&credentials, context)
            .map_err(|_| pending.original_error.clone())?;
        custody.check_live(store.root(), context)?;
        self.finish(
            pending.original,
            (source, credentials),
            result,
            true,
            (pending.seed_updated_at_ms, refreshed_updated_at_ms),
            context,
        )
    }
}

/// Ordinary write DATA, never a native permission or a caller-accepted receipt.
pub struct NativePersistenceData {
    pub document: SourceJson,
    pub credentials: NativeCredentials,
}
pub struct PublicationProjectionData {
    pub catalog: CatalogValidation,
    pub updated_at_ms: f64,
}
impl ValidatedNativeImport<'_> {
    pub fn require_original_context(
        &self,
        context: &NativeImportInputs<'_>,
    ) -> Result<(), GatewayError> {
        self.original.require_same(context)
    }
    pub fn require_catalog<C: gateway_authority_contracts::NativeRenewalCancellation>(
        &self,
        authority: &gateway_authority::native_control::import::remote::RemoteImportAuthority<'_, C>,
        context: &NativeImportInputs<'_>,
    ) -> Result<(), GatewayError> {
        context.require_remote_catalog(authority, &self.catalog)
    }
    pub fn native_persistence_data(
        &self,
        binding: &gateway_authority_native_records::import_effects::NativeImportBinding,
        previous_bytes: &[u8],
    ) -> Result<NativePersistenceData, GatewayError> {
        let parsed = SourceJson::parse_bytes(previous_bytes).map_err(|_| unavailable())?;
        let previous = document::credentials(&parsed).map_err(|_| unavailable())?;
        if previous.routing_account_hash() != binding.account_id_hash
            || previous.generation_hash() != binding.source_native_generation
            || self.credentials.account_id != previous.account_id
            || self.credentials.external_user_id.is_some()
        {
            return Err(unavailable());
        }
        let previous_document = SourceJson::parse_bytes(previous_bytes)?;
        let merged = document::merge(&previous_document, &self.document)?;
        if merged.as_bytes().len() as u64 > 1_048_576 {
            return Err(unavailable());
        }
        let parsed = SourceJson::parse_bytes(merged.as_bytes()).map_err(|_| unavailable())?;
        let persisted = document::credentials(&parsed).map_err(|_| unavailable())?;
        if !persisted.same_credentials(&self.credentials)
            || persisted.account_id != previous.account_id
        {
            return Err(unavailable());
        }
        Ok(NativePersistenceData {
            document: merged,
            credentials: persisted,
        })
    }
    pub fn publication_projection(
        &self,
        context: &NativeImportInputs<'_>,
        persisted: &NativeCredentials,
    ) -> Result<PublicationProjectionData, GatewayError> {
        self.original.require_same(context)?;
        if !persisted.same_credentials(&self.credentials) {
            return Err(unavailable());
        }
        Ok(PublicationProjectionData {
            catalog: self.catalog.clone(),
            updated_at_ms: self.updated_at_ms,
        })
    }
}
