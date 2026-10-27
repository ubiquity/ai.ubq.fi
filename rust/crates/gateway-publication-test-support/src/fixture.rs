use gateway_auth::crypto::{Secret, base64url};
use gateway_auth::native::NATIVE_RPC_PATH;
use gateway_auth::native_auth::{
    AuthenticatedNativeOwner, NativeAuthHeaders, NativeAuthenticator, NativeCapability,
    NativeNonceAuthority, NativeTransportIdentity,
};
use gateway_auth::native_credentials::{NativeCredentials, NativeOwnerId};
use gateway_auth::provider_credentials::{
    CodexNativeOwner, CodexSourceAccount, CodexSourcePool, codex_source_pool_key,
};
use gateway_authority::keys;
use gateway_authority_routing_records::types::{PoolAccount, SubscriptionPool};
use gateway_core::error::GatewayError;
use gateway_core::identity::PoolVersionstamp;
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, StoredRecord,
};
use gateway_publication_policy::control_publication::{
    ConfiguredPaidInputs, bootstrap_fixture_owner, fixture_owner_statement,
};
use gateway_publication_policy::protocol::{
    PublicationCarrier, PublicationRequest, PublicationResponse,
};
use gateway_publication_transport::transport::PublicationReceiver;
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};
use std::future::Future;
use std::sync::{Mutex, MutexGuard};
use std::task::{Context, Poll, Waker};

pub struct AccessReadFixture(pub NativeCredentials);
impl gateway_publication::access_publication::AccessCredentialSource for AccessReadFixture {
    async fn read_current_access(
        &self,
        owner: &NativeOwnerId,
    ) -> Result<NativeCredentials, GatewayError> {
        assert_eq!(owner.0, "registered-owner");
        Ok(self.0.clone())
    }
}

pub fn fixture_access(expiry: i64, user: &str) -> NativeCredentials {
    let token = format!(
        "e30.{}.fixture",
        base64url(
            json!({"exp":expiry,"https://api.openai.com/auth":{"chatgpt_user_id":user,"chatgpt_account_id":"workspace"}})
                .to_string()
                .as_bytes()
        )
    );
    NativeCredentials {
        account_id: "workspace".into(),
        access_token: Secret::new(token),
        refresh_token: Secret::new(String::new()),
        email: None,
        native_refreshed_at: Some(
            if expiry == 1_792_341_000 {
                "2026-10-18T17:00:00Z"
            } else {
                "2026-10-18T17:01:00Z"
            }
            .into(),
        ),
        external_user_id: Some(user.into()),
    }
}

pub fn fixture_seed_access(store: &FixtureMemory, old: &NativeCredentials) {
    let account = CodexSourceAccount {
        account_id: old.account_id.clone(),
        access_token: old.access_token.clone(),
        refresh_token: old.refresh_token.clone(),
        updated_at_ms: 1.0,
        native_owner: Some(CodexNativeOwner {
            managed_source: None,
            codex_home: "/tmp/uos-native-fixture".into(),
            generation_hash: old.generation_hash(),
            native_refreshed_at: old.native_refreshed_at.clone(),
            external_user_id: old.external_user_id.clone(),
        }),
    };
    let other = CodexSourceAccount {
        account_id: "other-workspace".into(),
        access_token: Secret::new("fixture-other-access".into()),
        refresh_token: Secret::new("fixture-other-refresh".into()),
        updated_at_ms: 1.0,
        native_owner: None,
    };
    let raw = CodexSourcePool {
        accounts: vec![account, other],
        updated_at_ms: 1.0,
    };
    let accounts = raw
        .accounts
        .iter()
        .map(|a| {
            let id = a.identity();
            PoolAccount {
                account_id_hash: id.routing_account_hash,
                admin_account_hash: id.administrative_account_hash,
                credential_version: id.credential_version,
                enabled: true,
            }
        })
        .collect();
    let normalized = SubscriptionPool {
        v: 1,
        pool_versionstamp: PoolVersionstamp("raw-r1".into()),
        accounts,
    };
    fixture_set(
        store,
        codex_source_pool_key(),
        "raw-r1",
        raw.private_source_value(),
    );
    let normalized = match serde_json::to_value(normalized) {
        Ok(value) => value,
        Err(error) => panic!("fixture access pool encoding failed: {error}"),
    };
    fixture_set(store, keys::pool(), "normalized-r1", normalized);
}

pub fn fixture_run<F: Future>(future: F) -> F::Output {
    let mut context = Context::from_waker(Waker::noop());
    let mut future = std::pin::pin!(future);
    loop {
        if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
            return value;
        }
        std::thread::yield_now();
    }
}

