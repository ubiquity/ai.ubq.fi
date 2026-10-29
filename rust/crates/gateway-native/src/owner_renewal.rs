//! One fixed native owner action. Current actor/fences and start intent come
//! from the authenticated shared-store reverse receiver, never a local ledger.
use crate::authority_http::owned_claim::RemoteClaim;
use crate::authority_http::{NativeAuthorityHttp, authority_deadline};
use gateway_auth_credentials::{
    managed_access::ManagedAccessProjection,
    native_credentials::{
        NativeCredentials, NativeGeneration, NativeOwnerId, OwnerFence, native_refresh_time_ns,
    },
};
use gateway_auth_native_proof::native_auth::AuthenticatedNativeOwner;
use gateway_auth_native_proof::native_context::NativeControlContext;
use gateway_auth_native_wire::control::{
    NativeControlDisposition as Disposition, NativeControlEnvelope, NativeControlRequest,
    NativeControlResponse, unverified,
};
use gateway_native_authority_request::NativeRenewalAuthorityRequest as Request;
use gateway_native_authority_wire::{
    renewal::{NATIVE_RENEWAL_FENCE_VERSION, NativeRenewalFence, NativeRenewalMode},
    renewal_response::NativeRenewalAuthorityResponse as Response,
};
use gateway_native_http_admission::owner_control::NativeOwnerRenewalHandler;
use gateway_native_local_transport::{
    guard::NativeRenewalGuard,
    transport::{NativeOwnerTransport, renew_owned_guarded},
    unix::{NativeOwnerLocation, UnixOwnerTransport},
};
use gateway_source::error::GatewayError;

pub(crate) fn require_current_fence(fence: &NativeRenewalFence) -> Result<(), GatewayError> {
    if fence.v != NATIVE_RENEWAL_FENCE_VERSION {
        return Err(unverified());
    }
    Ok(())
}

pub struct PublishedAccess {
    pub publication_revision: String,
    pub already_current: bool,
}
/// Two distinct fixed publishers; a managed owner is never relabelled external.
/// Implementations must verify the matching signed current publication receipt.
pub trait NativeAccessPublisher: Send + Sync {
    fn managed(
        &self,
        request: &NativeControlEnvelope,
        fence: &NativeRenewalFence,
        projection: &ManagedAccessProjection,
        context: &NativeControlContext<'_>,
    ) -> Result<PublishedAccess, GatewayError>;
    fn external(
        &self,
        fence: &NativeRenewalFence,
        generation: &NativeGeneration,
        context: &NativeControlContext<'_>,
    ) -> Result<PublishedAccess, GatewayError>;
}

