//! Trusted native bindings. No request-selected destination, default public
//! target, provider token, arbitrary path, generic control or caller-selected owner.
use crate::{
    host::{PlatformHost, response_bytes},
    native_transport::{NativeTransport, unavailable},
    transport::stream::{ObservationHandle, ObservedStream},
};
use gateway_auth::{
    crypto::{base64url, decode_base64url},
    native::{
        AuthStream, NATIVE_CONTROL_PATH, NATIVE_FOLLOW_PATH, NATIVE_MAX_BODY_BYTES,
        NATIVE_RESPONSE_MAC_HEADER, NATIVE_RPC_PATH, NATIVE_WIRE_VERSION, NativeReadRequest,
        NativeRpcEnvelope, NativeRpcResponse,
    },
    native_auth::{NativeCapability, NativeTransportIdentity},
    native_control::{
        NATIVE_CONTROL_MAX_BYTES, NATIVE_RENEWAL_MAX_MS, NativeControlEnvelope,
        NativeControlResponse, unverified,
    },
    types::AuthResponse,
};
use gateway_authority::native_control::receiver::NativeRenewalAuthority;
use gateway_core::error::GatewayError;
use serde::Deserialize;
use std::{collections::BTreeSet, rc::Rc};
use worker::{Context, Env, Fetcher, Response};

/// One exact deployment binding. No binding-name scan or request-derived destination.
pub const NATIVE_CARRIER_BINDING: &str = "UOS_NATIVE_CARRIER";
pub const NATIVE_CARRIER_CONFIG_MAX_BYTES: usize = 16_384;

#[derive(Clone, Default)]
pub struct NativeBindings {
    transport: Option<NativeTransport>,
    renewal_owners: Option<Rc<gateway_authority::native_control::owners::RegisteredRenewalOwners>>,
}

type PlatformAuthority = gateway_authority::store::Authority<
    crate::authority::RemoteAuthorityStore,
    crate::host::PlatformTokens,
>;
type PlatformReceiver<'a> = NativeRenewalAuthority<
    'a,
    crate::authority::RemoteAuthorityStore,
    crate::host::PlatformTokens,
    PlatformHost,
>;
fn platform_authority(platform: &PlatformHost) -> PlatformAuthority {
    let (tokens, clock) = (crate::host::PlatformTokens, crate::host::PlatformClock);
    gateway_authority::store::Authority::with_clock(platform.store.clone(), tokens, clock)
}

impl NativeBindings {
    /// Read one exact deployment binding. Absence keeps the capability unavailable;
    /// malformed, partial or unsupported configuration fails closed. The binding is
    /// parsed before routing and cannot select a destination per request.
    pub fn from_worker_binding(env: &Env) -> Result<Option<Self>, GatewayError> {
        let raw = crate::host::setting(env, NATIVE_CARRIER_BINDING);
        let present = worker::js_sys::Reflect::has(
            env,
            &worker::wasm_bindgen::JsValue::from_str(NATIVE_CARRIER_BINDING),
        )
        .map_err(|_| invalid_configuration())?;
        if present && raw.is_none() {
            return Err(invalid_configuration());
        }
        Self::from_binding_value(raw.as_deref())
    }

    pub fn from_binding_value(raw: Option<&str>) -> Result<Option<Self>, GatewayError> {
        let Some(raw) = raw else {
            return Ok(None);
        };
        if raw.len() > NATIVE_CARRIER_CONFIG_MAX_BYTES {
            return Err(invalid_configuration());
        }
        let config: NativeCarrierConfig =
            serde_json::from_str(raw).map_err(|_| invalid_configuration())?;
        config.into_bindings().map(Some)
    }

    /// Trusted injection only. No client body/header or provider credential can
    /// acquire this capability.
    pub fn configured(carrier: Fetcher, identity: NativeTransportIdentity) -> Self {
        Self {
            transport: Some(NativeTransport::configured(carrier, identity)),
            renewal_owners: None,
        }
    }

