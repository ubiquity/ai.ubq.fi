//! Explicit private fixture adapter: existing Host defaults/mismatch cases are never modified.
use gateway_auth_contracts::types::AuthPrincipal;
use gateway_auth_credentials::provider_credentials::{
    codex_source_pool_key, parse_codex_source_pool,
};
use gateway_authority_paid_records::{PaidReservation, PaidUsage};
use gateway_authority_routing_selection::types::CohortProof;
use gateway_contracts::{claude::ClaudeErrorEvidence, model::ModelId};
use gateway_core::state::AuthorityStore;
use gateway_model_selection::aliases::SourceStringCollator;
use gateway_routing_journal::record::CredentialFence;
use gateway_routing_runtime::types::{
    ClaudeOutcome, ClaudeSelection, CodexRecovery, CredentialRequest, PublishedCredential,
    RefreshOutcome, ResponseHead, RoutingHost,
};
use gateway_routing_runtime_test_support::{host::Host, store::Store};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use gateway_wait_observation::outcome::WaitOutcome;
use std::{
    sync::{Arc, Mutex},
    task::{Poll, Waker},
};
#[derive(Default)]
pub(super) struct Pause(Mutex<(bool, Option<Waker>)>);
impl Pause {
    pub(super) fn release(&self) {
        let wake = {
            let mut state = self.0.lock().unwrap();
            state.0 = true;
            state.1.take()
        };
        if let Some(wake) = wake {
            wake.wake();
        }
    }
    async fn wait(&self) {
        std::future::poll_fn(|cx| {
            let mut state = self.0.lock().unwrap();
            if state.0 {
                Poll::Ready(())
            } else {
                state.1 = Some(cx.waker().clone());
                Poll::Pending
            }
        })
        .await
    }
}
#[derive(Clone)]
pub(super) struct NativeRefreshHost {
    base: Host,
    pause: Arc<Pause>,
}
impl NativeRefreshHost {
    pub(super) fn new(store: &Store) -> (Self, Arc<Pause>) {
        let pause = Arc::new(Pause::default());
        (
            Self {
                base: Host::new(store),
                pause: pause.clone(),
            },
            pause,
        )
    }
}
impl RoutingHost for NativeRefreshHost {
    type ClaudeHead = u64;
    fn source_string_collator(&self) -> Arc<dyn SourceStringCollator> {
        self.base.source_string_collator()
    }
    fn cancelled(&self) -> bool {
        self.base.cancelled()
    }
    fn cancellation_error(&self) -> GatewayError {
        self.base.cancellation_error()
    }
    async fn wait(&self, ms: i64) -> WaitOutcome {
        self.base.wait(ms).await
    }
    async fn credential(
        &self,
        input: CredentialRequest,
    ) -> Result<PublishedCredential, GatewayError> {
        let mut result = self.base.credential(input).await?;
        if let PublishedCredential::Scalar {
            material,
            fence:
                CredentialFence::Codex {
                    admission,
                    native_generation,
                    ..
                },
            ..
        } = &mut result
        {
            let raw = self
                .base
                .store
                .read(&codex_source_pool_key())
                .await?
                .ok_or_else(unavailable)?;
            let source = parse_codex_source_pool(raw.json_value()?)?.ok_or_else(unavailable)?;
            let slot = usize::try_from(admission.binding.slot).map_err(|_| unavailable())?;
            let account = source.accounts.get(slot).ok_or_else(unavailable)?;
            let identity = account.identity();
            if identity.routing_account_hash != admission.binding.account_id_hash
                || identity.credential_version != admission.binding.credential_version
            {
                return Err(unavailable());
            }
            material.secret = account.access_token.expose().into();
            material.account_id = Some(account.account_id.clone());
            *native_generation = identity.native_generation_hash;
        }
        Ok(result)
    }
    async fn ensure_credential_current(
        &self,
        f: &CredentialFence,
        p: &AuthPrincipal,
        m: &ModelId,
    ) -> Result<(), GatewayError> {
        self.base.ensure_credential_current(f, p, m).await
    }
    async fn refresh_codex(&self, _: &CredentialFence) -> Result<RefreshOutcome, GatewayError> {
        self.pause.wait().await;
        Ok(RefreshOutcome::Unavailable)
    }
    async fn select_claude(
        &self,
        p: &AuthPrincipal,
        m: &ModelId,
        r: &SourceJson,
        e: Option<&JsString16>,
    ) -> Result<ClaudeSelection, GatewayError> {
        self.base.select_claude(p, m, r, e).await
    }
    async fn observe_claude_response(
        &self,
        f: &CredentialFence,
        m: &ModelId,
        r: ResponseHead,
    ) -> Result<u64, GatewayError> {
        self.base.observe_claude_response(f, m, r).await
    }
    async fn finish_claude_response(
        &self,
        h: u64,
        e: ClaudeErrorEvidence,
    ) -> Result<ClaudeOutcome, GatewayError> {
        self.base.finish_claude_response(h, e).await
    }
    async fn recover_codex(
        &self,
        p: &CohortProof,
        m: &ModelId,
    ) -> Result<CodexRecovery, GatewayError> {
        self.base.recover_codex(p, m).await
    }
    async fn lookup_paid(&self, r: &PaidReservation) -> Result<Option<PaidUsage>, GatewayError> {
        self.base.lookup_paid(r).await
    }
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Private inference fixture source is unavailable.",
        "native_inference_fixture_unavailable",
    )
}
