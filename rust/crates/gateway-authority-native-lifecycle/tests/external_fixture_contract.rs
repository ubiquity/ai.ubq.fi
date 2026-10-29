//! Synthetic credential DATA exercises the real external-owner grammar, never provider authority.
mod support;
use gateway_auth_contracts::types::AuthConfig;
use gateway_auth_credentials::{
    native_credentials::{NativeCredentials, parse_native_document},
    provider_credentials::{
        CodexNativeOwner, CodexSourceAccount, CodexSourcePool, codex_source_pool_key,
        parse_codex_source_pool,
    },
};
use gateway_auth_primitives::crypto::{Secret, base64url};
use gateway_authority::store::Authority;
use gateway_authority_keys as keys;
use gateway_authority_native_preparation::{
    SourceNativePreparationContext, SourceNativeRenewalService,
};
use gateway_authority_native_records::{owner::RegisteredRenewalOwner, rows};
use gateway_authority_routing_records::types::{PoolAccount, SubscriptionPool};
use gateway_authority_routing_selection::SelectionOperations;
use gateway_contracts::provider::CodexQuotaClass;
use gateway_core::state::AuthorityStore;
use gateway_routing_runtime_test_support::{
    input::NOW as RUNTIME_NOW, seed::seed_pool, store::Store as RuntimeStore,
};
use serde_json::{Value, json};
use support::{Cancel, Host, NOW, Store, TestResult, Tokens, actor, fixture, identity, run, seed};

fn document(account: &str, access: &str, refresh: &str) -> Value {
    json!({
        "auth_mode":"chatgptAuthTokens", "OPENAI_API_KEY":null,
        "tokens":{"account_id":account,"access_token":access,
            "id_token":access,"refresh_token":refresh}
    })
}
fn checked_external(account: &str) -> TestResult<NativeCredentials> {
    let payload = json!({"https://api.openai.com/auth":{
        "chatgpt_user_id":"native-user","chatgpt_account_id":account
    }});
    let access = format!("e30.{}.fixture", base64url(&serde_json::to_vec(&payload)?));
    Ok(parse_native_document(&document(account, &access, ""))?)
}
fn external_owner(account: &CodexSourceAccount) -> CodexNativeOwner {
    CodexNativeOwner {
        codex_home: "/tmp/native-source".into(),
        generation_hash: account.identity().native_generation_hash,
        native_refreshed_at: None,
        external_user_id: Some("native-user".into()),
        managed_source: None,
    }
}
fn assert_checked_binding(account: &CodexSourceAccount, normalized: &PoolAccount) -> TestResult {
    let checked = parse_native_document(&document(
        &account.account_id,
        account.access_token.expose(),
        account.refresh_token.expose(),
    ))?;
    let owner = account.native_owner.as_ref().ok_or("native owner")?;
    assert_eq!(checked.external_user_id.as_deref(), Some("native-user"));
    assert_eq!(owner.external_user_id, checked.external_user_id);
    assert_eq!(owner.codex_home, "/tmp/native-source");
    assert!(owner.managed_source.is_none());
    assert!(owner.native_refreshed_at.is_none());
    assert!(checked.access_expires_at_ms().is_none());
    assert!(checked.native_refreshed_at.is_none());
    assert!(checked.refresh_token.expose().is_empty());
    assert_eq!(owner.generation_hash, checked.generation_hash());
    assert_eq!(normalized.account_id_hash.0, checked.routing_account_hash());
    assert_eq!(
        normalized.admin_account_hash.0,
        checked.administrative_account_hash()
    );
    assert_eq!(
        normalized.credential_version.0,
        checked.routing_credential_version()
    );
    Ok(())
}

#[test]
fn original_shared_external_tuple_is_rejected_before_native_action_effects() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        // Preserve the original malformed tuple independently of the repaired positive fixture.
        let mut account = CodexSourceAccount {
            account_id: "account".into(),
            access_token: Secret::new("SYNTHETIC_ACCESS".into()),
            refresh_token: Secret::new("SYNTHETIC_REFRESH".into()),
            updated_at_ms: (NOW - 420_001) as f64,
            native_owner: None,
        };
        account.native_owner = Some(external_owner(&account));
        let raw = CodexSourcePool {
            accounts: vec![account],
            updated_at_ms: NOW as f64,
        }
        .private_source_value();
        assert!(parse_codex_source_pool(&raw)?.is_none());
        assert!(
            parse_native_document(&document(
                "account",
                "SYNTHETIC_ACCESS",
                "SYNTHETIC_REFRESH"
            ))
            .is_err()
        );
        seed(&store, codex_source_pool_key(), "source", raw)?;
        let (actor, _) = actor(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let service = SourceNativeRenewalService::new(
            &store,
            &tokens,
            Some(&host.now),
            &host,
            &config,
            SourceNativePreparationContext::configured(&id, &owner, &cancel),
        );
        let error = service
            .prepare_source_live_mapped_renewal(&actor, "call")
            .await
            .err()
            .ok_or("malformed external tuple accepted")?;
        assert_eq!(error, rows::fail());
        assert!(
            !store
                .0
                .borrow()
                .keys()
                .any(|key| key.starts_with(&rows::prefix().0))
        );
        Ok(())
    })
}

