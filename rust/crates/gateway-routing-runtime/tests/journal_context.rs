mod journal_context_support;
use gateway_authority_contracts::{PAID_RETENTION_MS, REQUEST_LEASE_MS};
use gateway_authority_paid_evidence::capture_original;
use gateway_contracts::identity::PrincipalId;
use gateway_core::{source_control::encode_source_control_record, state::AuthorityStore};
use gateway_kv_codec::value::ValueEncoding;
use gateway_local_journal::context::decode_journal_context;
use gateway_routing_runtime::{
    custody::{validate_journal_row, validate_pending_journal_row},
    session::RoutingSession,
    settlement::recover_pending,
};
use gateway_routing_runtime_test_support::{
    exec::run, host::Host, input::NOW, seed::seed_pool, store::Store,
};
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};
use journal_context_support::{
    TestResult, authenticated_input, document, key, member, now, paid_request, record, replace,
};
use std::sync::{
    Arc,
    atomic::{AtomicU64, Ordering},
};

async fn source_roundtrip(subject: Vec<u16>, id: &str) -> TestResult {
    let store = Store::new();
    seed_pool(&store);
    let input = authenticated_input(&store, id, subject).await?;
    let context = input.context.clone();
    assert!(context.principal.try_scalar().is_err());
    assert!(serde_json::to_vec(&context).is_err());
    let session = RoutingSession::begin(
        input,
        store.clone(),
        Host::new(&store),
        store.clone(),
        store.clone(),
    )
    .await
    .map_err(|error| *error.error)?;
    assert_eq!(session.original_context(), &context);
    let stored = record(&store, id).await?;
    assert_eq!(stored.schema_version, 1);
    assert_eq!(stored.value_encoding, ValueEncoding::ProfileV2);
    assert_eq!(stored.expires_at_ms, None);
    let raw = document(&stored)?;
    assert_eq!(member(&raw, "v")?.as_str(), "3");
    assert_eq!(decode_journal_context(&raw)?, context);
    let pending = store
        .read(&key("pending", id))
        .await?
        .ok_or("missing pending")?;
    validate_pending_journal_row(&key("pending", id), &pending, &key("session", id), &stored)?;
    let checks = capture_original(
        &store,
        &paid_request(&context),
        &context,
        None,
        now(&store)?,
    )
    .await?;
    assert!(checks.iter().any(|check| check.key == key("session", id)
        && check.expected_revision.as_deref() == Some(stored.revision.as_str())));
    let mut other = context.clone();
    other.principal = PrincipalId::from_source(JsString16::from_units(vec![0xfffd]));
    assert!(
        capture_original(&store, &paid_request(&context), &other, None, now(&store)?)
            .await
            .is_err()
    );
    drop(session);
    // Restart only the request runtime; the original durable fixture store and exact rows survive.
    let restarted = store.clone();
    restarted.advance(REQUEST_LEASE_MS + 1);
    let recovered = recover_pending(
        restarted.clone(),
        Host::new(&restarted),
        restarted.clone(),
        restarted.clone(),
        100,
    )
    .await?;
    assert_eq!(recovered.len(), 1);
    let finished = record(&restarted, id).await?;
    assert_eq!(
        finished.expires_at_ms,
        Some(context.started_at_ms + PAID_RETENTION_MS)
    );
    assert!(restarted.read(&key("pending", id)).await?.is_none());
    validate_journal_row(&key("session", id), &finished)?;
    let after = document(&finished)?;
    assert_eq!(decode_journal_context(&after)?, context);
    assert_eq!(member(&after, "finished")?.as_str(), "true");
    assert!(
        capture_original(
            &restarted,
            &paid_request(&context),
            &context,
            None,
            now(&restarted)?
        )
        .await
        .is_err()
    );
    Ok(())
}

#[test]
fn real_passport_principal_v3_writer_survives_restart_custody_and_paid_reads() -> TestResult {
    run(async {
        for (id, subject) in [
            ("source-high", vec![0xd800]),
            ("source-low", vec![0xdfff]),
            ("source-mixed", vec![0, 0xd800, 0xdc00, 0xdfff]),
        ] {
            source_roundtrip(subject, id).await?;
        }
        Ok(())
    })
}

