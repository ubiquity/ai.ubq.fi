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
