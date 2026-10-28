//! Exact original finite non-inference account selectors; source principals stay UTF16.
use crate::SourceNativeContextFactsReader;
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_authority_keys as keys;
use gateway_authority_native_records::{live::mapping_key, rows::fail};
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_contracts::identity::ActiveSelection;
use gateway_core::state::AuthorityStore;
use gateway_native_authority_wire::context::NativeRenewalContext;
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use serde_json::Value;
impl<S: AuthorityStore, C: NativeRenewalCancellation> SourceNativeContextFactsReader<'_, S, C> {
    pub(crate) async fn live_account_hash(
        &mut self,
        context: &NativeRenewalContext,
        principal: &JsString16,
    ) -> Result<String, GatewayError> {
        Ok(match context {
            NativeRenewalContext::LiveCreate {
                binding,
                active_generation,
                ..
            } => {
                let active: ActiveSelection =
                    self.cache.read(&keys::active()).await?.ok_or_else(fail)?;
                if active.v != 1
                    || active.binding != *binding
                    || active.generation != *active_generation
                {
                    return Err(fail());
                }
                let routing: Option<Value> = self.cache.read(&keys::routing()).await?;
                if let Some(raw) = routing {
                    let state =
                        gateway_authority_routing_records::codec::parse_routing_state(&raw)?;
                    let slot = state
                        .slots
                        .get(usize::try_from(binding.slot).map_err(|_| fail())?)
                        .ok_or_else(fail)?;
                    if slot.account_id_hash.as_ref() != Some(&binding.account_id_hash)
                        || slot.credential_version != binding.credential_version
                        || slot.generation != binding.routing_generation
                        || slot.invalid_credential_version.as_ref()
                            == Some(&binding.credential_version)
                    {
                        return Err(fail());
                    }
                } else if binding.routing_generation != 0 {
                    return Err(fail());
                }
                let selection: Option<Value> = self.cache.read(&keys::provider_selection()).await?;
                if selection.as_ref().is_some_and(|v| {
                    !v.is_null()
                        && gateway_schema::selection::ProviderSelection::normalize(v).is_none()
                }) {
                    return Err(fail());
                }
                binding.account_id_hash.0.clone()
            }
            NativeRenewalContext::LiveMapped {
                call_id,
                mapping_revision,
            } => {
                let pending: Option<Value> = self.cache.read(&mapping_key(call_id, true)).await?;
                if pending.is_some() {
                    return Err(fail());
                }
                let key = mapping_key(call_id, false);
                let mapping: SourceJson = self
                    .cache
                    .read_source_control(&key)
                    .await?
                    .ok_or_else(fail)?;
                if self.cache.revision(&key).as_ref() != Some(mapping_revision)
                    || source::member(&mapping, "principal_id")
                        .as_ref()
                        .and_then(source::string)
                        .as_ref()
                        != Some(principal)
                    || self.cache.expiry(&key).is_some_and(|v| v <= self.cache.now)
                {
                    return Err(fail());
                }
                let account: String = source::member(&mapping, "account_id")
                    .ok_or_else(fail)?
                    .deserialize()
                    .map_err(|_| fail())?;
                if account.is_empty() {
                    return Err(fail());
                }
                sha256_hex(format!("uos_ai\0codex_routing_account\0{account}").as_bytes())
            }
            NativeRenewalContext::CacheExperiment { context } => {
                let now = self.time(self.cache.now)?;
                self.validate_cache_context(context, now).await?;
                let pool: SubscriptionPool =
                    self.cache.read(&keys::pool()).await?.ok_or_else(fail)?;
                if pool.accounts.len() != 2 {
                    return Err(fail());
                }
                pool.accounts[0].account_id_hash.0.clone()
            }
            NativeRenewalContext::Inference { .. } => return Err(fail()),
        })
    }
}
