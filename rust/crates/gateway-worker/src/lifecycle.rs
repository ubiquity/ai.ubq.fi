//! Exact source-derived authenticated principal identity shared by admission/routing.
use gateway_auth::types::AuthPrincipal;

pub fn principal_id(principal: &AuthPrincipal) -> gateway_contracts::identity::PrincipalId {
    principal.method.principal_id(
        principal
            .token
            .as_ref()
            .map(gateway_auth::crypto::Secret::expose),
    )
}
