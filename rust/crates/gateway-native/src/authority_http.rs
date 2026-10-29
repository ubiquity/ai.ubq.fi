//! Authenticated fixed reverse authority over one preconnected TLS capability.
//! Parsed references are observations until the same-store receiver verifies
//! the original actor/action. This client cannot mint a local action or claim.
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_native_proof::native_context::NativeControlContext;
use gateway_auth_native_wire::{
    control::{
        NATIVE_AUTHORITY_MAX_BYTES, NATIVE_CONTROL_MAX_BYTES, NATIVE_RENEWAL_MAX_MS, unverified,
    },
    native_read::{
        NATIVE_KEY_HEADER, NATIVE_MAC_HEADER, NATIVE_NONCE_HEADER, NATIVE_RENEWAL_AUTHORITY_PATH,
        NATIVE_RESPONSE_MAC_HEADER, NATIVE_TIME_HEADER, NATIVE_VERSION_HEADER,
    },
};
use gateway_auth_primitives::crypto::base64url;
use gateway_native_authority_request::NativeRenewalAuthorityRequest as Request;
use gateway_native_authority_request::response::{identifier, validate, validate_source};
use gateway_native_authority_wire::{
    renewal::NativeRenewalMode, renewal_response::NativeRenewalAuthorityResponse as Response,
    source_response::SourceNativeRenewalAuthorityResponseV2 as SourceResponse,
};
use gateway_native_private_https::transport::PrivateHttps;
use gateway_source::error::GatewayError;
use std::{net::TcpStream, time::Instant};

pub struct NativeAuthorityHttp {
    client: std::sync::Arc<()>,
    identity: NativeTransportIdentity,
    transport: PrivateHttps,
}
impl NativeAuthorityHttp {
    pub fn from_connected(
        target: &str,
        socket: TcpStream,
        identity: NativeTransportIdentity,
    ) -> Result<Self, GatewayError> {
        Self::with_roots(target, socket, identity, ureq::tls::RootCerts::WebPki)
    }
    /// Trusted fixture/operator preparation only; roots never come from a
    /// control request. The normal constructor retains WebPKI validation.
    pub fn with_roots(
        target: &str,
        socket: TcpStream,
        identity: NativeTransportIdentity,
        roots: ureq::tls::RootCerts,
    ) -> Result<Self, GatewayError> {
        Ok(Self {
            client: std::sync::Arc::new(()),
            identity,
            transport: PrivateHttps::from_connected(
                target,
                NATIVE_RENEWAL_AUTHORITY_PATH,
                socket,
                roots,
            )?,
        })
    }
    fn response_peer(&self) -> (&str, &str) {
        (self.identity.owner_id(), self.identity.owner_epoch())
    }
    pub fn exchange(
        &self,
        request: &Request,
        context: &NativeControlContext<'_>,
    ) -> Result<Response, GatewayError> {
        let (body, now) = self.exchange_body(request, context)?;
        let decoded: Response = serde_json::from_slice(&body).map_err(|_| unverified())?;
        validate(request, &decoded, now, self.response_peer())?;
        Ok(decoded)
    }
    /// Strict signed source response DATA only; no CurrentGuard or Start is minted.
    pub fn exchange_source(
        &self,
        request: &Request,
        context: &NativeControlContext<'_>,
    ) -> Result<SourceResponse, GatewayError> {
        let (body, now) = self.exchange_body(request, context)?;
        let decoded: SourceResponse = serde_json::from_slice(&body).map_err(|_| unverified())?;
        validate_source(request, &decoded, now, self.response_peer())?;
        Ok(decoded)
    }
    fn exchange_body(
        &self,
        request: &Request,
        context: &NativeControlContext<'_>,
    ) -> Result<(Vec<u8>, i64), GatewayError> {
        context.check()?;
        request.validate()?;
        let body = serde_json::to_vec(request).map_err(|_| unverified())?;
        if body.len() > NATIVE_AUTHORITY_MAX_BYTES {
            return Err(unverified());
        }
        let mut nonce_bytes = [0u8; 32];
        getrandom::fill(&mut nonce_bytes).map_err(|_| unverified())?;
        let nonce = base64url(&nonce_bytes);
        let now = context.now_ms()?;
        let signature = self
            .identity
            .sign(NATIVE_RENEWAL_AUTHORITY_PATH, now, &nonce, &body)?;
        let headers = vec![
            (NATIVE_VERSION_HEADER.into(), "1".into()),
            (NATIVE_KEY_HEADER.into(), self.identity.key_id().into()),
            (NATIVE_TIME_HEADER.into(), now.to_string()),
            (NATIVE_NONCE_HEADER.into(), nonce.clone()),
            (NATIVE_MAC_HEADER.into(), signature),
        ];
        let response = self.transport.send(
            &headers,
            &body,
            context.deadline(),
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
        context.check()?;
        if response.status != 200 {
            return Err(unverified());
        }
        Ok((response.body, now))
    }
}

/// The preopened transport never extends its original effect lease.
pub fn authority_deadline(
    context: &NativeControlContext<'_>,
    deadline_ms: i64,
) -> Result<Instant, GatewayError> {
    let remaining = deadline_ms
        .checked_sub(context.now_ms()?)
        .filter(|value| *value > 0 && *value <= NATIVE_RENEWAL_MAX_MS)
        .ok_or_else(unverified)?;
    Ok(context
        .deadline()
        .min(Instant::now() + std::time::Duration::from_millis(remaining as u64)))
}

#[cfg(all(test, target_os = "linux"))]
mod tests;

pub(crate) mod owned_claim;
