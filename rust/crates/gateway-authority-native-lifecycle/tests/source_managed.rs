mod support;
use gateway_auth_contracts::types::AuthConfig;
use gateway_auth_credentials::provider_credentials::{
    CodexNativeOwner, MANAGED_NATIVE_SOURCE_VERSION, ManagedNativeSource,
    codex_credential_identity, codex_source_pool_key, parse_codex_source_pool,
};
use gateway_auth_native_proof::native_auth::{NativeCapability, NativeTransportIdentity};
use gateway_auth_passport_sessions::sessions::session_key;
use gateway_auth_primitives::crypto::{Secret, base64url};
use gateway_authority_keys as keys;
use gateway_authority_native_preparation::{
    SourceNativePreparationContext, SourceNativeRenewalService,
};
use gateway_authority_native_records::{
    managed::NativeManagedPublicationDisposition, owner::RegisteredRenewalOwner, rows,
};
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_core::state::AuthorityStore;
use gateway_source::error::GatewayError;
use support::{
    Cancel, Host, NOW, Nonces, Store, TestResult, Tokens, actor, control_body, fixture, run, seed,
    signed_owner, unavailable,
};
fn managed_identity() -> Result<NativeTransportIdentity, GatewayError> {
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
async fn managed_fixture(store: &Store, id: &NativeTransportIdentity) -> Result<(), GatewayError> {
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
fn publication_body(
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
async fn signed_publication(
    identity: &NativeTransportIdentity,
    body: &[u8],
    n: u8,
) -> Result<gateway_auth_native_proof::native_auth::AuthenticatedNativeOwner, GatewayError> {
    use gateway_auth_native_proof::native_auth::{NativeAuthHeaders, NativeAuthenticator};
    let path = gateway_auth_native_wire::native_read::NATIVE_PUBLICATION_PATH;
    let nonce = base64url(&[n; 32]);
    let mac = identity.sign(path, NOW, &nonce, body)?;
    NativeAuthenticator::new(identity, &Nonces::default())
        .authenticate(
            path,
            NativeAuthHeaders {
                version: "1",
                key_id: identity.key_id(),
                time_ms: &NOW.to_string(),
                nonce: &nonce,
                mac: &mac,
            },
            body,
            NOW,
        )
        .await
}
#[test]
fn actual_source_managed_publication_atomically_advances_generation_and_replays_known_terminal()
-> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = managed_identity()?;
        managed_fixture(&store, &id).await?;
        let (actor, token) = actor(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let prep = SourceNativeRenewalService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            SourceNativePreparationContext::configured(&id, &owner, &cancel),
        );
        let action = prep
            .prepare_source_live_mapped_renewal(&actor, "call")
            .await?;
        let lifecycle = gateway_authority_native_lifecycle::SourceNativeLifecycleService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            gateway_authority_native_lifecycle::SourceNativeLifecycleContext::configured(
                &id, &owner, &cancel,
            ),
        );
        let control = control_body(action.action_ref(), "operation")?;
        let proof = signed_owner(&id, &control, 30).await?;
        let mut claim = lifecycle
            .claim(&proof, action.action_ref(), "operation", &control)
            .await?;
        lifecycle.start(&mut claim).await?;
        let service = gateway_authority_native_managed::SourceNativeManagedService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            gateway_authority_native_managed::SourceNativeManagedContext::configured(
                &id, &owner, &cancel,
            ),
        );
        let body = publication_body(&action)?;
        let proof = signed_publication(&id, &body, 31).await?;
        let receipt = service.publish_managed(&proof, &body).await?;
        assert_eq!(
            receipt.disposition,
            NativeManagedPublicationDisposition::Applied
        );
        assert_eq!(receipt.current_source_revision, "publication");
        assert_eq!(receipt.current_normalized_revision, "publication");
        let fence = service
            .verify_action_publication(&action, "operation", "publication")
            .await?;
        assert_eq!(fence.source_native_generation, "b".repeat(64));
        assert_ne!(fence.access_generation, action.fence().access_generation);
        assert_eq!(
            service.publish_managed(&proof, &body).await?.disposition,
            NativeManagedPublicationDisposition::AlreadyCurrent
        );
        assert_eq!(
            lifecycle
                .published(&claim, "publication")
                .await?
                .publication_revision(),
            "publication"
        );
        let raw = store
            .read(&rows::key("operation", "operation"))
            .await?
            .ok_or("operation")?;
        let operation: rows::OperationRow = serde_json::from_value(raw.json_value()?.clone())?;
        assert_eq!(operation.phase, rows::Phase::Published);
        let terminal = operation.terminal.ok_or("terminal")?;
        assert_eq!(terminal.source_revision, "publication");
        assert_eq!(terminal.normalized_revision, "publication");
        let key = rows::generation_key_parts(
            &action.fence().native_owner_id,
            &action.fence().account_id_hash,
            &action.fence().source_native_generation,
        )?;
        let raw = store.read(&key).await?.ok_or("generation")?;
        let generation: rows::GenerationEffect = serde_json::from_value(raw.json_value()?.clone())?;
        assert_eq!(
            generation
                .terminal
                .ok_or("generation terminal")?
                .publication_revision,
            "publication"
        );
        store.0.borrow_mut().remove(&session_key(&token).0);
        assert!(
            service
                .verify_action_publication(&action, "operation", "publication")
                .await
                .is_err()
        );
        let raw = store
            .read(&rows::key("operation", "operation"))
            .await?
            .ok_or("retained")?;
        assert_eq!(raw.json_value()?["phase"], "published");
        Ok(())
    })
}
#[test]
fn logout_at_managed_publication_final_cas_writes_none_of_the_original_atomic_group() -> TestResult
{
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = managed_identity()?;
        managed_fixture(&store, &id).await?;
        let (actor, token) = actor(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let prep = SourceNativeRenewalService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            SourceNativePreparationContext::configured(&id, &owner, &cancel),
        );
        let action = prep
            .prepare_source_live_mapped_renewal(&actor, "call")
            .await?;
        let lifecycle = gateway_authority_native_lifecycle::SourceNativeLifecycleService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            gateway_authority_native_lifecycle::SourceNativeLifecycleContext::configured(
                &id, &owner, &cancel,
            ),
        );
        let control = control_body(action.action_ref(), "operation")?;
        let proof = signed_owner(&id, &control, 32).await?;
        let mut claim = lifecycle
            .claim(&proof, action.action_ref(), "operation", &control)
            .await?;
        lifecycle.start(&mut claim).await?;
        let keys = [
            codex_source_pool_key(),
            keys::pool(),
            rows::key("operation", "operation"),
            rows::generation_key_parts(
                &action.fence().native_owner_id,
                &action.fence().account_id_hash,
                &action.fence().source_native_generation,
            )?,
        ];
        let mut before = Vec::new();
        for key in &keys {
            before.push(store.read(key).await?.ok_or("before")?);
        }
        let service = gateway_authority_native_managed::SourceNativeManagedService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            gateway_authority_native_managed::SourceNativeManagedContext::configured(
                &id, &owner, &cancel,
            ),
        );
        let body = publication_body(&action)?;
        let proof = signed_publication(&id, &body, 33).await?;
        *store.1.borrow_mut() = Some(session_key(&token));
        assert!(service.publish_managed(&proof, &body).await.is_err());
        for (key, before) in keys.iter().zip(before) {
            let after = store.read(key).await?.ok_or("after")?;
            assert_eq!(before.revision, after.revision);
            assert_eq!(before.value, after.value);
        }
        lifecycle.unknown(&claim).await?;
        Ok(())
    })
}
