mod runtime_support;
use gateway_auth_contracts::types::AuthPrincipal;
use gateway_auth_primitives::crypto::Secret;
use gateway_contracts::{identity::RequestContext, lifecycle::SemanticTerminal};
use gateway_extras_contract::types::{ExtrasHost, ExtrasRoute, KernelDisposition};
use gateway_extras_runtime::ExtrasRuntime;
use gateway_extras_runtime_test_support::fixture::Memory;
use gateway_kv_codec::{key::RecordKey, tuple_key::TupleKey};
use runtime_support::{
    Host, context, embedding, poll, principal, request, run, scalar_source, setup,
};
use serde_json::{Value, json};

#[test]
fn sync_deduplicates_orders_cache_hits_and_measures_only_actual_voyage_usage()
-> Result<(), Box<dyn std::error::Error>> {
    let (store, host, auth, ctx) = setup();
    let rt = ExtrasRuntime::new(store.clone(), host.clone());
    let r = run(rt.handle(request(
        ExtrasRoute::Embeddings,
        embedding(json!(["a", "a", "b"])),
        &auth,
        &ctx,
    )))
    .unwrap();
    assert_eq!(r.status, 200);
    assert_eq!(
        scalar_source(&r.body)?["data"][0]["embedding"],
        scalar_source(&r.body)?["data"][1]["embedding"]
    );
    assert_eq!(host.count(), 1);
    assert_eq!(host.0.calls.borrow()[0].1["input"], json!(["a", "b"]));
    assert_eq!(scalar_source(&r.body)?["usage"]["total_tokens"], 2);
    let cached = run(rt.handle(request(
        ExtrasRoute::Embeddings,
        embedding(json!(["b", "a"])),
        &auth,
        &ctx,
    )))
    .unwrap();
    assert_eq!(host.count(), 1);
    assert_eq!(scalar_source(&cached.body)?["usage"]["total_tokens"], 0);
    assert!(cached.usage.is_none());
    assert_eq!(cached.kernel, KernelDisposition::Completed);
    Ok(())
}

#[test]
fn idempotency_replays_without_credentials_and_conflict_never_redispatches()
-> Result<(), Box<dyn std::error::Error>> {
    let (store, host, auth, ctx) = setup();
    let rt = ExtrasRuntime::new(store, host.clone());
    let mut q = request(ExtrasRoute::Embeddings, embedding(json!("a")), &auth, &ctx);
    q.idempotency_key = Some("key".into());
    let first = run(rt.handle(q)).unwrap();
    host.0.configured.set(false);
    let mut q = request(ExtrasRoute::Embeddings, embedding(json!("a")), &auth, &ctx);
    q.idempotency_key = Some("key".into());
    let replay = run(rt.handle(q)).unwrap();
    assert_eq!(replay.body, first.body);
    assert_eq!(replay.headers["x-uos-idempotency-replayed"], "true");
    assert_eq!(
        replay.kernel,
        KernelDisposition::Incomplete("idempotency_replay")
    );
    let mut q = request(ExtrasRoute::Embeddings, embedding(json!("b")), &auth, &ctx);
    q.idempotency_key = Some("key".into());
    let conflict = run(rt.handle(q)).unwrap();
    assert_eq!(conflict.status, 409);
    assert_eq!(
        scalar_source(&conflict.body)?["error"]["code"],
        "embedding_idempotency_conflict"
    );
    assert_eq!(host.count(), 1);
    Ok(())
}

#[test]
fn dispatched_expired_idempotency_is_indeterminate_and_cannot_steal_the_lease()
-> Result<(), Box<dyn std::error::Error>> {
    let (store, host, auth, ctx) = setup();
    let rt = ExtrasRuntime::new(store, host.clone());
    host.0.pending.set(true);
    let mut q = request(ExtrasRoute::Embeddings, embedding(json!("a")), &auth, &ctx);
    q.idempotency_key = Some("key".into());
    let mut pending = Box::pin(rt.handle(q));
    assert!(poll(pending.as_mut()).is_pending());
    let mut q = request(ExtrasRoute::Embeddings, embedding(json!("a")), &auth, &ctx);
    q.idempotency_key = Some("key".into());
    let busy = run(rt.handle(q)).unwrap();
    assert_eq!(
        scalar_source(&busy.body)?["error"]["code"],
        "embedding_idempotency_in_progress"
    );
    host.0.clock.set(host.now_ms() + 60_001);
    let mut q = request(ExtrasRoute::Embeddings, embedding(json!("a")), &auth, &ctx);
    q.idempotency_key = Some("key".into());
    let retired = run(rt.handle(q)).unwrap();
    assert_eq!(
        scalar_source(&retired.body)?["error"]["code"],
        "embedding_idempotency_indeterminate"
    );
    assert_eq!(host.count(), 1);
    drop(pending);
    Ok(())
}

