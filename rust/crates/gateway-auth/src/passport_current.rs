//! Concrete Passport preflight. Authority owns its private single-use effect proof.
use crate::resolve::AuthService;
use gateway_auth_contracts::types::AuthHost;
use gateway_auth_passport_contracts::{
    actor_data::{PassportAppSessionBinding, PassportAppSessionData},
    session::PassportDate,
};
use gateway_auth_passport_sessions::sessions::PassportSessions;
use gateway_core::state::AuthorityStore;
use gateway_source::error::GatewayError;

impl<S: AuthorityStore, H: AuthHost + PassportDate> AuthService<'_, S, H> {
    pub(crate) async fn passport_cookie(
        &self,
        request: &gateway_auth_contracts::types::AuthRequest,
    ) -> Result<Option<gateway_auth_contracts::types::AuthPrincipal>, GatewayError> {
        use gateway_auth_contracts::types::{AuthMethod, AuthPrincipal};
        use gateway_auth_passport::{
            browser::{cookie, production_request},
            passport::verify_source,
        };
        use gateway_auth_passport_contracts::{
            actor_data::PassportSessionData, passport::PASSPORT_COOKIE,
        };
        let Some(token) = cookie(request, PASSPORT_COOKIE) else {
            return Ok(None);
        };
        if !production_request(request) {
            return Err(gateway_auth_primitives::error::unauthorized());
        }
        let identity = if gateway_auth_passport_sessions::sessions::token_shape(&token) {
            PassportSessionData::App(
                PassportSessions {
                    store: self.store,
                    host: self.host,
                }
                .authenticate(&token)
                .await?,
            )
        } else {
            PassportSessionData::Jwt(verify_source(self.host, &token).await?)
        };
        let is_admin = identity.is_admin();
        Ok(Some(AuthPrincipal {
            token: None,
            method: AuthMethod::PassportSession { identity },
            is_admin,
            is_super_admin: false,
        }))
    }
    /// Performs fixed central introspection outside an Authority transaction, then
    /// rereads the exact row revision/token/subject/date/fixed binding. DATA only.
    pub async fn check_passport_app_session(
        &self,
        binding: &PassportAppSessionBinding,
    ) -> Result<PassportAppSessionData, GatewayError> {
        PassportSessions {
            store: self.store,
            host: self.host,
        }
        .check_bound(binding)
        .await
    }
}
