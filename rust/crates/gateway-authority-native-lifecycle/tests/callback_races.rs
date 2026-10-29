use gateway_core::state::AuthorityStore;
use std::sync::atomic::Ordering;
mod callback_actor_support;
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
fn callback_each_currentness_phase_refuses_logout_config_source_and_expiry_changes() -> TestResult {
    run(async {
        for phase in ["claim", "start", "recheck"] {
            for loss in ["logout", "configuration", "source", "expiry"] {
                let store = Store::default();
                let host = Host::default();
                let config = AuthConfig::default();
                let tokens = Tokens::default();
                let id = identity()?;
                fixture(&store, &id, None)?;
                let (actor, token) = support::actor(&store, &host, &config).await?;
                let owner = RegisteredRenewalOwner::configured(
                    "native".into(),
                    "/tmp/native-source".into(),
                )?;
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
                let request = if phase == "claim" {
                    callback_support::claim(&id, action.action_ref(), "operation", 210)?
                } else {
                    let (claim_ref, permit) = callback_support::decoded_claim(
                        callback_support::execute(
                            &service,
                            &store,
                            &host,
                            &id,
                            &callback_support::claim(&id, action.action_ref(), "operation", 210)?,
                            110,
                        )
                        .await?,
                    )?;
                    if phase == "start" {
                        Request::Start {
                            v: 1,
                            operation_id: "operation".into(),
                            claim_ref,
                            start_permit: permit,
                        }
                    } else {
                        Request::Recheck {
                            v: 1,
                            operation_id: "operation".into(),
                            claim_ref,
                        }
                    }
                };
                let calls = host.calls.get();
                host.hold.set(true);
                let mut future = Box::pin(callback_support::execute(
                    &service, &store, &host, &id, &request, 111,
                ));
                let mut cx = Context::from_waker(Waker::noop());
                assert!(future.as_mut().poll(&mut cx).is_pending());
                assert!(host.calls.get() > calls);
                match loss {
                    "logout" => {
                        store.0.borrow_mut().remove(
                            &gateway_auth_passport_sessions::sessions::session_key(&token).0,
                        );
                    }
                    "configuration" => {
                        store
                            .0
                            .borrow_mut()
                            .get_mut(&gateway_authority_keys::paid_control_config().0)
                            .ok_or("config")?
                            .revision = "config-during-actual-c".into();
                    }
                    "source" => {
                        store.0.borrow_mut().get_mut(&gateway_auth_credentials::provider_credentials::codex_source_pool_key().0).ok_or("source")?.revision="source-during-actual-c".into();
                    }
                    "expiry" => host.now.0.store(NOW + 60_000, Ordering::SeqCst),
                    _ => unreachable!(),
                }
                host.hold.set(false);
                assert!(future.await.is_err());
                let operation = store.read(&rows::key("operation", "operation")).await?;
                if phase == "claim" {
                    assert!(operation.is_none())
                } else {
                    let row: rows::OperationRow = serde_json::from_value(
                        operation.ok_or("existing claim")?.json_value()?.clone(),
                    )?;
                    assert_eq!(row.phase, rows::Phase::Claimed)
                }
            }
        }
        Ok(())
    })
}
