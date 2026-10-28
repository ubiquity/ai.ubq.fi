#![cfg(not(target_arch = "wasm32"))]
use gateway_claude_policy::{
    host::SelectionInput,
    quota::{self, quota_key},
};
use gateway_claude_runtime::{
    host::OutcomeDecision, persistence::clear_guards, pool::ClaudePoolSession,
};
use gateway_claude_runtime_test_support::fixture::{
    Host, KEY, MODEL, NOW, Store, account, apply, fixture_effort, raw, run, seed, selected,
};
use gateway_claude_usage_protocol::snapshot;
use gateway_claude_usage_quota::{headers, parse_record, parse_source_state, stored_state};
use gateway_kv_codec::value::ValueEncoding;
use gateway_provider_protocol::types::Headers;
use gateway_source::{source_json::SourceJson, source_serialization as source};
use std::rc::Rc;
type TestResult = Result<(), Box<dyn std::error::Error>>;

fn meter(window: &str, value: &str, content_type: &str) -> Headers {
    Headers::from([
        (
            format!("anthropic-ratelimit-unified-{window}-utilization"),
            value.into(),
        ),
        ("content-type".into(), content_type.into()),
    ])
}

#[test]
fn header_codec_roundtrips_json_and_profile_v2_without_losing_old_usage() -> TestResult {
    let observation = gateway_claude_policy::response::observe_headers(
        &meter("5h", "0.4", "application/json"),
        NOW,
    )
    .ok_or("headers absent")?;
    let header = headers::merge(None, Some(&observation)).ok_or("header evidence absent")?;
    let plain = SourceJson::from_serializable(
        &serde_json::json!({"updated_at_ms":NOW,"response_headers":header}),
    )?;
    let state = parse_source_state(&plain).ok_or("plain state absent")?;
    let record = stored_state(&state, "header-json".into())?;
    assert_eq!(record.value_encoding, ValueEncoding::Json);
    assert_eq!(parse_record(&record, NOW)?, Some(state));
    let payload = SourceJson::parse(
        r#"{"five_hour":{"utilization":99,"resets_at":null},"opaque":"\ud800"}"#,
    )?;
    let legacy = snapshot::parse(&payload, NOW - 100, &|_| None).ok_or("legacy snapshot absent")?;
    let usage = SourceJson::parse(&format!(
        r#"{{"source":"oauth_usage","state":"scope_missing","status":403,"message":"old \udfff","attempted_at_ms":{},"observed_at_ms":null}}"#,
        NOW - 100
    ))?;
    let document = source::object(vec![
        ("updated_at_ms", SourceJson::from_serializable(&NOW)?),
        ("response_headers", SourceJson::from_serializable(&header)?),
        ("usage_snapshot", snapshot::serialize(&legacy)?),
        ("usage", usage),
    ])?;
    let state = parse_source_state(&document).ok_or("profile state absent")?;
    let record = stored_state(&state, "header-profile".into())?;
    assert_eq!(record.value_encoding, ValueEncoding::ProfileV2);
    let reopened = parse_record(&record, NOW)?.ok_or("reopened profile absent")?;
    assert_eq!(reopened, state);
    assert_eq!(reopened.response_headers, Some(header));
    assert_eq!(
        reopened.usage.as_ref().ok_or("old usage absent")?.state,
        "scope_missing"
    );
    assert_eq!(
        snapshot::provenance(
            reopened
                .usage_snapshot
                .as_ref()
                .ok_or("old snapshot absent")?
        )
        .ok_or("old provenance absent")?
        .0,
        payload
    );
    Ok(())
}

