//! Complete fixed native callback command execution. HTTP and response MAC ownership stays with the caller.
use gateway_auth_contracts::types::AuthHost;
use gateway_auth_native_proof::native_auth::{
    AuthenticatedNativeOwner, NativeAuthenticator, NativeTransportIdentity,
};
use gateway_auth_native_protocol::native_replay::StoreNonceAuthority;
use gateway_auth_native_wire::native_read::NATIVE_CONTROL_PATH;
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_authority::native_control::{NativeRenewalControl, receiver::NativeRenewalAuthority};
use gateway_authority_contracts::{AuthorityClock, NativeRenewalCancellation, TokenSource};
use gateway_authority_native_lifecycle::SourceNativeLifecycleService;
use gateway_core::state::AuthorityStore;
use gateway_native_authority_request::NativeRenewalAuthorityRequest as RenewalRequest;
use gateway_native_authority_wire::{
    renewal_response::NativeRenewalAuthorityResponse as RenewalResponse,
    source_response::SourceNativeRenewalAuthorityResponseV2 as SourceResponse,
};
use gateway_source::error::GatewayError;

pub async fn execute_legacy<S: AuthorityStore, I: TokenSource, C: NativeRenewalCancellation>(
    receiver: &NativeRenewalAuthority<'_, S, I, C>,
    identity: &gateway_auth_native_proof::native_auth::NativeTransportIdentity,
    replay: &StoreNonceAuthority<'_, S>,
    clock: &dyn AuthorityClock,
    bytes: &[u8],
) -> Result<RenewalResponse, GatewayError> {
    let request = RenewalRequest::parse(bytes)?;
    match request {
        RenewalRequest::Claim {
            original_auth_headers,
            ..
        } => {
            let original = RenewalRequest::parse(bytes)?.original_control_body()?;
            let proof = NativeAuthenticator::new(identity, replay)
                .authenticate(
                    NATIVE_CONTROL_PATH,
                    original_auth_headers.borrowed(),
                    &original,
                    clock.now_ms()?,
                )
                .await?;
            let envelope =
                gateway_auth_native_wire::control::NativeControlEnvelope::parse(&original)?;
            let gateway_auth_native_wire::control::NativeControlRequest::CodexRenewAccess {
                action_ref,
            } = &envelope.request;
            let claim = receiver
                .claim(&proof, action_ref, &envelope.operation_id, &original)
                .await?;
            Ok(RenewalResponse::Claim {
                v: 1,
                action_ref: claim.action().action_ref().into(),
                operation_id: claim.operation_id().into(),
                claim_ref: claim.claim_ref().into(),
                start_permit: claim.start_permit().map(str::to_owned),
                publication_revision: claim.publication_revision().map(str::to_owned),
                mode: claim.mode(),
                deadline_ms: claim.action().deadline_ms(),
                fence: claim.action().fence().clone(),
            })
        }
        RenewalRequest::Start {
            operation_id,
            claim_ref,
            start_permit,
            ..
        } => {
            let mut claim = receiver
                .resolve_claim(&operation_id, &claim_ref, Some(&start_permit))
                .await?;
            Ok(RenewalResponse::Start {
                v: 1,
                mode: receiver.start(&mut claim).await?,
            })
        }
        RenewalRequest::Recheck {
            operation_id,
            claim_ref,
            ..
        } => {
            let claim = receiver
                .resolve_claim(&operation_id, &claim_ref, None)
                .await?;
            Ok(RenewalResponse::Recheck {
                v: 1,
                fence: receiver.recheck(&claim).await?,
            })
        }
        RenewalRequest::Published {
            operation_id,
            claim_ref,
            publication_revision,
            ..
        } => {
            let claim = receiver
                .resolve_claim(&operation_id, &claim_ref, None)
                .await?;
            let receipt = receiver.published(&claim, &publication_revision).await?;
            Ok(RenewalResponse::Published {
                v: 1,
                publication_revision: receipt.publication_revision().into(),
            })
        }
        RenewalRequest::Unknown {
            operation_id,
            claim_ref,
            ..
        } => {
            let claim = receiver
                .resolve_claim(&operation_id, &claim_ref, None)
                .await?;
            receiver.unknown(&claim).await?;
            Ok(RenewalResponse::Unknown { v: 1 })
        }
    }
}

pub async fn execute_source<
    S: AuthorityStore,
    I: TokenSource,
    H: AuthHost + PassportDate,
    C: NativeRenewalCancellation,
>(
    receiver: &SourceNativeLifecycleService<'_, S, I, H, C>,
    outer_peer: &AuthenticatedNativeOwner,
    identity: &NativeTransportIdentity,
    replay: &StoreNonceAuthority<'_, S>,
    clock: &dyn AuthorityClock,
    bytes: &[u8],
) -> Result<SourceResponse, GatewayError> {
    receiver.require_callback_peer(outer_peer, bytes)?;
    match RenewalRequest::parse(bytes)? {
        RenewalRequest::Claim {
            original_auth_headers,
            ..
        } => {
            let original = RenewalRequest::parse(bytes)?.original_control_body()?;
            let proof = NativeAuthenticator::new(identity, replay)
                .authenticate(
                    NATIVE_CONTROL_PATH,
                    original_auth_headers.borrowed(),
                    &original,
                    clock.now_ms()?,
                )
                .await?;
            let envelope =
                gateway_auth_native_wire::control::NativeControlEnvelope::parse(&original)?;
            let gateway_auth_native_wire::control::NativeControlRequest::CodexRenewAccess {
                action_ref,
            } = &envelope.request;
            let claim = receiver
                .claim(&proof, action_ref, &envelope.operation_id, &original)
                .await?;
            Ok(SourceResponse::Claim {
                v: 2,
                action_ref: claim.action_ref().into(),
                operation_id: claim.operation_id().into(),
                claim_ref: claim.claim_ref().into(),
                start_permit: claim.start_permit().map(str::to_owned),
                publication_revision: claim.publication_revision().map(str::to_owned),
                mode: claim.mode(),
                deadline_ms: claim.deadline_ms(),
                fence: claim.fence().clone(),
            })
        }
        RenewalRequest::Start {
            operation_id,
            claim_ref,
            start_permit,
            ..
        } => {
            let mut claim = receiver
                .resolve_claim(&operation_id, &claim_ref, Some(&start_permit))
                .await?;
            Ok(SourceResponse::Start {
                v: 2,
                mode: receiver.start(&mut claim).await?,
            })
        }
        RenewalRequest::Recheck {
            operation_id,
            claim_ref,
            ..
        } => {
            let claim = receiver
                .resolve_claim(&operation_id, &claim_ref, None)
                .await?;
            Ok(SourceResponse::Recheck {
                v: 2,
                fence: receiver.recheck(&claim).await?,
            })
        }
        RenewalRequest::Published {
            operation_id,
            claim_ref,
            publication_revision,
            ..
        } => {
            let claim = receiver
                .resolve_claim(&operation_id, &claim_ref, None)
                .await?;
            let receipt = receiver.published(&claim, &publication_revision).await?;
            Ok(SourceResponse::Published {
                v: 2,
                publication_revision: receipt.publication_revision().into(),
            })
        }
        RenewalRequest::Unknown { .. } => {
            receiver.record_unknown(outer_peer, bytes).await?;
            Ok(SourceResponse::Unknown { v: 2 })
        }
    }
}