    /// Trusted injection of one fixed HTTPS origin; never read from a request or
    /// discovered from the environment. This grants no renewal ownership.
    pub fn configured_https(
        origin: &str,
        identity: NativeTransportIdentity,
    ) -> Result<Self, GatewayError> {
        Ok(Self {
            transport: Some(NativeTransport::configured_https(origin, identity)?),
            renewal_owners: None,
        })
    }

    /// The fixed native owner registry is explicit trusted configuration. A
    /// supervisor carrier alone never supplies credential-renewal ownership.
    pub fn with_renewal_owners(
        mut self,
        owners: gateway_authority::native_control::owners::RegisteredRenewalOwners,
    ) -> Self {
        self.renewal_owners = Some(Rc::new(owners));
        self
    }

    pub(crate) fn transport(&self) -> Result<&NativeTransport, GatewayError> {
        self.transport.as_ref().ok_or_else(unavailable)
    }

    pub fn transport_identity(&self) -> Result<&NativeTransportIdentity, GatewayError> {
        self.transport().map(|transport| &transport.identity)
    }

    /// Configured fixed owner registry; unavailable unless trusted configuration set it.
    pub fn renewal_owners(
        &self,
    ) -> Result<&gateway_authority::native_control::owners::RegisteredRenewalOwners, GatewayError>
    {
        self.renewal_owners.as_deref().ok_or_else(unavailable)
    }

    /// Only shared current authority mints this action; the native carrier still
    /// resolves and rechecks its selector before opening any provider socket.
    pub async fn live_upgrade(
        &self,
        action: &gateway_authority::native_live::NativeLiveAction,
        platform: &PlatformHost,
    ) -> Result<crate::response::GatewayResponse, GatewayError> {
        self.transport()?
            .live_upgrade(action.action_ref(), platform)
            .await
    }

    pub(crate) async fn refresh_routing(
        &self,
        fence: &gateway_routing_runtime::types::CredentialFence,
        platform: &PlatformHost,
        original: &gateway_core::identity::RequestContext,
    ) -> Result<gateway_routing_runtime::types::RefreshOutcome, GatewayError> {
        let gateway_routing_runtime::types::CredentialFence::Codex {
            admission,
            source_revision,
            native_generation,
            ..
        } = fence
        else {
            return Err(unverified());
        };
        let identity = &self.transport()?.identity;
        let authority = &platform_authority(platform);
        let config = crate::host::auth_config(&platform.env);
        let actor = gateway_authority::native_control::actor::authenticate_client_actor(
            authority,
            platform,
            &config,
            &platform.original,
            original,
        )
        .await?;
        let owner = self
            .renewal_owners()?
            .for_inference(authority, admission)
            .await?;
        let receiver =
            NativeRenewalAuthority::configured(authority, identity, owner, &config, platform);
        let action = receiver.prepare_routing(&actor).await?;
        let current = action.fence();
        if current.account_id_hash != admission.binding.account_id_hash.0
            || current.credential_version != admission.binding.credential_version.0
            || current.source_revision != *source_revision
            || current.access_generation != *native_generation
        {
            return Err(unverified());
        }
        self.complete_renewal(&action, &receiver, platform).await?;
        Ok(gateway_routing_runtime::types::RefreshOutcome::Published)
    }

    pub(crate) async fn renew_live_create(
        &self,
        actor: &gateway_authority::native_control::actor::NativeRenewalActor,
        selected: &gateway_authority::routing::LiveAccountSelection,
        platform: &PlatformHost,
        config: &gateway_auth::types::AuthConfig,
    ) -> Result<(), GatewayError> {
        let identity = &self.transport()?.identity;
        let authority = &platform_authority(platform);
        let owner = self
            .renewal_owners()?
            .for_live_create(authority, selected)
            .await?;
        let receiver =
            NativeRenewalAuthority::configured(authority, identity, owner, config, platform);
        let action = receiver
            .prepare_live_create_renewal(actor, selected)
            .await?;
        self.complete_renewal(&action, &receiver, platform).await?;
        Ok(())
    }