#[test]
fn generated_extras_v1_non_scalar_remains_open_and_refuses_at_original_encoding() -> TestResult {
    run(async {
        let store = Store::new();
        let mut input = authenticated_input(&store, "legacy-extras", vec![0xd800]).await?;
        input.context.route = "embeddings".into();
        input.raw = SourceJson::parse(r#"{"model":"gpt-test","input":"fixture"}"#)?;
        let calls = Arc::new(AtomicU64::new(0));
        let tokens = journal_context_support::CountingTokens {
            store: store.clone(),
            calls: calls.clone(),
        };
        let result = RoutingSession::begin_extras(
            input,
            store.clone(),
            Host::new(&store),
            store.clone(),
            tokens,
            gateway_extras_contract::family::ExtrasFamily::Embeddings,
        )
        .await;
        let error = match result {
            Ok(_) => return Err("legacy source context unexpectedly persisted".into()),
            Err(error) => error,
        };
        assert_eq!(error.error.status, 503);
        assert_eq!(
            error.error.envelope.error.code.as_deref(),
            Some("routing_session_unavailable")
        );
        assert_eq!(
            error.error.envelope.error.message,
            "Routing journal encoding failed."
        );
        assert_eq!(calls.load(Ordering::SeqCst), 1);
        assert!(
            store
                .read(&key("session", "legacy-extras"))
                .await?
                .is_none()
        );
        assert!(
            store
                .read(&key("pending", "legacy-extras"))
                .await?
                .is_none()
        );
        Ok(())
    })
}

#[test]
fn scalar_complete_v3_journal_keeps_frozen_writer_bytes() -> TestResult {
    run(async {
        let store = Store::new();
        seed_pool(&store);
        let input = authenticated_input(
            &store,
            "scalar-golden",
            "scalar😀\0".encode_utf16().collect(),
        )
        .await?;
        let context = input.context.clone();
        let digest =
            gateway_routing_runtime::local_work::original_source_digest(input.original_source())?;
        let _session = RoutingSession::begin(
            input,
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await
        .map_err(|error| *error.error)?;
        let stored = record(&store, "scalar-golden").await?;
        assert_eq!(stored.value_encoding, ValueEncoding::Json);
        let expected = format!(
            r#"{{"v":3,"context":{context},"input_digest":{digest},"api":null,"kernel":null,"cursor":0,"attempts":[],"active":null,"model_sibling_retries":0,"refreshed_versions":[],"recovery_attempted":false,"claude_unavailable":false,"semantic_committed":false,"terminal_intent":null,"finished":false,"recovering":false,"lease_expires_at_ms":{lease},"updated_at_ms":{now},"extras_family":null,"extras_child_count":null,"extras_dispatch":null,"children":[],"kernel_override":null}}"#,
            context = serde_json::to_string(&context)?,
            digest = serde_json::to_string(&digest)?,
            lease = NOW + REQUEST_LEASE_MS,
            now = NOW
        );
        assert_eq!(document(&stored)?.as_bytes(), expected.as_bytes());
        Ok(())
    })
}

#[test]
fn complete_v3_shape_and_legacy_non_scalar_refusals_remain_closed() -> TestResult {
    run(async {
        let store = Store::new();
        seed_pool(&store);
        let input = authenticated_input(&store, "shape-source", vec![0xd800]).await?;
        let _session = RoutingSession::begin(
            input,
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await
        .map_err(|error| *error.error)?;
        let stored = record(&store, "shape-source").await?;
        let raw = document(&stored)?;
        let original_context = member(&raw, "context")?;
        let mut malformed = vec![];
        for version in ["1", "4", "3.0"] {
            malformed.push(replace(&raw, "v", Some(SourceJson::parse(version)?))?);
        }
        let local = SourceJson::parse(
            r#"{"kind":"codex_responses_compaction","owner_nonce":"fixture","binding_version":2,"phase":"provider_planned","terminal":null}"#,
        )?;
        malformed.push(replace(
            &replace(&raw, "v", Some(SourceJson::parse("2")?))?,
            "local_work",
            Some(local),
        )?);
        malformed.push(replace(
            &raw,
            "local_work",
            Some(SourceJson::parse("null")?),
        )?);
        malformed.push(replace(&raw, "unknown", Some(SourceJson::parse("true")?))?);
        malformed.push(replace(
            &raw,
            "context",
            Some(replace(&original_context, "key_id", None)?),
        )?);
        malformed.push(replace(
            &raw,
            "context",
            Some(replace(
                &original_context,
                "unknown",
                Some(SourceJson::parse("true")?),
            )?),
        )?);
        for value in malformed {
            let record =
                encode_source_control_record(&stored.revision, &value, stored.expires_at_ms)?;
            assert!(validate_journal_row(&key("session", "shape-source"), &record).is_err());
        }
        let reordered = source::object_keys(raw.object_entries()?.into_iter().rev().collect())?;
        let reordered =
            encode_source_control_record(&stored.revision, &reordered, stored.expires_at_ms)?;
        validate_journal_row(&key("session", "shape-source"), &reordered)?;
        Ok(())
    })
}
