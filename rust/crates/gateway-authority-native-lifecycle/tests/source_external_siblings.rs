mod external_support;
mod support;
use external_support::{Rig, command, credentials, fixture, group, identity, signed, snapshot};
use gateway_auth_contracts::types::AuthConfig;
use gateway_auth_credentials::provider_credentials::{
    CodexNativeOwner, CodexSourceAccount, codex_source_pool_key, parse_codex_source_pool,
};
use gateway_authority_keys as keys;
use gateway_authority_native_records::owner::RegisteredRenewalOwner;
use gateway_authority_routing_records::types::{PoolAccount, SubscriptionPool};
use gateway_core::state::AuthorityStore;
use support::{Cancel, Host, NOW, Store, TestResult, Tokens, run};
#[test]
fn changed_or_reordered_sibling_cannot_be_hidden_by_matching_new_normalization() -> TestResult {
    run(async {
        for reorder in [false, true] {
            let store = Store::default();
            let host = Host::default();
            let config = AuthConfig::default();
            let tokens = Tokens::default();
            let id = identity()?;
            fixture(&store, &id).await?;
            let payload = serde_json::to_vec(
                &serde_json::json!({"exp":NOW/1000+300,"https://api.openai.com/auth":{"chatgpt_user_id":"sibling-user","chatgpt_account_id":"sibling"}}),
            )?;
            let access = format!(
                "e30.{}.fixture",
                gateway_auth_primitives::crypto::base64url(&payload)
            );
            let external = gateway_auth_credentials::native_credentials::parse_native_document(
                &serde_json::json!({"auth_mode":"chatgptAuthTokens","OPENAI_API_KEY":null,"tokens":{"account_id":"sibling","access_token":access,"id_token":access,"refresh_token":""}}),
            )?;
            let row = store
                .read(&codex_source_pool_key())
                .await?
                .ok_or("source")?;
            let mut source = parse_codex_source_pool(row.json_value()?)?.ok_or("pool")?;
            let generation = external.generation_hash();
            source.accounts.push(CodexSourceAccount {
                account_id: external.account_id,
                access_token: external.access_token,
                refresh_token: external.refresh_token,
                updated_at_ms: NOW as f64,
                native_owner: Some(CodexNativeOwner {
                    codex_home: "/tmp/sibling".into(),
                    generation_hash: generation,
                    native_refreshed_at: None,
                    external_user_id: external.external_user_id,
                    managed_source: None,
                }),
            });
            let normalized = SubscriptionPool {
                v: 1,
                pool_versionstamp: gateway_contracts::identity::PoolVersionstamp("source".into()),
                accounts: source
                    .accounts
                    .iter()
                    .map(|a| {
                        let i = a.identity();
                        PoolAccount {
                            account_id_hash: i.routing_account_hash,
                            admin_account_hash: i.administrative_account_hash,
                            credential_version: i.credential_version,
                            enabled: true,
                        }
                    })
                    .collect(),
            };
            support::seed(
                &store,
                codex_source_pool_key(),
                "source",
                source.private_source_value(),
            )?;
            support::seed(&store, keys::pool(), "normalized", normalized.clone())?;
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
            let (action, claim, _) = rig.prepare("siblings").await?;
            let mut normalized = normalized;
            if reorder {
                source.accounts.swap(0, 1);
                normalized.accounts.swap(0, 1);
            } else {
                source.accounts[1].updated_at_ms += 1.0;
            }
            support::seed(
                &store,
                codex_source_pool_key(),
                "source",
                source.private_source_value(),
            )?;
            support::seed(&store, keys::pool(), "normalized", normalized)?;
            let keys = group(&action, "siblings")?;
            let before = snapshot(&store, &keys).await?;
            let body = command(&action, &claim, &credentials(600, "next")?, "publication")?;
            let proof = signed(&store, &id, &body, 201).await?;
            assert!(rig.service().publish_external(&proof, &body).await.is_err());
            assert_eq!(snapshot(&store, &keys).await?, before);
        }
        Ok(())
    })
}
