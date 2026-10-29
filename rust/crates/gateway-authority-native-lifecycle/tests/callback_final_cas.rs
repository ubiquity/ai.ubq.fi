use gateway_core::state::AuthorityStore;
mod callback_actor_support;
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
use support::{Cancel, Host, NOW, Store, TestResult, Tokens, fixture, identity, run};
#[test]
fn callback_original_final_cas_refuses_stale_actor_source_mapping_and_version() -> TestResult {
    run(async {
        for phase in ["claim", "start", "recheck"] {
            for target in ["actor", "source", "mapping", "version"] {
                let store = callback_race_support::RaceStore::new();
                let host = Host::default();
                let config = AuthConfig::default();
                let tokens = Tokens::default();
                let id = identity()?;
                fixture(&store.inner, &id, None)?;
                let (actor, token) =
                    callback_actor_support::actor(&store, &host, &config, false).await?;
                let owner = RegisteredRenewalOwner::configured(
                    "native".into(),
                    "/tmp/native-source".into(),
                )?;
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
                let request = if phase == "claim" {
                    callback_support::claim(&id, action.action_ref(), "operation", 212)?
                } else {
                    let (claim_ref, permit) = callback_support::decoded_claim(
                        callback_support::execute(
                            &service,
                            &store,
                            &host,
                            &id,
                            &callback_support::claim(&id, action.action_ref(), "operation", 212)?,
                            112,
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
                *store.action.borrow_mut() = Some(rows::key("action", action.action_ref()));
                store
                    .mutation_only
                    .set(phase == "claim" || phase == "start");
                store.skip_reads.set(if phase == "recheck" { 1 } else { 0 });
                use callback_race_support::Mutation;
                *store.next.borrow_mut() = Some(match target {
                    "actor" => Mutation::Delete(
                        gateway_auth_passport_sessions::sessions::session_key(&token),
                    ),
                    "source" => Mutation::Revision(
                        gateway_auth_credentials::provider_credentials::codex_source_pool_key(),
                    ),
                    "mapping" => Mutation::Revision(
                        gateway_authority_native_records::live::mapping_key("call", false),
                    ),
                    "version" => Mutation::ActionVersion(rows::key("action", action.action_ref())),
                    _ => unreachable!(),
                });
                assert!(
                    callback_support::execute(&service, &store, &host, &id, &request, 113)
                        .await
                        .is_err()
                );
                assert!(store.next.borrow().is_none());
                let operation = store.read(&rows::key("operation", "operation")).await?;
                if phase == "claim" {
                    assert!(operation.is_none())
                } else {
                    let row: rows::OperationRow =
                        serde_json::from_value(operation.ok_or("claim")?.json_value()?.clone())?;
                    assert_eq!(row.phase, rows::Phase::Claimed)
                }
            }
        }
        Ok(())
    })
}