#[test]
fn explicit_failure_releases_idempotency_but_unknown_transport_never_retries()
-> Result<(), Box<dyn std::error::Error>> {
    let (store, host, auth, ctx) = setup();
    let rt = ExtrasRuntime::new(store, host.clone());
    host.reply(400, json!({"private":"not echoed"}));
    let mut q = request(ExtrasRoute::Embeddings, embedding(json!("a")), &auth, &ctx);
    q.idempotency_key = Some("key".into());
    assert_eq!(run(rt.handle(q)).unwrap().status, 502);
    let mut q = request(ExtrasRoute::Embeddings, embedding(json!("a")), &auth, &ctx);
    q.idempotency_key = Some("key".into());
    assert_eq!(run(rt.handle(q)).unwrap().status, 200);
    assert_eq!(host.count(), 2);
    host.0
        .replies
        .borrow_mut()
        .push_back(Err(DispatchFailure::transport(error("network"))));
    let mut q = request(ExtrasRoute::Embeddings, embedding(json!("b")), &auth, &ctx);
    q.idempotency_key = Some("unknown".into());
    let r = run(rt.handle(q)).unwrap();
    assert_eq!(
        scalar_source(&r.body)?["error"]["code"],
        "embedding_idempotency_indeterminate"
    );
    assert_eq!(host.count(), 3);
    Ok(())
}

#[test]
fn partial_chunk_success_is_not_replayed_after_global_token_budget_exhaustion()
-> Result<(), Box<dyn std::error::Error>> {
    let (store, host, auth, ctx) = setup();
    let rt = ExtrasRuntime::new(store, host.clone());
    let mut q = request(
        ExtrasRoute::Embeddings,
        embedding(json!([
            "a".repeat(20_000),
            "b".repeat(20_000),
            "c".repeat(20_000)
        ])),
        &auth,
        &ctx,
    );
    q.idempotency_key = Some("partial".into());
    let r = run(rt.handle(q)).unwrap();
    assert_eq!(
        scalar_source(&r.body)?["error"]["code"],
        "embedding_idempotency_indeterminate"
    );
    assert_eq!(host.count(), 1);
    Ok(())
}

#[test]
fn budget_retries_cas_then_waits_only_inside_the_twenty_second_deadline() {
    let (store, host, auth, ctx) = setup();
    store.rate_conflicts.set(2);
    let rt = ExtrasRuntime::new(store.clone(), host.clone());
    assert_eq!(
        run(rt.handle(request(
            ExtrasRoute::Embeddings,
            embedding(json!("a")),
            &auth,
            &ctx
        )))
        .unwrap()
        .status,
        200
    );
    assert_eq!(&host.0.sleeps.borrow()[..2], &[5, 10]);
    store.budget(3, 0, host.now_ms() - 59_000);
    assert_eq!(
        run(rt.handle(request(
            ExtrasRoute::Embeddings,
            embedding(json!("b")),
            &auth,
            &ctx
        )))
        .unwrap()
        .status,
        200
    );
    assert!(host.0.sleeps.borrow().contains(&1000));
    store.budget(3, 0, host.now_ms());
    let r = run(rt.handle(request(
        ExtrasRoute::Embeddings,
        embedding(json!("c")),
        &auth,
        &ctx,
    )))
    .unwrap();
    assert_eq!(r.status, 429);
    assert_eq!(r.headers["Retry-After"], "60");
    assert_eq!(host.count(), 2);
}

