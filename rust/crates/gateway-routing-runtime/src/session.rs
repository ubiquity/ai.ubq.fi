use crate::{
    journal::{self, Journal, unavailable},
    policy::{self, RoutePlan},
    settlement,
    types::*,
};
use gateway_auth::types::AuthMethod;
use gateway_authority::routing_types::RouteSelection;
use gateway_authority::{paid_control::PaidRoutingReceipt, store::Authority};
use gateway_authority::{paid_types::PaidAuthorization, routing_types::AdmittedAccount};
use gateway_authority_api_quota::ApiQuotaOperations;
use gateway_authority_api_release::ApiReleaseOperations;
use gateway_authority_claude_proof::ClaudeProofOperations;
use gateway_authority_contracts::{AuthorityClock, TokenSource};
use gateway_authority_kernel::KernelOperations;
use gateway_authority_kernel_protocol::KernelTerminal;
use gateway_authority_paid_control_records::{PaidRequestContext, PaidSyntheticHop};
use gateway_core::{error::GatewayError, identity::ApiKeyId, model::ModelId};
use gateway_core::{provider::UpstreamProvider, query::QueryStore};
use gateway_providers::{policy::LithosWindows, policy::WaitBudget, types::PreparedClaudeOrdinary};
use std::{cell::RefCell, collections::BTreeSet};

pub struct RoutingSession<S, I, C, H> {
    pub(crate) admission: crate::currentness::OriginalRoutingAdmission,
    pub(crate) store: S,
    pub(crate) tokens: I,
    pub(crate) clock: C,
    pub(crate) host: H,
    pub(crate) input: RoutingInput,
    pub(crate) plan: RoutePlan,
    pub(crate) lithos: RefCell<LithosWindows>,
    pub(crate) lithos_no_retry: RefCell<BTreeSet<ModelId>>,
    pub(crate) lithos_attempted: RefCell<BTreeSet<ModelId>>,
    pub(crate) claude_ordinary: RefCell<Option<(ModelId, PreparedClaudeOrdinary)>>,
    pub(crate) waits: RefCell<WaitBudget>,
    conversation_id: String,
    pub(crate) elapsed_wait_ms: std::cell::Cell<i64>,
    pub(crate) source_collator: std::sync::Arc<dyn gateway_schema::aliases::SourceStringCollator>,
    pub(crate) extra_lock: std::rc::Rc<futures_util::lock::Mutex<()>>,
}
impl<
    S: QueryStore + Clone,
    I: TokenSource + Clone,
    C: AuthorityClock + Clone + 'static,
    H: RoutingHost,
