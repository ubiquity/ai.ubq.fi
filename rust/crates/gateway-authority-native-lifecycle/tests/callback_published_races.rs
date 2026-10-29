use gateway_core::state::AuthorityStore;
mod callback_actor_support;
mod callback_managed_support;
mod callback_race_support;
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
use support::{Cancel, Host, NOW, TestResult, Tokens, run};
#[test]
fn callback_published_currentness_and_final_cas_refuse_without_erasing_real_terminal() -> TestResult
{
    run(async {
        for loss in [
            "logout-await",
            "config-await",
            "source-await",
            "expiry-await",
            "logout-cas",
            "source-cas",
            "version-cas",
        ] {
            let store = callback_race_support::RaceStore::new();
            let host = Host::default();
            let config = AuthConfig::default();
            let tokens = Tokens::default();
            let id = callback_managed_support::managed_identity()?;
            callback_managed_support::managed_fixture(&store.inner, &id).await?;
            let (actor, token) =
                callback_actor_support::actor(&store, &host, &config, false).await?;
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
            let service = SourceNativeLifecycleService::new(
                &store,
                &tokens,
                Some(&host),
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
                    &callback_support::claim(&id, action.action_ref(), "operation", 218)?,
                    121,
                )
                .await?,
            )?;
            callback_support::execute(
                &service,
                &store,
                &host,
                &id,
                &Request::Start {
                    v: 1,
                    operation_id: "operation".into(),
                    claim_ref: claim_ref.clone(),
                    start_permit: permit,
                },
                122,
            )
            .await?;
            let publisher = gateway_authority_native_managed::SourceNativeManagedService::new(
                &store,
                &tokens,
                Some(&host),
                &host,
                &config,
                gateway_authority_native_managed::SourceNativeManagedContext::configured(
                    &id, &owner, &cancel,
                ),
            );
            let body = callback_managed_support::publication_body(&action)?;
            let replay =
                gateway_auth_native_protocol::native_replay::StoreNonceAuthority::new(&store);
            let peer = callback_support::peer(
                &id,
                &replay,
                gateway_auth_native_wire::native_read::NATIVE_PUBLICATION_PATH,
                &body,
                NOW,
                123,
            )
            .await?;
            publisher.publish_managed(&peer, &body).await?;
            let request = Request::Published {
                v: 1,
                operation_id: "operation".into(),
                claim_ref,
                publication_revision: "publication".into(),
            };
            let before = store.read(&rows::key("operation", "operation")).await?;
            if loss.ends_with("await") {
                host.hold.set(true);
                let mut future = Box::pin(callback_support::execute(
                    &service, &store, &host, &id, &request, 124,
                ));
                let mut cx = Context::from_waker(Waker::noop());
                assert!(future.as_mut().poll(&mut cx).is_pending());
                match loss {
                    "logout-await" => {
                        store.inner.0.borrow_mut().remove(
                            &gateway_auth_passport_sessions::sessions::session_key(&token).0,
                        );
                    }
                    "config-await" => {
                        store
                            .inner
                            .0
                            .borrow_mut()
                            .get_mut(&gateway_authority_keys::paid_control_config().0)
                            .ok_or("config")?
                            .revision = "published-config-race".into();
                    }
                    "source-await" => {
                        store.inner.0.borrow_mut().get_mut(&gateway_auth_credentials::provider_credentials::codex_source_pool_key().0).ok_or("source")?.revision="published-source-race".into();
                    }
                    "expiry-await" => host.now.set(NOW + 60_000),
                    _ => unreachable!(),
                }
                host.hold.set(false);
                assert!(future.await.is_err());
            } else {
                *store.action.borrow_mut() = Some(rows::key("action", action.action_ref()));
                store.skip_reads.set(1);
                use callback_race_support::Mutation;
                *store.next.borrow_mut() = Some(match loss {
                    "logout-cas" => Mutation::Delete(
                        gateway_auth_passport_sessions::sessions::session_key(&token),
                    ),
                    "source-cas" => Mutation::Revision(
                        gateway_auth_credentials::provider_credentials::codex_source_pool_key(),
                    ),
                    "version-cas" => {
                        Mutation::ActionVersion(rows::key("action", action.action_ref()))
                    }
                    _ => unreachable!(),
                });
                assert!(
                    callback_support::execute(&service, &store, &host, &id, &request, 124)
                        .await
                        .is_err()
                );
                assert!(store.next.borrow().is_none());
            }
            assert_eq!(
                store.read(&rows::key("operation", "operation")).await?,
                before
            );
        }
        Ok(())
    })
}
