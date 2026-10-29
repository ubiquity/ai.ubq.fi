use gateway_core::state::AuthorityStore;
mod callback_actor_support;
mod callback_support;
mod support;
use gateway_auth_contracts::types::AuthConfig;
use gateway_authority::store::Authority;
use gateway_authority_native_records::owner::RegisteredRenewalOwner;
use gateway_authority_routing_selection::{SelectionOperations, types::LiveRouteSelection};
use gateway_native_authority_request::NativeRenewalAuthorityRequest as Request;
use gateway_native_authority_wire::{
    context::NativeRenewalContext,
    source_response::SourceNativeRenewalAuthorityResponseV2 as Response,
};
use gateway_routing_runtime_test_support::{seed::seed_pool, store::Store as RuntimeStore};
use support::{Cancel, Host, NOW, Store, TestResult, fixture, identity, run};
#[test]
fn source_callback_live_create_and_mapped_pair_response2_and_utf16() -> TestResult {
    run(async {
        for index in 0..2 {
            let store = RuntimeStore::new();
            seed_pool(&store);
            let host = Host::default();
            let config = AuthConfig::default();
            let id = identity()?;
            let inputs = Store::default();
            fixture(&inputs, &id, None)?;
            let input_rows = inputs
                .0
                .borrow()
                .iter()
                .map(|(key, row)| gateway_core::state::RecordMutation::Put {
                    key: gateway_kv_codec::key::RecordKey(key.clone()),
                    record: row.clone(),
                })
                .collect();
            store
                .commit(gateway_core::state::AtomicTransition {
                    checks: Vec::new(),
                    mutations: input_rows,
                })
                .await?;
            let authority = Authority::with_clock(store.clone(), store.clone(), store.clone());
            let (actor, _) =
                callback_actor_support::actor(authority.store(), &host, &config, false).await?;
            let owner =
                RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
            let cancel = Cancel(false);
            let LiveRouteSelection::Eligible(selected) =
                authority.select_live_account(None, NOW).await?
            else {
                return Err("actual live selection required".into());
            };
            let prep = authority.source_native_renewals(&host, &config, &id, &owner, &cancel);
            let action = if index == 0 {
                prep.prepare_source_live_create_renewal(&actor, &selected)
                    .await?
            } else {
                prep.prepare_source_live_mapped_renewal(&actor, "call")
                    .await?
            };
            let service = authority.source_native_lifecycle(&host, &config, &id, &owner, &cancel);
            let op = format!("callback-live-{index}");
            let request =
                callback_support::claim(&id, action.action_ref(), &op, 190 + index as u8)?;
            let response = callback_support::execute(
                &service,
                authority.store(),
                &host,
                &id,
                &request,
                90 + index as u8,
            )
            .await?;
            let Response::Claim {
                v: 2,
                fence,
                claim_ref,
                ..
            } = response
            else {
                return Err("source response2".into());
            };
            assert_eq!(&fence.principal, actor.principal().as_source());
            assert!(fence.principal.as_units().contains(&0xd800));
            assert!(fence.principal.as_units().contains(&0));
            if index == 0 {
                assert!(matches!(
                    fence.context,
                    NativeRenewalContext::LiveCreate { .. }
                ));
            } else {
                assert!(matches!(
                    fence.context,
                    NativeRenewalContext::LiveMapped { .. }
                ));
            }
            let recheck = Request::Recheck {
                v: 1,
                operation_id: op,
                claim_ref,
            };
            assert!(matches!(
                callback_support::execute(
                    &service,
                    authority.store(),
                    &host,
                    &id,
                    &recheck,
                    92 + index as u8
                )
                .await?,
                Response::Recheck { v: 2, .. }
            ));
        }
        Ok(())
    })
}
