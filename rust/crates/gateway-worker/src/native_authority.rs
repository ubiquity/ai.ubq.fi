//! Fixed reverse native protocols, authenticated against the common durable nonce authority.
pub(crate) mod import_dispatch;
use crate::host::PlatformClock;
use crate::host::PlatformTokens;
use crate::native_host::NativeBindings;
use gateway_auth_native_contracts::transport::{NativeAuthHeaders, NativeCapability};
use gateway_auth_native_proof::native_auth::NativeAuthenticator;
use gateway_auth_native_protocol::native_replay::StoreNonceAuthority;
use gateway_auth_native_wire::native_read::{
    NATIVE_KEY_HEADER, NATIVE_MAC_HEADER, NATIVE_NONCE_HEADER, NATIVE_PUBLICATION_PATH,
    NATIVE_RENEWAL_AUTHORITY_PATH, NATIVE_RESPONSE_MAC_HEADER, NATIVE_TIME_HEADER,
    NATIVE_VERSION_HEADER,
};
use gateway_authority::native_control::receiver::NativeRenewalAuthority;
use gateway_authority::store::Authority;
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_native_authority_request::NativeRenewalAuthorityRequest as RenewalRequest;
use gateway_source::error::GatewayError;
use gateway_worker_authority_binding::authority::RemoteAuthorityStore;
use gateway_worker_http_body::body::request_bytes;
use gateway_worker_platform::cancellation::Cancellation;
use gateway_worker_platform::headers::HeaderLimits;
use gateway_worker_platform::headers::bounded_headers;
use gateway_worker_storage::store::now_ms;
use serde::Serialize;
use worker::{Env, Method, Request, Response};

pub fn is_path(path: &str) -> bool {
    path == NATIVE_RENEWAL_AUTHORITY_PATH
        || path == gateway_auth_native_proof::native_live::NATIVE_LIVE_AUTHORITY_PATH
}
struct Incoming(std::rc::Rc<Cancellation>);
impl NativeRenewalCancellation for Incoming {
    fn cancelled(&self) -> bool {
        self.0.signal().aborted()
    }
}
struct Headers {
    version: String,
    key_id: String,
    time_ms: String,
    nonce: String,
    mac: String,
}
impl Headers {
    fn read(request: &Request) -> Result<Self, GatewayError> {
        let values = bounded_headers(
            request,
            HeaderLimits {
                total_bytes: 16_384,
                value_bytes: 256,
                fields: 2000,
            },
        )?;
        let one = |name: &str| {
            let entries = values
                .iter()
                .filter(|(key, _)| key.eq_ignore_ascii_case(name))
                .collect::<Vec<_>>();
            if entries.len() != 1 {
                Err(invalid())
            } else {
                Ok(entries[0].1.clone())
            }
        };
        Ok(Self {
            version: one(NATIVE_VERSION_HEADER)?,
            key_id: one(NATIVE_KEY_HEADER)?,
            time_ms: one(NATIVE_TIME_HEADER)?,
            nonce: one(NATIVE_NONCE_HEADER)?,
            mac: one(NATIVE_MAC_HEADER)?,
        })
    }
    fn borrowed(&self) -> NativeAuthHeaders<'_> {
        NativeAuthHeaders {
            version: &self.version,
            key_id: &self.key_id,
            time_ms: &self.time_ms,
            nonce: &self.nonce,
            mac: &self.mac,
        }
    }
}
fn invalid() -> GatewayError {
    GatewayError::new(
        400,
        "Native authority request is invalid",
        Some("native_authority_invalid"),
    )
}
fn body<T: Serialize>(value: &T) -> Result<Vec<u8>, GatewayError> {
    serde_json::to_vec(value).map_err(|_| invalid())
}
fn result<T: Serialize>(response: Result<T, GatewayError>) -> Result<(u16, Vec<u8>), GatewayError> {
    match response {
        Ok(value) => Ok((200, body(&value)?)),
        Err(error) => Ok((error.status, body(&error.envelope)?)),
    }
}
fn response(status: u16, body: Vec<u8>, mac: String) -> Result<Response, GatewayError> {
    let mut response = Response::from_bytes(body)
        .map_err(|_| invalid())?
        .with_status(status);
    for (key, value) in [
        ("Content-Type", "application/json"),
        ("Cache-Control", "no-store"),
        (NATIVE_RESPONSE_MAC_HEADER, &mac),
    ] {
        response
            .headers_mut()
            .set(key, value)
            .map_err(|_| invalid())?;
    }
    Ok(response)
}

