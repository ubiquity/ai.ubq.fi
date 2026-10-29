use gateway_core::state::AuthorityStore;
use std::sync::atomic::Ordering;
mod callback_actor_support;
mod callback_managed_support;
mod callback_support;
mod support;
use gateway_auth_contracts::types::AuthConfig;
use gateway_authority_native_lifecycle::{
    SourceNativeLifecycleContext, SourceNativeLifecycleService,
};
use gateway_authority_native_preparation::{
    SourceNativePreparationContext, SourceNativeRenewalService,
};
use gateway_authority_native_records::{owner::RegisteredRenewalOwner, rows};
use gateway_native_authority_request::NativeRenewalAuthorityRequest as Request;
use gateway_native_authority_wire::source_response::SourceNativeRenewalAuthorityResponseV2 as Response;
use std::{
    future::Future,
    task::{Context, Waker},
};
use support::{Cancel, Host, NOW, Store, TestResult, Tokens, fixture, identity, run};
#[test]
fn callback_unknown_after_cancel_deadline_logout_and_pool_loss_needs_no_new_c() -> TestResult {
    run(async {
        for loss in ["cancel", "deadline", "logout", "pool", "all"] {
            let store = Store::default();
            let host = Host::default();
            let config = AuthConfig::default();
            let tokens = Tokens::default();
            let id = identity()?;
            fixture(&store, &id, None)?;
            let (actor, token) = support::actor(&store, &host, &config).await?;
            let owner =
                RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
            let cancel = Cancel(false);
            let prep = SourceNativeRenewalService::new(
                &store,
                &tokens,
                Some(&host.now),
                &host,
                &config,
                SourceNativePreparationContext::configured(&id, &owner, &cancel),
            );
            let action = prep
                .prepare_source_live_mapped_renewal(&actor, "call")
                .await?;
            let service = SourceNativeLifecycleService::new(
                &store,
                &tokens,
                Some(&host.now),
                &host,
                &config,
                SourceNativeLifecycleContext::configured(&id, &owner, &cancel),
            );
            let (claim_ref, _) = callback_support::decoded_claim(
                callback_support::execute(
                    &service,
                    &store,
                    &host,
                    &id,
                    &callback_support::claim(&id, action.action_ref(), "operation", 214)?,
                    114,
                )
                .await?,
            )?;
            if loss == "logout" || loss == "all" {
                store
                    .0
                    .borrow_mut()
                    .remove(&gateway_auth_passport_sessions::sessions::session_key(&token).0);
                host.status.set(401)
            }
            if loss == "pool" || loss == "all" {
                store.0.borrow_mut().remove(
                    &gateway_auth_credentials::provider_credentials::codex_source_pool_key().0,
                );
                store
                    .0
                    .borrow_mut()
                    .remove(&gateway_authority_keys::pool().0);
            }
            if loss == "deadline" || loss == "all" {
                host.now.0.store(NOW + 60_000, Ordering::SeqCst)
            }
            let cancel = Cancel(loss == "cancel" || loss == "all");
            let cleanup = SourceNativeLifecycleService::new(
                &store,
                &tokens,
                Some(&host.now),
                &host,
                &config,
                SourceNativeLifecycleContext::configured(&id, &owner, &cancel),
            );
            let request = Request::Unknown {
                v: 1,
                operation_id: "operation".into(),
                claim_ref,
            };
            let calls = host.calls.get();
            host.hold.set(true);
            let mut result = Box::pin(callback_support::execute(
                &cleanup, &store, &host, &id, &request, 115,
            ));
            let mut cx = Context::from_waker(Waker::noop());
            let std::task::Poll::Ready(result) = result.as_mut().poll(&mut cx) else {
                return Err("owned cleanup reacquired pending C".into());
            };
            assert!(matches!(result?, Response::Unknown { v: 2 }));
            assert_eq!(host.calls.get(), calls);
            host.hold.set(false);
            let op: rows::OperationRow = serde_json::from_value(
                store
                    .read(&rows::key("operation", "operation"))
                    .await?
                    .ok_or("operation")?
                    .json_value()?
                    .clone(),
            )?;
            assert_eq!(op.phase, rows::Phase::Unknown);
        }
        Ok(())
    })
}