> RoutingSession<S, I, C, H>
{
    /// Selected data from this genuine planned session; grants no new admission or plan.
    pub fn selected_model(&self) -> &ModelId {
        &self.plan.model
    }
    /// Pricing metadata from this genuine plan; policy stays with the usage owner.
    pub fn surplus_billing(&self) -> Option<&gateway_core::source_json::SourceJson> {
        self.plan.paid.surplus_billing.as_ref()
    }
    pub fn original_source(&self) -> &gateway_core::source_json::SourceJson {
        self.input.original_source()
    }
    pub fn original_context(&self) -> &gateway_core::identity::RequestContext {
        &self.input.context
    }
    pub fn elapsed_wait_ms(&self) -> i64 {
        self.elapsed_wait_ms.get()
    }
    pub(crate) fn authority(&self) -> Authority<S, I> {
        Authority::with_clock(self.store.clone(), self.tokens.clone(), self.clock.clone())
            .with_source_string_collator(self.source_collator.clone())
    }
    pub(crate) fn now(&self) -> Result<i64, GatewayError> {
        let now = self.clock.now_ms()?;
        if now < self.input.context.started_at_ms || now > 9_007_199_254_740_991 {
            return Err(unavailable(
                "The routing clock moved backwards or overflowed.",
            ));
        }
        Ok(now)
    }
    pub(crate) fn id(&self) -> &str {
        &self.input.context.request_id.0
    }
    pub async fn begin(
        input: RoutingInput,
        store: S,
        host: H,
        clock: C,
        tokens: I,
    ) -> RoutingResult<Self> {
        Self::initialize(input, store, host, clock, tokens, None).await
    }
    pub(crate) async fn initialize(
        input: RoutingInput,
        store: S,
        host: H,
        clock: C,
        tokens: I,
        family: Option<crate::extras::ExtrasFamily>,
    ) -> RoutingResult<Self> {
        if host.cancelled() {
            return Err(host.cancellation_error().into());
        }
        let now = clock.now_ms()?;
        if input.context.started_at_ms < 0
            || now < input.context.started_at_ms
            || now > 9_007_199_254_740_991
            || input.context.request_id.0.trim().is_empty()
        {
            return Err(unavailable("Invalid original request identity or clock.").into());
        }
        let source_collator = host.source_string_collator();
        let plan = if family.is_some() {
            policy::admission_plan(&input)
        } else {
            policy::plan(&input, now, source_collator.as_ref())?
        };
        let streamed = input.streamed();
        // Fresh ordinary requests use source binding v3; generated extras keep v1 bytes.
        let mut row = Journal::empty(
            if family.is_some() { 1 } else { 3 },
            input.context.clone(),
            if family.is_some() {
                gateway_auth::crypto::sha256_hex(input.original_source().as_bytes())
            } else {
                crate::local_work::original_source_digest(input.original_source())?
            },
            now.checked_add(gateway_authority_contracts::REQUEST_LEASE_MS)
                .ok_or_else(|| unavailable("Lease overflow."))?,
            now,
        );
        row.extras_family = family;
        journal::create(&store, &tokens, &row).await?;
        let authority = Authority::with_clock(store.clone(), tokens.clone(), clock.clone());
        if let Err(error) = reserve_original(
            &input.principal,
            &input.context,
            &mut row,
            &store,
            &tokens,
            &clock,
        )
        .await
        {
            if let Some(api) = &row.api {
                authority
                    .release_api_key(api, true, "routing_admission_failed", clock.now_ms()?)
                    .await?;
            }
            if let Some(kernel) = &row.kernel {
                authority
                    .settle_kernel(
                        kernel,
                        KernelTerminal::Released,
                        "routing_admission_failed",
                        clock.now_ms()?,
                    )
                    .await?;
            }
            journal::change(
                &store,
                &tokens,
                &input.context.request_id.0,
                clock.now_ms()?,
                &[],
                |j| {
                    j.finished = true;
                    Ok(())
                },
            )
            .await?;
            return Err(error.into());
        }
        let conversation_id = tokens.next_token()?;
        let admission = crate::currentness::OriginalRoutingAdmission::capture(
            row.v,
            row.input_digest,
            row.api,
            row.kernel,
            None,
        );
        Ok(Self {
            admission,
            store,
            tokens,
            clock,
            host,
            input,
            plan,
            lithos: RefCell::new(LithosWindows::default()),
            lithos_no_retry: RefCell::new(BTreeSet::new()),
            lithos_attempted: RefCell::new(BTreeSet::new()),
            claude_ordinary: RefCell::new(None),
            waits: RefCell::new(WaitBudget {
                waits: 0,
                waited_ms: 0,
                streamed,
            }),
            conversation_id,
            source_collator,
            elapsed_wait_ms: std::cell::Cell::new(0),
            extra_lock: std::rc::Rc::new(futures_util::lock::Mutex::new(())),
        })
    }
    /// Consume M34's original Planning capability; never reserve another lifetime.
    pub async fn from_local_admission(
        admission: crate::local_work::LocalProviderAdmission<S, I, C, H>,
    ) -> RoutingResult<Self> {
        let input = RoutingInput::from_source_request(
            admission.original_metadata().clone(),
            admission
                .original
                .input
                .original
                .for_surface(gateway_schema::request::RequestSurface::Responses),
        );
        let source_collator = admission.original.host.source_string_collator();
        let planned: RoutingResult<(RoutePlan, String)> = (|| {
            if admission.original.host.cancelled() {
                return Err(admission.original.host.cancellation_error().into());
            }
            let now = admission.original.clock.now_ms()?;
            if now < input.context.started_at_ms || now > 9_007_199_254_740_991 {
                return Err(unavailable("Invalid original request identity or clock.").into());
            }
            let plan = policy::plan(&input, now, source_collator.as_ref())?;
            Ok((plan, admission.original.tokens.next_token()?))
        })();
        let (plan, conversation_id) = match planned {
            Ok(planned) => planned,
            Err(error) => {
                return Err(match admission.fail_planning().await {
                    Ok(_) => error,
                    Err(cleanup) => error.with_cleanup_diagnostic(&cleanup),
                });
            }
        };
        let parts = admission.attest_plan(plan).await?;
        // The opaque parts came from the SAME original admission and checked revisions.
        debug_assert_eq!(
            parts.input.original.original_source(),
            input.original_source()
        );
        debug_assert_eq!(parts.input.metadata.context, input.context);
        let streamed = input.streamed();
        Ok(Self {
            admission: parts.admission,
            store: parts.store,
            tokens: parts.tokens,
            clock: parts.clock,
            host: parts.host,
            input,
            plan: parts.plan,
            lithos: RefCell::new(LithosWindows::default()),
            lithos_no_retry: RefCell::new(BTreeSet::new()),
            lithos_attempted: RefCell::new(BTreeSet::new()),
            claude_ordinary: RefCell::new(None),
            waits: RefCell::new(WaitBudget {
                waits: 0,
                waited_ms: 0,
                streamed,
            }),
            conversation_id,
            source_collator,
            elapsed_wait_ms: std::cell::Cell::new(0),
            extra_lock: std::rc::Rc::new(futures_util::lock::Mutex::new(())),
        })
    }
    pub(crate) async fn change<F: Fn(&mut Journal) -> Result<(), GatewayError>>(
        &self,
        update: F,
    ) -> Result<Journal, GatewayError> {
        journal::change(
            &self.store,
            &self.tokens,
            self.id(),
            self.now()?,
            &[],
            update,
        )
        .await
    }
    pub(crate) async fn cursor(&self, value: usize) -> Result<(), GatewayError> {
        self.change(|j| {
            settlement::ensure_active(j)?;
            j.cursor = value;
            j.active = None;
            Ok(())
        })
        .await?;
        Ok(())
    }
    pub(crate) async fn credential(
        &self,
        provider: UpstreamProvider,
        model: &ModelId,
        codex: Option<AdmittedAccount>,
    ) -> Result<PublishedCredential, GatewayError> {
        let publication = self
            .host
            .credential(CredentialRequest {
                provider,
                model: model.clone(),
                codex: codex.clone(),
            })
            .await?;
        let PublishedCredential::Scalar { material, .. } = &publication else {
            return Err(unavailable(
                "Credential publication does not match the admitted provider/account.",
            ));
        };
        if material.secret.trim().is_empty() {
            return Err(unavailable(
                "The selected provider credential is unavailable.",
            ));
        }
        let paid_provider = matches!(
            provider,
            UpstreamProvider::Surplus | UpstreamProvider::Metered
        );
        match (&codex, publication.fence()) {
            (
                Some(expected),
                CredentialFence::Codex {
                    admission,
                    source_revision,
                    native_generation,
                    ..
                },
            ) if expected == admission
                && !source_revision.is_empty()
                && !native_generation.is_empty() => {}
            (None, CredentialFence::Direct { provider: observed })
                if provider == *observed && !paid_provider => {}
            (
                None,
                CredentialFence::Paid {
                    provider: observed, ..
                },
            ) if provider == *observed && paid_provider => {}
            _ => {
                return Err(unavailable(
                    "Credential publication does not match the admitted provider/account.",
                ));
            }
        }
        self.host
            .ensure_credential_current(publication.fence(), &self.input.principal, model)
            .await?;
        Ok(publication)
    }
    pub(crate) fn claude_ordinary(&self, model: &ModelId) -> RoutingResult<PreparedClaudeOrdinary> {
        let mut retained = self.claude_ordinary.borrow_mut();
        let (selected_model, phase) = match &mut *retained {
            Some(retained) => retained,
            slot @ None => {
                let phase = crate::wire::claude_ordinary(&self.input, model, self.now()?)?;
                slot.insert((model.clone(), phase))
            }
        };
        if selected_model != model || phase.surface() != self.input.surface {
            return Err(
                unavailable("Claude original model or surface association changed.").into(),
            );
        }
        Ok(phase.clone())
    }

    pub(crate) fn prepare(
        &self,
        provider: UpstreamProvider,
        model: &ModelId,
        publication: &PublishedCredential,
        nonce: &str,
    ) -> RoutingResult<PreparedWireRequest> {
        if provider == UpstreamProvider::ClaudeCode {
            let phase = self.claude_ordinary.borrow();
            let Some((selected_model, phase)) = phase.as_ref() else {
                return Err(unavailable(
                    "Claude ordinary phase was not prepared before account selection.",
                )
                .into());
            };
            if selected_model != model {
                return Err(unavailable("Claude original model association changed.").into());
            }
            return crate::wire::finalize_claude(
                &self.input,
                publication,
                phase,
                nonce,
                self.now()?,
            );
        }
        crate::wire::prepare(
            &self.input,
            &self.conversation_id,
            self.now()?,
            provider,
            model,
            publication,
            nonce,
        )
    }
    pub(crate) fn paid_context(
        &self,
        provider: gateway_core::provider::PaidProvider,
        synthetic: bool,
    ) -> RoutingResult<PaidRequestContext> {
        let mut body = self.input.raw.clone();
        crate::wire::set(&mut body, "model", self.plan.model.to_source_json()?)?;
        Ok(PaidRequestContext {
            original_model: self.plan.model.clone(),
            body,
            surface: self.input.surface,
            synthetic_hop: synthetic.then_some(match provider {
                gateway_core::provider::PaidProvider::Surplus => PaidSyntheticHop::Surplus,
                gateway_core::provider::PaidProvider::Metered => PaidSyntheticHop::Openlux,
            }),
        })
    }
    pub(crate) fn check_paid_credential(
        fence: &CredentialFence,
        receipt: &PaidRoutingReceipt,
    ) -> Result<(), GatewayError> {
        matches!(fence, CredentialFence::Paid {
            owner_id, owner_epoch, configuration_generation, credential_binding, ..
        } if owner_id == receipt.owner_id() && owner_epoch == receipt.owner_epoch()
            && configuration_generation == receipt.configuration_generation()
            && credential_binding == receipt.credential_binding())
        .then_some(())
        .ok_or_else(|| {
            unavailable("The paid credential does not match the current paid routing receipt.")
        })
    }
    pub(crate) async fn paid_authorization(
        &self,
        row: &Journal,
    ) -> Result<PaidAuthorization, GatewayError> {
        if row.claude_unavailable {
            return self
                .authority()
                .prove_claude_unavailable(&self.plan.model, self.now()?)
                .await?
                .map(PaidAuthorization::ClaudeSubscriptionUnavailable)
                .ok_or_else(|| unavailable("Claude full-cohort proof is unavailable or stale."));
        }
        if self.plan.paid.codex_enabled && !self.plan.paid.metered_only {
            match self
                .authority()
                .select_account(&self.plan.model, self.plan.quota_class, self.now()?)
                .await?
            {
                RouteSelection::QuotaBlocked { proof, .. }
                | RouteSelection::CredentialsInvalid { proof }
                    if proof.full_cohort_unavailable() =>
                {
                    Ok(PaidAuthorization::SubscriptionUnavailable(proof))
                }
                _ => Err(unavailable(
                    "Current full subscription-pool proof is required before paid dispatch.",
                )),
            }
        } else {
            Ok(PaidAuthorization::DirectPaidModel)
        }
    }
}

