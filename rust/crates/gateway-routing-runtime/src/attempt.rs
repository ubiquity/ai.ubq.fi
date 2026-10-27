use crate::{journal, journal::AttemptRecord, journal::Journal, journal::Phase};
use crate::{journal::unavailable, policy::RouteHop, session::RoutingSession, settlement};
use crate::{types::ClaudeSelection, types::CodexRecovery, types::CredentialFence};
use crate::{types::PreparedAttempt, types::PublishedCredential, types::RoutingError};
use crate::{types::RoutingHost, types::RoutingResult, types::SourceRefusalBody};
use gateway_authority_contracts::{AuthorityClock, TokenSource};
use gateway_authority_paid_admission::PaidAdmissionOperations;
use gateway_authority_paid_ledger::PaidLedgerOperations;
use gateway_authority_paid_records::{PaidAdmission, PaidRequest, PaidReservation};
use gateway_authority_paid_route::{PaidRouteOperations, authorization::PaidAuthorization};
use gateway_authority_routing_selection::{SelectionOperations, types::RouteSelection};
use gateway_authority_routing_signal::SignalOperations;
use gateway_core::{error::GatewayError, model::ModelId, provider::UpstreamProvider::ChatgptCodex};
use gateway_core::{identity::PaidAttemptId, provider::UpstreamProvider, query::QueryStore};
use gateway_core::{provider::PaidProvider, provider::UpstreamProvider::ClaudeCode};
use gateway_providers::types::ClientSurface;
use gateway_schema::source_request::error::SourceRequestError;

type SelectedHop = (
    UpstreamProvider,
    ModelId,
    PublishedCredential,
    Option<PaidReservation>,
);

impl<
    S: QueryStore + Clone,
    I: TokenSource + Clone,
    C: AuthorityClock + Clone + 'static,
    H: RoutingHost,
