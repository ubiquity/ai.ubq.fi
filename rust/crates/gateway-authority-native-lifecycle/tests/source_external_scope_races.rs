mod external_support;
mod support;
use external_support::{Rig, command, credentials, fixture, group, identity, signed, snapshot};
use gateway_auth_contracts::types::AuthConfig;
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_authority_native_records::owner::RegisteredRenewalOwner;
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use std::{
    cell::Cell,
    future::Future,
    task::{Context, Waker},
};
use support::{Cancel, Host, NOW, Store, TestResult, Tokens, run};
struct Abort(Cell<bool>);
impl NativeRenewalCancellation for Abort {
    fn cancelled(&self) -> bool {
        self.0.get()
    }
}
struct CasCancel<'a> {
    store: &'a Store,
    key: RecordKey,
    cancel: &'a Abort,
    hits: Cell<usize>,
}
impl AuthorityStore for CasCancel<'_> {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        self.store.read(key).await
    }
    async fn commit(&self, tx: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        if tx
            .mutations
            .iter()
            .any(|m| matches!(m,RecordMutation::Put{key,..} if key==&self.key))
        {
            self.hits.set(self.hits.get() + 1);
            self.cancel.0.set(true);
            return Ok(CommitOutcome::Conflict);
        }
        self.store.commit(tx).await
    }
}
#[test]
fn cancellation_event_at_real_final_cas_is499_and_commits_no_terminal() -> TestResult {
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
        let (action, claim, _) = rig.prepare("cancel-cas").await?;
        let keys = group(&action, "cancel-cas")?;
        let before = snapshot(&store, &keys).await?;
        let aborted = Abort(Cell::new(false));
        let cas = CasCancel {
            store: &store,
            key: keys[2].clone(),
            cancel: &aborted,
            hits: Cell::new(0),
        };
        let service = gateway_authority_native_external::SourceNativeExternalService::new(
            &cas,
            &tokens,
            Some(&host.now),
            &host,
            &config,
            gateway_authority_native_external::SourceNativeExternalContext::configured(
                &id, &owner, &aborted,
            ),
        );
        let body = command(&action, &claim, &credentials(600, "next")?, "publication")?;
        let proof = signed(&store, &id, &body, 211).await?;
        let error = service
            .publish_external(&proof, &body)
            .await
            .err()
            .ok_or("cancel error")?;
        assert_eq!(error.status, 499);
        assert_eq!(cas.hits.get(), 1);
        assert_eq!(snapshot(&store, &keys).await?, before);
        Ok(())
    })
}
#[test]
fn actual_passport_await_cannot_hide_config_control_or_clock_changes() -> TestResult {
    run(async {
        for mode in 0..4 {
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
            let (action, claim, _) = rig.prepare("await-race").await?;
            let keys = group(&action, "await-race")?;
            let before = snapshot(&store, &keys).await?;
            let body = command(&action, &claim, &credentials(600, "next")?, "publication")?;
            let proof = signed(&store, &id, &body, 212).await?;
            host.hold.set(true);
            let service = rig.service();
            let mut effect = Box::pin(service.publish_external(&proof, &body));
            let mut cx = Context::from_waker(Waker::noop());
            assert!(effect.as_mut().poll(&mut cx).is_pending());
            match mode {
                0 => {
                    store
                        .0
                        .borrow_mut()
                        .remove(&gateway_schema::catalog_store::runtime_config_key().0);
                }
                1 => {
                    store
                        .0
                        .borrow_mut()
                        .remove(&gateway_authority_keys::paid_control_config().0);
                }
                2 => host.status.set(401),
                _ => host
                    .now
                    .0
                    .store(NOW + 300_000, std::sync::atomic::Ordering::SeqCst),
            }
            host.hold.set(false);
            assert!(effect.await.is_err());
            assert_eq!(snapshot(&store, &keys).await?, before);
        }
        Ok(())
    })
}
#[test]
fn fresh_claim_all_wire_fence_mismatches_refuse_before_any_atomic_publication() -> TestResult {
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
        let (action, claim, _) = rig.prepare("fence-fields").await?;
        let body = command(&action, &claim, &credentials(600, "next")?, "publication")?;
        let keys = group(&action, "fence-fields")?;
        let before = snapshot(&store, &keys).await?;
        for (index, field) in [
            "action_ref",
            "operation_id",
            "claim_ref",
            "native_owner_id",
            "owner_revision",
            "source_revision",
            "normalized_revision",
            "previous_native_source_generation",
        ]
        .into_iter()
        .enumerate()
        {
            let mut changed: serde_json::Value = serde_json::from_slice(&body)?;
            changed[field] = serde_json::json!(if field == "previous_native_source_generation" {
                "d".repeat(64)
            } else {
                "foreign".into()
            });
            let changed = serde_json::to_vec(&changed)?;
            let proof = signed(&store, &id, &changed, 220 + u8::try_from(index)?).await?;
            assert!(
                rig.service()
                    .publish_external(&proof, &changed)
                    .await
                    .is_err(),
                "{field}"
            );
        }
        assert_eq!(snapshot(&store, &keys).await?, before);
        Ok(())
    })
}
