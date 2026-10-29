use gateway_cache_experiment_campaign::CacheCampaignOperations;
use gateway_cache_experiment_dispatch::CacheDispatchOperations;
use gateway_cache_experiment_runner::CacheRunOperations;
use gateway_core::state::AuthorityStore;
mod callback_actor_support;
mod callback_support;
mod support;
use gateway_auth_contracts::types::AuthConfig;
use gateway_authority::store::Authority;
use gateway_authority_native_records::owner::RegisteredRenewalOwner;
use gateway_cache_experiment_records::model::{STEPS, Sample, Usage};
use gateway_native_authority_request::NativeRenewalAuthorityRequest as Request;
use gateway_native_authority_wire::{
    context::NativeRenewalContext,
    source_response::SourceNativeRenewalAuthorityResponseV2 as Response,
};
use gateway_routing_runtime_test_support::{seed::seed_pool, store::Store as RuntimeStore};
use support::{Cancel, Host, NOW, Store, TestResult, fixture, identity, run};
#[test]
fn source_callback_cache_experiment_keeps_actual_admin_and_refresh_permit() -> TestResult {
    run(async {
        let store = RuntimeStore::new();
        let host = Host::default();
        let config = AuthConfig::default();
        let id = identity()?;
        let input: serde_json::Value = serde_json::from_str(include_str!(
            "../../gateway-authority/tests/fixtures/cache-experiment-inventory-source-v2.json"
        ))?;
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
        let mut source = gateway_auth_credentials::provider_credentials::parse_codex_source_pool(
            &input["input"]["codexAuthPool"],
        )?
        .ok_or("pool")?;
        for account in &mut source.accounts {
            let identity = account.identity();
            account.native_owner = Some(
                gateway_auth_credentials::provider_credentials::CodexNativeOwner {
                    codex_home: "/tmp/native-source".into(),
                    generation_hash: identity.native_generation_hash,
                    native_refreshed_at: None,
                    external_user_id: Some("native-user".into()),
                    managed_source: None,
                },
            );
        }
        let raw = source.private_source_value();
        let normalized = gateway_authority_routing_records::types::SubscriptionPool {
            v: 1,
            pool_versionstamp: gateway_contracts::identity::PoolVersionstamp("pool-1".into()),
            accounts: source
                .accounts
                .iter()
                .map(|a| {
                    let i = a.identity();
                    gateway_authority_routing_records::types::PoolAccount {
                        account_id_hash: i.routing_account_hash,
                        admin_account_hash: i.administrative_account_hash,
                        credential_version: i.credential_version,
                        enabled: true,
                    }
                })
                .collect(),
        };
        store
            .commit(gateway_core::state::AtomicTransition {
                checks: Vec::new(),
                mutations: vec![gateway_core::state::RecordMutation::Put {
                    key: gateway_auth_credentials::provider_credentials::codex_source_pool_key(),
                    record: gateway_core::state::StoredRecord::json("pool-1", raw, None),
                }],
            })
            .await?;
        store.seed(gateway_authority_keys::pool(), normalized);
        let catalog = gateway_kv_codec::tuple_key::TupleKey::from_scalar_parts(&[
            gateway_kv_codec::tuple_key::ScalarKeyPart::String("ubq_ai"),
            gateway_kv_codec::tuple_key::ScalarKeyPart::String("codex_models"),
        ])
        .to_record_key();
        store
            .commit(gateway_core::state::AtomicTransition {
                checks: Vec::new(),
                mutations: vec![
                    gateway_core::state::RecordMutation::Put {
                        key: catalog,
                        record: gateway_core::state::StoredRecord::json(
                            "catalog-1",
                            input["input"]["snapshot"].clone(),
                            None,
                        ),
                    },
                    gateway_core::state::RecordMutation::Put {
                        key: gateway_schema::catalog_store::runtime_config_key(),
                        record: gateway_core::state::StoredRecord::json(
                            "runtime",
                            input["runtime"].clone(),
                            None,
                        ),
                    },
                ],
            })
            .await?;
        let authority = Authority::with_clock(store.clone(), store.clone(), store.clone());
        let (actor, _) =
            callback_actor_support::actor(authority.store(), &host, &config, true).await?;
        let prepared = authority
            .prepare_cache_experiment(&"1".repeat(40), NOW)
            .await?;
        let target = prepared.binding();
        let cycle = authority
            .acquire_cache_cycle(target, &target.auth_pool_versionstamp, NOW)
            .await?;
        let prefix = std::array::from_fn(|i| {
            let write = i == 0 || i == 2;
            let usage = Usage {
                input_tokens: 2560,
                cached_tokens: if write { 0 } else { 2560 },
                cache_write_tokens: if write { 2560 } else { 0 },
                output_tokens: 12,
                total_tokens: 2572,
            };
            Sample {
                step: STEPS[i].into(),
                slot: 1,
                raw_usage: usage.clone(),
                usage,
                elapsed_ms: 12,
            }
        });
        let permit = authority
            .cache_refresh_permit(&cycle, target, prefix, &"1".repeat(40), NOW)
            .await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let prep = authority.source_native_renewals(&host, &config, &id, &owner, &cancel);
        let action = prep
            .prepare_source_cache_experiment_renewal(&actor, &permit)
            .await?;
        let service = authority.source_native_lifecycle(&host, &config, &id, &owner, &cancel);
        let response = callback_support::execute(
            &service,
            authority.store(),
            &host,
            &id,
            &callback_support::claim(&id, action.action_ref(), "callback-cache", 194)?,
            94,
        )
        .await?;
        let Response::Claim {
            v: 2,
            fence,
            claim_ref,
            ..
        } = response
        else {
            return Err("cache response2".into());
        };
        assert_eq!(&fence.principal, actor.principal().as_source());
        assert!(matches!(
            fence.context,
            NativeRenewalContext::CacheExperiment { .. }
        ));
        host.admin.set(false);
        let recheck = Request::Recheck {
            v: 1,
            operation_id: "callback-cache".into(),
            claim_ref,
        };
        assert!(
            callback_support::execute(&service, authority.store(), &host, &id, &recheck, 95)
                .await
                .is_err()
        );
        Ok(())
    })
}