pub struct NativeOwnerRenewalService<P> {
    authority: NativeAuthorityHttp,
    publisher: P,
    locations: Vec<NativeOwnerLocation>,
}
impl<P: NativeAccessPublisher> NativeOwnerRenewalService<P> {
    pub fn configured(
        authority: NativeAuthorityHttp,
        publisher: P,
        locations: Vec<NativeOwnerLocation>,
    ) -> Result<Self, GatewayError> {
        UnixOwnerTransport::new(locations.clone())?;
        Ok(Self {
            authority,
            publisher,
            locations,
        })
    }
    fn execute(
        &self,
        claim: &RemoteClaim<NativeRenewalFence>,
        context: &NativeControlContext<'_>,
    ) -> Result<NativeControlResponse, GatewayError> {
        require_current_fence(&claim.fence)?;
        if let Some(revision) = &claim.publication_revision {
            self.authority.exchange(
                &Request::Published {
                    v: 1,
                    operation_id: claim.operation_id.clone(),
                    claim_ref: claim.claim_ref.clone(),
                    publication_revision: revision.clone(),
                },
                context,
            )?;
            return Ok(NativeControlResponse {
                v: 1,
                operation_id: claim.operation_id.clone(),
                disposition: Disposition::AlreadyCurrent,
                publication_revision: Some(revision.clone()),
                reason: None,
            });
        }
        let guard = CurrentGuard {
            authority: &self.authority,
            claim,
            context,
        };
        futures_executor::block_on(guard.ensure_current())?;
        let owner = NativeOwnerId(claim.fence.native_owner_id.clone());
        if !self
            .locations
            .iter()
            .any(|location| location.owner == owner)
        {
            return Err(unverified());
        }
        let deadline = if claim.mode == NativeRenewalMode::RenewOnce {
            authority_deadline(context, claim.deadline_ms)?
        } else {
            context.deadline()
        };
        let transport = UnixOwnerTransport::until(self.locations.clone(), deadline)?;
        let observed = futures_executor::block_on(transport.read_credentials(&owner))?;
        futures_executor::block_on(guard.ensure_current())?;
        if observed.routing_account_hash() != claim.fence.account_id_hash {
            return Err(changed());
        }
        let changed_generation = observed.generation_hash() != claim.fence.source_native_generation;
        if changed_generation {
            verify_progress(&claim.fence, &observed, context.now_ms()?)?;
        }
        let now = context.now_ms()?;
        let usable = observed
            .access_expires_at_ms()
            .is_some_and(|expiry| expiry > now.saturating_add(120_000));
        let fence = OwnerFence {
            owner,
            account_id: observed.account_id.clone(),
            generation_hash: observed.generation_hash(),
            authority_revision: claim.fence.owner_revision.clone(),
        };
        let generation = if changed_generation && usable {
            NativeGeneration {
                fence,
                credentials: observed,
                observed_at_ms: context.now_ms()?,
            }
        } else {
            if claim.mode != NativeRenewalMode::RenewOnce || observed.external_user_id.is_some() {
                return Err(unverified());
            }
            futures_executor::block_on(renew_owned_guarded(&transport, &guard, fence, &observed))?
        };
        futures_executor::block_on(guard.ensure_current())?;
        let publication = if generation.credentials.external_user_id.is_some() {
            self.publisher
                .external(&claim.fence, &generation, context)?
        } else {
            let projection = ManagedAccessProjection::from_native(&generation)?;
            let request = NativeControlEnvelope {
                v: 1,
                operation_id: claim.operation_id.clone(),
                request: NativeControlRequest::CodexRenewAccess {
                    action_ref: claim.action_ref.clone(),
                },
            };
            self.publisher
                .managed(&request, &claim.fence, &projection, context)?
        };
        let response = self.authority.exchange(
            &Request::Published {
                v: 1,
                operation_id: claim.operation_id.clone(),
                claim_ref: claim.claim_ref.clone(),
                publication_revision: publication.publication_revision.clone(),
            },
            context,
        )?;
        if !matches!(response, Response::Published { .. }) {
            return Err(unverified());
        }
        Ok(NativeControlResponse {
            v: 1,
            operation_id: claim.operation_id.clone(),
            disposition: if publication.already_current {
                Disposition::AlreadyCurrent
            } else {
                Disposition::Published
            },
            publication_revision: Some(publication.publication_revision),
            reason: None,
        })
    }
}
impl<P: NativeAccessPublisher> NativeOwnerRenewalHandler for NativeOwnerRenewalService<P> {
    fn renew(
        &self,
        proof: &AuthenticatedNativeOwner,
        request: &NativeControlEnvelope,
        raw: &[u8],
        context: &NativeControlContext<'_>,
    ) -> Result<NativeControlResponse, GatewayError> {
        let owned = self.authority.claim_for_control(proof, raw, context)?;
        let result = match owned.legacy() {
            Ok(claim) if claim.operation_id == request.operation_id => self.execute(claim, context),
            _ => Err(unverified()),
        };
        if result.is_err() {
            // This consumes only retained report ownership. The original active
            // context remains cancelled/expired, and the original error wins.
            let _ = owned.report_execution_error(&self.authority);
        }

        result
    }
}

struct CurrentGuard<'a> {
    authority: &'a NativeAuthorityHttp,
    claim: &'a RemoteClaim<NativeRenewalFence>,
    context: &'a NativeControlContext<'a>,
}
impl NativeRenewalGuard for CurrentGuard<'_> {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        self.context.now_ms()
    }
    async fn ensure_current(&self) -> Result<(), GatewayError> {
        self.context.check()?;
        let result = self.authority.exchange(
            &Request::Recheck {
                v: 1,
                operation_id: self.claim.operation_id.clone(),
                claim_ref: self.claim.claim_ref.clone(),
            },
            self.context,
        )?;
        match result {
            Response::Recheck { fence, .. } if fence == self.claim.fence => Ok(()),
            _ => Err(changed()),
        }
    }
    async fn start_refresh(&self) -> Result<(), GatewayError> {
        self.context.check()?;
        let permit = self
            .claim
            .start_permit
            .borrow_mut()
            .take()
            .ok_or_else(unverified)?;
        let result = self.authority.exchange(
            &Request::Start {
                v: 1,
                operation_id: self.claim.operation_id.clone(),
                claim_ref: self.claim.claim_ref.clone(),
                start_permit: permit,
            },
            self.context,
        )?;
        match result {
            Response::Start {
                mode: NativeRenewalMode::RenewOnce,
                ..
            } => Ok(()),
            _ => Err(unverified()),
        }
    }
}
fn verify_progress(
    previous: &NativeRenewalFence,
    observed: &NativeCredentials,
    now: i64,
) -> Result<(), GatewayError> {
    let expiry = observed.access_expires_at_ms().ok_or_else(unverified)?;
    let advanced = matches!((native_refresh_time_ns(previous.native_refreshed_at.as_deref()),native_refresh_time_ns(observed.native_refreshed_at.as_deref())),(Some(old),Some(new)) if new>old);
    if previous
        .access_expires_at_ms
        .is_some_and(|old| expiry < old || expiry == old && !advanced)
        || expiry <= now.saturating_add(120_000)
            && previous.access_expires_at_ms.is_none()
            && !advanced
    {
        return Err(changed());
    }
    Ok(())
}
fn changed() -> GatewayError {
    GatewayError::new(
        409,
        "Current native owner authority changed",
        Some("native_owner_changed"),
    )
}

#[cfg(test)]
mod renewal_v2_tests;
