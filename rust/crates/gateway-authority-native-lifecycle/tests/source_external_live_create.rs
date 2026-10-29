mod external_runtime_support;
mod support;
use gateway_auth_contracts::types::AuthConfig;
use gateway_authority::store::Authority;
use gateway_authority_native_records::owner::RegisteredRenewalOwner;
use gateway_authority_routing_selection::{SelectionOperations, types::LiveRouteSelection};
use gateway_routing_runtime_test_support::store::Store;
use support::{Cancel, Host, NOW, TestResult, run};
#[test]
fn genuine_live_create_selection_actor_action_claim_and_external_terminal() -> TestResult {
    run(async {
        let store = Store::new();
        let host = Host::default();
        let config = AuthConfig::default();
        let id = external_runtime_support::identity()?;
        external_runtime_support::seed(&store, &id).await?;
        let authority = Authority::with_clock(store.clone(), store.clone(), store.clone());
        let actor =
            external_runtime_support::actor(authority.store(), &host, &config, false).await?;
        let selected = authority.select_live_account(None, NOW).await?;
        let LiveRouteSelection::Eligible(selected) = selected else {
            return Err("actual eligible live selection required".into());
        };
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let action = authority
            .source_native_renewals(&host, &config, &id, &owner, &cancel)
            .prepare_source_live_create_renewal(&actor, &selected)
            .await?;
        assert!(matches!(
            action.fence().context,
            gateway_native_authority_wire::context::NativeRenewalContext::LiveCreate { .. }
        ));
        external_runtime_support::publish(&authority, &host, &config, &id, &owner, &action, false)
            .await?;
        Ok(())
    })
}