    pub(crate) async fn renew_live_mapped(
        &self,
        actor: &gateway_authority::native_control::actor::NativeRenewalActor,
        call_id: &str,
        platform: &PlatformHost,
        config: &gateway_auth::types::AuthConfig,
    ) -> Result<(), GatewayError> {
        let identity = &self.transport()?.identity;
        let authority = &platform_authority(platform);
        let owner = self
            .renewal_owners()?
            .for_live_mapped(authority, actor, config, call_id)
            .await?;
        let receiver =
            NativeRenewalAuthority::configured(authority, identity, owner, config, platform);
        let action = receiver.prepare_live_mapped_renewal(actor, call_id).await?;
        self.complete_renewal(&action, &receiver, platform).await?;
        Ok(())
    }

    pub(crate) async fn renew_cache_experiment(
        &self,
        actor: &gateway_authority::native_control::actor::NativeRenewalActor,
        permit: &gateway_authority::cache_experiment::RefreshPermit,
        platform: &PlatformHost,
        config: &gateway_auth::types::AuthConfig,
    ) -> Result<gateway_core::identity::PoolVersionstamp, GatewayError> {
        let identity = &self.transport()?.identity;
        let authority = &platform_authority(platform);
        let owner = self
            .renewal_owners()?
            .for_cache_experiment(authority, permit)
            .await?;
        let receiver =
            NativeRenewalAuthority::configured(authority, identity, owner, config, platform);
        let action = receiver
            .prepare_cache_experiment_renewal(actor, permit)
            .await?;
        let verified = self.complete_renewal(&action, &receiver, platform).await?;
        Ok(gateway_core::identity::PoolVersionstamp(
            verified.source_revision,
        ))
    }

    async fn complete_renewal(
        &self,
        action: &gateway_authority::native_control::NativeRenewalAction,
        receiver: &PlatformReceiver<'_>,
        platform: &PlatformHost,
    ) -> Result<gateway_authority::native_control::NativeRenewalFence, GatewayError> {
        use gateway_auth::native_control::{NativeControlDisposition, NativeControlRequest};
        let envelope = NativeControlEnvelope {
            v: NATIVE_WIRE_VERSION,
            operation_id: crate::transport::random::opaque_token()?,
            request: NativeControlRequest::CodexRenewAccess {
                action_ref: action.action_ref().to_owned(),
            },
        };
        let response = self.renew(&envelope, platform).await?;
        if !matches!(
            response.disposition,
            NativeControlDisposition::Published | NativeControlDisposition::AlreadyCurrent
        ) {
            return Err(GatewayError::unavailable(
                "The current native credential owner did not establish a renewed access publication.",
                "codex_auth_owner_unavailable",
            ));
        }
        let publication = response
            .publication_revision
            .as_deref()
            .ok_or_else(unverified)?;
        receiver
            .verify_action_publication(action, &envelope.operation_id, publication)
            .await
    }

    pub async fn rpc(
        &self,
        request: NativeReadRequest,
        platform: &PlatformHost,
    ) -> Result<AuthResponse, GatewayError> {
        let transport = self.transport.as_ref().ok_or_else(unavailable)?;
        let envelope = NativeRpcEnvelope {
            v: NATIVE_WIRE_VERSION,
            request,
        };
        envelope.validate()?;
        let body = serde_json::to_vec(&envelope).map_err(|_| unavailable())?;
        let mut exchange = transport
            .send(NATIVE_RPC_PATH, body, platform, 15_000)
            .await?;
        let result = async {
            if exchange.response.status_code() != 200 {
                return Err(unavailable());
            }
            let body = response_bytes(&mut exchange.response, NATIVE_MAX_BODY_BYTES).await?;
            let response: NativeRpcResponse =
                serde_json::from_slice(&body).map_err(|_| unavailable())?;
            response.validate_for(&envelope)?;
            let mut output = AuthResponse::json(response.status, response.body);
            if response.stream.is_some() {
                // The placeholder headers are merged onto the physical follow
                // response; JSON here would overwrite its actual SSE type.
                output.headers.insert(
                    "content-type".into(),
                    "text/event-stream; charset=utf-8".into(),
                );
            }
            output.stream = response.stream;
            Ok(output)
        }
        .await;
        exchange.cancellation.abort();
        result
    }