#[test]
fn quota_driven_cache_eviction_crosses_profiles_and_drops_the_oldest_pointer() {
    let (store, host, auth, ctx) = setup();
    let rt = ExtrasRuntime::new(store.clone(), host.clone());
    let mut a = embedding(json!("a"));
    a["input_type"] = json!("query");
    assert_eq!(
        run(rt.handle(request(ExtrasRoute::Embeddings, a, &auth, &ctx)))
            .unwrap()
            .status,
        200
    );
    host.0.clock.set(host.now_ms() + 1);
    store.cache_quota.set(1);
    assert_eq!(
        run(rt.handle(request(
            ExtrasRoute::Embeddings,
            embedding(json!("b")),
            &auth,
            &ctx
        )))
        .unwrap()
        .status,
        200
    );
    let rows = store.rows.borrow();
    let caches: Vec<_> = rows
        .iter()
        .filter(|(k, _)| {
            TupleKey::from_record_key(&RecordKey((*k).clone()))
                .unwrap()
                .is_strict_descendant_of(
                    &TupleKey::from_json_parts(&[json!("embeddings"), json!("v2"), json!("cache")])
                        .unwrap(),
                )
        })
        .collect();
    assert_eq!(caches.len(), 1);
    assert!(caches[0].1.value["embedding"].is_array());
}

#[test]
fn cancelled_sync_never_dispatches_and_returns_distinct_499() {
    let (store, host, auth, ctx) = setup();
    let rt = ExtrasRuntime::new(store, host.clone());
    host.0.cancelled.set(true);
    let r = run(rt.handle(request(
        ExtrasRoute::Embeddings,
        embedding(json!("a")),
        &auth,
        &ctx,
    )))
    .unwrap();
    assert_eq!(r.status, 499);
    assert_eq!(host.count(), 0);
}

fn queued_job(
    store: &Memory,
    host: &Host,
    auth: &AuthPrincipal,
    ctx: &RequestContext,
) -> Result<(ExtrasRuntime<Memory, Host>, String), Box<dyn std::error::Error>> {
    store.budget(3, 0, host.now_ms());
    let rt = ExtrasRuntime::new(store.clone(), host.clone());
    let mut body = embedding(json!("sensitive-text-for-encryption"));
    body["input_type"] = json!("document");
    let r = run(rt.handle(request(ExtrasRoute::JobCreate, body, auth, ctx))).unwrap();
    assert_eq!(r.status, 202);
    assert_eq!(r.kernel, KernelDisposition::Completed);
    Ok((
        rt,
        scalar_source(&r.body)?["id"].as_str().unwrap().to_owned(),
    ))
}
#[test]
fn retained_jobs_encrypt_inputs_and_owner_survives_bearer_rotation()
-> Result<(), Box<dyn std::error::Error>> {
    let (store, host, mut auth, ctx) = setup();
    let (rt, id) = queued_job(&store, &host, &auth, &ctx)?;
    assert!(
        !serde_json::to_string(
            &store
                .rows
                .borrow()
                .values()
                .map(|r| &r.value)
                .collect::<Vec<_>>()
        )
        .unwrap()
        .contains("sensitive-text")
    );
    assert_eq!(host.count(), 0);
    let other = principal("other");
    let other_ctx = context("other");
    assert_eq!(
        run(rt.handle(request(
            ExtrasRoute::JobGet(id.clone()),
            Value::Null,
            &other,
            &other_ctx
        )))
        .unwrap()
        .status,
        404
    );
    auth.token = Some(Secret::new("new-bearer".into()));
    host.0.clock.set(host.now_ms() + 60_001);
    let r = run(rt.handle(request(ExtrasRoute::JobGet(id), Value::Null, &auth, &ctx))).unwrap();
    assert_eq!(r.status, 200);
    assert_eq!(scalar_source(&r.body)?["status"], "succeeded");
    assert_eq!(
        r.kernel,
        KernelDisposition::Incomplete("embedding_job_read_not_counted")
    );
    assert_eq!(host.count(), 1);
    assert!(host.0.decrypts.get() >= 2);
    Ok(())
}

