//! Fixed native import validation. Candidates are DATA, never mutation or publication authority.
use gateway_auth::native_credentials::NativeCredentials;
use gateway_core::source_json::SourceJson;
use gateway_native_import_source::{
    catalog::{
        CatalogValidation, invalid_catalog, model_error, normalized_catalog, selected_version,
        tokens_replaced,
    },
    documents as document,
};

// Only the fixed models/OAuth producer below may create this; callers cannot deserialize it.
pub(super) struct ValidatedNativeImport<'a> {
    pub(super) original: &'a AuthenticatedNativeImport,
    pub(super) document: SourceJson,
    pub(super) credentials: NativeCredentials,
    pub(super) catalog: CatalogValidation,
    pub(super) refreshed: bool,
    pub(super) updated_at_ms: f64,
    pub(super) content_type: Option<String>,
    pub(super) raw_models_body: Option<String>,
    pub(super) etag: Option<String>,
}

use super::{ExistingAccountImportStore, live_custody::LiveImportCustody};
use crate::{
    import_custody_protocol::unavailable,
    native_import::NativeImportContext,
    private_https::{PrivateHttps, PrivateResponse},
};
use gateway_authority::native_control::import::AuthenticatedNativeImport;
use gateway_core::{error::GatewayError, js_string::JsString16};
use std::{
    net::TcpStream,
    time::{Duration, Instant},
};

/// Constructor-preconnected fixed TLS origins, never a caller URL or a ProviderLive connector.
pub(super) struct ImportValidation {
    models: PrivateHttps,
    oauth: Option<PrivateHttps>,
    client_version: String,
}
pub(super) struct NeedsRefresh401<'a> {
    original: &'a AuthenticatedNativeImport,
    document: SourceJson,
    credentials: NativeCredentials,
    original_error: GatewayError,
    seed_updated_at_ms: f64,
}
pub(super) enum ValidationOutcome<'a> {
    Valid(ValidatedNativeImport<'a>),
    Original401(NeedsRefresh401<'a>),
}
impl ImportValidation {
    pub(super) fn from_connected(
        models: TcpStream,
        oauth: Option<TcpStream>,
        version: Option<&JsString16>,
        roots: ureq::tls::RootCerts,
    ) -> Result<Self, GatewayError> {
        let client_version = selected_version(version);
        let models = PrivateHttps::models_connected(models, roots.clone(), &client_version)?;
        let oauth = oauth
            .map(|socket| {
                PrivateHttps::from_connected(
                    "https://auth.openai.com/oauth/token",
                    "/oauth/token",
                    socket,
                    roots,
                )
            })
            .transpose()?;
        Ok(Self {
            models,
            oauth,
            client_version,
        })
    }
    pub(super) fn probe<'a>(
        &self,
        context: &NativeImportContext<'a>,
    ) -> Result<ValidationOutcome<'a>, GatewayError> {
        context.check()?;
        let source = context.auth_data().auth().clone();
        let credentials = document::credentials(&source)?;
        let seed_updated_at_ms = context.now_ms()? as f64;
        if credentials.external_user_id.is_some() {
            return Err(unavailable());
        }
        match context.original_proof().target() {
            gateway_auth::native_import::NativeImportTarget::ExistingAccount {
                account_id_hash,
            } if *account_id_hash == credentials.routing_account_hash() => (),
            _ => return Err(unavailable()),
        }
        let response = self.models(&credentials, context)?;
        if response.status == 401 {
            return Ok(ValidationOutcome::Original401(NeedsRefresh401 {
                original: context.original_proof(),
                original_error: model_error(response.status, &response.body, &credentials),
                document: source,
                credentials,
                seed_updated_at_ms,
            }));
        }
        self.finish(
            context.original_proof(),
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
        context: &NativeImportContext<'_>,
    ) -> Result<PrivateResponse, GatewayError> {
        context.check()?;
        let headers = vec![
            (
                "Authorization".into(),
                format!("Bearer {}", credentials.access_token.expose()),
            ),
            ("ChatGPT-Account-ID".into(), credentials.account_id.clone()),
            ("originator".into(), "codex_cli_rs".into()),
            (
                "user-agent".into(),
                format!("codex_cli_rs/{} (ai.ubq.fi)", self.client_version),
            ),
        ];
        let result = self.models.get_models(&headers, context.deadline()).map_err(|_| GatewayError::new(502,
            "Upstream validation request failed: Codex upstream request failed: upstream unreachable.", Some("codex_upstream_unreachable")))?;
        context.check()?;
        Ok(result)
    }
    fn finish<'a>(
        &self,
        original: &'a AuthenticatedNativeImport,
        document: (SourceJson, NativeCredentials),
        response: PrivateResponse,
        refreshed: bool,
        timestamps: (f64, f64),
        context: &NativeImportContext<'_>,
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
            normalized_catalog(
                &body,
                &self.client_version,
                response.status,
                context.now_ms()?,
            )?
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
    pub(super) fn refresh_once<'a>(
        &mut self,
        pending: NeedsRefresh401<'a>,
        custody: &mut LiveImportCustody<'_>,
        store: &ExistingAccountImportStore,
        proof: &'a AuthenticatedNativeImport,
        body: &[u8],
        context: &NativeImportContext<'_>,
    ) -> Result<ValidatedNativeImport<'a>, GatewayError> {
        if !std::ptr::eq(pending.original, proof) || !std::ptr::eq(context.original_proof(), proof)
        {
            return Err(unavailable());
        }
        custody.check_live(store, proof, body, context)?;
        let transport = self.oauth.take().ok_or_else(unavailable)?;
        let request = serde_json::to_vec(&serde_json::json!({"client_id":"app_EMoamEEZ73f0CkXaXp7hrann", "grant_type":"refresh_token",
            "refresh_token":pending.credentials.refresh_token.expose(), "scope":"openid profile email"})).map_err(|_| unavailable())?;
        // Taking the unique transport before sending prevents retry after any ambiguous result.
        let response = transport.send(
            &[],
            &request,
            context
                .deadline()
                .min(Instant::now() + Duration::from_secs(10)),
            usize::MAX,
        );
        custody.check_live(store, proof, body, context)?;
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
        super::durability::retain_rotated(store, custody, proof, body, context, &source)?;
        let result = self
            .models(&credentials, context)
            .map_err(|_| pending.original_error.clone())?;
        custody.check_live(store, proof, body, context)?;
        self.finish(
            proof,
            (source, credentials),
            result,
            true,
            (pending.seed_updated_at_ms, refreshed_updated_at_ms),
            context,
        )
    }
}