    pub async fn follow(
        &self,
        descriptor: AuthStream,
        platform: &PlatformHost,
        context: Rc<Context>,
    ) -> Result<Response, GatewayError> {
        let transport = self.transport.as_ref().ok_or_else(unavailable)?;
        let body = serde_json::to_vec(&descriptor).map_err(|_| unavailable())?;
        // Source Supervisor FOLLOW_MAX_DURATION_MS is ten minutes. Keep the
        // cancellation and absolute timer alive for the entire outgoing body.
        let mut exchange = transport
            .send(NATIVE_FOLLOW_PATH, body, platform, 600_000)
            .await?;
        let content_type = exchange
            .response
            .headers()
            .get("content-type")
            .map_err(|_| unavailable())?;
        if exchange.response.status_code() != 200
            || !content_type.as_deref().is_some_and(|value| {
                value
                    .split(';')
                    .next()
                    .is_some_and(|kind| kind.trim().eq_ignore_ascii_case("text/event-stream"))
            })
        {
            exchange.cancellation.abort();
            return Err(unavailable());
        }
        let source = exchange.response.stream().map_err(|_| unavailable())?;
        // Child signals do not own the root's incoming abort listener. Retain
        // that guard after routing returns headers, until this body finalizes.
        let caller_cancellation = platform.cancellation.clone();
        let stream = ObservedStream::new(
            source,
            ObservationHandle::default(),
            context,
            exchange.cancellation,
            Some(exchange.deadline),
            Box::new(move |_| {
                Box::pin(async move {
                    drop(caller_cancellation);
                })
            }),
        );
        let mut response = stream.into_response().map_err(|_| unavailable())?;
        for (name, value) in [
            ("content-type", "text/event-stream; charset=utf-8"),
            ("cache-control", "no-store"),
            ("x-accel-buffering", "no"),
        ] {
            response
                .headers_mut()
                .set(name, value)
                .map_err(|_| unavailable())?;
        }
        Ok(response)
    }

    /// The action reference must already have been prepared by shared current
    /// authority. This transport authenticates a result; it grants no action.
    pub async fn renew(
        &self,
        envelope: &NativeControlEnvelope,
        platform: &PlatformHost,
    ) -> Result<NativeControlResponse, GatewayError> {
        envelope.validate()?;
        let authority = platform_authority(platform);
        let gateway_auth::native_control::NativeControlRequest::CodexRenewAccess { action_ref } =
            &envelope.request;
        self.renewal_owners()?
            .for_action(&authority, action_ref)
            .await?;
        let transport = self.transport.as_ref().ok_or_else(unavailable)?;
        let body = serde_json::to_vec(envelope).map_err(|_| unavailable())?;
        let mut exchange = transport
            .send(
                NATIVE_CONTROL_PATH,
                body,
                platform,
                NATIVE_RENEWAL_MAX_MS as i32,
            )
            .await?;
        let result = async {
            let status = exchange.response.status_code();
            let signatures = exchange
                .response
                .headers()
                .entries()
                .filter(|(name, _)| name.eq_ignore_ascii_case(NATIVE_RESPONSE_MAC_HEADER))
                .collect::<Vec<_>>();
            let body = response_bytes(&mut exchange.response, NATIVE_CONTROL_MAX_BYTES)
                .await
                .map_err(|_| unverified())?;
            if signatures.len() != 1 {
                return Err(unverified());
            }
            transport
                .identity
                .verify_control_response(
                    &exchange.nonce,
                    &exchange.request,
                    status,
                    &body,
                    &signatures[0].1,
                )
                .map_err(|_| unverified())?;
            if status != 200 {
                return Err(unverified());
            }
            let response: NativeControlResponse =
                serde_json::from_slice(&body).map_err(|_| unverified())?;
            response.validate_for(envelope)?;
            Ok(response)
        }
        .await;
        exchange.cancellation.abort();
        result
    }
}