#[test]
fn real_aes_gcm_tamper_is_a_failed_job_without_upstream_dispatch()
-> Result<(), Box<dyn std::error::Error>> {
    let (store, host, auth, ctx) = setup();
    let (rt, id) = queued_job(&store, &host, &auth, &ctx)?;
    for row in store.rows.borrow_mut().values_mut() {
        if row.value.get("data_b64").is_some() {
            let value = row.value["data_b64"].as_str().unwrap();
            let mut bytes = value.as_bytes().to_vec();
            bytes[0] = if bytes[0] == b'A' { b'B' } else { b'A' };
            row.value["data_b64"] = json!(String::from_utf8(bytes).unwrap());
        }
    }
    host.0.clock.set(host.now_ms() + 60_001);
    let r = run(rt.handle(request(ExtrasRoute::JobGet(id), Value::Null, &auth, &ctx))).unwrap();
    assert_eq!(r.status, 200);
    assert_eq!(scalar_source(&r.body)?["status"], "failed");
    assert_eq!(
        scalar_source(&r.body)?["error"]["code"],
        "embeddings_job_input_decrypt_failed"
    );
    assert_eq!(r.semantic_terminal, Some(SemanticTerminal::Error));
    assert_eq!(host.count(), 0);
    Ok(())
}

#[test]
fn live_job_lease_refuses_a_second_runner_and_input_expiry_is_not_refreshed()
-> Result<(), Box<dyn std::error::Error>> {
    let (store, host, auth, ctx) = setup();
    let (rt, id) = queued_job(&store, &host, &auth, &ctx)?;
    host.0.clock.set(host.now_ms() + 60_001);
    host.0.pending.set(true);
    let mut first = Box::pin(rt.handle(request(
        ExtrasRoute::JobGet(id.clone()),
        Value::Null,
        &auth,
        &ctx,
    )));
    assert!(poll(first.as_mut()).is_pending());
    let second =
        run(rt.handle(request(ExtrasRoute::JobGet(id), Value::Null, &auth, &ctx))).unwrap();
    assert_eq!(second.status, 202);
    assert_eq!(second.headers["Retry-After"], "30");
    assert_eq!(host.count(), 1);
    drop(first);
    Ok(())
}

#[test]
fn expired_input_fails_after_a_poll_extended_only_the_job_and_lookup_ttls()
-> Result<(), Box<dyn std::error::Error>> {
    let (store, host, auth, ctx) = setup();
    let (rt, id) = queued_job(&store, &host, &auth, &ctx)?;
    host.0.clock.set(1_000_000 + 86_400_000 - 1000);
    store.budget(3, 0, host.now_ms());
    assert_eq!(
        run(rt.handle(request(
            ExtrasRoute::JobGet(id.clone()),
            Value::Null,
            &auth,
            &ctx
        )))
        .unwrap()
        .status,
        202
    );
    host.0.clock.set(host.now_ms() + 2000);
    let r = run(rt.handle(request(ExtrasRoute::JobGet(id), Value::Null, &auth, &ctx))).unwrap();
    assert_eq!(
        scalar_source(&r.body)?["error"]["code"],
        "embeddings_job_input_missing"
    );
    assert_eq!(host.count(), 0);
    Ok(())
}

#[test]
fn sync_retries_only_explicit_retryable_statuses_with_bounded_backoff() {
    let (store, host, auth, ctx) = setup();
    let rt = ExtrasRuntime::new(store, host.clone());
    host.reply(429, json!({}));
    host.reply(503, json!({}));
    let r = run(rt.handle(request(
        ExtrasRoute::Embeddings,
        embedding(json!("a")),
        &auth,
        &ctx,
    )))
    .unwrap();
    assert_eq!(r.status, 200);
    assert_eq!(host.count(), 3);
    assert_eq!(&*host.0.sleeps.borrow(), &[250, 500]);
}

#[test]
fn job_retry_is_poll_driven_and_does_not_schedule_a_hidden_request()
-> Result<(), Box<dyn std::error::Error>> {
    let (store, host, auth, ctx) = setup();
    let rt = ExtrasRuntime::new(store, host.clone());
    host.reply(429, json!({}));
    let mut body = embedding(json!("a"));
    body["input_type"] = json!("document");
    let queued = run(rt.handle(request(ExtrasRoute::JobCreate, body, &auth, &ctx))).unwrap();
    assert_eq!(queued.status, 202);
    assert_eq!(queued.headers["Retry-After"], "60");
    assert_eq!(host.count(), 1);
    assert!(host.0.sleeps.borrow().is_empty());
    let id = scalar_source(&queued.body)?["id"]
        .as_str()
        .unwrap()
        .to_owned();
    let r = run(rt.handle(request(ExtrasRoute::JobGet(id), Value::Null, &auth, &ctx))).unwrap();
    assert_eq!(scalar_source(&r.body)?["status"], "succeeded");
    assert_eq!(host.count(), 2);
    Ok(())
}

