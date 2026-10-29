use super::support::{self, Host, NOW, Store};
use gateway_auth_contracts::types::{AuthConfig, AuthRequest};
use gateway_auth_credentials::native_credentials::NativeCredentials;
use gateway_auth_native_contracts::transport::{NativeAuthHeaders, NativeCapability};
use gateway_auth_native_proof::native_auth::{
    AuthenticatedNativeOwner, NativeAuthenticator, NativeTransportIdentity,
};
use gateway_auth_primitives::crypto::base64url;
use gateway_authority_actor_currentness::{
    app::ActorService, source_actor::SourceNativeRenewalActor,
};
use gateway_authority_native_records::owner::RegisteredRenewalOwner;
use gateway_core::state::{AtomicTransition, AuthorityStore, RecordMutation};
use gateway_routing_runtime_test_support::{
    input::NOW as RUNTIME_NOW, store::Store as RuntimeStore,
};
use gateway_source::{error::GatewayError, js_string::JsString16};
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
pub(super) fn credentials(
    account: &str,
    offset: i64,
    tag: &str,
) -> Result<NativeCredentials, GatewayError> {
    let payload=serde_json::to_vec(&serde_json::json!({"exp":NOW/1000+offset,"fixture":tag,"https://api.openai.com/auth":{"chatgpt_user_id":"native-user","chatgpt_account_id":account}})).map_err(|_|support::unavailable())?;
    let access = format!("e30.{}.fixture", base64url(&payload));
    gateway_auth_credentials::native_credentials::parse_native_document(
        &serde_json::json!({"auth_mode":"chatgptAuthTokens","OPENAI_API_KEY":null,"tokens":{"account_id":account,"access_token":access,"id_token":access,"refresh_token":""}}),
    )
}
pub(super) async fn seed(
    store: &RuntimeStore,
    id: &NativeTransportIdentity,
) -> Result<(), GatewayError> {
    store.advance(NOW - RUNTIME_NOW);
    let input = Store::default();
    support::fixture(&input, id, None)?;
    let mutations = input
        .0
        .borrow()
        .iter()
        .map(|(key, row)| RecordMutation::Put {
            key: gateway_kv_codec::key::RecordKey(key.clone()),
            record: row.clone(),
        })
        .collect();
    store
        .commit(AtomicTransition {
            checks: Vec::new(),
            mutations,
        })
        .await?;
    Ok(())
}
pub(super) async fn actor<'a>(
    store: &'a RuntimeStore,
    host: &Host,
    config: &AuthConfig,
    admin: bool,
) -> Result<SourceNativeRenewalActor<'a, RuntimeStore>, GatewayError> {
    let grant = gateway_source::source_json::SourceJson::parse(&format!(
        r#"{{"subject":"\ud800\u0000","clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"10/27/2026","token":"uos_app_session_{}"}}"#,
        "A".repeat(43)
    ))?;
    let token = gateway_auth_passport_sessions::sessions::PassportSessions { store, host }
        .create(&grant, &JsString16::from_units(vec![0xd800, 0]), None)
        .await?;
    let request = AuthRequest {
        method: "POST".into(),
        url: "https://ai.ubq.fi/v1/responses".into(),
        headers: std::collections::BTreeMap::from([(
            "cookie".into(),
            format!("__Host-uos_ai_passport_session={token}"),
        )]),
        ..Default::default()
    };
    let context = gateway_contracts::identity::RequestContext {
        request_id: gateway_contracts::identity::RequestId("external-context".into()),
        principal: gateway_contracts::identity::PrincipalId::from_source(JsString16::from_units(
            "passport-user:".encode_utf16().chain([0xd800, 0]).collect(),
        )),
        key_id: None,
        route: "live.sideband".into(),
        started_at_ms: NOW,
    };
    let service = ActorService::new(store, host, config);
    if admin {
        service
            .authenticate_source_admin_actor(&request, &context)
            .await
    } else {
        service
            .authenticate_source_client_actor(&request, &context)
            .await
    }
}
pub(super) async fn signed(
    store: &RuntimeStore,
    id: &NativeTransportIdentity,
    body: &[u8],
    publication: bool,
    n: u8,
) -> Result<AuthenticatedNativeOwner, GatewayError> {
    let path = if publication {
        gateway_auth_native_wire::native_read::NATIVE_PUBLICATION_PATH
    } else {
        gateway_auth_native_wire::native_read::NATIVE_CONTROL_PATH
    };
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
pub(super) async fn publish(
    authority: &gateway_authority::store::Authority<RuntimeStore, RuntimeStore>,
    host: &Host,
    config: &AuthConfig,
    id: &NativeTransportIdentity,
    owner: &RegisteredRenewalOwner,
    action: &gateway_authority_native_preparation::SourceNativeRenewalAction,
    revoke_at_cas: bool,
) -> Result<(), GatewayError> {
    let cancel = support::Cancel(false);
    let lifecycle = authority.source_native_lifecycle(host, config, id, owner, &cancel);
    let control = support::control_body(action.action_ref(), "context-operation")?;
    let proof = signed(authority.store(), id, &control, false, 111).await?;
    let mut claim = lifecycle
        .claim(&proof, action.action_ref(), "context-operation", &control)
        .await?;
    lifecycle.start(&mut claim).await?;
    let raw = authority
        .store()
        .read(&gateway_auth_credentials::provider_credentials::codex_source_pool_key())
        .await?
        .ok_or_else(support::unavailable)?;
    let pool =
        gateway_auth_credentials::provider_credentials::parse_codex_source_pool(raw.json_value()?)?
            .ok_or_else(support::unavailable)?;
    let account = pool
        .accounts
        .iter()
        .find(|a| a.identity().routing_account_hash.0 == action.fence().account_id_hash)
        .ok_or_else(support::unavailable)?;
    let next = credentials(&account.account_id, 600, "next")?;
    let command=gateway_native_external_publication_wire::command::PreparedSourceExternalAccessCommand::from_registered(gateway_native_external_publication_wire::command::ExpectedSourceExternalAccessPublication{fence:action.fence(),action_ref:action.action_ref(),operation_id:claim.operation_id(),claim_ref:claim.claim_ref(),publication_revision:"context-publication"},&next)?;
    let body = command.bytes()?;
    let proof = signed(authority.store(), id, &body, true, 112).await?;
    let operation = gateway_authority_native_records::rows::key("operation", "context-operation");
    let generation = gateway_authority_native_records::rows::generation_key_parts(
        &action.fence().native_owner_id,
        &action.fence().account_id_hash,
        &action.fence().source_native_generation,
    )?;
    let before_operation = authority
        .store()
        .read(&operation)
        .await?
        .ok_or_else(support::unavailable)?;
    let before_generation = authority
        .store()
        .read(&generation)
        .await?
        .ok_or_else(support::unavailable)?;
    let cas = AdminAtCas {
        store: authority.store(),
        host,
        key: operation.clone(),
        revoke: revoke_at_cas,
        hits: std::cell::Cell::new(0),
    };
    let service = gateway_authority_native_external::SourceNativeExternalService::new(
        &cas,
        authority.store(),
        Some(authority.store()),
        host,
        config,
        gateway_authority_native_external::SourceNativeExternalContext::configured(
            id, owner, &cancel,
        ),
    );
    let result = service.publish_external(&proof, &body).await;
    if revoke_at_cas {
        assert!(result.is_err());
        assert!(cas.hits.get() > 0);
        assert_eq!(
            authority
                .store()
                .read(&operation)
                .await?
                .ok_or_else(support::unavailable)?,
            before_operation
        );
        assert_eq!(
            authority
                .store()
                .read(&generation)
                .await?
                .ok_or_else(support::unavailable)?,
            before_generation
        );
        return Ok(());
    }
    let receipt = result?;
    assert_eq!(receipt.current_source_revision, "context-publication");
    assert_eq!(receipt.claim_ref, claim.claim_ref());
    assert!(action.fence().principal.try_utf8().is_err());
    assert_eq!(
        lifecycle
            .published(&claim, "context-publication")
            .await?
            .publication_revision(),
        "context-publication"
    );
    Ok(())
}

struct AdminAtCas<'a> {
    store: &'a RuntimeStore,
    host: &'a Host,
    key: gateway_kv_codec::key::RecordKey,
    revoke: bool,
    hits: std::cell::Cell<usize>,
}
impl AuthorityStore for AdminAtCas<'_> {
    async fn read(
        &self,
        key: &gateway_kv_codec::key::RecordKey,
    ) -> Result<Option<gateway_core::state::StoredRecord>, GatewayError> {
        self.store.read(key).await
    }
    async fn commit(
        &self,
        tx: AtomicTransition,
    ) -> Result<gateway_core::state::CommitOutcome, GatewayError> {
        if self.revoke
            && tx
                .mutations
                .iter()
                .any(|m| matches!(m,RecordMutation::Put{key,..} if key==&self.key))
        {
            self.hits.set(self.hits.get() + 1);
            self.host.admin.set(false);
            return Ok(gateway_core::state::CommitOutcome::Conflict);
        }
        self.store.commit(tx).await
    }
}
