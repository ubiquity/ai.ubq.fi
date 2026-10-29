mod external_support;
mod support;
use external_support::{Rig, command, credentials, fixture, group, identity, signed, snapshot};
use gateway_auth_contracts::types::AuthConfig;
use gateway_authority_native_records::owner::RegisteredRenewalOwner;
use gateway_core::state::AuthorityStore;
use support::{Cancel, Host, Store, TestResult, Tokens, run};
#[test]
fn equal_expiry_new_credentials_require_strictly_advanced_native_refresh_time() -> TestResult {
    run(async {
        for advance in [false, true] {
            let store = Store::default();
            let host = Host::default();
            let config = AuthConfig::default();
            let tokens = Tokens::default();
            let id = identity()?;
            fixture(&store, &id).await?;
            let source = gateway_auth_credentials::provider_credentials::codex_source_pool_key();
            store
                .0
                .borrow_mut()
                .get_mut(&source.0)
                .ok_or("source")?
                .value["accounts"][0]["native_owner"]["native_refreshed_at"] =
                serde_json::json!("2026-10-27T00:00:00Z");
            let owner =
                RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
            let cancel = Cancel(false);
            let rig = Rig {
                store: &store,
                host: &host,
                config: &config,
                tokens: &tokens,
                identity: &id,
                owner: &owner,
                cancel: &cancel,
            };
            let (action, claim, _) = rig.prepare("timestamp").await?;
            let keys = group(&action, "timestamp")?;
            let before = snapshot(&store, &keys).await?;
            let mut next = credentials(300, "changed-credential")?;
            next.native_refreshed_at = Some(
                if advance {
                    "2026-10-27T00:01:00Z"
                } else {
                    "2026-10-26T23:59:00Z"
                }
                .into(),
            );
            let body = command(&action, &claim, &next, "publication")?;
            let proof = signed(&store, &id, &body, 231).await?;
            let result = rig.service().publish_external(&proof, &body).await;
            if advance {
                assert_eq!(result?.current_source_revision, "publication");
            } else {
                assert!(result.is_err());
                assert_eq!(snapshot(&store, &keys).await?, before);
            }
        }
        Ok(())
    })
}
#[test]
fn same_credentials_observation_keeps_legacy_timestamp_and_actual_revision_semantics() -> TestResult
{
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let rig = Rig {
            store: &store,
            host: &host,
            config: &config,
            tokens: &tokens,
            identity: &id,
            owner: &owner,
            cancel: &cancel,
        };
        let (action, claim, _) = rig.prepare("unchanged-time").await?;
        let key = gateway_auth_credentials::provider_credentials::codex_source_pool_key();
        let before = store.read(&key).await?.ok_or("source")?;
        let mut next = credentials(300, "original")?;
        next.native_refreshed_at = Some("2026-10-27T00:01:00Z".into());
        let body = command(&action, &claim, &next, "ack-new")?;
        let proof = signed(&store, &id, &body, 232).await?;
        let receipt = rig.service().publish_external(&proof, &body).await?;
        assert_eq!(receipt.current_source_revision, "source");
        assert_eq!(receipt.publication_revision, "ack-new");
        assert_eq!(store.read(&key).await?.ok_or("same source")?, before);
        Ok(())
    })
}
