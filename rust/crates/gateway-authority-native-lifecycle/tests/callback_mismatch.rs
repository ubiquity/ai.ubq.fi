use gateway_core::state::AuthorityStore;
mod callback_support;
mod support;
use callback_support::{claim, decoded_claim, execute, peer};
use gateway_auth_contracts::types::AuthConfig;
use gateway_auth_native_wire::native_read::{NATIVE_CONTROL_PATH, NATIVE_RENEWAL_AUTHORITY_PATH};
use gateway_authority_native_lifecycle::{
    SourceNativeLifecycleContext, SourceNativeLifecycleService,
};
use gateway_authority_native_preparation::{
    SourceNativePreparationContext, SourceNativeRenewalService,
};
use gateway_authority_native_records::{owner::RegisteredRenewalOwner, rows};
use gateway_native_authority_request::NativeRenewalAuthorityRequest as Request;
use gateway_native_authority_wire::{
    renewal::NativeRenewalMode, source_response::SourceNativeRenewalAuthorityResponseV2 as Response,
};
use support::{Cancel, Host, NOW, Store, TestResult, Tokens, actor, fixture, identity, run};
#[test]
fn callback_action_operation_claim_and_permit_mismatches_never_start() -> TestResult {
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
        let (claim_ref, permit) = decoded_claim(
            execute(
                &service,
                &store,
                &host,
                &id,
                &claim(&id, action.action_ref(), "operation", 101)?,
                1,
            )
            .await?,
        )?;
        let initial = store
            .read(&rows::key("operation", "operation"))
            .await?
            .ok_or("operation")?;
        for (index, (operation, reference, start)) in [
            ("wrong-operation", claim_ref.as_str(), permit.as_str()),
            ("operation", "wrong-claim", permit.as_str()),
            ("operation", claim_ref.as_str(), "wrong-permit"),
        ]
        .into_iter()
        .enumerate()
        {
            let request = Request::Start {
                v: 1,
                operation_id: operation.into(),
                claim_ref: reference.into(),
                start_permit: start.into(),
            };
            assert!(
                execute(&service, &store, &host, &id, &request, 50 + index as u8)
                    .await
                    .is_err()
            );
            assert_eq!(
                store.read(&rows::key("operation", "operation")).await?,
                Some(initial.clone())
            );
        }
        let wrong_action = claim(&id, "unbound-action", "wrong-operation", 150)?;
        assert!(
            execute(&service, &store, &host, &id, &wrong_action, 54)
                .await
                .is_err()
        );
        for (index, v) in [0, 2, 3, 4].into_iter().enumerate() {
            let request = Request::Recheck {
                v,
                operation_id: "operation".into(),
                claim_ref: claim_ref.clone(),
            };
            assert!(
                execute(&service, &store, &host, &id, &request, 55 + index as u8)
                    .await
                    .is_err()
            );
        }
        let _ = token;
        Ok(())
    })
}
#[test]
fn callback_duplicate_start_and_old_new_generation_replay_start_once() -> TestResult {
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
        let (claim_ref, permit) = decoded_claim(
            execute(
                &service,
                &store,
                &host,
                &id,
                &claim(&id, action.action_ref(), "operation", 101)?,
                1,
            )
            .await?,
        )?;
        let request = Request::Start {
            v: 1,
            operation_id: "operation".into(),
            claim_ref: claim_ref.clone(),
            start_permit: permit,
        };
        assert!(matches!(
            execute(&service, &store, &host, &id, &request, 70).await?,
            Response::Start {
                v: 2,
                mode: NativeRenewalMode::RenewOnce
            }
        ));
        let after = store
            .read(&rows::key("operation", "operation"))
            .await?
            .ok_or("operation")?;
        assert!(
            execute(&service, &store, &host, &id, &request, 71)
                .await
                .is_err()
        );
        assert_eq!(
            store.read(&rows::key("operation", "operation")).await?,
            Some(after)
        );
        let claimed = execute(
            &service,
            &store,
            &host,
            &id,
            &claim(&id, action.action_ref(), "operation", 172)?,
            72,
        )
        .await?;
        assert!(matches!(
            claimed,
            Response::Claim {
                v: 2,
                mode: NativeRenewalMode::ReconcileOnly,
                start_permit: None,
                ..
            }
        ));
        let second = claim(&id, action.action_ref(), "different-operation", 173)?;
        assert!(
            execute(&service, &store, &host, &id, &second, 73)
                .await
                .is_err()
        );
        let _ = token;
        Ok(())
    })
}