pub async fn handle(
    mut request: Request,
    env: &Env,
    native: &NativeBindings,
    request_id: &str,
) -> Result<Response, GatewayError> {
    if request.method() != Method::Post {
        return Err(GatewayError::new(
            405,
            "Method not allowed",
            Some("method_not_allowed"),
        ));
    }
    let transport = native.transport()?;
    let headers = Headers::read(&request)?;
    let cancellation = std::rc::Rc::new(Cancellation::from_request(&request)?);
    let incoming = Incoming(cancellation.clone());
    let path = request.path();
    let maximum = if path == NATIVE_RENEWAL_AUTHORITY_PATH {
        // The authenticator still applies each complete typed operation's old/new limit.
        gateway_auth_native_wire::native_read::NATIVE_MAX_BODY_BYTES
    } else {
        gateway_auth_native_proof::native_live::NATIVE_LIVE_AUTHORITY_MAX_BYTES
    };
    let bytes = request_bytes(&mut request, maximum).await?;
    let store = RemoteAuthorityStore::from_env(env)?;
    let replay = StoreNonceAuthority::new(&store);
    let authority = Authority::with_clock(store.clone(), PlatformTokens, PlatformClock);
    let config = crate::host::auth_config(env);
    let now = now_ms()?;
    if path == gateway_auth_native_proof::native_live::NATIVE_LIVE_AUTHORITY_PATH {
        let proof = gateway_auth_native_proof::native_live::NativeLiveAuthenticator::new(
            &transport.identity,
            &replay,
        )
        .authenticate_authority(headers.borrowed(), &bytes, now)
        .await?;
        let receiver = gateway_authority::native_live::NativeLiveAuthority::configured(
            &authority,
            &transport.identity,
            &config,
        );
        let (status, output) = result(receiver.execute(&proof, &bytes).await)?;
        let mac = transport.identity.sign_live_authority_response(
            &headers.nonce,
            &bytes,
            status,
            &output,
        )?;
        response(status, output, mac)
    } else {
        let proof = NativeAuthenticator::new(&transport.identity, &replay)
            .authenticate(
                NATIVE_RENEWAL_AUTHORITY_PATH,
                headers.borrowed(),
                &bytes,
                now,
            )
            .await?;
        if let Some(response) = import_dispatch::authority(
            &authority,
            &proof,
            native,
            &config,
            &incoming,
            (&headers.nonce, &bytes),
        )
        .await?
        {
            return Ok(response);
        }
        proof.require_request(
            NativeCapability::OwnerRenewal,
            NATIVE_RENEWAL_AUTHORITY_PATH,
            &bytes,
            now,
        )?;
        let parsed = RenewalRequest::parse(&bytes)?;
        let owners = native.renewal_owners()?;
        let selected = owners.for_callback(&authority, &parsed).await?;
        let owner = selected.owner();
        let (status, output) = match selected.version() {
            gateway_authority_native_callback_owner::RenewalActionVersion::Legacy2 => {
                let receiver = NativeRenewalAuthority::configured(
                    &authority,
                    &transport.identity,
                    owner,
                    &config,
                    &incoming,
                );
                result(
                    gateway_authority_native_callback::execute_legacy(
                        &receiver,
                        &transport.identity,
                        &replay,
                        &PlatformClock,
                        &bytes,
                    )
                    .await,
                )?
            }
            gateway_authority_native_callback_owner::RenewalActionVersion::Source3 => {
                let (_, original) = crate::runtime::original_auth_request(&request, &path)?;
                let host = crate::host::PlatformHost {
                    cancellation,
                    store: store.clone(),
                    env: env.clone(),
                    native: native.clone(),
                    original,
                    request_id: gateway_contracts::identity::RequestId(request_id.into()),
                };
                let receiver = authority.source_native_lifecycle(
                    &host,
                    &config,
                    &transport.identity,
                    owner,
                    &incoming,
                );
                result(
                    gateway_authority_native_callback::execute_source(
                        &receiver,
                        &proof,
                        &transport.identity,
                        &replay,
                        &PlatformClock,
                        &bytes,
                    )
                    .await,
                )?
            }
        };
        let mac =
            transport
                .identity
                .sign_authority_response(&headers.nonce, &bytes, status, &output)?;
        response(status, output, mac)
    }
}

