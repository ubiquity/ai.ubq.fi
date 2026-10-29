//! Private native protocol ownership, minted only after the actual signed Claim exchange.
use super::*;
use gateway_auth_native_proof::native_auth::{AuthenticatedNativeOwner, NativeCapability};
use gateway_native_authority_request::OriginalControlHeaders;
use gateway_native_authority_request::response::{
    RenewalResponseVersion as Version, response_version,
};
use gateway_native_authority_wire::{
    renewal::NativeRenewalFence, source_fence::SourceNativeRenewalFenceV3,
};
use std::{cell::RefCell, sync::Arc};
pub(crate) struct RemoteClaim<F> {
    pub(crate) action_ref: String,
    pub(crate) operation_id: String,
    pub(crate) claim_ref: String,
    pub(crate) start_permit: RefCell<Option<String>>,
    pub(crate) mode: NativeRenewalMode,
    pub(crate) deadline_ms: i64,
    pub(crate) fence: F,
    pub(crate) publication_revision: Option<String>,
}
enum Payload {
    Legacy(RemoteClaim<NativeRenewalFence>),
    Source(RemoteClaim<SourceNativeRenewalFenceV3>),
}
pub(crate) struct VerifiedOwnedRenewalClaim {
    payload: Payload,
    report: OwnedUnknownReport,
}
pub(crate) struct OwnedUnknownReport {
    client: Arc<()>,
    owner_id: String,
    owner_epoch: String,
    fingerprint: String,
    operation_id: String,
    claim_ref: String,
    request_digest: String,
    version: Version,
}
impl VerifiedOwnedRenewalClaim {
    pub(crate) fn legacy(&self) -> Result<&RemoteClaim<NativeRenewalFence>, GatewayError> {
        match &self.payload {
            Payload::Legacy(claim) => Ok(claim),
            Payload::Source(_) => Err(unverified()),
        }
    }
    pub(crate) fn report_execution_error(
        self,
        authority: &NativeAuthorityHttp,
    ) -> Result<(), GatewayError> {
        authority.record_unknown_once(self.report)
    }
}
impl NativeAuthorityHttp {
    pub(crate) fn claim_for_control(
        &self,
        proof: &AuthenticatedNativeOwner,
        raw: &[u8],
        context: &NativeControlContext<'_>,
    ) -> Result<VerifiedOwnedRenewalClaim, GatewayError> {
        proof.require_request(
            NativeCapability::OwnerRenewal,
            gateway_auth_native_wire::native_read::NATIVE_CONTROL_PATH,
            raw,
            context.now_ms()?,
        )?;
        context.require_owner_context(proof)?;
        if proof.owner_id() != self.identity.owner_id()
            || proof.owner_epoch() != self.identity.owner_epoch()
            || proof.transport_key_fingerprint() != self.identity.key_fingerprint()
        {
            return Err(unverified());
        }
        let auth = context.original_auth_headers();
        let request = Request::Claim {
            v: 1,
            original_control_body_base64url: base64url(raw),
            original_auth_headers: OriginalControlHeaders {
                version: auth.version.into(),
                key_id: auth.key_id.into(),
                time_ms: auth.time_ms.into(),
                nonce: auth.nonce.into(),
                mac: auth.mac.into(),
            },
        };
        let (body, now) = self.exchange_body(&request, context)?;
        let version = response_version(&body)?;
        let payload = match version {
            Version::Legacy => {
                let decoded: Response = serde_json::from_slice(&body).map_err(|_| unverified())?;
                validate(&request, &decoded, now, self.response_peer())?;
                let Response::Claim {
                    action_ref,
                    operation_id,
                    claim_ref,
                    start_permit,
                    mode,
                    deadline_ms,
                    fence,
                    publication_revision,
                    ..
                } = decoded
                else {
                    return Err(unverified());
                };
                Payload::Legacy(RemoteClaim {
                    action_ref,
                    operation_id,
                    claim_ref,
                    start_permit: RefCell::new(start_permit),
                    mode,
                    deadline_ms,
                    fence,
                    publication_revision,
                })
            }
            Version::Source => {
                let decoded: SourceResponse =
                    serde_json::from_slice(&body).map_err(|_| unverified())?;
                validate_source(&request, &decoded, now, self.response_peer())?;
                let SourceResponse::Claim {
                    action_ref,
                    operation_id,
                    claim_ref,
                    start_permit,
                    mode,
                    deadline_ms,
                    fence,
                    publication_revision,
                    ..
                } = decoded
                else {
                    return Err(unverified());
                };
                Payload::Source(RemoteClaim {
                    action_ref,
                    operation_id,
                    claim_ref,
                    start_permit: RefCell::new(start_permit),
                    mode,
                    deadline_ms,
                    fence,
                    publication_revision,
                })
            }
        };
        let (operation_id, claim_ref) = match &payload {
            Payload::Legacy(claim) => (claim.operation_id.clone(), claim.claim_ref.clone()),
            Payload::Source(claim) => (claim.operation_id.clone(), claim.claim_ref.clone()),
        };
        Ok(VerifiedOwnedRenewalClaim {
            payload,
            report: OwnedUnknownReport {
                client: self.client.clone(),
                owner_id: self.identity.owner_id().into(),
                owner_epoch: self.identity.owner_epoch().into(),
                fingerprint: self.identity.key_fingerprint().into(),
                operation_id,
                claim_ref,
                request_digest: gateway_auth_primitives::crypto::sha256_hex(raw),
                version,
            },
        })
    }
    fn record_unknown_once(&self, report: OwnedUnknownReport) -> Result<(), GatewayError> {
        if !Arc::ptr_eq(&self.client, &report.client)
            || report.owner_id != self.identity.owner_id()
            || report.owner_epoch != self.identity.owner_epoch()
            || report.fingerprint != self.identity.key_fingerprint()
            || !identifier(&report.operation_id)
            || !identifier(&report.claim_ref)
            || !gateway_auth_credentials::provider_credentials::valid_native_generation(
                &report.request_digest,
            )
        {
            return Err(unverified());
        }
        let request = Request::Unknown {
            v: 1,
            operation_id: report.operation_id,
            claim_ref: report.claim_ref,
        };
        request.validate()?;
        let body = serde_json::to_vec(&request).map_err(|_| unverified())?;
        if body.len() > NATIVE_AUTHORITY_MAX_BYTES {
            return Err(unverified());
        }
        let mut bytes = [0u8; 32];
        getrandom::fill(&mut bytes).map_err(|_| unverified())?;
        let nonce = base64url(&bytes);
        let now = gateway_auth_native_proof::native_context::now_ms()?;
        let mac = self
            .identity
            .sign(NATIVE_RENEWAL_AUTHORITY_PATH, now, &nonce, &body)?;
        let headers = vec![
            (NATIVE_VERSION_HEADER.into(), "1".into()),
            (NATIVE_KEY_HEADER.into(), self.identity.key_id().into()),
            (NATIVE_TIME_HEADER.into(), now.to_string()),
            (NATIVE_NONCE_HEADER.into(), nonce.clone()),
            (NATIVE_MAC_HEADER.into(), mac),
        ];
        let response = self.transport.send(
            &headers,
            &body,
            Instant::now() + crate::private_https::NATIVE_HTTPS_SEND_TIMEOUT,
            NATIVE_CONTROL_MAX_BYTES,
        )?;
        let signatures = response
            .headers
            .iter()
            .filter(|(name, _)| name.eq_ignore_ascii_case(NATIVE_RESPONSE_MAC_HEADER))
            .collect::<Vec<_>>();
        if signatures.len() != 1 {
            return Err(unverified());
        }
        self.identity
            .verify_authority_response(
                &nonce,
                &body,
                response.status,
                &response.body,
                &signatures[0].1,
            )
            .map_err(|_| unverified())?;
        if response.status != 200 || response_version(&response.body)? != report.version {
            return Err(unverified());
        }
        match report.version {
            Version::Legacy => {
                let decoded: Response =
                    serde_json::from_slice(&response.body).map_err(|_| unverified())?;
                validate(&request, &decoded, now, self.response_peer())?;
                if !matches!(decoded, Response::Unknown { v: 1 }) {
                    return Err(unverified());
                }
            }
            Version::Source => {
                let decoded: SourceResponse =
                    serde_json::from_slice(&response.body).map_err(|_| unverified())?;
                validate_source(&request, &decoded, now, self.response_peer())?;
                if !matches!(decoded, SourceResponse::Unknown { v: 2 }) {
                    return Err(unverified());
                }
            }
        }
        Ok(())
    }
}
