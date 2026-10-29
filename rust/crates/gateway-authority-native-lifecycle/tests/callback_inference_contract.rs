//! Expected-red controls use the actual callback producer before its behavior correction.
mod callback_inference_fixture;
mod inference_fixture;
use callback_inference_fixture::{Host, TestResult, inference_input, refusal, run};
use gateway_auth_credentials::{
    native_credentials::parse_native_document,
    provider_credentials::{CodexNativeOwner, codex_source_pool_key, parse_codex_source_pool},
};
use gateway_authority::store::Authority;
use gateway_authority_keys as keys;
use gateway_authority_native_records::rows;
use gateway_authority_paid_evidence::capture_original;
use gateway_authority_paid_records::PaidRequest;
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_authority_routing_selection::SelectionOperations;
use gateway_contracts::provider::{CodexQuotaClass, PaidProvider};
use gateway_core::state::AuthorityStore;
use gateway_kv_codec::value::ValueEncoding;
use gateway_routing_journal::{
    reader,
    record::{CredentialFence, Phase},
};
use gateway_routing_runtime::session::RoutingSession;
use gateway_routing_runtime_test_support::{
    input::NOW as RUNTIME_NOW,
    seed::{seed_pool, source_account},
    store::Store as RuntimeStore,
};
use inference_fixture::NativeRefreshHost;
use std::{
    future::Future,
    task::{Context, Waker},
};

