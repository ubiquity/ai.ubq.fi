//! Fixed preconnected publication, with distinct managed-v2 and external-v1
//! constructors and authenticated receipts. Refresh material stays native.
use crate::{
    owner_renewal::{NativeAccessPublisher, PublishedAccess, require_current_fence},
    private_https::PrivateHttps,
};
use gateway_auth_credentials::{
    managed_access::ManagedAccessProjection, native_credentials::NativeGeneration,
};
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_native_proof::native_context::NativeControlContext;
use gateway_auth_native_wire::{
    control::{NativeControlEnvelope, NativeControlRequest, unverified},
    native_read::{NATIVE_MAX_BODY_BYTES, NATIVE_PUBLICATION_PATH},
};
use gateway_auth_primitives::crypto::base64url;
use gateway_native_authority_wire::renewal::NativeRenewalFence;
use gateway_publication::{sender, wire};
use gateway_publication_transport::managed;
use gateway_source::error::GatewayError;
use std::{net::TcpStream, time::Instant};

type Request = gateway_publication_policy::protocol::PublicationRequest;
type Response = gateway_publication_policy::protocol::PublicationResponse;

pub struct NativeAccessPublicationHttp {
    identity: NativeTransportIdentity,
    transport: PrivateHttps,
}
impl NativeAccessPublicationHttp {
    pub fn from_connected(
        target: &str,
        socket: TcpStream,
        identity: NativeTransportIdentity,
    ) -> Result<Self, GatewayError> {
        Self::with_roots(target, socket, identity, ureq::tls::RootCerts::WebPki)
    }
    /// Trusted operator/fixture input only; no request-supplied roots or target.
    pub fn with_roots(
        target: &str,
        socket: TcpStream,
        identity: NativeTransportIdentity,
        roots: ureq::tls::RootCerts,
    ) -> Result<Self, GatewayError> {
        Ok(Self {
            identity,
            transport: PrivateHttps::from_connected(
                target,
                NATIVE_PUBLICATION_PATH,
                socket,
                roots,
            )?,
        })
    }
}
struct BoundPublication<'a> {
    transport: &'a PrivateHttps,
    deadline: Instant,
}
impl gateway_publication_policy::protocol::PublicationCarrier for BoundPublication<'_> {
    async fn send(&self, request: Request) -> Result<Response, GatewayError> {
        if request.method != "POST"
            || request.path != NATIVE_PUBLICATION_PATH
            || request.raw_body.len() > NATIVE_MAX_BODY_BYTES
        {
            return Err(unverified());
        }
        let response = self.transport.send(
            &request.headers.pairs(),
            &request.raw_body,
            self.deadline,
            65_536,
        )?;
        Ok(gateway_publication_policy::protocol::PublicationResponse {
            status: response.status,
            headers: response.headers,
            raw_body: response.body,
        })
    }
}
impl NativeAccessPublisher for NativeAccessPublicationHttp {
    fn managed(
        &self,
        request: &NativeControlEnvelope,
        fence: &NativeRenewalFence,
        projection: &ManagedAccessProjection,
        context: &NativeControlContext<'_>,
    ) -> Result<PublishedAccess, GatewayError> {
        require_current_fence(fence)?;
        context.check()?;
        let NativeControlRequest::CodexRenewAccess { action_ref } = &request.request;
        let command = managed::PreparedManagedAccessCommand::managed_access_from_registered(
            managed::ExpectedManagedAccessPublication {
                action_ref: action_ref.clone(),
                operation_id: request.operation_id.clone(),
                native_owner_id: fence.native_owner_id.clone(),
                owner_revision: fence.owner_revision.clone(),
                source_revision: fence.source_revision.clone(),
                normalized_revision: fence.normalized_revision.clone(),
                publication_revision: base64url(&nonce()?),
                account_id_hash: fence.account_id_hash.clone(),
                previous_native_source_generation: fence.source_native_generation.clone(),
            },
            projection,
        )?;
        let carrier = BoundPublication {
            transport: &self.transport,
            deadline: context.deadline(),
        };
        let receipt = futures_executor::block_on(managed::send_managed(
            &self.identity,
            &carrier,
            &command,
            nonce()?,
            context.now_ms()?,
        ))?;
        context.check()?;
        Ok(PublishedAccess {
            publication_revision: receipt.publication_revision,
            already_current: receipt.disposition == wire::PublicationDisposition::AlreadyCurrent,
        })
    }
    fn external(
        &self,
        fence: &NativeRenewalFence,
        generation: &NativeGeneration,
        context: &NativeControlContext<'_>,
    ) -> Result<PublishedAccess, GatewayError> {
        require_current_fence(fence)?;
        context.check()?;
        let command = wire::PreparedPublicationCommand::access_from_registered(
            wire::ExpectedAccessPublication {
                native_owner_id: fence.native_owner_id.clone(),
                owner_revision: fence.owner_revision.clone(),
                source_revision: fence.source_revision.clone(),
                normalized_revision: Some(fence.normalized_revision.clone()),
                account_id_hash: fence.account_id_hash.clone(),
                user_id: generation
                    .credentials
                    .external_user_id
                    .clone()
                    .ok_or_else(unverified)?,
                publication_revision: base64url(&nonce()?),
            },
            &generation.credentials,
        )?;
        let carrier = BoundPublication {
            transport: &self.transport,
            deadline: context.deadline(),
        };
        let receipt = futures_executor::block_on(
            sender::NativePublicationSender::configured(&self.identity, &carrier).send(
                &command,
                nonce()?,
                context.now_ms()?,
            ),
        )?;
        context.check()?;
        Ok(PublishedAccess {
            // V1 AlreadyCurrent can retain an older actual raw revision while
            // acknowledging the new request's publication identifier.
            publication_revision: receipt.source_revision.ok_or_else(unverified)?,
            already_current: receipt.disposition == wire::PublicationDisposition::AlreadyCurrent,
        })
    }
}
fn nonce() -> Result<[u8; 32], GatewayError> {
    let mut bytes = [0; 32];
    getrandom::fill(&mut bytes).map_err(|_| unverified())?;
    Ok(bytes)
}
