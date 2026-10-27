//! Fixed native-only receiver and authenticated source-data boundary.
//! No socket, refresh, provider effect or publication implementation.
use crate::owner_control::NativeControlContext;
use gateway_auth_native_contracts::native_import::{NativeImportKind, NativeImportMetadata};
use gateway_authority::{
    native_control::NativeRenewalCancellation, native_control::import::AuthenticatedNativeImport,
    native_control::receiver::NativeRenewalAuthority,
};
use gateway_authority_contracts::TokenSource;
use gateway_core::state::AuthorityStore;
use gateway_native_import_source::{body as import_body, result::NativeImportResult};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};

/// Read-only native credential DATA, not proof, effect authority or CF completion.
/// Constructed only by dispatch from the one original-body decoder after binding.
/// Deliberately no Clone/Debug/Serialize/Deserialize or public constructor.
pub struct NativeAuthImportData<'a> {
    body: import_body::AuthBody<'a>,
}
impl NativeAuthImportData<'_> {
    pub fn auth(&self) -> &SourceJson {
        self.body.auth()
    }
    pub fn client_version(&self) -> Option<&JsString16> {
        self.body.client_version()
    }
}

/// Source-faithful AUTH read/transport/decoder error mapping shared with ingress.
pub(crate) fn auth_body_error() -> GatewayError {
    let error = import_body::BodyError::AuthBody;
    GatewayError::new(error.status(), error.message(), Some(error.code()))
}
fn binding_error() -> GatewayError {
    GatewayError::new(
        403,
        "Native import context does not match the original proof",
        Some("native_capability_forbidden"),
    )
}
fn check_original_document(
    proof: &AuthenticatedNativeImport,
    metadata: &NativeImportMetadata,
    body: &[u8],
    original: &NativeControlContext<'_>,
) -> Result<(), GatewayError> {
    original.check()?;
    if proof.metadata() != metadata {
        return Err(binding_error());
    }
    proof.require_body(body, original.now_ms()?)
}

/// The supported type path does not expose its proof/body/original binding.
/// ```compile_fail,E0451
/// use gateway_native::{native_import::{NativeImportContext,NativeAuthImportData},owner_control::NativeControlContext};
/// use gateway_authority::native_control::import::AuthenticatedNativeImport;
/// fn forge<'a>(original: &'a NativeControlContext<'a>,proof: &'a AuthenticatedNativeImport,auth: NativeAuthImportData<'a>)->NativeImportContext<'a> {
///     NativeImportContext { original,proof,auth }
/// }
/// ```
pub struct NativeImportContext<'a> {
    original: &'a NativeControlContext<'a>,
    proof: &'a AuthenticatedNativeImport,
    auth: NativeAuthImportData<'a>,
}
impl<'a> NativeImportContext<'a> {
    /// Checks only the exact original-context identity; callers also retain document and authority checks.
    pub fn require_original(
        &self,
        original: &NativeControlContext<'_>,
    ) -> Result<(), GatewayError> {
        self.check()?;
        if !std::ptr::eq(self.original, original) {
            return Err(binding_error());
        }
        Ok(())
    }
    pub fn auth_data(&self) -> &NativeAuthImportData<'_> {
        &self.auth
    }
    pub fn check(&self) -> Result<(), GatewayError> {
        self.original.check()
    }
    pub fn check_document(
        &self,
        proof: &AuthenticatedNativeImport,
        metadata: &NativeImportMetadata,
        body: &[u8],
    ) -> Result<(), GatewayError> {
        self.check()?;
        if !std::ptr::eq(self.proof, proof) || proof.metadata() != metadata {
            return Err(binding_error());
        }
        check_original_document(proof, metadata, body, self.original)?;
        proof.require_body(self.auth.body.raw_body(), self.original.now_ms()?)
    }
    pub async fn check_current<S: AuthorityStore, I: TokenSource, C: NativeRenewalCancellation>(
        &self,
        authority: &NativeRenewalAuthority<'_, S, I, C>,
    ) -> Result<(), GatewayError> {
        self.check()?;
        authority.recheck_import(self.proof).await?;
        self.check()
    }
}
/// Admitted borrows cannot escape the finite callback.
/// ```compile_fail,E0521
/// use gateway_native::{native_import::{NativeImportHandler,NativeImportContext},owner_control::NativeControlContext};
/// use gateway_native_import_source::result::NativeImportResult;
/// use gateway_auth_native_contracts::native_import::NativeImportMetadata;
/// use gateway_authority::native_control::import::AuthenticatedNativeImport;
/// use gateway_source::error::GatewayError;
/// struct Keep(std::sync::Mutex<Option<&'static AuthenticatedNativeImport>>);
/// impl NativeImportHandler for Keep {
///     fn import_document(&self,proof:&AuthenticatedNativeImport,_:&NativeImportMetadata,_:&[u8],_:&NativeImportContext<'_>,_:&NativeControlContext<'_>)->Result<NativeImportResult,GatewayError> {
///         *self.0.lock().unwrap()=Some(proof);
///         Err(GatewayError::unavailable("refused","refused"))
///     }
/// }
/// ```
pub trait NativeImportHandler: Send + Sync {
    fn import_document(
        &self,
        proof: &AuthenticatedNativeImport,
        metadata: &NativeImportMetadata,
        original_body: &[u8],
        context: &NativeImportContext<'_>,
        original: &NativeControlContext<'_>,
    ) -> Result<NativeImportResult, GatewayError>;
}
/// The only context-construction path binds the actual original cancellation and current proof.
pub async fn dispatch_import<
    S: AuthorityStore,
    I: TokenSource,
    C: NativeRenewalCancellation,
    H: NativeImportHandler + ?Sized,
>(
    handler: &H,
    authority: &NativeRenewalAuthority<'_, S, I, C>,
    proof: &AuthenticatedNativeImport,
    metadata: &NativeImportMetadata,
    body: &[u8],
    original: &NativeControlContext<'_>,
) -> Result<NativeImportResult, GatewayError> {
    check_original_document(proof, metadata, body, original)?;
    authority.recheck_import(proof).await?;
    check_original_document(proof, metadata, body, original)?;
    let context = decode_context(proof, metadata, body, original)?;
    context.require_original(original)?;
    handler.import_document(proof, metadata, body, &context, original)
}
/// Both fixed local/remote receivers reach this one original-body decoder only
/// after their actual authority admission and finish-current exchange.
pub(crate) fn decode_context<'a>(
    proof: &'a AuthenticatedNativeImport,
    metadata: &NativeImportMetadata,
    body: &'a [u8],
    original: &'a NativeControlContext<'a>,
) -> Result<NativeImportContext<'a>, GatewayError> {
    check_original_document(proof, metadata, body, original)?;
    if proof.target().kind() != NativeImportKind::CodexAuth {
        return Err(GatewayError::unavailable(
            "Native profile import is unavailable",
            "native_import_unavailable",
        ));
    }
    let auth = NativeAuthImportData {
        body: import_body::decode_auth(body, None).map_err(|_| auth_body_error())?,
    };
    let context = NativeImportContext {
        original,
        proof,
        auth,
    };
    context.check_document(proof, metadata, body)?;
    Ok(context)
}

#[cfg(all(test, unix))]
mod tests;
