mod support;
use gateway_auth_contracts::types::AuthConfig;
use gateway_auth_passport_sessions::sessions::session_key;
use gateway_authority_native_preparation::{
    SourceNativePreparationContext, SourceNativeRenewalService,
};
use gateway_authority_native_records::source_rows::SourceRenewalActionRowV3;
use gateway_authority_native_records::{owner::RegisteredRenewalOwner, rows};
use gateway_core::source_control::decode_source_control_record;
use gateway_core::state::AuthorityStore;
use std::{
    future::Future,
    task::{Context, Waker},
};
use support::{
    Cancel, Host, Store, TestResult, Tokens, actor, control_body, fixture, identity, run,
    signed_owner,
};
#[test]
fn actual_source_actor_and_complete_cas_mint_one_bound_v3_action_with_original_context()
-> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (actor, _) = actor(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let service = SourceNativeRenewalService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            SourceNativePreparationContext::configured(&id, &owner, &cancel),
        );
        let action = service
            .prepare_source_live_mapped_renewal(&actor, "call")
            .await?;
        assert_eq!(action.fence().v, 3);
        assert_eq!(&action.fence().principal, actor.principal().as_source());
        let raw = store
            .read(&rows::key("action", action.action_ref()))
            .await?
            .ok_or("action")?;
        let row = SourceRenewalActionRowV3::from_source_json(&decode_source_control_record(&raw)?)?;
        assert_eq!(&row.context, actor.context());
        assert_eq!(row.fence, *action.fence());
        assert!(row.operation_id.is_none());
        let repeated = service
            .prepare_source_live_mapped_renewal(&actor, "call")
            .await?;
        assert_eq!(repeated.action_ref(), action.action_ref());
        assert_eq!(
            store
                .0
                .borrow()
                .keys()
                .filter(|key| key.starts_with(&rows::prefix().0))
                .count(),
            1
        );
        Ok(())
    })
}
#[test]
fn logout_at_mapping_revision_cas_never_mints_or_publishes_a_source_action() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (actor, token) = actor(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let service = SourceNativeRenewalService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            SourceNativePreparationContext::configured(&id, &owner, &cancel),
        );
        *store.1.borrow_mut() = Some(session_key(&token));
        assert!(
            service
                .prepare_source_live_mapped_renewal(&actor, "call")
                .await
                .is_err()
        );
        assert!(
            !store
                .0
                .borrow()
                .keys()
                .any(|key| key.starts_with(&rows::prefix().0))
        );
        Ok(())
    })
}
#[test]
fn opaque_actor_from_same_typed_foreign_store_cannot_prepare_here() -> TestResult {
    run(async {
        let store = Store::default();
        let foreign = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (actor, _) = actor(&foreign, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let service = SourceNativeRenewalService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            SourceNativePreparationContext::configured(&id, &owner, &cancel),
        );
        assert!(
            service
                .prepare_source_live_mapped_renewal(&actor, "call")
                .await
                .is_err()
        );
        assert!(
            !store
                .0
                .borrow()
                .keys()
                .any(|key| key.starts_with(&rows::prefix().0))
        );
        Ok(())
    })
}
#[test]
fn cancellation_keeps_the_original_499_before_new_action_effects() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (actor, _) = actor(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(true);
        let service = SourceNativeRenewalService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            SourceNativePreparationContext::configured(&id, &owner, &cancel),
        );
        let error = service
            .prepare_source_live_mapped_renewal(&actor, "call")
            .await
            .err()
            .ok_or("cancel accepted")?;
        assert_eq!(error.status, 499);
        assert!(
            !store
                .0
                .borrow()
                .keys()
                .any(|key| key.starts_with(&rows::prefix().0))
        );
        Ok(())
    })
}