#[test]
fn original_callback_external_tuple_is_rejected_by_defining_boundaries() -> TestResult {
    run(async {
        let store = RuntimeStore::new();
        seed_pool(&store);
        let mut source = parse_codex_source_pool(&store.value(codex_source_pool_key()))?
            .ok_or("original source pool")?;
        let account = &mut source.accounts[0];
        assert_eq!(account.account_id, "a");
        assert_eq!(account.access_token.expose(), "fixture-access-a");
        assert_eq!(account.refresh_token.expose(), "fixture-refresh-a");
        let identity = account.identity();
        account.native_owner = Some(CodexNativeOwner {
            codex_home: "/tmp/native-source".into(),
            generation_hash: identity.native_generation_hash,
            native_refreshed_at: None,
            external_user_id: Some("native-user".into()),
            managed_source: None,
        });
        assert_eq!(source.accounts.len(), 2);
        assert_eq!(source.accounts[1], source_account("b"));
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
            .ok_or("original malformed callback tuple accepted")?;
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
fn actual_callback_fixture_roundtrips_checked_external_source_and_normalization() -> TestResult {
    run(async {
        let store = RuntimeStore::new();
        let host = Host::default();
        inference_input(&store, &host, "callback-fixture-roundtrip").await?;
        let raw = store
            .read(&codex_source_pool_key())
            .await?
            .ok_or("callback source")?;
        let source = parse_codex_source_pool(raw.json_value()?)?
            .ok_or("actual callback external source is malformed")?;
        assert!(raw.expires_at_ms.is_none());
        assert_eq!(source.accounts.len(), 2);
        assert_eq!(source.accounts[0].account_id, "a");
        assert_eq!(source.accounts[0].updated_at_ms, RUNTIME_NOW as f64);
        assert_eq!(source.updated_at_ms, RUNTIME_NOW as f64);
        assert_eq!(source.accounts[1], source_account("b"));
        assert_eq!(
            parse_codex_source_pool(&source.private_source_value())?,
            Some(source.clone())
        );
        let account = &source.accounts[0];
        let checked = parse_native_document(&serde_json::json!({
            "auth_mode":"chatgptAuthTokens", "OPENAI_API_KEY":null,
            "tokens":{"account_id":account.account_id,"access_token":account.access_token.expose(),
                "id_token":account.access_token.expose(),"refresh_token":account.refresh_token.expose()}
        }))?;
        let owner = account
            .native_owner
            .as_ref()
            .ok_or("external callback owner")?;
        assert_eq!(owner.codex_home, "/tmp/native-source");
        assert_eq!(owner.external_user_id.as_deref(), Some("native-user"));
        assert_eq!(checked.external_user_id, owner.external_user_id);
        assert!(owner.managed_source.is_none());
        assert!(owner.native_refreshed_at.is_none());
        assert!(checked.native_refreshed_at.is_none());
        assert!(checked.access_expires_at_ms().is_none());
        assert!(checked.refresh_token.expose().is_empty());
        assert_eq!(owner.generation_hash, checked.generation_hash());
        let normalized: SubscriptionPool = serde_json::from_value(store.value(keys::pool()))?;
        assert_eq!(normalized.v, 1);
        assert_eq!(normalized.pool_versionstamp.0, raw.revision);
        assert_eq!(normalized.accounts.len(), 2);
        assert!(normalized.accounts.iter().all(|account| account.enabled));
        assert_eq!(
            normalized.accounts[0].account_id_hash.0,
            checked.routing_account_hash()
        );
        assert_eq!(
            normalized.accounts[0].admin_account_hash.0,
            checked.administrative_account_hash()
        );
        assert_eq!(
            normalized.accounts[0].credential_version.0,
            checked.routing_credential_version()
        );
        let sibling = source_account("b").identity();
        assert_eq!(
            normalized.accounts[1].account_id_hash,
            sibling.routing_account_hash
        );
        assert_eq!(
            normalized.accounts[1].admin_account_hash,
            sibling.administrative_account_hash
        );
        assert_eq!(
            normalized.accounts[1].credential_version,
            sibling.credential_version
        );
        Ok(())
    })
}

#[test]
fn genuine_callback_admission_retains_revision_and_refuses_paused_paid_capture() -> TestResult {
    run(async {
        let store = RuntimeStore::new();
        let auth = Host::default();
        let (input, _, _, _) = inference_input(&store, &auth, "callback-phase-contract").await?;
        let original = input.context.clone();
        let (host, pause) = NativeRefreshHost::new(&store);
        let session =
            RoutingSession::begin(input, store.clone(), host, store.clone(), store.clone())
                .await
                .map_err(|error| *error.error)?;
        let admitted = store
            .read(&rows::session_key(&original.request_id.0))
            .await?
            .ok_or("admitted callback journal")?;
        assert_eq!(admitted.value_encoding, ValueEncoding::ProfileV2);
        let request = PaidRequest {
            key_id: gateway_contracts::identity::ApiKeyId("paid-target".into()),
            request_id: original.request_id.clone(),
            attempt_id: gateway_contracts::identity::PaidAttemptId("paid-attempt".into()),
            provider: PaidProvider::Surplus,
            model: "gpt-test".into(),
            route: original.route.clone(),
            path: "/v1/responses".into(),
            stream: false,
            reasoning: None,
            created_at_ms: original.started_at_ms,
        };
        let checks = capture_original(&store, &request, &original, None, RUNTIME_NOW).await?;
        assert!(checks.iter().any(|check| {
            check.key == rows::session_key(&original.request_id.0)
                && check.expected_revision.as_deref() == Some(admitted.revision.as_str())
        }));
        let attempt = session
            .next_attempt()
            .await
            .map_err(|error| *error.error)?
            .ok_or("attempt")?;
        session
            .before_dispatch(&attempt)
            .await
            .map_err(|error| *error.error)?;
        session
            .transport_started(&attempt)
            .await
            .map_err(|error| *error.error)?;
        let mut refresh = Box::pin(session.refusal(&attempt, refusal()));
        let mut cx = Context::from_waker(Waker::noop());
        assert!(refresh.as_mut().poll(&mut cx).is_pending());
        let (paused_record, paused) = reader::load(&store, &original.request_id.0).await?;
        assert_eq!(paused.v, 3);
        assert_eq!(paused.context, original);
        assert_eq!(paused.active.as_deref(), Some(attempt.id.as_str()));
        assert_ne!(paused_record.revision, admitted.revision);
        assert!(paused.lease_expires_at_ms > RUNTIME_NOW);
        let active = paused
            .attempts
            .iter()
            .find(|record| record.id == attempt.id)
            .ok_or("active")?;
        assert_eq!(active.phase, Phase::Started);
        let CredentialFence::Codex { admission, .. } = &attempt.credential else {
            return Err("callback contract selected a non-Codex attempt".into());
        };
        assert!(
            paused
                .refreshed_versions
                .contains(&admission.binding.credential_version.0)
        );
        let error = capture_original(&store, &request, &original, None, RUNTIME_NOW)
            .await
            .err()
            .ok_or("active callback attempt accepted by prospective paid capture")?;
        assert_eq!(error.status, 503);
        assert_eq!(
            error.envelope.error.code.as_deref(),
            Some("paid_control_unavailable")
        );
        assert_eq!(
            error.envelope.error.message,
            "Original request ownership is missing, expired, finalizing or changed."
        );
        pause.release();
        refresh.await.map_err(|error| *error.error)?;
        Ok(())
    })
}
