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
            browser::{binding_for_request, cookie, cookie_names},
            passport::verify_source_for_config,
        };
        use gateway_auth_passport_contracts::{
            actor_data::PassportSessionData, passport::PASSPORT_COOKIE,
        };
        let Some(config) = binding_for_request(self.config, request) else {
            return if cookie(request, PASSPORT_COOKIE).is_some() {
                Err(gateway_auth_primitives::error::unauthorized())
            } else {
                Ok(None)
            };
        };
        let Some(token) = cookie(request, &cookie_names(&config)[0]) else {
            return Ok(None);
        };
        let identity = if gateway_auth_passport_sessions::sessions::token_shape(&token) {
            PassportSessionData::App(
                PassportSessions {
                    store: self.store,
                    host: self.host,
                }
                .authenticate_for(&token, &config)
                .await?,
            )
        } else {
            PassportSessionData::Jwt(verify_source_for_config(self.host, &config, &token).await?)
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
        let config = self
            .config
            .passport
            .clone()
            .unwrap_or_else(gateway_auth_passport_contracts::passport::PassportConfig::production)
            .validated()
            .ok_or_else(gateway_auth_primitives::error::unauthorized)?;
        PassportSessions {
            store: self.store,
            host: self.host,
        }
        .check_bound_for(binding, &config)
        .await
    }
}