/// The one original admission operation shared by ordinary and local work.
pub(crate) async fn reserve_original<
    S: QueryStore + Clone,
    I: TokenSource + Clone,
    C: AuthorityClock + Clone + 'static,
>(
    principal: &gateway_auth::types::AuthPrincipal,
    context: &gateway_core::identity::RequestContext,
    row: &mut Journal,
    store: &S,
    tokens: &I,
    clock: &C,
) -> Result<(), GatewayError> {
    let authority = Authority::with_clock(store.clone(), tokens.clone(), clock.clone());
    if let AuthMethod::KvApiKey { key_id, .. } = &principal.method {
        let id = ApiKeyId(key_id.clone());
        if context.key_id.as_ref() != Some(&id) {
            return Err(unavailable("Principal and original key identity differ."));
        }
        row.api = Some(
            authority
                .reserve_api_key_deferred(&id, &context.request_id, &context.route, clock.now_ms()?)
                .await?,
        );
        journal::change(
            store,
            tokens,
            &context.request_id.0,
            clock.now_ms()?,
            &[],
            |j| {
                j.api = row.api.clone();
                Ok(())
            },
        )
        .await?;
    }
    if let AuthMethod::GithubToken { owner, repo, .. } = &principal.method {
        row.kernel = Some(
            authority
                .reserve_kernel(
                    owner,
                    repo,
                    &context.request_id,
                    &context.route,
                    clock.now_ms()?,
                )
                .await?,
        );
        journal::change(
            store,
            tokens,
            &context.request_id.0,
            clock.now_ms()?,
            &[],
            |j| {
                j.kernel = row.kernel.clone();
                Ok(())
            },
        )
        .await?;
    }
    Ok(())
}
