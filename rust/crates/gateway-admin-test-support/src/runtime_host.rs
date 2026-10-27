use gateway_auth_contracts::{
    types::AuthHost,
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
    webauthn::{CosePublicKey, SignatureInput, WebAuthnCrypto},
};
use gateway_source::error::GatewayError;
use std::{
    cell::Cell,
    future::Future,
    task::{Context as TaskContext, Poll, Waker},
};

pub struct Host {
    pub now: Cell<i64>,
    pub serial: Cell<u64>,
}
impl Default for Host {
    fn default() -> Self {
        Self {
            now: Cell::new(1_000_000),
            serial: Cell::new(0),
        }
    }
}
impl WebAuthnCrypto for Host {
    async fn validate_key(&self, _: &CosePublicKey) -> Result<(), GatewayError> {
        Err(GatewayError::unavailable(
            "No cryptographic fixture",
            "fixture_missing",
        ))
    }
    async fn verify_signature(&self, _: SignatureInput) -> Result<bool, GatewayError> {
        Err(GatewayError::unavailable(
            "No cryptographic fixture",
            "fixture_missing",
        ))
    }
}
impl AuthHost for Host {
    fn now_ms(&self) -> i64 {
        self.now.get()
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
            "Network denied",
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
            "No kernel fixture",
            "fixture_missing",
        ))
    }
}

pub fn run<F: Future>(future: F) -> F::Output {
    let mut cx = TaskContext::from_waker(Waker::noop());
    let mut future = std::pin::pin!(future);
    loop {
        if let Poll::Ready(value) = future.as_mut().poll(&mut cx) {
            return value;
        }
        std::thread::yield_now();
    }
}