> RoutingSession<S, I, C, H>
{
    pub async fn next_attempt(&self) -> RoutingResult<Option<PreparedAttempt>> {
        for _ in 0..34 {
            if self.host.cancelled() {
                return Err(self.host.cancellation_error().into());
            }
            let (_, row) = journal::load(&self.store, self.id()).await?;
            settlement::ensure_active(&row)?;
            if row.extras_family.is_some() {
                return Err(unavailable("Extras dispatch requires an opaque child scope.").into());
            }
            if row.semantic_committed {
                return Ok(None);
            }
            if row.active.is_some() {
                return Err(
                    unavailable("The previous attempt has not been refused or finalized.").into(),
                );
            }
            let Some(hop) = self.plan.hops.get(row.cursor).cloned() else {
                return Ok(None);
            };
            let Some(selected) = self.select_hop(&row, hop).await? else {
                continue;
            };
            let id = self.tokens.next_token()?;
            let paid = selected.3.clone();
            let retained = self.retain_attempt(selected, id, row.cursor).await;
            if retained.is_err()
                && let Some(paid) = &paid
            {
                self.authority()
                    .release_paid_before_transport(paid, self.now()?)
                    .await?;
            }
            return retained.map(Some);
        }
        Err(unavailable("Provider/account selection did not settle.").into())
    }
    async fn retain_attempt(
        &self,
        selected: SelectedHop,
        id: String,
        cursor: usize,
    ) -> RoutingResult<PreparedAttempt> {
        let (provider, model, publication, paid) = selected;
        let request = self.prepare(provider, &model, &publication, &id)?;
        let attempt = AttemptRecord {
            id: id.clone(),
            provider,
            model,
            cursor,
            fence: publication.fence().clone(),
            paid: paid.clone(),
            phase: Phase::Prepared,
            transport_started: false,
            response_nonce: None,
            response_count: 0,
            workspace_nonce: None,
            workspace_continuations: 0,
            known_not_started: false,
            observation: None,
            provider_request_id: None,
            billing: None,
        };
        self.change(|j| {
            settlement::ensure_active(j)?;
            if j.cursor != cursor || j.active.is_some() {
                return Err(unavailable("Another provider attempt won admission."));
            }
            j.active = Some(id.clone());
            j.attempts.push(attempt.clone());
            Ok(())
        })
        .await?;
        self.admission.capture_attempt(attempt);
        Ok(PreparedAttempt {
            id,
            request,
            credential: publication.into_fence(),
            paid: paid.clone(),
            original_request_id: self.id().into(),
        })
    }
    async fn select_hop(&self, row: &Journal, hop: RouteHop) -> RoutingResult<Option<SelectedHop>> {
        let (provider, model, publication) = match hop {
            RouteHop::Direct { provider, model } => {
                let model = if provider == UpstreamProvider::Lithos {
                    let no_retry = self.lithos_no_retry.borrow();
                    let attempted = self.lithos_attempted.borrow();
                    self.lithos
                        .borrow()
                        .eligible(&model, self.now()?, &no_retry, &attempted)
                        .unwrap_or(model)
                } else {
                    model
                };
                let credential = self.credential(provider, &model, None).await?;
                (provider, model, credential)
            }
            RouteHop::Claude => {
                if !self.input.catalog.enabled("claude_code") {
                    self.cursor(row.cursor + 1).await?;
                    return Ok(None);
                }
                let ordinary = self.claude_ordinary(&self.plan.model)?;
                let credential = match self
                    .host
                    .select_claude(
                        &self.input.principal,
                        &self.plan.model,
                        &self.input.raw,
                        ordinary.effective_effort(),
                    )
                    .await?
                {
                    ClaudeSelection::NotApplicable => {
                        self.cursor(row.cursor + 1).await?;
                        return Ok(None);
                    }
                    ClaudeSelection::Refused {
                        status,
                        headers,
                        body,
                    } => {
                        return Err(RoutingError {
                            error: Box::new(GatewayError::new(
                                status,
                                "Claude subscription request was refused.",
                                Some("claude_code_refused"),
                            )),
                            headers,
                            source_body: Some(SourceRefusalBody::Json(body)),
                        });
                    }
                    ClaudeSelection::InternalServerError => {
                        return Err(RoutingError {
                            error: Box::new(GatewayError::new(500, "Internal Server Error", None)),
                            headers: gateway_providers::types::Headers::from([(
                                "Content-Type".into(),
                                "text/plain; charset=utf-8".into(),
                            )]),
                            source_body: Some(SourceRefusalBody::InternalServerError),
                        });
                    }
                    ClaudeSelection::Unavailable => {
                        self.change(|j| {
                            j.claude_unavailable = true;
                            j.cursor += 1;
                            Ok(())
                        })
                        .await?;
                        return Ok(None);
                    }
                    ClaudeSelection::Eligible(credential) => {
                        if !matches!(
                            credential.as_ref(),
                            PublishedCredential::Claude {
                                fence: CredentialFence::Claude { .. },
                                ..
                            }
                        ) {
                            return Err(unavailable("Claude credential binding is invalid.").into());
                        }
                        *credential
                    }
                };
                (ClaudeCode, self.plan.model.clone(), credential)
            }
            RouteHop::Codex => {
                let Some(credential) = self.select_codex(row).await? else {
                    return Ok(None);
                };
                (ChatgptCodex, self.plan.model.clone(), credential)
            }
            RouteHop::Paid {
                provider,
                model,
                synthetic,
            } => {
                return self
                    .select_paid(row, provider, model, synthetic)
                    .await
                    .map(Some);
            }
        };
        Ok(Some((provider, model, publication, None)))
    }
    async fn select_codex(&self, row: &Journal) -> RoutingResult<Option<PublishedCredential>> {
        let mut account = match self
            .authority()
            .select_account(&self.plan.model, self.plan.quota_class, self.now()?)
            .await?
        {
            RouteSelection::Eligible(account) => account,
            RouteSelection::QuotaBlocked { proof, .. } => {
                let recovery = if proof.full_cohort_exhausted() && !row.recovery_attempted {
                    self.change(|journal| {
                        journal.recovery_attempted = true;
                        Ok(())
                    })
                    .await?;
                    self.host.recover_codex(&proof, &self.plan.model).await?
                } else {
                    CodexRecovery::Declined
                };
                match recovery {
                    CodexRecovery::Recovered(account)
                        if account.requested_model != self.plan.model
                            || account.quota_class != self.plan.quota_class
                            || account.probe_required
                            || account.probe.is_none() =>
                    {
                        return Err(unavailable(
                            "The elected recovery admission does not match this request.",
                        )
                        .into());
                    }
                    CodexRecovery::Recovered(account) => {
                        self.authority()
                            .ensure_admission_current(&account, self.now()?)
                            .await?;
                        account
                    }
                    CodexRecovery::Reselect => return Ok(None),
                    CodexRecovery::Declined if !proof.full_cohort_unavailable() => {
                        return Err(unavailable(
                            "Subscription pool is not authoritatively unavailable.",
                        )
                        .into());
                    }
                    CodexRecovery::Declined => {
                        self.cursor(row.cursor + 1).await?;
                        return Ok(None);
                    }
                }
            }
            RouteSelection::CredentialsInvalid { proof } => {
                if !proof.full_cohort_unavailable() {
                    return Err(
                        unavailable("Credential failure does not cover the current pool.").into(),
                    );
                }
                self.cursor(row.cursor + 1).await?;
                return Ok(None);
            }
            RouteSelection::ModelUnavailable { model, detail } => {
                let detail = detail.unwrap_or_else(|| {
                    let mut units = "The model '".encode_utf16().collect::<Vec<_>>();
                    units.extend_from_slice(model.as_js_string().as_units());
                    units.extend("' is not available for these subscriptions.".encode_utf16());
                    gateway_core::js_string::JsString16::from_units(units)
                });
                return Err(SourceRequestError::new(
                    404,
                    detail,
                    "invalid_request_error",
                    Some("model_not_found"),
                    None,
                )
                .into());
            }
            RouteSelection::RoutingUnavailable => {
                return Err(
                    unavailable("The current subscription routing state is unavailable.").into(),
                );
            }
        };
        if account.probe_required {
            account = self
                .authority()
                .claim_probe(&account, None, self.now()?)
                .await?
                .ok_or_else(|| unavailable("The current account probe is leased."))?;
        }
        let credential = self
            .credential(ChatgptCodex, &self.plan.model, Some(account))
            .await?;
        Ok(Some(credential))
    }
    async fn select_paid(
        &self,
        row: &Journal,
        provider: PaidProvider,
        model: ModelId,
        synthetic: bool,
    ) -> RoutingResult<SelectedHop> {
        let (upstream, label) = match provider {
            PaidProvider::Surplus => (UpstreamProvider::Surplus, "surplus"),
            PaidProvider::Metered => (UpstreamProvider::Metered, "metered"),
        };
        let credential = self.credential(upstream, &model, None).await?;
        // Local schema/credential failure cannot create paid exposure.
        self.prepare(upstream, &model, &credential, "validation")?;
        let key = self.input.context.key_id.clone().ok_or_else(|| {
            GatewayError::new(
                403,
                "Paid inference requires an eligible stored API key.",
                Some("paid_fallback_unavailable"),
            )
        })?;
        let attempt_id = if synthetic {
            gateway_providers::policy::paid_hop_identity(self.id(), provider)
        } else {
            format!("{}:paid:{label}:{}", self.id(), self.tokens.next_token()?)
        };
        let authorization = if synthetic {
            PaidAuthorization::DirectPaidModel
        } else {
            self.paid_authorization(row).await?
        };
        let input = PaidRequest {
            key_id: key,
            request_id: self.input.context.request_id.clone(),
            attempt_id: PaidAttemptId(attempt_id),
            provider,
            model: model.clone(),
            route: self.input.context.route.clone(),
            path: if self.input.surface == ClientSurface::Chat {
                "/v1/chat/completions"
            } else {
                "/v1/responses"
            }
            .into(),
            stream: self.input.streamed(),
            reasoning: self.input.requested_reasoning(),
            created_at_ms: self.input.context.started_at_ms,
        };
        let context = self.paid_context(provider, synthetic)?;
        let receipt = self
            .authority()
            .authorize_paid_route(&input, &context, self.now()?)
            .await?;
        let receipt = self
            .authority()
            .bind_paid_original_request(&receipt, &self.input.context, self.now()?)
            .await?;
        Self::check_paid_credential(credential.fence(), &receipt)?;
        let PaidAdmission::Reserved(reservation) = self
            .authority()
            .reserve_paid(&input, &authorization, &receipt, self.now()?)
            .await?
        else {
            return Err(unavailable(
                "A retained paid attempt cannot be redispatched without reconciliation.",
            )
            .into());
        };
        Ok((upstream, model, credential, Some(reservation)))
    }
}
