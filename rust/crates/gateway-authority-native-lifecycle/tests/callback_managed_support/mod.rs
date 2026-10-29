use super::support::{NOW, Store, fixture, seed, unavailable};
use gateway_auth_credentials::provider_credentials::{
    CodexNativeOwner, MANAGED_NATIVE_SOURCE_VERSION, ManagedNativeSource,
    codex_credential_identity, codex_source_pool_key, parse_codex_source_pool,
};
use gateway_auth_native_contracts::transport::NativeCapability;
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_primitives::crypto::{Secret, base64url};
use gateway_authority_keys as keys;
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_core::state::AuthorityStore;
use gateway_source::error::GatewayError;
pub(super) fn managed_identity() -> Result<NativeTransportIdentity, GatewayError> {
    NativeTransportIdentity::new(
        "key".into(),
        "owner".into(),
        "epoch".into(),
        [73; 32],
        std::collections::BTreeSet::from([
            NativeCapability::OwnerRenewal,
            NativeCapability::AccessPublish,
        ]),
    )
}
pub(super) async fn managed_fixture(
    store: &Store,
    id: &NativeTransportIdentity,
) -> Result<(), GatewayError> {
    fixture(store, id, None)?;
    let raw = store
        .read(&codex_source_pool_key())
        .await?
        .ok_or_else(unavailable)?;
    let mut source = parse_codex_source_pool(raw.json_value()?)?.ok_or_else(unavailable)?;
    let account = &mut source.accounts[0];
    account.refresh_token = Secret::new(String::new());
    let identity = account.identity();
    account.native_owner = Some(CodexNativeOwner {
        codex_home: "/tmp/native-source".into(),
        generation_hash: identity.native_generation_hash,
        native_refreshed_at: None,
        external_user_id: None,
        managed_source: Some(ManagedNativeSource {
            v: MANAGED_NATIVE_SOURCE_VERSION,
            generation_hash: "a".repeat(64),
        }),
    });
    let raw = store.read(&keys::pool()).await?.ok_or_else(unavailable)?;
    let mut normalized: SubscriptionPool =
        serde_json::from_value(raw.json_value()?.clone()).map_err(|_| unavailable())?;
    normalized.accounts[0].credential_version = identity.credential_version;
    seed(
        store,
        codex_source_pool_key(),
        "source",
        source.private_source_value(),
    )?;
    seed(store, keys::pool(), "normalized", normalized)?;
    Ok(())
}
pub(super) fn publication_body(
    action: &gateway_authority_native_preparation::SourceNativeRenewalAction,
) -> Result<Vec<u8>, GatewayError> {
    let payload =
        serde_json::to_vec(&serde_json::json!({"exp":NOW/1000+600})).map_err(|_| unavailable())?;
    let access = format!("{}.{}.synthetic", base64url(b"{}"), base64url(&payload));
    let identity = codex_credential_identity(
        "account",
        &Secret::new(access.clone()),
        &Secret::new(String::new()),
    );
    let fence = action.fence();
    serde_json::to_vec(&serde_json::json!({"operation":"managed_access_publish","v":2,"action_ref":action.action_ref(),"operation_id":"operation","native_owner_id":fence.native_owner_id,"owner_revision":fence.owner_revision,"source_revision":fence.source_revision,"normalized_revision":fence.normalized_revision,"publication_revision":"publication","account_id_hash":fence.account_id_hash,"previous_native_source_generation":fence.source_native_generation,"native_source_generation":"b".repeat(64),"access_generation":identity.native_generation_hash,"document":{"account_id":"account","access_token":access,"native_refreshed_at":null}})).map_err(|_|unavailable())
}
