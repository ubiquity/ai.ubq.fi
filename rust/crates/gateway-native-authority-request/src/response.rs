//! Finite reply shape and request/peer correlations; never a claim or currentness grant.
use crate::NativeRenewalAuthorityRequest as Request;
use gateway_auth_native_wire::control::{
    NATIVE_RENEWAL_MAX_MS, NativeControlEnvelope, NativeControlRequest, unverified,
};
use gateway_authority_actor_records::versions::{AUTHORITY_RESPONSE_V2, RENEWAL_FENCE_V3};
use gateway_native_authority_wire::{
    renewal::{NATIVE_RENEWAL_FENCE_VERSION, NativeRenewalMode},
    renewal_response::NativeRenewalAuthorityResponse as Response,
    source_response::SourceNativeRenewalAuthorityResponseV2 as SourceResponse,
};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum RenewalResponseVersion {
    Legacy,
    Source,
}

pub fn response_version(body: &[u8]) -> Result<RenewalResponseVersion, GatewayError> {
    let raw = SourceJson::parse_bytes(body).map_err(|_| unverified())?;
    let mut members = raw
        .object_entries()
        .map_err(|_| unverified())?
        .into_iter()
        .filter(|(key, _)| key == &JsString16::from_utf8("v"));
    let value = members.next().ok_or_else(unverified)?.1;
    if members.next().is_some() {
        return Err(unverified());
    }
    match value.deserialize::<u8>().map_err(|_| unverified())? {
        1 => Ok(RenewalResponseVersion::Legacy),
        2 => Ok(RenewalResponseVersion::Source),
        _ => Err(unverified()),
    }
}

pub fn validate(
    request: &Request,
    response: &Response,
    now: i64,
    peer: (&str, &str),
) -> Result<(), GatewayError> {
    let valid = match (request, response) {
        (
            Request::Claim { .. },
            Response::Claim {
                v,
                action_ref,
                operation_id,
                claim_ref,
                start_permit,
                mode,
                deadline_ms,
                fence,
                publication_revision,
            },
        ) => {
            let original = NativeControlEnvelope::parse(&request.original_control_body()?)?;
            let NativeControlRequest::CodexRenewAccess {
                action_ref: expected,
            } = original.request;
            *v == 1
                && fence.v == NATIVE_RENEWAL_FENCE_VERSION
                && action_ref == &expected
                && operation_id == &original.operation_id
                && identifier(claim_ref)
                && fence.owner_id == peer.0
                && fence.owner_epoch == peer.1
                && publication_revision.as_deref().is_none_or(identifier)
                && (publication_revision.is_none() || *mode == NativeRenewalMode::ReconcileOnly)
                && match mode {
                    NativeRenewalMode::RenewOnce => {
                        start_permit.as_deref().is_some_and(identifier)
                            && *deadline_ms > now
                            && *deadline_ms <= now.saturating_add(NATIVE_RENEWAL_MAX_MS)
                    }
                    NativeRenewalMode::ReconcileOnly => start_permit.is_none(),
                }
        }
        (Request::Recheck { .. }, Response::Recheck { v, fence }) => {
            *v == 1
                && fence.v == NATIVE_RENEWAL_FENCE_VERSION
                && fence.owner_id == peer.0
                && fence.owner_epoch == peer.1
        }
        (Request::Start { .. }, Response::Start { v, .. })
        | (Request::Unknown { .. }, Response::Unknown { v }) => *v == 1,
        (
            Request::Published {
                publication_revision: expected,
                ..
            },
            Response::Published {
                v,
                publication_revision,
            },
        ) => *v == 1 && publication_revision == expected,
        _ => false,
    };
    if !valid {
        return Err(unverified());
    }
    Ok(())
}

pub fn validate_source(
    request: &Request,
    response: &SourceResponse,
    now: i64,
    peer: (&str, &str),
) -> Result<(), GatewayError> {
    let valid = match (request, response) {
        (
            Request::Claim { .. },
            SourceResponse::Claim {
                v,
                action_ref,
                operation_id,
                claim_ref,
                start_permit,
                mode,
                deadline_ms,
                fence,
                publication_revision,
            },
        ) => {
            let original = NativeControlEnvelope::parse(&request.original_control_body()?)?;
            let NativeControlRequest::CodexRenewAccess {
                action_ref: expected,
            } = original.request;
            *v == AUTHORITY_RESPONSE_V2
                && fence.v == RENEWAL_FENCE_V3
                && action_ref == &expected
                && operation_id == &original.operation_id
                && identifier(claim_ref)
                && fence.owner_id == peer.0
                && fence.owner_epoch == peer.1
                && publication_revision.as_deref().is_none_or(identifier)
                && (publication_revision.is_none() || *mode == NativeRenewalMode::ReconcileOnly)
                && match mode {
                    NativeRenewalMode::RenewOnce => {
                        start_permit.as_deref().is_some_and(identifier)
                            && *deadline_ms > now
                            && *deadline_ms <= now.saturating_add(NATIVE_RENEWAL_MAX_MS)
                    }
                    NativeRenewalMode::ReconcileOnly => start_permit.is_none(),
                }
        }
        (Request::Recheck { .. }, SourceResponse::Recheck { v, fence }) => {
            *v == AUTHORITY_RESPONSE_V2
                && fence.v == RENEWAL_FENCE_V3
                && fence.owner_id == peer.0
                && fence.owner_epoch == peer.1
        }
        (Request::Start { .. }, SourceResponse::Start { v, .. })
        | (Request::Unknown { .. }, SourceResponse::Unknown { v }) => *v == AUTHORITY_RESPONSE_V2,
        (
            Request::Published {
                publication_revision: expected,
                ..
            },
            SourceResponse::Published {
                v,
                publication_revision,
            },
        ) => *v == AUTHORITY_RESPONSE_V2 && publication_revision == expected,
        _ => false,
    };
    if !valid {
        return Err(unverified());
    }
    Ok(())
}

pub fn identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 256
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.' | b':'))
}