/// `renewal_owners` is the fixed native-owner registry (owner id to Codex home). It is
/// present exactly when the identity holds `owner_renewal` and `access_publish`.
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct NativeCarrierConfig {
    origin: String,
    identity: NativeCarrierIdentity,
    #[serde(default)]
    renewal_owners: Vec<NativeCarrierRenewalOwner>,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct NativeCarrierRenewalOwner {
    owner_id: String,
    codex_home: String,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct NativeCarrierIdentity {
    key_id: String,
    owner_id: String,
    owner_epoch: String,
    key_base64url: String,
    capabilities: Vec<NativeCarrierCapability>,
}
#[derive(Deserialize)]
#[serde(rename_all = "snake_case")]
enum NativeCarrierCapability {
    SupervisorRead,
    ConfigurationPublish,
    AccessPublish,
    OwnerRenewal,
    CodexAuthImport,
    NativeProfileImport,
    LiveSideband,
}
impl NativeCarrierConfig {
    fn into_bindings(self) -> Result<NativeBindings, GatewayError> {
        use gateway_authority::native_control::{owners, receiver::RegisteredRenewalOwner};
        let key =
            decode_base64url(&self.identity.key_base64url).map_err(|_| invalid_configuration())?;
        if key.len() != 32 || base64url(&key) != self.identity.key_base64url {
            return Err(invalid_configuration());
        }
        let key: [u8; 32] = key.try_into().map_err(|_| invalid_configuration())?;
        let capabilities = self
            .identity
            .capabilities
            .into_iter()
            .map(|value| match value {
                NativeCarrierCapability::SupervisorRead => NativeCapability::SupervisorRead,
                NativeCarrierCapability::ConfigurationPublish => {
                    NativeCapability::ConfigurationPublish
                }
                NativeCarrierCapability::AccessPublish => NativeCapability::AccessPublish,
                NativeCarrierCapability::OwnerRenewal => NativeCapability::OwnerRenewal,
                NativeCarrierCapability::CodexAuthImport => NativeCapability::CodexAuthImport,
                NativeCarrierCapability::NativeProfileImport => {
                    NativeCapability::NativeProfileImport
                }
                NativeCarrierCapability::LiveSideband => NativeCapability::LiveSideband,
            });
        let capabilities = capabilities.collect::<BTreeSet<_>>();
        let renewal = [
            NativeCapability::OwnerRenewal,
            NativeCapability::AccessPublish,
        ];
        // A partial pairing would silently disable refresh or grant unregistered renewal.
        if renewal.map(|cap| capabilities.contains(&cap)) != [!self.renewal_owners.is_empty(); 2] {
            return Err(invalid_configuration());
        }
        let identity = NativeTransportIdentity::new(
            self.identity.key_id,
            self.identity.owner_id,
            self.identity.owner_epoch,
            key,
            capabilities,
        )?;
        let bindings = NativeBindings::configured_https(&self.origin, identity)
            .map_err(|_| invalid_configuration())?;
        if self.renewal_owners.is_empty() {
            return Ok(bindings);
        }
        let registry = self
            .renewal_owners
            .into_iter()
            .map(|owner| RegisteredRenewalOwner::configured(owner.owner_id, owner.codex_home));
        let registry = registry.collect::<Result<Vec<_>, _>>();
        let registry = registry.and_then(owners::RegisteredRenewalOwners::configured);
        Ok(bindings.with_renewal_owners(registry.map_err(|_| invalid_configuration())?))
    }
}

fn invalid_configuration() -> GatewayError {
    GatewayError::unavailable(
        "Native carrier configuration is invalid",
        "native_configuration_invalid",
    )
}