#[test]
fn base64_vectors_use_source_little_endian_float32_output_without_forwarding_the_encoding_option()
-> Result<(), Box<dyn std::error::Error>> {
    use base64::Engine;
    let (store, host, auth, ctx) = setup();
    let rt = ExtrasRuntime::new(store, host.clone());
    let mut body = embedding(json!("a"));
    body["encoding_format"] = json!("base64");
    let r = run(rt.handle(request(ExtrasRoute::Embeddings, body, &auth, &ctx))).unwrap();
    let bytes = base64::engine::general_purpose::STANDARD
        .decode(
            scalar_source(&r.body)?["data"][0]["embedding"]
                .as_str()
                .unwrap(),
        )
        .unwrap();
    assert_eq!(bytes.len(), 256 * 4);
    assert_eq!(&bytes[..4], &1.0f32.to_le_bytes());
    assert!(host.0.calls.borrow()[0].1.get("encoding_format").is_none());
    Ok(())
}

#[test]
fn provider_configuration_and_invalid_vector_statuses_stay_distinct()
-> Result<(), Box<dyn std::error::Error>> {
    let (store, host, auth, ctx) = setup();
    let rt = ExtrasRuntime::new(store, host.clone());
    host.0.configured.set(false);
    let missing = run(rt.handle(request(
        ExtrasRoute::Embeddings,
        embedding(json!("a")),
        &auth,
        &ctx,
    )))
    .unwrap();
    assert_eq!(missing.status, 503);
    assert_eq!(host.count(), 0);
    host.0.configured.set(true);
    host.reply(200, json!({"data":[{"embedding":vec![1.0;255]}]}));
    let malformed = run(rt.handle(request(
        ExtrasRoute::Embeddings,
        embedding(json!("a")),
        &auth,
        &ctx,
    )))
    .unwrap();
    assert_eq!(malformed.status, 502);
    assert_eq!(
        scalar_source(&malformed.body)?["error"]["code"],
        "upstream_dimension_mismatch"
    );
    Ok(())
}

#[test]
fn cache_timestamp_projection_preserves_source_negative_zero_key_identity() {
    let (store, host, auth, ctx) = setup();
    let profile = json!([
        "voyage-profile-v2",
        "voyage-4-large",
        "document",
        256,
        "float",
        "float",
        true
    ])
    .to_string();
    let hash = gateway_auth_primitives::crypto::sha256_hex(b"zero");
    store.seed(
        json!(["embeddings", "v2", "cache_index_by_hash", profile, hash]),
        json!(-0.0),
        None,
    );
    store.seed(
        json!(["embeddings", "v2", "cache_index", profile, -0.0, hash]),
        json!(1),
        None,
    );
    store.seed(
        json!([
            "embeddings",
            "v2",
            "cache_index_global",
            -0.0,
            profile,
            hash
        ]),
        json!(1),
        None,
    );
    store.seed(
        json!(["embeddings", "v2", "cache", profile, hash]),
        json!({"embedding":vec![1.0;255],"created_at":"1970-01-01T00:00:00.000Z"}),
        None,
    );
    let rt = ExtrasRuntime::new(store.clone(), host);
    let r = run(rt.handle(request(
        ExtrasRoute::Embeddings,
        embedding(json!("zero")),
        &auth,
        &ctx,
    )))
    .unwrap();
    assert_eq!(r.status, 200);
    let prefix = TupleKey::from_json_parts(&[
        json!("embeddings"),
        json!("v2"),
        json!("cache_index_global"),
    ])
    .unwrap();
    let rows = store.rows.borrow();
    let indexes: Vec<_> = rows
        .keys()
        .map(|k| TupleKey::from_record_key(&RecordKey(k.clone())).unwrap())
        .filter(|k| k.is_strict_descendant_of(&prefix))
        .collect();
    assert_eq!(indexes.len(), 1);
    let parts = indexes[0].decoded_parts().unwrap();
    let gateway_kv_codec::tuple_key::DecodedKeyPart::Number(at) = parts[3] else {
        panic!("timestamp")
    };
    assert_eq!(at.to_bits(), (-0.0f64).to_bits());
}
