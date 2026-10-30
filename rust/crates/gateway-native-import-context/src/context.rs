use gateway_auth_native_contracts::native_import::NativeImportMetadata;
use gateway_authority::native_control::import::AuthenticatedNativeImport;
use gateway_authority::native_control::receiver::NativeRenewalAuthority;
use gateway_authority_contracts::{NativeRenewalCancellation, TokenSource};
use gateway_core::state::AuthorityStore;
use gateway_source::error::GatewayError;
use std::time::Instant;

/// Dynamic-extent input for the real existing-account operation; never a wire/permission constructor.
pub struct NativeImportInputs<'a> {
    proof: &'a AuthenticatedNativeImport,
    body: &'a [u8],
    context: &'a gateway_native_http_admission::native_import::NativeImportContext<'a>,
    original: &'a gateway_auth_native_proof::native_context::NativeControlContext<'a>,
}
impl<'a> NativeImportInputs<'a> {
    pub fn checked(
        proof: &'a AuthenticatedNativeImport,
        metadata: &'a NativeImportMetadata,
        body: &'a [u8],
        context: &'a gateway_native_http_admission::native_import::NativeImportContext<'a>,
        original: &'a gateway_auth_native_proof::native_context::NativeControlContext<'a>,
    ) -> Result<Self, GatewayError> {
        context.require_original(original)?;
        context.check_document(proof, metadata, body)?;
        Ok(Self {
            proof,
            body,
            context,
            original,
        })
    }
    pub(crate) fn original_proof(&self) -> &'a AuthenticatedNativeImport {
        self.proof
    }
    pub(crate) fn original_body(&self) -> &'a [u8] {
        self.body
    }
    pub(crate) fn auth_data(
        &self,
    ) -> &gateway_native_http_admission::native_import::NativeAuthImportData<'_> {
        self.context.auth_data()
    }
    pub fn check(&self) -> Result<(), GatewayError> {
        self.context.require_original(self.original)?;
        self.context.check()
    }
    pub(crate) fn check_document(
        &self,
        proof: &AuthenticatedNativeImport,
        metadata: &NativeImportMetadata,
        body: &[u8],
    ) -> Result<(), GatewayError> {
        self.context.require_original(self.original)?;
        self.context.check_document(proof, metadata, body)
    }
    pub fn now_ms(&self) -> Result<i64, GatewayError> {
        self.original.now_ms()
    }
    pub fn deadline(&self) -> std::time::Instant {
        let now = std::time::Instant::now();
        let remaining = self
            .original
            .now_ms()
            .ok()
            .and_then(|ms| self.proof.deadline_ms().checked_sub(ms))
            .and_then(|ms| u64::try_from(ms).ok())
            .map(std::time::Duration::from_millis)
            .unwrap_or_default();
        self.original
            .deadline()
            .min(now.checked_add(remaining).unwrap_or(now))
    }
}