#[derive(Default)]
pub struct FixtureMemory(pub Mutex<BTreeMap<String, StoredRecord>>);
fn fixture_rows(
    memory: &Mutex<BTreeMap<String, StoredRecord>>,
) -> MutexGuard<'_, BTreeMap<String, StoredRecord>> {
    match memory.lock() {
        Ok(rows) => rows,
        Err(error) => panic!("fixture memory lock poisoned: {error}"),
    }
}
impl AuthorityStore for FixtureMemory {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Ok(fixture_rows(&self.0).get(&key.0).cloned())
    }
    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        let mut rows = fixture_rows(&self.0);
        if transition.checks.iter().any(|check| {
            rows.get(&check.key.0).map(|r| &r.revision) != check.expected_revision.as_ref()
        }) {
            return Ok(CommitOutcome::Conflict);
        }
        for mutation in transition.mutations {
            match mutation {
                RecordMutation::Put { key, record } => {
                    rows.insert(key.0, record);
                }
                RecordMutation::Delete { key } => {
                    rows.remove(&key.0);
                }
            }
        }
        Ok(CommitOutcome::Committed)
    }
}

pub struct FixtureNonces;
impl NativeNonceAuthority for FixtureNonces {
    async fn consume_nonce(&self, _: &str, _: &str, _: i64, _: i64) -> Result<(), GatewayError> {
        Ok(())
    }
}

pub fn fixture_identity(capability: NativeCapability) -> NativeTransportIdentity {
    match NativeTransportIdentity::new(
        "fixture-key".into(),
        "fixture-owner".into(),
        "fixture-epoch".into(),
        [11; 32],
        BTreeSet::from([capability]),
    ) {
        Ok(identity) => identity,
        Err(error) => panic!("fixture identity construction failed: {error}"),
    }
}

pub async fn fixture_proof(
    identity: &NativeTransportIdentity,
    body: &[u8],
    now: i64,
) -> AuthenticatedNativeOwner {
    let nonce = base64url(&[7; 32]);
    let mac = match identity.sign(NATIVE_RPC_PATH, now, &nonce, body) {
        Ok(mac) => mac,
        Err(error) => panic!("fixture proof signing failed: {error}"),
    };
    let time = now.to_string();
    match NativeAuthenticator::new(identity, &FixtureNonces)
        .authenticate(
            NATIVE_RPC_PATH,
            NativeAuthHeaders {
                version: "1",
                key_id: identity.key_id(),
                time_ms: &time,
                nonce: &nonce,
                mac: &mac,
            },
            body,
            now,
        )
        .await
    {
        Ok(proof) => proof,
        Err(error) => panic!("fixture proof authentication failed: {error}"),
    }
}

pub fn fixture_inputs(secret: &str) -> ConfiguredPaidInputs {
    match ConfiguredPaidInputs::new(
        Some(Secret::new(secret.into())),
        Some(Secret::new("fixture-openlux-key".into())),
        Some(Secret::new("fixture-openlux-key".into())),
        json!({"overage_enabled":true}),
    ) {
        Ok(inputs) => inputs,
        Err(error) => panic!("fixture paid inputs construction failed: {error}"),
    }
}

pub async fn fixture_bootstrap(
    store: &FixtureMemory,
    identity: &NativeTransportIdentity,
    now: i64,
) {
    let body = match fixture_owner_statement(identity) {
        Ok(body) => body,
        Err(error) => panic!("fixture owner statement failed: {error}"),
    };
    let proof = fixture_proof(identity, &body, now).await;
    if let Err(error) = bootstrap_fixture_owner(store, identity, &proof, now, "owner-r1").await {
        panic!("fixture owner bootstrap failed: {error}");
    }
}

pub fn fixture_set(store: &FixtureMemory, key: RecordKey, revision: &str, value: Value) {
    fixture_rows(&store.0).insert(key.0, StoredRecord::json(revision, value, None));
}

pub fn fixture_now() -> i64 {
    1_792_339_000_000
}

pub fn fixture_publication_identity() -> NativeTransportIdentity {
    match NativeTransportIdentity::new(
        "fixture-key".into(),
        "fixture-owner".into(),
        "fixture-epoch".into(),
        [11; 32],
        BTreeSet::from([
            NativeCapability::ConfigurationPublish,
            NativeCapability::AccessPublish,
        ]),
    ) {
        Ok(identity) => identity,
        Err(error) => panic!("fixture publication identity construction failed: {error}"),
    }
}

pub struct FixtureCarrier<'a> {
    pub receiver: &'a PublicationReceiver,
    pub store: &'a FixtureMemory,
    pub lose_response: std::cell::Cell<bool>,
    pub tamper_response: bool,
}
impl PublicationCarrier for FixtureCarrier<'_> {
    async fn send(&self, request: PublicationRequest) -> Result<PublicationResponse, GatewayError> {
        let mut response = self.receiver.handle(self.store, request).await;
        if self.lose_response.replace(false) {
            return Err(GatewayError::unavailable(
                "Fixture lost committed response",
                "native_publication_outcome_unknown",
            ));
        }
        if self.tamper_response {
            response.raw_body.push(b' ');
        }
        Ok(response)
    }
}

pub fn fixture_expect_error<T>(result: Result<T, GatewayError>) -> GatewayError {
    match result {
        Ok(_) => panic!("fixture expected an error"),
        Err(error) => error,
    }
}
