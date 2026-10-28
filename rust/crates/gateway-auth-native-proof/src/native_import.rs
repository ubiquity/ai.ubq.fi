// Native-only import authentication. Neither transport proof grants an effect.
use crate::native_auth::{NativeAuthenticator, NativeTransportIdentity};
use gateway_auth_native_contracts::{
    native_import::{NativeImportKind, NativeImportMetadata},
    nonce::NativeNonceAuthority,
    transport::{NativeCapability, NativeImportAuthHeaders},
};
use gateway_auth_native_protocol::import_frame::signature_input;
use gateway_auth_native_wire::native_read::NATIVE_AUTH_WINDOW_MS;
use gateway_auth_primitives::crypto::{base64url, decode_base64url, sha256_hex};
use gateway_source::error::GatewayError;
use hmac::{Hmac, Mac};
use sha2::Sha256;

/// Header admission only. Cannot be serialized, cloned, or used as effect authority.
/// ```compile_fail
/// use gateway_auth_native_proof::native_import::NativeImportAdmission;
/// let forged:NativeImportAdmission=serde_json::from_str("{}").unwrap();
/// ```
pub struct NativeImportAdmission {
    #[cfg(unix)]
    admission_identity: std::sync::Arc<()>,
    kind: NativeImportKind,
    metadata: NativeImportMetadata,
    body_sha256: String,
    owner_id: String,
    owner_epoch: String,
    fingerprint: String,
    authenticated_at_ms: i64,
    valid_through_ms: i64,
}
impl NativeImportAdmission {
    pub fn kind(&self) -> NativeImportKind {
        self.kind
    }
    pub fn metadata(&self) -> &NativeImportMetadata {
        &self.metadata
    }
    pub fn valid_through_ms(&self) -> i64 {
        self.valid_through_ms
    }
    pub fn require_owner(
        &self,
        identity: &NativeTransportIdentity,
        now: i64,
    ) -> Result<(), GatewayError> {
        self.require_time(now)?;
        if self.owner_id != identity.owner_id()
            || self.owner_epoch != identity.owner_epoch()
            || self.fingerprint != identity.key_fingerprint()
            || !identity.capabilities.contains(&capability(self.kind))
        {
            return Err(forbidden());
        }
        Ok(())
    }
    fn require_time(&self, now: i64) -> Result<(), GatewayError> {
        if now < self.authenticated_at_ms || now > self.valid_through_ms {
            return Err(forbidden());
        }
        Ok(())
    }
    pub fn verify_body(
        self,
        body: &[u8],
        now: i64,
    ) -> Result<VerifiedNativeImportBody, GatewayError> {
        self.require_time(now)?;
        if sha256_hex(body) != self.body_sha256 {
            return Err(forbidden());
        }
        Ok(VerifiedNativeImportBody { admission: self })
    }
}
/// Exact native body binding; still not an actor/action/source permission.
pub struct VerifiedNativeImportBody {
    admission: NativeImportAdmission,
}
impl VerifiedNativeImportBody {
    #[cfg(unix)]
    pub fn require_context(
        &self,
        context: &crate::native_context::NativeControlContext<'_>,
    ) -> Result<(), GatewayError> {
        context.require_identity(&self.admission.admission_identity)
    }
    pub fn kind(&self) -> NativeImportKind {
        self.admission.kind()
    }
    pub fn metadata(&self) -> &NativeImportMetadata {
        self.admission.metadata()
    }
    pub fn require_owner(
        &self,
        identity: &NativeTransportIdentity,
        now: i64,
    ) -> Result<(), GatewayError> {
        self.admission.require_owner(identity, now)
    }
    pub fn require_body(&self, body: &[u8], now: i64) -> Result<(), GatewayError> {
        self.admission.require_time(now)?;
        if sha256_hex(body) != self.admission.body_sha256 {
            return Err(forbidden());
        }
        Ok(())
    }
}
impl NativeTransportIdentity {
    pub fn sign_import(
        &self,
        kind: NativeImportKind,
        metadata: &NativeImportMetadata,
        body_sha256: &str,
        time_ms: i64,
        nonce: &str,
    ) -> Result<String, GatewayError> {
        let input = signature_input(kind, metadata, body_sha256, self.key_id(), time_ms, nonce)?;
        let mut mac = Hmac::<Sha256>::new_from_slice(&self.key).map_err(|_| {
            gateway_auth_primitives::error::invalid("Invalid native import metadata")
        })?;
        mac.update(&input);
        Ok(base64url(&mac.finalize().into_bytes()))
    }
}
impl<R: NativeNonceAuthority> NativeAuthenticator<'_, R> {
    #[cfg(unix)]
    pub async fn admit_import_with_context<'c>(
        &self,
        kind: NativeImportKind,
        headers: Result<NativeImportAuthHeaders<'_>, GatewayError>,
        metadata: &NativeImportMetadata,
        lifetime: crate::native_context::NativeRequestLifetime<'c>,
    ) -> Result<
        (
            NativeImportAdmission,
            crate::native_context::NativeControlContext<'c>,
        ),
        GatewayError,
    > {
        lifetime.check()?;
        let headers = headers?;
        let admission = self
            .admit_import(kind, headers, metadata, crate::native_context::now_ms()?)
            .await?;
        let context = crate::native_context::NativeControlContext::admitted(
            lifetime,
            headers.auth,
            admission.admission_identity.clone(),
        );
        Ok((admission, context))
    }
    pub async fn admit_import(
        &self,
        kind: NativeImportKind,
        headers: NativeImportAuthHeaders<'_>,
        metadata: &NativeImportMetadata,
        now: i64,
    ) -> Result<NativeImportAdmission, GatewayError> {
        let auth = headers.auth;
        if auth.version != "1"
            || auth.key_id != self.identity.key_id()
            || !(0..=gateway_contracts::identity::MAX_SAFE_INTEGER).contains(&now)
        {
            return Err(unauthorized());
        }
        let time = auth
            .time_ms
            .parse::<i64>()
            .ok()
            .filter(|t| t.to_string() == auth.time_ms)
            .ok_or_else(unauthorized)?;
        if now.abs_diff(time) > NATIVE_AUTH_WINDOW_MS as u64 {
            return Err(unauthorized());
        }
        let input = signature_input(
            kind,
            metadata,
            headers.body_sha256,
            auth.key_id,
            time,
            auth.nonce,
        )
        .map_err(|_| unauthorized())?;
        let signature = decode_base64url(auth.mac).map_err(|_| unauthorized())?;
        if signature.len() != 32 || base64url(&signature) != auth.mac {
            return Err(unauthorized());
        }
        let mut mac =
            Hmac::<Sha256>::new_from_slice(&self.identity.key).map_err(|_| unauthorized())?;
        mac.update(&input);
        mac.verify_slice(&signature).map_err(|_| unauthorized())?;
        if !self.identity.capabilities.contains(&capability(kind)) {
            return Err(forbidden());
        }
        let valid_through_ms = time
            .checked_add(NATIVE_AUTH_WINDOW_MS)
            .ok_or_else(unauthorized)?;
        self.replay
            .consume_nonce(auth.key_id, auth.nonce, valid_through_ms, now)
            .await?;
        Ok(NativeImportAdmission {
            #[cfg(unix)]
            admission_identity: std::sync::Arc::new(()),
            kind,
            metadata: metadata.clone(),
            body_sha256: headers.body_sha256.into(),
            owner_id: self.identity.owner_id().into(),
            owner_epoch: self.identity.owner_epoch().into(),
            fingerprint: self.identity.key_fingerprint().into(),
            authenticated_at_ms: now,
            valid_through_ms,
        })
    }
}
fn capability(kind: NativeImportKind) -> NativeCapability {
    match kind {
        NativeImportKind::CodexAuth => NativeCapability::CodexAuthImport,
        NativeImportKind::NativeProfile => NativeCapability::NativeProfileImport,
    }
}
fn unauthorized() -> GatewayError {
    GatewayError::new(
        401,
        "Native carrier authentication failed",
        Some("native_auth_invalid"),
    )
}
fn forbidden() -> GatewayError {
    GatewayError::new(
        403,
        "Native import proof is unavailable or mismatched",
        Some("native_capability_forbidden"),
    )
}