pub struct RequestIdentity<'a> {
    proof: &'a AuthenticatedNativeImport,
}
impl RequestIdentity<'_> {
    pub fn require_same(&self, context: &NativeImportInputs<'_>) -> Result<(), GatewayError> {
        if !std::ptr::eq(self.proof, context.proof) {
            return Err(gateway_native_custody_io::protocol::unavailable());
        }
        Ok(())
    }
}
impl<'a> NativeImportInputs<'a> {
    pub fn check_original(&self) -> Result<(), GatewayError> {
        self.check_document(self.proof, self.proof.metadata(), self.body)
    }
    pub fn validation_seed(
        &self,
    ) -> Result<(RequestIdentity<'a>, gateway_source::source_json::SourceJson), GatewayError> {
        self.check()?;
        Ok((
            RequestIdentity { proof: self.proof },
            self.auth_data().auth().clone(),
        ))
    }
    pub fn require_credentials_target(
        &self,
        credentials: &gateway_auth_credentials::native_credentials::NativeCredentials,
    ) -> Result<(), GatewayError> {
        match self.proof.target() {
            gateway_auth_native_contracts::native_import::NativeImportTarget::ExistingAccount {
                account_id_hash,
            } if *account_id_hash == credentials.routing_account_hash() => Ok(()),
            _ => Err(gateway_native_custody_io::protocol::unavailable()),
        }
    }
    pub fn require_start(
        &self,
        start: &gateway_authority::native_control::import::effects::NativeImportStart<'_>,
    ) -> Result<(), GatewayError> {
        start.require_body(self.body, self.now_ms()?)
    }
    pub fn require_start_identity(
        &self,
        start: &gateway_authority::native_control::import::effects::NativeImportStart<'_>,
    ) -> Result<(), GatewayError> {
        if start.binding().action_ref != self.proof.metadata().action_ref
            || start.binding().operation_id != self.proof.metadata().operation_id
        {
            return Err(gateway_native_custody_io::protocol::unavailable());
        }
        Ok(())
    }
    pub fn original_sha256(&self) -> String {
        gateway_auth_primitives::crypto::sha256_hex(self.body)
    }
    pub fn client_version_data(&self) -> Option<gateway_source::js_string::JsString16> {
        self.auth_data().client_version().cloned()
    }
    pub fn fixture_document_data(&self) -> Result<serde_json::Value, GatewayError> {
        self.auth_data()
            .auth()
            .deserialize()
            .map_err(|_| gateway_native_custody_io::protocol::unavailable())
    }
    pub fn recheck_remote<C: NativeRenewalCancellation>(
        &self,
        authority: &gateway_authority::native_control::import::remote::RemoteImportAuthority<'_, C>,
    ) -> Result<(), GatewayError> {
        authority.recheck_import(self.proof)
    }
    pub fn claim_remote<C: NativeRenewalCancellation>(
        &self,
        authority: &gateway_authority::native_control::import::remote::RemoteImportAuthority<'_, C>,
    ) -> Result<gateway_authority::native_control::import::effects::NativeImportClaim, GatewayError>
    {
        authority.claim_import(self.proof)
    }
    pub fn start_remote<C: NativeRenewalCancellation>(
        &self,
        authority: &gateway_authority::native_control::import::remote::RemoteImportAuthority<'_, C>,
        claim: &mut gateway_authority::native_control::import::effects::NativeImportClaim,
    ) -> Result<
        gateway_authority::native_control::import::effects::NativeImportStart<'a>,
        GatewayError,
    > {
        authority
            .start_import(claim, self.proof)?
            .ok_or_else(gateway_native_custody_io::protocol::unavailable)
    }
    pub fn require_remote_catalog<C: NativeRenewalCancellation>(
        &self,
        authority: &gateway_authority::native_control::import::remote::RemoteImportAuthority<'_, C>,
        candidate: &gateway_native_import_source::catalog::CatalogValidation,
    ) -> Result<(), GatewayError> {
        self.check()?;
        authority.recheck_import(self.proof)?;
        if matches!(
            candidate,
            gateway_native_import_source::catalog::CatalogValidation::AuthenticatedLimited
        ) && authority
            .observe_import_catalog(self.proof)?
            .revision()
            .is_none()
        {
            return Err(GatewayError::new(
                409,
                "Cannot store rate-limited Codex auth without an existing model catalog",
                Some("codex_catalog_required"),
            ));
        }
        self.check()
    }
    pub async fn check_current_local<
        S: AuthorityStore,
        I: TokenSource,
        C: NativeRenewalCancellation,
    >(
        &self,
        authority: &NativeRenewalAuthority<'_, S, I, C>,
    ) -> Result<(), GatewayError> {
        self.context.check_current(authority).await
    }
}

impl<'a> NativeImportInputs<'a> {
    pub fn checked_request_identity(&self) -> Result<RequestIdentity<'a>, GatewayError> {
        self.check_original()?;
        Ok(RequestIdentity { proof: self.proof })
    }
    pub fn require_start_at(
        &self,
        start: &gateway_authority::native_control::import::effects::NativeImportStart<'_>,
        now_ms: i64,
    ) -> Result<(), GatewayError> {
        start.require_body(self.body, now_ms)
    }
}

impl NativeImportInputs<'_> {
    /// Guarded custody framing retains the same private original context during I/O.
    pub fn send_custody_frame(
        &self,
        stream: &mut std::net::TcpStream,
        frame: &gateway_native_custody_wire::wire::Frame,
        deadline: Instant,
    ) -> Result<(), GatewayError> {
        gateway_native_custody_io::protocol::send(stream, frame, deadline, Some(self.context))
    }
    /// The decoded frame is DATA; it cannot construct or renew live custody.
    pub fn receive_custody_frame(
        &self,
        stream: &mut std::net::TcpStream,
        deadline: Instant,
    ) -> Result<gateway_native_custody_wire::wire::Frame, GatewayError> {
        gateway_native_custody_io::protocol::receive(stream, deadline, Some(self.context))
    }
}
