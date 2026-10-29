mod support;
use gateway_auth_contracts::types::AuthConfig;
use gateway_auth_credentials::provider_credentials::codex_source_pool_key;
use gateway_authority_contracts::AuthorityClock;
use gateway_authority_keys as keys;
use gateway_authority_native_callback_owner::{CallbackOwnerSelector, RenewalActionVersion};
use gateway_authority_native_lifecycle::{
    SourceNativeLifecycleContext, SourceNativeLifecycleService,
};
use gateway_authority_native_preparation::{
    SourceNativePreparationContext, SourceNativeRenewalService,
};
use gateway_authority_native_records::owner::RegisteredRenewalOwner;
use gateway_native_authority_request::NativeRenewalAuthorityRequest as Request;
use support::{
    Cancel, Host, Store, TestResult, Tokens, actor, control_body, fixture, identity, run,
    signed_owner,
};

#[test]
fn actual_source_action_selects_only_registered_owner_and_owned_cleanup_survives_pool_loss()
-> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (actor, _) = actor(&store, &host, &config).await?;
        let owners = [RegisteredRenewalOwner::configured(
            "native".into(),
            "/tmp/native-source".into(),
        )?];
        let cancel = Cancel(false);
        let prep = SourceNativeRenewalService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            SourceNativePreparationContext::configured(&id, &owners[0], &cancel),
        );
        let action = prep
            .prepare_source_live_mapped_renewal(&actor, "call")
            .await?;
        let lifecycle = SourceNativeLifecycleService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            SourceNativeLifecycleContext::configured(&id, &owners[0], &cancel),
        );
        let body = control_body(action.action_ref(), "operation")?;
        let claim = lifecycle
            .claim(
                &signed_owner(&id, &body, 71).await?,
                action.action_ref(),
                "operation",
                &body,
            )
            .await?;
        let selector = CallbackOwnerSelector::new(&store, Some(&host), &owners);
        let request = Request::Recheck {
            v: 1,
            operation_id: "operation".into(),
            claim_ref: claim.claim_ref().into(),
        };
        let selected = selector.callback(&request).await?;
        assert_eq!(selected.version(), RenewalActionVersion::Source3);
        assert!(std::ptr::eq(selected.owner(), &owners[0]));
        assert!(selector.legacy_action(action.action_ref()).await.is_err());
        store.0.borrow_mut().remove(&codex_source_pool_key().0);
        store.0.borrow_mut().remove(&keys::pool().0);
        host.now.set(AuthorityClock::now_ms(&host)? + 60_000);
        assert!(selector.callback(&request).await.is_err());
        let cleanup = Request::Unknown {
            v: 1,
            operation_id: "operation".into(),
            claim_ref: claim.claim_ref().into(),
        };
        let retained = selector.callback(&cleanup).await?;
        assert_eq!(retained.version(), RenewalActionVersion::Source3);
        assert!(std::ptr::eq(retained.owner(), &owners[0]));
        let wrong = Request::Unknown {
            v: 1,
            operation_id: "operation".into(),
            claim_ref: "wrong-claim".into(),
        };
        assert!(selector.callback(&wrong).await.is_err());
        let foreign = [RegisteredRenewalOwner::configured(
            "foreign".into(),
            "/tmp/foreign-source".into(),
        )?];
        assert!(
            CallbackOwnerSelector::new(&store, Some(&host), &foreign)
                .callback(&cleanup)
                .await
                .is_err()
        );
        Ok(())
    })
}
