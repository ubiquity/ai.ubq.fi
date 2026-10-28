//! Fixed live HTTP/stream adapter. Account and owner capabilities are request-owned.
mod account;
pub(crate) mod body;
mod health;
mod platform;

use crate::host::PlatformClock;
use crate::host::PlatformHost;
use crate::host::PlatformTokens;
use gateway_auth_contracts::{
    types::AuthConfig, types::AuthMethod, types::AuthPrincipal, types::AuthRequest,
};
use gateway_authority::{
    native_control::actor::NativeRenewalActor, native_control::actor::authenticate_client_actor,
    store::Authority,
};
use gateway_contracts::identity::{ApiKeyId, PrincipalId, RequestContext, RequestId};
use gateway_live_runtime::protocol::{LiveRoute, live_route, principal_id};
use gateway_source::error::GatewayError;
use gateway_worker_storage::store::now_ms;
use std::rc::Rc;
use worker::Context;

/// Captures the actual authenticated request, not an actor reconstructed from request ID/time.
pub struct WorkerLiveHost {
    platform: PlatformHost,
    context: Rc<Context>,
    config: AuthConfig,
    actor: Rc<NativeRenewalActor>,
    mapped_call_id: Option<String>,
}

impl WorkerLiveHost {
    pub async fn bind(
        platform: PlatformHost,
        context: Rc<Context>,
        original: AuthRequest,
        config: AuthConfig,
        principal: AuthPrincipal,
        request_id: RequestId,
    ) -> Result<Self, GatewayError> {
        let url = worker::Url::parse(&original.url).map_err(|_| account::missing())?;
        let route = live_route(&original.method, url.path()).ok_or_else(account::missing)?;
        let request = RequestContext {
            request_id,
            principal: PrincipalId(principal_id(&principal)),
            key_id: match &principal.method {
                AuthMethod::KvApiKey { key_id, .. } => Some(ApiKeyId(key_id.clone())),
                _ => None,
            },
            route: match &route {
                LiveRoute::Create => "live.create",
                LiveRoute::Sideband(_) => "live.sideband",
            }
            .into(),
            started_at_ms: now_ms()?,
        };
        let authority =
            Authority::with_clock(platform.store.clone(), PlatformTokens, PlatformClock);
        let actor =
            authenticate_client_actor(&authority, &platform, &config, &original, &request).await?;
        if actor.principal() != principal_id(&principal) {
            return Err(account::missing());
        }
        Ok(Self {
            platform,
            context,
            config,
            actor: Rc::new(actor),
            mapped_call_id: match route {
                LiveRoute::Create => None,
                LiveRoute::Sideband(call_id) => Some(call_id),
            },
        })
    }
}