#[test]
fn callback_unknown_preserves_genuine_terminal_publication() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = callback_managed_support::managed_identity()?;
        callback_managed_support::managed_fixture(&store, &id).await?;
        let (actor, token) = support::actor(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let prep = SourceNativeRenewalService::new(
            &store,
            &tokens,
            Some(&host.now),
            &host,
            &config,
            SourceNativePreparationContext::configured(&id, &owner, &cancel),
        );
        let action = prep
            .prepare_source_live_mapped_renewal(&actor, "call")
            .await?;
        let service = SourceNativeLifecycleService::new(
            &store,
            &tokens,
            Some(&host.now),
            &host,
            &config,
            SourceNativeLifecycleContext::configured(&id, &owner, &cancel),
        );
        let (claim_ref, permit) = callback_support::decoded_claim(
            callback_support::execute(
                &service,
                &store,
                &host,
                &id,
                &callback_support::claim(&id, action.action_ref(), "operation", 216)?,
                116,
            )
            .await?,
        )?;
        let start = Request::Start {
            v: 1,
            operation_id: "operation".into(),
            claim_ref: claim_ref.clone(),
            start_permit: permit,
        };
        callback_support::execute(&service, &store, &host, &id, &start, 117).await?;
        let publisher = gateway_authority_native_managed::SourceNativeManagedService::new(
            &store,
            &tokens,
            Some(&host.now),
            &host,
            &config,
            gateway_authority_native_managed::SourceNativeManagedContext::configured(
                &id, &owner, &cancel,
            ),
        );
        let body = callback_managed_support::publication_body(&action)?;
        let replay = gateway_auth_native_protocol::native_replay::StoreNonceAuthority::new(&store);
        let peer = callback_support::peer(
            &id,
            &replay,
            gateway_auth_native_wire::native_read::NATIVE_PUBLICATION_PATH,
            &body,
            NOW,
            118,
        )
        .await?;
        publisher.publish_managed(&peer, &body).await?;
        let published = Request::Published {
            v: 1,
            operation_id: "operation".into(),
            claim_ref: claim_ref.clone(),
            publication_revision: "publication".into(),
        };
        assert!(matches!(
            callback_support::execute(&service, &store, &host, &id, &published, 119).await?,
            Response::Published { v: 2, .. }
        ));
        let generation = rows::generation_key_parts(
            &action.fence().native_owner_id,
            &action.fence().account_id_hash,
            &action.fence().source_native_generation,
        )?;
        let before = (
            store.read(&rows::key("operation", "operation")).await?,
            store.read(&generation).await?,
        );
        store
            .0
            .borrow_mut()
            .remove(&gateway_auth_passport_sessions::sessions::session_key(&token).0);
        store
            .0
            .borrow_mut()
            .remove(&gateway_auth_credentials::provider_credentials::codex_source_pool_key().0);
        store
            .0
            .borrow_mut()
            .remove(&gateway_authority_keys::pool().0);
        host.now.0.store(NOW + 60_000, Ordering::SeqCst);
        let stopped = Cancel(true);
        let cleanup = SourceNativeLifecycleService::new(
            &store,
            &tokens,
            Some(&host.now),
            &host,
            &config,
            SourceNativeLifecycleContext::configured(&id, &owner, &stopped),
        );
        let request = Request::Unknown {
            v: 1,
            operation_id: "operation".into(),
            claim_ref,
        };
        let calls = host.calls.get();
        assert!(matches!(
            callback_support::execute(&cleanup, &store, &host, &id, &request, 120).await?,
            Response::Unknown { v: 2 }
        ));
        assert_eq!(host.calls.get(), calls);
        assert_eq!(
            (
                store.read(&rows::key("operation", "operation")).await?,
                store.read(&generation).await?
            ),
            before
        );
        Ok(())
    })
}