#[test]
fn original_inference_external_tuple_is_rejected_by_checked_selection() -> TestResult {
    run(async {
        let store = RuntimeStore::new();
        seed_pool(&store);
        let mut source =
            parse_codex_source_pool(&store.value(codex_source_pool_key()))?.ok_or("source pool")?;
        // This is the original inference setup: ordinary OAuth DATA plus an external tag.
        assert_eq!(source.accounts[0].account_id, "a");
        assert_eq!(source.accounts[0].access_token.expose(), "fixture-access-a");
        assert_eq!(
            source.accounts[0].refresh_token.expose(),
            "fixture-refresh-a"
        );
        source.accounts[0].native_owner = Some(external_owner(&source.accounts[0]));
        let raw = source.private_source_value();
        assert!(parse_codex_source_pool(&raw)?.is_none());
        store.seed(codex_source_pool_key(), raw);
        let revision = store
            .read(&codex_source_pool_key())
            .await?
            .ok_or("source")?
            .revision;
        let mut normalized: SubscriptionPool = serde_json::from_value(store.value(keys::pool()))?;
        normalized.pool_versionstamp.0 = revision;
        store.seed(keys::pool(), normalized);
        let root = Authority::with_clock(store.clone(), store.clone(), store.clone());
        let error = root
            .select_account(&"gpt-test".into(), CodexQuotaClass::Standard, RUNTIME_NOW)
            .await
            .err()
            .ok_or("malformed inference tuple accepted")?;
        assert_eq!(
            error.envelope.error.code.as_deref(),
            Some("authority_state_invalid")
        );
        assert_eq!(
            error.envelope.error.message,
            "Current raw Codex source pool is malformed."
        );
        assert!(store.value(keys::routing()).is_null());
        Ok(())
    })
}

#[test]
fn corrected_external_tuples_roundtrip_defining_native_and_source_apis() -> TestResult {
    run(async {
        let shared = Store::default();
        fixture(&shared, &identity()?, None)?;
        let row = shared
            .read(&codex_source_pool_key())
            .await?
            .ok_or("shared source")?;
        assert!(row.expires_at_ms.is_none());
        let source = parse_codex_source_pool(row.json_value()?)?.ok_or("shared pool")?;
        assert_eq!(source.accounts[0].account_id, "account");
        assert_eq!(source.accounts[0].updated_at_ms, (NOW - 420_001) as f64);
        assert_eq!(source.updated_at_ms, NOW as f64);
        assert_eq!(
            parse_codex_source_pool(&source.private_source_value())?,
            Some(source.clone())
        );
        let normalized: SubscriptionPool = serde_json::from_value(
            shared
                .read(&keys::pool())
                .await?
                .ok_or("shared normalized")?
                .json_value()?
                .clone(),
        )?;
        assert_eq!(normalized.v, 1);
        assert_eq!(normalized.pool_versionstamp.0, row.revision);
        assert!(normalized.accounts[0].enabled);
        assert_checked_binding(&source.accounts[0], &normalized.accounts[0])?;

        let store = RuntimeStore::new();
        seed_pool(&store);
        let mut source = parse_codex_source_pool(&store.value(codex_source_pool_key()))?
            .ok_or("inference source")?;
        let sibling = source.accounts[1].clone();
        let mut normalized: SubscriptionPool = serde_json::from_value(store.value(keys::pool()))?;
        let normalized_sibling = normalized.accounts[1].clone();
        let checked = checked_external(&source.accounts[0].account_id)?;
        let generation = checked.generation_hash();
        let account = &mut source.accounts[0];
        account.access_token = checked.access_token;
        account.refresh_token = checked.refresh_token;
        account.native_owner = Some(CodexNativeOwner {
            codex_home: "/tmp/native-source".into(),
            generation_hash: generation,
            native_refreshed_at: checked.native_refreshed_at,
            external_user_id: checked.external_user_id,
            managed_source: None,
        });
        let identity = account.identity();
        normalized.accounts[0].account_id_hash = identity.routing_account_hash;
        normalized.accounts[0].admin_account_hash = identity.administrative_account_hash;
        normalized.accounts[0].credential_version = identity.credential_version;
        assert_eq!(
            parse_codex_source_pool(&source.private_source_value())?,
            Some(source.clone())
        );
        assert_eq!(source.accounts[0].account_id, "a");
        assert_eq!(source.accounts[0].updated_at_ms, RUNTIME_NOW as f64);
        assert_eq!(source.accounts[1], sibling);
        assert_eq!(normalized.accounts[1], normalized_sibling);
        assert_eq!(normalized.v, 1);
        assert!(normalized.accounts.iter().all(|account| account.enabled));
        assert_checked_binding(&source.accounts[0], &normalized.accounts[0])?;
        Ok(())
    })
}