pub async fn managed_publication(
    request: &Request,
    env: &Env,
    native: &NativeBindings,
    store: &RemoteAuthorityStore,
    bytes: &[u8],
) -> Result<Response, GatewayError> {
    if request.method() != Method::Post
        || bytes.len() > gateway_publication_transport::managed::MANAGED_PUBLICATION_MAX_BYTES
    {
        return Err(invalid());
    }
    let transport = native.transport()?;
    let headers = Headers::read(request)?;
    let incoming = Incoming(std::rc::Rc::new(Cancellation::from_request(request)?));
    let replay = StoreNonceAuthority::new(store);
    let proof = NativeAuthenticator::new(&transport.identity, &replay)
        .authenticate(
            NATIVE_PUBLICATION_PATH,
            headers.borrowed(),
            bytes,
            now_ms()?,
        )
        .await?;
    let authority = Authority::with_clock(store.clone(), PlatformTokens, PlatformClock);
    let config = crate::host::auth_config(env);
    // The authenticated action reference only selects a configured owner. The
    // common receiver still validates the entire v2 publication and current claim.
    #[derive(serde::Deserialize)]
    struct Selector {
        action_ref: String,
    }
    let selector: Selector = serde_json::from_slice(bytes).map_err(|_| invalid())?;
    let owner = native
        .renewal_owners()?
        .for_action(&authority, &selector.action_ref)
        .await?;
    let receiver = NativeRenewalAuthority::configured(
        &authority,
        &transport.identity,
        owner,
        &config,
        &incoming,
    );
    use gateway_authority_native_records::managed::NativeManagedPublicationDisposition as Disposition;
    use gateway_publication::wire::PublicationDisposition;
    let applied = receiver
        .publish_managed(&proof, bytes)
        .await
        .map(|receipt| {
            let disposition = match receipt.disposition {
                Disposition::Applied => PublicationDisposition::Applied,
                Disposition::AlreadyCurrent => PublicationDisposition::AlreadyCurrent,
            };
            gateway_publication_transport::managed::ManagedPublicationReceipt {
                v: 2,
                operation: "managed_access_publish".into(),
                disposition,
                owner_id: receipt.owner_id,
                owner_epoch: receipt.owner_epoch,
                action_ref: receipt.action_ref,
                operation_id: receipt.operation_id,
                native_owner_id: receipt.native_owner_id,
                owner_revision: receipt.owner_revision,
                source_revision: receipt.source_revision,
                normalized_revision: receipt.normalized_revision,
                publication_revision: receipt.publication_revision,
                current_source_revision: receipt.current_source_revision,
                current_normalized_revision: receipt.current_normalized_revision,
                previous_native_source_generation: receipt.previous_native_source_generation,
                native_source_generation: receipt.native_source_generation,
                access_generation: receipt.access_generation,
            }
        });
    let (status, output) = result(applied)?;
    let mac =
        transport
            .identity
            .sign_publication_response(&headers.nonce, bytes, status, &output)?;
    response(status, output, mac)
}
