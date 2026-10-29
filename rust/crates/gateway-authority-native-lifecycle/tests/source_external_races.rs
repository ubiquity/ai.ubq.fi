mod external_support;
mod support;
use external_support::{Rig, command, credentials, fixture, group, identity, signed, snapshot};
use gateway_auth_contracts::types::AuthConfig;
use gateway_auth_passport_sessions::sessions::session_key;
use gateway_authority_native_records::owner::RegisteredRenewalOwner;
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use std::cell::Cell;
use support::{Cancel, Host, NOW, Store, TestResult, Tokens, run};
struct FinalStore<'a> {
    inner: &'a Store,
    operation: RecordKey,
    logout: Option<RecordKey>,
    fail: bool,
    hit: Cell<usize>,
}
impl AuthorityStore for FinalStore<'_> {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        self.inner.read(key).await
    }
    async fn commit(&self, tx: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        if tx
            .mutations
            .iter()
            .any(|m| matches!(m,RecordMutation::Put{key,..} if key==&self.operation))
        {
            self.hit.set(self.hit.get() + 1);
            assert_eq!(tx.mutations.len(), 4);
            if let Some(key) = &self.logout {
                self.inner.0.borrow_mut().remove(&key.0);
                return Ok(CommitOutcome::Conflict);
            }
            if self.fail {
                return Err(support::unavailable());
            }
        }
        self.inner.commit(tx).await
    }
}
#[test]
fn actual_final_cas_logout_and_write_failure_leave_all_four_rows_unchanged() -> TestResult {
    run(async {
        for fail in [false, true] {
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
            let (action, claim, token) = rig.prepare("atomic").await?;
            let keys = group(&action, "atomic")?;
            let before = snapshot(&store, &keys).await?;
            let final_store = FinalStore {
                inner: &store,
                operation: keys[2].clone(),
                logout: (!fail).then(|| session_key(&token)),
                fail,
                hit: Cell::new(0),
            };
            let service = gateway_authority_native_external::SourceNativeExternalService::new(
                &final_store,
                &tokens,
                Some(&host.now),
                &host,
                &config,
                gateway_authority_native_external::SourceNativeExternalContext::configured(
                    &id, &owner, &cancel,
                ),
            );
            let body = command(&action, &claim, &credentials(600, "next")?, "publication")?;
            let proof = signed(&store, &id, &body, 71).await?;
            assert!(service.publish_external(&proof, &body).await.is_err());
            assert_eq!(final_store.hit.get(), 1);
            assert_eq!(snapshot(&store, &keys).await?, before);
        }
        Ok(())
    })
}
#[test]
fn wrong_fixed_owner_and_request_binding_do_not_publish() -> TestResult {
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
        let (action, claim, _) = rig.prepare("foreign").await?;
        let body = command(&action, &claim, &credentials(600, "next")?, "publication")?;
        let proof = signed(&store, &id, &body, 72).await?;
        let keys = group(&action, "foreign")?;
        let before = snapshot(&store, &keys).await?;
        let wrong = RegisteredRenewalOwner::configured("other".into(), "/tmp/other".into())?;
        let service = gateway_authority_native_external::SourceNativeExternalService::new(
            &store,
            &tokens,
            Some(&host.now),
            &host,
            &config,
            gateway_authority_native_external::SourceNativeExternalContext::configured(
                &id, &wrong, &cancel,
            ),
        );
        assert!(service.publish_external(&proof, &body).await.is_err());
        let mut changed = body.clone();
        changed.push(b' ');
        assert!(
            rig.service()
                .publish_external(&proof, &changed)
                .await
                .is_err()
        );
        assert_eq!(snapshot(&store, &keys).await?, before);
        Ok(())
    })
}
#[test]
fn malformed_raw_normalized_codec_version_expiry_and_generation_refuse_at_original_boundary()
-> TestResult {
    run(async {
        for corruption in 0..6 {
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
            let (action, claim, _) = rig.prepare("corrupt").await?;
            let keys = group(&action, "corrupt")?;
            {
                let mut rows = store.0.borrow_mut();
                let row = rows
                    .get_mut(&keys[if corruption == 3 { 1 } else { 0 }].0)
                    .ok_or("row")?;
                match corruption {
                    0 => row.schema_version = 77,
                    1 => row.value_encoding = gateway_kv_codec::value::ValueEncoding::ProfileV2,
                    2 => row.expires_at_ms = Some(NOW - 1),
                    3 => row.value["pool_versionstamp"] = serde_json::json!("foreign"),
                    4 => {
                        row.value["accounts"][0]["native_owner"]["generation_hash"] =
                            serde_json::json!("b".repeat(64))
                    }
                    _ => row.value["accounts"][0]["refresh_token"] = serde_json::json!("forbidden"),
                }
            }
            let before = store.0.borrow().clone();
            let body = command(&action, &claim, &credentials(600, "next")?, "publication")?;
            let proof = signed(&store, &id, &body, 73).await?;
            assert!(rig.service().publish_external(&proof, &body).await.is_err());
            for key in &keys {
                assert_eq!(store.0.borrow().get(&key.0), before.get(&key.0));
            }
        }
        Ok(())
    })
}
#[test]
fn unusable_or_regressed_access_cancellation_and_clock_loss_never_mint_terminal() -> TestResult {
    run(async {
        for mode in 0..5 {
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
            let (action, claim, _) = rig.prepare("unusable").await?;
            let keys = group(&action, "unusable")?;
            let before = snapshot(&store, &keys).await?;
            let body = command(
                &action,
                &claim,
                &credentials(
                    if mode == 0 {
                        60
                    } else if mode == 1 {
                        250
                    } else {
                        600
                    },
                    "next",
                )?,
                "publication",
            )?;
            let proof = signed(&store, &id, &body, 74).await?;
            let stopped = Cancel(mode == 2);
            let service = gateway_authority_native_external::SourceNativeExternalService::new(
                &store,
                &tokens,
                if mode == 3 { None } else { Some(&host.now) },
                &host,
                &config,
                gateway_authority_native_external::SourceNativeExternalContext::configured(
                    &id, &owner, &stopped,
                ),
            );
            if mode == 4 {
                host.now
                    .0
                    .store(NOW - 1, std::sync::atomic::Ordering::SeqCst);
            }
            assert!(service.publish_external(&proof, &body).await.is_err());
            assert_eq!(snapshot(&store, &keys).await?, before);
        }
        Ok(())
    })
}
