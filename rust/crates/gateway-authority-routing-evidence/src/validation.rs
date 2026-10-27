//! Pure validation of source routing identities; a valid value is not a capability.
use gateway_authority_contracts::{generation, invalid, text};
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_contracts::identity::AccountBinding;
use gateway_source::error::GatewayError;
use std::collections::BTreeSet;

pub fn validate_binding(binding: &AccountBinding) -> Result<(), GatewayError> {
    text(&binding.account_id_hash.0)?;
    text(&binding.credential_version.0)?;
    text(&binding.pool_versionstamp.0)?;
    generation(binding.slot)?;
    generation(binding.routing_generation)?;
    Ok(())
}

pub fn validate_pool(pool: &SubscriptionPool) -> Result<(), GatewayError> {
    if pool.v != 1 || pool.accounts.is_empty() {
        return Err(invalid("Codex subscription pool is missing or malformed."));
    }
    text(&pool.pool_versionstamp.0)?;
    let mut identities = BTreeSet::new();
    for account in &pool.accounts {
        text(&account.account_id_hash.0)?;
        text(&account.admin_account_hash.0)?;
        text(&account.credential_version.0)?;
        if !identities.insert(&account.account_id_hash.0) {
            return Err(invalid("Duplicate Codex account identity."));
        }
    }
    Ok(())
}