#[test]
fn signed_source_claim_consumes_one_start_and_shares_original_generation_namespace() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (actor, _) = actor(&store, &host, &config).await?;
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
        let service = gateway_authority_native_lifecycle::SourceNativeLifecycleService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            gateway_authority_native_lifecycle::SourceNativeLifecycleContext::configured(
                &id, &owner, &cancel,
            ),
        );
        let body = control_body(action.action_ref(), "operation1")?;
        let proof = signed_owner(&id, &body, 1).await?;
        let mut claim = service
            .claim(&proof, action.action_ref(), "operation1", &body)
            .await?;
        use gateway_native_authority_wire::renewal::NativeRenewalMode;
        assert_eq!(claim.mode(), NativeRenewalMode::RenewOnce);
        assert_eq!(claim.fence(), action.fence());
        assert_eq!(
            service.start(&mut claim).await?,
            NativeRenewalMode::RenewOnce
        );
        assert_eq!(
            service.start(&mut claim).await?,
            NativeRenewalMode::ReconcileOnly
        );
        let body = control_body(action.action_ref(), "operation1")?;
        let proof = signed_owner(&id, &body, 2).await?;
        let repeated = service
            .claim(&proof, action.action_ref(), "operation1", &body)
            .await?;
        assert_eq!(repeated.mode(), NativeRenewalMode::ReconcileOnly);
        assert!(repeated.start_permit().is_none());
        let key = rows::generation_key_parts(
            &action.fence().native_owner_id,
            &action.fence().account_id_hash,
            &action.fence().source_native_generation,
        )?;
        let row = store.read(&key).await?.ok_or("generation")?;
        let generation: rows::GenerationEffect = serde_json::from_value(row.json_value()?.clone())?;
        assert_eq!(generation.v, 1);
        assert_eq!(generation.operation_id, "operation1");
        assert_eq!(generation.claim_ref, claim.claim_ref());
        Ok(())
    })
}
#[test]
fn logout_during_real_start_currentness_refuses_start_but_owned_unknown_still_settles() -> TestResult
{
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
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
        let service = gateway_authority_native_lifecycle::SourceNativeLifecycleService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            gateway_authority_native_lifecycle::SourceNativeLifecycleContext::configured(
                &id, &owner, &cancel,
            ),
        );
        let body = control_body(action.action_ref(), "operation")?;
        let proof = signed_owner(&id, &body, 3).await?;
        let mut claim = service
            .claim(&proof, action.action_ref(), "operation", &body)
            .await?;
        let calls = host.calls.get();
        host.hold.set(true);
        let mut start = Box::pin(service.start(&mut claim));
        let mut cx = Context::from_waker(Waker::noop());
        assert!(start.as_mut().poll(&mut cx).is_pending());
        assert!(host.calls.get() > calls);
        store.0.borrow_mut().remove(&session_key(&token).0);
        host.hold.set(false);
        assert!(start.await.is_err());
        let before = host.calls.get();
        service.unknown(&claim).await?;
        assert_eq!(host.calls.get(), before);
        let row = store
            .read(&rows::key("operation", "operation"))
            .await?
            .ok_or("operation")?;
        let operation: rows::OperationRow = serde_json::from_value(row.json_value()?.clone())?;
        assert_eq!(operation.phase, rows::Phase::Unknown);
        assert!(operation.terminal.is_none());
        assert!(service.recheck(&claim).await.is_err());
        Ok(())
    })
}
#[test]
fn logout_at_original_claim_cas_leaves_no_operation_or_generation_effect() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
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
        let service = gateway_authority_native_lifecycle::SourceNativeLifecycleService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            gateway_authority_native_lifecycle::SourceNativeLifecycleContext::configured(
                &id, &owner, &cancel,
            ),
        );
        let body = control_body(action.action_ref(), "operation")?;
        let proof = signed_owner(&id, &body, 4).await?;
        *store.1.borrow_mut() = Some(session_key(&token));
        assert!(
            service
                .claim(&proof, action.action_ref(), "operation", &body)
                .await
                .is_err()
        );
        assert!(
            store
                .read(&rows::key("operation", "operation"))
                .await?
                .is_none()
        );
        let key = rows::generation_key_parts(
            &action.fence().native_owner_id,
            &action.fence().account_id_hash,
            &action.fence().source_native_generation,
        )?;
        assert!(store.read(&key).await?.is_none());
        let row = store
            .read(&rows::key("action", action.action_ref()))
            .await?
            .ok_or("action")?;
        let row = SourceRenewalActionRowV3::from_source_json(&decode_source_control_record(&row)?)?;
        assert!(row.operation_id.is_none());
        Ok(())
    })
}
