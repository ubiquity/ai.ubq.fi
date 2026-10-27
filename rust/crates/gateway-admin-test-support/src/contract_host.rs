//! Original admin-contract clock, entropy and denied host capabilities.
use gateway_auth::types::AuthHost;
use gateway_auth::upstream::{AuthUpstreamRequest, AuthUpstreamResponse};
use gateway_source::error::GatewayError;
use std::cell::Cell;

#[derive(Default)]
pub struct Host {
    serial: Cell<u64>,
}
impl AuthHost for Host {
    fn now_ms(&self) -> i64 {
        1_000_000
    }
    fn random_bytes(&self, len: usize) -> Result<Vec<u8>, GatewayError> {
        let n = self.serial.get() + 1;
        self.serial.set(n);
        Ok((0..len).map(|i| (n + i as u64) as u8).collect())
    }
    async fn fetch_auth(
        &self,
        _: AuthUpstreamRequest,
    ) -> Result<AuthUpstreamResponse, GatewayError> {
        Err(GatewayError::unavailable(
            "Fixture network denied",
            "fixture_network_denied",
        ))
    }
    async fn kernel_principal(
        &self,
        _: &str,
        _: &str,
        _: &str,
        _: &str,
    ) -> Result<String, GatewayError> {
        Err(GatewayError::unavailable(
            "Fixture kernel absent",
            "fixture_kernel_absent",
        ))
    }
}
impl gateway_authority::store::TokenSource for &Host {
    fn next_token(&self) -> Result<String, GatewayError> {
        gateway_auth::crypto::uuid_from_random(&self.random_bytes(16)?)
    }
}
impl gateway_auth::webauthn::WebAuthnCrypto for Host {
    async fn validate_key(
        &self,
        _: &gateway_auth::webauthn::CosePublicKey,
    ) -> Result<(), GatewayError> {
        Err(GatewayError::unavailable(
            "Fixture verifier absent",
            "fixture_verifier_absent",
        ))
    }
    async fn verify_signature(
        &self,
        _: gateway_auth::webauthn::SignatureInput,
    ) -> Result<bool, GatewayError> {
        Err(GatewayError::unavailable(
            "Fixture verifier absent",
            "fixture_verifier_absent",
        ))
    }
}
