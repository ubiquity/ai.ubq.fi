//! Private fixed-Codex fixture host over the real RemoteAuthorityStore.
use super::{TokenClock, invalid};
use gateway_auth_contracts::types::AuthPrincipal;
use gateway_auth_credentials::provider_credentials::{
    codex_source_pool_key, read_codex_source_pool,
};
use gateway_authority_contracts::AuthorityClock;
use gateway_authority_paid_records::{PaidReservation, PaidUsage};
use gateway_authority_routing_selection::{SelectionOperations, types::CohortProof};
use gateway_contracts::{claude::ClaudeErrorEvidence, model::ModelId, provider::UpstreamProvider};
use gateway_provider_protocol::types::{AuthMaterial, PrepareOptions};
use gateway_routing_journal::record::CredentialFence;
use gateway_routing_runtime::types::{
    ClaudeOutcome, ClaudeSelection, CodexRecovery, CredentialRequest, PublishedCredential,
    RefreshOutcome, ResponseHead, RoutingHost,
};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use gateway_wait_observation::outcome::WaitOutcome;
use gateway_worker_authority_binding::authority::RemoteAuthorityStore;
use gateway_worker_platform::cancellation::Cancellation;
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    task::{Poll, Waker},
};
#[derive(Default)]
pub(super) struct Pause {
    ready: Cell<bool>,
    outcome: RefCell<Option<Result<(), GatewayError>>>,
    waker: RefCell<Option<Waker>>,
}
impl Pause {
    pub fn release(&self) {
        self.ready.set(true);
        if let Some(waker) = self.waker.borrow_mut().take() {
            waker.wake();
        }
    }
    pub fn finish(&self, result: Result<(), GatewayError>) {
        *self.outcome.borrow_mut() = Some(result);
        if let Some(waker) = self.waker.borrow_mut().take() {
            waker.wake();
        }
    }
    pub async fn settlement(&self) -> Result<(), GatewayError> {
        std::future::poll_fn(|cx| {
            if let Some(result) = self.outcome.borrow_mut().take() {
                Poll::Ready(result)
            } else {
                *self.waker.borrow_mut() = Some(cx.waker().clone());
                Poll::Pending
            }
        })
        .await
    }
    async fn wait(&self) {
        std::future::poll_fn(|cx| {
            if self.ready.get() {
                Poll::Ready(())
            } else {
                *self.waker.borrow_mut() = Some(cx.waker().clone());
                Poll::Pending
            }
        })
        .await
    }
}
#[derive(Clone)]
pub(super) struct CodexHost {
    pub store: RemoteAuthorityStore,
    pub pause: Rc<Pause>,
    pub cancellation: Rc<Cancellation>,
}
impl RoutingHost for CodexHost {
    type ClaudeHead = ();
    fn source_string_collator(
        &self,
    ) -> std::sync::Arc<dyn gateway_model_selection::aliases::SourceStringCollator> {
        gateway_worker_platform::source_collator::shared()
    }
    fn cancelled(&self) -> bool {
        self.cancellation.signal().aborted()
    }
    fn cancellation_error(&self) -> GatewayError {
        self.cancellation.routing_error()
    }
    async fn wait(&self, _: i64) -> WaitOutcome {
        WaitOutcome::NotStarted { error: invalid() }
    }
    async fn credential(
        &self,
        input: CredentialRequest,
    ) -> Result<PublishedCredential, GatewayError> {
        if input.provider != UpstreamProvider::ChatgptCodex {
            return Err(invalid());
        }
        let admission = input.codex.ok_or_else(invalid)?;
        let source = read_codex_source_pool(&self.store, TokenClock.now_ms()?)
            .await?
            .ok_or_else(invalid)?;
        if source.revision != admission.binding.pool_versionstamp.0 {
            return Err(invalid());
        }
        let account = source
            .pool
            .accounts
            .into_iter()
            .find(|a| {
                let i = a.identity();
                i.routing_account_hash == admission.binding.account_id_hash
                    && i.credential_version == admission.binding.credential_version
            })
            .ok_or_else(invalid)?;
        let generation = account.identity().native_generation_hash;
        Ok(PublishedCredential::Scalar {
            material: AuthMaterial {
                secret: account.access_token.expose().into(),
                account_id: Some(account.account_id),
                ..Default::default()
            },
            fence: CredentialFence::Codex {
                admission,
                source_key: codex_source_pool_key(),
                source_revision: source.revision,
                native_generation: generation,
            },
            options: Box::new(PrepareOptions::default()),
        })
    }
    async fn ensure_credential_current(
        &self,
        fence: &CredentialFence,
        _: &AuthPrincipal,
        _: &ModelId,
    ) -> Result<(), GatewayError> {
        let CredentialFence::Codex {
            admission,
            source_key,
            source_revision,
            native_generation,
        } = fence
        else {
            return Err(invalid());
        };
        let now = TokenClock.now_ms()?;
        gateway_authority::store::Authority::with_clock(self.store.clone(), TokenClock, TokenClock)
            .ensure_admission_current(admission, now)
            .await?;
        let source = read_codex_source_pool(&self.store, now)
            .await?
            .ok_or_else(invalid)?;
        if *source_key != codex_source_pool_key()
            || source.revision != *source_revision
            || !source.pool.accounts.iter().any(|a| {
                let i = a.identity();
                i.routing_account_hash == admission.binding.account_id_hash
                    && i.credential_version == admission.binding.credential_version
                    && i.native_generation_hash == *native_generation
            })
        {
            return Err(invalid());
        }
        Ok(())
    }
    async fn refresh_codex(&self, _: &CredentialFence) -> Result<RefreshOutcome, GatewayError> {
        self.pause.wait().await;
        Ok(RefreshOutcome::Unavailable)
    }
    async fn select_claude(
        &self,
        _: &AuthPrincipal,
        _: &ModelId,
        _: &SourceJson,
        _: Option<&JsString16>,
    ) -> Result<ClaudeSelection, GatewayError> {
        Err(invalid())
    }
    async fn observe_claude_response(
        &self,
        _: &CredentialFence,
        _: &ModelId,
        _: ResponseHead,
    ) -> Result<(), GatewayError> {
        Err(invalid())
    }
    async fn finish_claude_response(
        &self,
        _: (),
        _: ClaudeErrorEvidence,
    ) -> Result<ClaudeOutcome, GatewayError> {
        Err(invalid())
    }
    async fn recover_codex(
        &self,
        _: &CohortProof,
        _: &ModelId,
    ) -> Result<CodexRecovery, GatewayError> {
        Err(invalid())
    }
    async fn lookup_paid(&self, _: &PaidReservation) -> Result<Option<PaidUsage>, GatewayError> {
        Err(invalid())
    }
}
