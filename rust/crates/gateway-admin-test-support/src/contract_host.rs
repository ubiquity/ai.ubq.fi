//! Original admin-contract clock, entropy and denied host capabilities.
use gateway_auth_contracts::types::AuthHost;
use gateway_auth_contracts::{upstream::AuthUpstreamRequest, upstream::AuthUpstreamResponse};
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
impl gateway_authority_contracts::TokenSource for &Host {
    fn next_token(&self) -> Result<String, GatewayError> {
        gateway_auth_primitives::crypto::uuid_from_random(&self.random_bytes(16)?)
    }
}
impl gateway_auth_contracts::webauthn::WebAuthnCrypto for Host {
    async fn validate_key(
        &self,
        _: &gateway_auth_contracts::webauthn::CosePublicKey,
    ) -> Result<(), GatewayError> {
        Err(GatewayError::unavailable(
            "Fixture verifier absent",
            "fixture_verifier_absent",
        ))
    }
    async fn verify_signature(
        &self,
        _: gateway_auth_contracts::webauthn::SignatureInput,
    ) -> Result<bool, GatewayError> {
        Err(GatewayError::unavailable(
            "Fixture verifier absent",
            "fixture_verifier_absent",
        ))
    }
}