#[test]
fn ordinary_streaming_nonstream_error_and_headerless_heads_keep_independent_evidence() {
    let store = Store::new();
    let account = account(1);
    let config = seed(&store, vec![account.clone()]);
    let session = ClaudePoolSession::new(config.clone());
    let host = Host::new(&store);
    let raw = raw();
    let first = selected(&session, &store, &host, &raw);
    assert!(matches!(
        apply(
            &session,
            &store,
            &host,
            &first,
            200,
            meter("5h", "0.4", "text/event-stream"),
            NOW - 100
        ),
        OutcomeDecision::Preserve
    ));
    let first_state = store.quota(&account).unwrap();
    let proofs = first_state.pool_proofs.clone();
    let repeated = run(session.select(
        &store,
        &host,
        SelectionInput {
            key_id: KEY,
            model: &MODEL.into(),
            raw: &raw,
            effective_effort: fixture_effort(&raw),
            selection: None,
        },
    ))
    .unwrap_err();
    assert_eq!(repeated.status, 409);
    assert_eq!(
        repeated.envelope.error.message,
        "Claude account selection changed during the request."
    );
    // Independent ordinary requests share storage, never the request-local attempt set.
    let session = ClaudePoolSession::new(config.clone());
    let next = selected(&session, &store, &host, &raw);
    apply(
        &session,
        &store,
        &host,
        &next,
        200,
        meter("7d", "0.2", "application/json"),
        NOW - 50,
    );
    let evidence = store.quota(&account).unwrap().response_headers.unwrap();
    assert_eq!(evidence.windows["5h"].observed_at_ms, (NOW - 100) as f64);
    assert_eq!(evidence.windows["7d"].observed_at_ms, (NOW - 50) as f64);
    let session = ClaudePoolSession::new(config.clone());
    let next = selected(&session, &store, &host, &raw);
    apply(&session, &store, &host, &next, 200, Headers::new(), NOW);
    let headerless = store.quota(&account).unwrap();
    assert_eq!(headerless.response_headers, Some(evidence));
    assert_eq!(headerless.pool_proofs, proofs);
    assert!(headerless.cooldown_until_ms.is_none());
    host.fail_telemetry.set(true);
    let session = ClaudePoolSession::new(config);
    let next = selected(&session, &store, &host, &raw);
    apply(
        &session,
        &store,
        &host,
        &next,
        429,
        meter("5h", "0.5", "application/json"),
        NOW,
    );
    let rejected = store.quota(&account).unwrap();
    assert_eq!(
        rejected.response_headers.as_ref().unwrap().windows["5h"].utilization,
        Some(0.5)
    );
    assert!(rejected.cooldown_until_ms.is_some());
    assert!(rejected.blocked_until_ms.is_none());
    assert_eq!(rejected.pool_proofs, proofs);
    assert!(
        host.accepted
            .borrow()
            .iter()
            .all(|event| event.source.as_deref() == Some("response_headers"))
    );
}

#[test]
fn a_real_current_binding_cas_rejects_late_header_meter_publication() {
    let store = Store::new();
    let account = account(1);
    let config = seed(&store, vec![account.clone()]);
    let host = Rc::new(Host::new(&store));
    let session = ClaudePoolSession::new(config);
    let raw = raw();
    let receipt = selected(&session, &store, &host, &raw);
    let other_store = store.clone();
    let other_host = host.clone();
    let old = account.clone();
    *store.before_commit.borrow_mut() = Some(Box::new(move || {
        run(clear_guards(&other_store, &*other_host, &old))
            .unwrap()
            .unwrap();
    }));
    apply(
        &session,
        &store,
        &host,
        &receipt,
        200,
        meter("5h", "0.8", "application/json"),
        NOW,
    );
    let current = store.roster().accounts[0].clone();
    assert_ne!(current.evidence_generation, account.evidence_generation);
    assert!(store.quota(&current).unwrap().response_headers.is_none());
    assert!(host.accepted.borrow().is_empty());
}

#[test]
fn actual_sqlite_reopen_keeps_header_evidence_and_routing_state() -> TestResult {
    let path = std::env::temp_dir().join(format!(
        "claude-header-parity-{}.sqlite",
        std::process::id()
    ));
    let account = account(1);
    {
        let store = Store::open(&path);
        let config = seed(&store, vec![account.clone()]);
        let host = Host::new(&store);
        let session = ClaudePoolSession::new(config);
        let raw = raw();
        let receipt = selected(&session, &store, &host, &raw);
        apply(
            &session,
            &store,
            &host,
            &receipt,
            200,
            meter("5h", "0.3", "application/json"),
            NOW,
        );
    }
    let store = Store::open(&path);
    let state = store.quota(&account).ok_or("reopened quota absent")?;
    assert_eq!(
        state
            .response_headers
            .as_ref()
            .ok_or("reopened headers absent")?
            .windows["5h"]
            .utilization,
        Some(0.3)
    );
    assert_eq!(state.windows["5h"].utilization, Some(0.3));
    assert!(
        quota::applicable_proof(Some(&state), &account, &"claude-fixture".into(), NOW).is_none()
    );
    assert!(store.get(&quota_key(&account)).is_some());
    drop(store);
    std::fs::remove_file(path)?;
    Ok(())
}
