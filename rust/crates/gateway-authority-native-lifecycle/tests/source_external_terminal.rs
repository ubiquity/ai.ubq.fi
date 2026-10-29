mod external_support;
mod support;
use external_support::{Rig, command, credentials, fixture, group, identity, signed, snapshot};
use gateway_auth_contracts::types::AuthConfig;
use gateway_authority_native_records::{owner::RegisteredRenewalOwner, rows};
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use std::cell::Cell;
use support::{Cancel, Host, NOW, Store, TestResult, Tokens, run};
struct ChangedAtCas<'a> {
    store: &'a Store,
    operation: RecordKey,
    source: RecordKey,
    mode: u8,
    host: &'a Host,
    hits: Cell<usize>,
}
impl AuthorityStore for ChangedAtCas<'_> {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        self.store.read(key).await
    }
    async fn commit(&self, tx: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        if tx
            .mutations
            .iter()
            .any(|m| matches!(m,RecordMutation::Put{key,..} if key==&self.operation))
            && self.hits.replace(self.hits.get() + 1) == 0
        {
            match self.mode {
                0 => {
                    self.store.0.borrow_mut().remove(&self.source.0);
                }
                1 => {
                    let mut rows = self.store.0.borrow_mut();
                    let row = rows
                        .get_mut(&self.source.0)
                        .ok_or_else(support::unavailable)?;
                    row.revision = "source-raced".into();
                }
                2 => self.host.status.set(401),
                3 => self
                    .host
                    .now
                    .0
                    .store(NOW + 300_000, std::sync::atomic::Ordering::SeqCst),
                4 => {
                    self.store
                        .0
                        .borrow_mut()
                        .remove(&gateway_authority_keys::paid_control_config().0);
                }
                _ => {
                    self.store.0.borrow_mut().remove(
                        &gateway_authority_native_records::live::mapping_key("call", false).0,
                    );
                }
            }
            return Ok(CommitOutcome::Conflict);
        }
        self.store.commit(tx).await
    }
}
#[test]
fn final_cas_source_control_auth_and_clock_races_cannot_partially_publish() -> TestResult {
    run(async {
        for mode in 0..6 {
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
            let (action, claim, _) = rig.prepare("race").await?;
            let keys = group(&action, "race")?;
            let before = snapshot(&store, &keys).await?;
            let changed = ChangedAtCas {
                store: &store,
                operation: keys[2].clone(),
                source: keys[0].clone(),
                mode,
                host: &host,
                hits: Cell::new(0),
            };
            let service = gateway_authority_native_external::SourceNativeExternalService::new(
                &changed,
                &tokens,
                Some(&host.now),
                &host,
                &config,
                gateway_authority_native_external::SourceNativeExternalContext::configured(
                    &id, &owner, &cancel,
                ),
            );
            let body = command(&action, &claim, &credentials(600, "next")?, "publication")?;
            let proof = signed(&store, &id, &body, 161).await?;
            assert!(service.publish_external(&proof, &body).await.is_err());
            assert!(changed.hits.get() > 0);
            for index in 1..4 {
                assert_eq!(
                    store.read(&keys[index]).await?.ok_or("retained")?,
                    before[index]
                );
            }
        }
        Ok(())
    })
}
#[test]
fn retained_terminal_tampering_in_every_atomic_row_is_rejected_without_republication() -> TestResult
{
    run(async {
        for index in 0..4 {
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
            let (action, claim, _) = rig.prepare("terminal").await?;
            let body = command(&action, &claim, &credentials(600, "next")?, "publication")?;
            let proof = signed(&store, &id, &body, 162).await?;
            rig.service().publish_external(&proof, &body).await?;
            let keys = group(&action, "terminal")?;
            {
                let mut all = store.0.borrow_mut();
                let row = all.get_mut(&keys[index].0).ok_or("row")?;
                match index {
                    0 => {
                        row.value["accounts"][0]["native_owner"]["generation_hash"] =
                            serde_json::json!("c".repeat(64))
                    }
                    1 => row.value["pool_versionstamp"] = serde_json::json!("foreign"),
                    _ => {
                        row.value["terminal"]["publication_request_digest"] =
                            serde_json::json!("foreign")
                    }
                }
            }
            let before = snapshot(&store, &keys).await?;
            let proof = signed(&store, &id, &body, 163).await?;
            assert!(rig.service().publish_external(&proof, &body).await.is_err());
            assert_eq!(snapshot(&store, &keys).await?, before);
        }
        Ok(())
    })
}
struct Collision;
impl gateway_authority_contracts::TokenSource for Collision {
    fn next_token(&self) -> Result<String, GatewayError> {
        Ok("publication".into())
    }
}
#[test]
fn publication_token_collision_and_legacy_receipt_do_not_create_a_terminal() -> TestResult {
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
        let (action, claim, _) = rig.prepare("collision").await?;
        let keys = group(&action, "collision")?;
        let before = snapshot(&store, &keys).await?;
        let body = command(&action, &claim, &credentials(600, "next")?, "publication")?;
        let proof = signed(&store, &id, &body, 164).await?;
        let collision = Collision;
        let service = gateway_authority_native_external::SourceNativeExternalService::new(
            &store,
            &collision,
            Some(&host.now),
            &host,
            &config,
            gateway_authority_native_external::SourceNativeExternalContext::configured(
                &id, &owner, &cancel,
            ),
        );
        assert!(service.publish_external(&proof, &body).await.is_err());
        assert_eq!(snapshot(&store, &keys).await?, before);
        let raw = store
            .read(&gateway_auth_credentials::provider_credentials::codex_source_pool_key())
            .await?
            .ok_or("source")?;
        let source = gateway_auth_credentials::provider_credentials::parse_codex_source_pool(
            raw.json_value()?,
        )?
        .ok_or("pool")?;
        let old = &source.accounts[0];
        let native = old.native_owner.as_ref().ok_or("owner")?;
        let observed = gateway_auth_credentials::native_credentials::NativeCredentials {
            account_id: old.account_id.clone(),
            access_token: old.access_token.clone(),
            refresh_token: old.refresh_token.clone(),
            email: None,
            native_refreshed_at: native.native_refreshed_at.clone(),
            external_user_id: native.external_user_id.clone(),
        };
        let command =
            gateway_publication::wire::PreparedPublicationCommand::access_from_registered(
                gateway_publication::wire::ExpectedAccessPublication {
                    native_owner_id: "native".into(),
                    owner_revision: "owner".into(),
                    source_revision: "source".into(),
                    normalized_revision: Some("normalized".into()),
                    account_id_hash: old.identity().routing_account_hash.0,
                    user_id: "native-user".into(),
                    publication_revision: "legacy-ack".into(),
                },
                &observed,
            )?;
        let carrier = LegacyActual {
            store: &store,
            identity: &id,
        };
        let receipt =
            gateway_publication::sender::NativePublicationSender::configured(&id, &carrier)
                .send(&command, [171; 32], NOW)
                .await;
        let receipt = receipt?;
        assert_eq!(receipt.publication_revision, "legacy-ack");
        assert_eq!(receipt.source_revision.as_deref(), Some("source"));
        assert!(
            rig.lifecycle()
                .published(&claim, "legacy-ack")
                .await
                .is_err()
        );
        assert_eq!(snapshot(&store, &keys).await?, before);
        Ok(())
    })
}
struct LegacyActual<'a> {
    store: &'a Store,
    identity: &'a gateway_auth_native_proof::native_auth::NativeTransportIdentity,
}
impl gateway_publication_policy::protocol::PublicationCarrier for LegacyActual<'_> {
    async fn send(
        &self,
        request: gateway_publication_policy::protocol::PublicationRequest,
    ) -> Result<gateway_publication_policy::protocol::PublicationResponse, GatewayError> {
        let pairs = request.headers.pairs();
        let replay =
            gateway_auth_native_protocol::native_replay::StoreNonceAuthority::new(self.store);
        let proof = gateway_auth_native_proof::native_auth::NativeAuthenticator::new(
            self.identity,
            &replay,
        )
        .authenticate(
            gateway_auth_native_wire::native_read::NATIVE_PUBLICATION_PATH,
            gateway_auth_native_contracts::transport::NativeAuthHeaders {
                version: &pairs[0].1,
                key_id: &pairs[1].1,
                time_ms: &pairs[2].1,
                nonce: &pairs[3].1,
                mac: &pairs[4].1,
            },
            &request.raw_body,
            NOW,
        )
        .await?;
        let inputs = gateway_publication_policy::control_publication::ConfiguredPaidInputs::new(
            None,
            None,
            None,
            serde_json::json!({}),
        )?;
        let registered =
            gateway_publication::access_publication::RegisteredAccessOwner::configured(
                gateway_auth_credentials::native_credentials::NativeOwnerId("native".into()),
                "/tmp/native-source".into(),
            )?;
        let receipt = gateway_publication::apply::apply_publication(
            self.store,
            &proof,
            &request.raw_body,
            self.identity,
            &inputs,
            &[registered],
            || NOW,
        )
        .await?;
        let body = serde_json::to_vec(&receipt).map_err(|_| support::unavailable())?;
        let mac =
            self.identity
                .sign_publication_response(&pairs[3].1, &request.raw_body, 200, &body)?;
        Ok(gateway_publication_policy::protocol::PublicationResponse {
            status: 200,
            headers: vec![(
                gateway_auth_native_wire::native_read::NATIVE_RESPONSE_MAC_HEADER.into(),
                mac,
            )],
            raw_body: body,
        })
    }
}
