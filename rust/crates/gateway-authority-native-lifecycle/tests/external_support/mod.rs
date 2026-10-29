use super::support::{self, Cancel, Host, NOW, Store, Tokens, unavailable};
use gateway_auth_contracts::types::AuthConfig;
use gateway_auth_credentials::{
    native_credentials::NativeCredentials,
    provider_credentials::{codex_source_pool_key, parse_codex_source_pool},
};
use gateway_auth_native_contracts::transport::{NativeAuthHeaders, NativeCapability};
use gateway_auth_native_proof::native_auth::{
    AuthenticatedNativeOwner, NativeAuthenticator, NativeTransportIdentity,
};
use gateway_auth_primitives::crypto::{Secret, base64url};
use gateway_authority_keys as keys;
use gateway_authority_native_lifecycle::{
    SourceNativeLifecycleContext, SourceNativeLifecycleService, SourceNativeRenewalClaim,
};
use gateway_authority_native_preparation::{
    SourceNativePreparationContext, SourceNativeRenewalAction, SourceNativeRenewalService,
};
use gateway_authority_native_records::{owner::RegisteredRenewalOwner, rows};
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_core::state::{AuthorityStore, StoredRecord};
use gateway_kv_codec::key::RecordKey;
use gateway_native_external_publication_wire::command::{
    ExpectedSourceExternalAccessPublication, PreparedSourceExternalAccessCommand,
};
use gateway_source::error::GatewayError;
pub(super) fn identity() -> Result<NativeTransportIdentity, GatewayError> {
    NativeTransportIdentity::new(
        "key".into(),
        "owner".into(),
        "epoch".into(),
        [73; 32],
        [
            NativeCapability::OwnerRenewal,
            NativeCapability::AccessPublish,
        ]
        .into_iter()
        .collect(),
    )
}
pub(super) fn credentials(offset: i64, tag: &str) -> Result<NativeCredentials, GatewayError> {
    let payload=serde_json::to_vec(&serde_json::json!({"exp":NOW/1000+offset,"fixture":tag,"https://api.openai.com/auth":{"chatgpt_user_id":"native-user","chatgpt_account_id":"account"}})).map_err(|_|unavailable())?;
    let access = format!("e30.{}.fixture", base64url(&payload));
    gateway_auth_credentials::native_credentials::parse_native_document(
        &serde_json::json!({"auth_mode":"chatgptAuthTokens","OPENAI_API_KEY":null,"tokens":{"account_id":"account","access_token":access,"id_token":access,"refresh_token":""},"last_refresh":null}),
    )
}
pub(super) async fn fixture(
    store: &Store,
    id: &NativeTransportIdentity,
) -> Result<(), GatewayError> {
    support::fixture(store, id, None)?;
    let row = store
        .read(&codex_source_pool_key())
        .await?
        .ok_or_else(unavailable)?;
    let mut pool = parse_codex_source_pool(row.json_value()?)?.ok_or_else(unavailable)?;
    let old = credentials(300, "original")?;
    let account = &mut pool.accounts[0];
    account.access_token = old.access_token;
    account.refresh_token = Secret::new(String::new());
    let identity = account.identity();
    account
        .native_owner
        .as_mut()
        .ok_or_else(unavailable)?
        .generation_hash = identity.native_generation_hash;
    let row = store.read(&keys::pool()).await?.ok_or_else(unavailable)?;
    let mut normalized: SubscriptionPool =
        serde_json::from_value(row.json_value()?.clone()).map_err(|_| unavailable())?;
    normalized.accounts[0].credential_version = identity.credential_version;
    support::seed(
        store,
        codex_source_pool_key(),
        "source",
        pool.private_source_value(),
    )?;
    support::seed(store, keys::pool(), "normalized", normalized)
}
pub(super) struct Rig<'a> {
    pub store: &'a Store,
    pub host: &'a Host,
    pub config: &'a AuthConfig,
    pub tokens: &'a Tokens,
    pub identity: &'a NativeTransportIdentity,
    pub owner: &'a RegisteredRenewalOwner,
    pub cancel: &'a Cancel,
}
impl<'a> Rig<'a> {
    pub fn lifecycle(&self) -> SourceNativeLifecycleService<'a, Store, Tokens, Host, Cancel> {
        SourceNativeLifecycleService::new(
            self.store,
            self.tokens,
            Some(&self.host.now),
            self.host,
            self.config,
            SourceNativeLifecycleContext::configured(self.identity, self.owner, self.cancel),
        )
    }
    pub fn service(
        &self,
    ) -> gateway_authority_native_external::SourceNativeExternalService<
        'a,
        Store,
        Tokens,
        Host,
        Cancel,
    > {
        gateway_authority_native_external::SourceNativeExternalService::new(
            self.store,
            self.tokens,
            Some(&self.host.now),
            self.host,
            self.config,
            gateway_authority_native_external::SourceNativeExternalContext::configured(
                self.identity,
                self.owner,
                self.cancel,
            ),
        )
    }
    pub async fn prepare(
        &self,
        operation: &str,
    ) -> Result<
        (
            SourceNativeRenewalAction,
            SourceNativeRenewalClaim<'a, Store>,
            String,
        ),
        GatewayError,
    > {
        let (actor, token) = support::actor(self.store, self.host, self.config).await?;
        let prep = SourceNativeRenewalService::new(
            self.store,
            self.tokens,
            Some(&self.host.now),
            self.host,
            self.config,
            SourceNativePreparationContext::configured(self.identity, self.owner, self.cancel),
        );
        let action = prep
            .prepare_source_live_mapped_renewal(&actor, "call")
            .await?;
        let body = support::control_body(action.action_ref(), operation)?;
        let path = gateway_auth_native_wire::native_read::NATIVE_CONTROL_PATH;
        let digest = gateway_auth_primitives::crypto::sha256_hex(&body);
        let nonce = base64url(&digest.as_bytes()[..32]);
        let mac = self.identity.sign(path, NOW, &nonce, &body)?;
        let replay =
            gateway_auth_native_protocol::native_replay::StoreNonceAuthority::new(self.store);
        let proof = NativeAuthenticator::new(self.identity, &replay)
            .authenticate(
                path,
                NativeAuthHeaders {
                    version: "1",
                    key_id: self.identity.key_id(),
                    time_ms: &NOW.to_string(),
                    nonce: &nonce,
                    mac: &mac,
                },
                &body,
                NOW,
            )
            .await?;
        let mut claim = self
            .lifecycle()
            .claim(&proof, action.action_ref(), operation, &body)
            .await?;
        self.lifecycle().start(&mut claim).await?;
        Ok((action, claim, token))
    }
}
pub(super) fn command(
    action: &SourceNativeRenewalAction,
    claim: &SourceNativeRenewalClaim<'_, Store>,
    next: &NativeCredentials,
    publication: &str,
) -> Result<Vec<u8>, GatewayError> {
    PreparedSourceExternalAccessCommand::from_registered(
        ExpectedSourceExternalAccessPublication {
            fence: action.fence(),
            action_ref: action.action_ref(),
            operation_id: claim.operation_id(),
            claim_ref: claim.claim_ref(),
            publication_revision: publication,
        },
        next,
    )?
    .bytes()
}
pub(super) async fn signed(
    store: &Store,
    id: &NativeTransportIdentity,
    body: &[u8],
    n: u8,
) -> Result<AuthenticatedNativeOwner, GatewayError> {
    let path = gateway_auth_native_wire::native_read::NATIVE_PUBLICATION_PATH;
    let nonce = base64url(&[n; 32]);
    let mac = id.sign(path, NOW, &nonce, body)?;
    let replay = gateway_auth_native_protocol::native_replay::StoreNonceAuthority::new(store);
    NativeAuthenticator::new(id, &replay)
        .authenticate(
            path,
            NativeAuthHeaders {
                version: "1",
                key_id: id.key_id(),
                time_ms: &NOW.to_string(),
                nonce: &nonce,
                mac: &mac,
            },
            body,
            NOW,
        )
        .await
}
pub(super) fn group(
    action: &SourceNativeRenewalAction,
    operation: &str,
) -> Result<Vec<RecordKey>, GatewayError> {
    Ok(vec![
        codex_source_pool_key(),
        keys::pool(),
        rows::key("operation", operation),
        rows::generation_key_parts(
            &action.fence().native_owner_id,
            &action.fence().account_id_hash,
            &action.fence().source_native_generation,
        )?,
    ])
}
pub(super) async fn snapshot(
    store: &Store,
    keys: &[RecordKey],
) -> Result<Vec<StoredRecord>, GatewayError> {
    let mut rows = Vec::new();
    for key in keys {
        rows.push(store.read(key).await?.ok_or_else(unavailable)?);
    }
    Ok(rows)
}
