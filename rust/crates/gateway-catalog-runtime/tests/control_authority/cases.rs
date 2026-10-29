use super::original_request_fixture;
use super::{
    Clock, Host, Store, TestResult, Tokens, context, paid_request, run, set_source_field, surplus,
};
use gateway_authority_keys as keys;
use gateway_authority_paid_control_records::PaidSyntheticHop;
use gateway_catalog_control::control::publish_paid_routing;
use gateway_catalog_inputs::state;
use gateway_catalog_runtime::CatalogRuntime;
use gateway_contracts::provider::PaidProvider;
use gateway_routing_runtime_test_support::source_catalog;
use serde_json::json;
use std::{cell::Cell, collections::BTreeMap};
#[test]
fn actual_publisher_receipt_and_reservation_share_one_authority_and_reject_stale_existing()
-> TestResult {
    use gateway_authority::store::Authority;
    use gateway_authority_api_protocol::ApiKeyPolicy;
    use gateway_authority_paid_records::{PaidAdmission, PaidAuthorization};
    let store = Store::new();
    store.install(true, false);
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    host.queue(surplus("paid-only"));
    run(publish_paid_routing(&runtime, &store, &host))?;
    let request = paid_request(&store, PaidProvider::Surplus, "paid-only");
    store.put(
        keys::api_policy(&request.key_id.0),
        "key-policy",
        json!(ApiKeyPolicy {
            v: 3,
            key_id: request.key_id.clone(),
            expires_at_ms: -1,
            usage_limit_requests: -1,
            window_ms: 86400000,
            usage_reset_at_ms: store.now.get() + 86400000,
            paid_fallback_enabled: true,
            paid_fallback_limit_microcredits: 1000000,
            paid_fallback_quota_per_credit: 1000,
            paid_fallback_pricing_checked_at_ms: store.now.get(),
            paid_fallback_models: vec!["paid-only".into()],
            paid_fallback_max_exposure_microcredits: BTreeMap::from([("paid-only".into(), 1000)])
        }),
    );
    let authority =
        Authority::with_clock(store.clone(), Tokens(Cell::new(0)), Clock(store.now.get()));
    let receipt = run(original_request_fixture::authorize(
        &authority,
        &request,
        &context("paid-only")?,
        store.now.get(),
    ))?;
    let reserved = run(authority.reserve_paid(
        &request,
        &PaidAuthorization::DirectPaidModel,
        &receipt,
        store.now.get(),
    ))?;
    assert!(matches!(reserved, PaidAdmission::Reserved(_)));
    drop(reserved);
    let existing = run(authority.reserve_paid(
        &request,
        &PaidAuthorization::DirectPaidModel,
        &receipt,
        store.now.get(),
    ))?;
    assert!(matches!(existing, PaidAdmission::Existing(_)));
    drop(existing);
    store.put(
        state::selection_key(),
        "changed-after-reservation",
        json!({"provider_ids":["codex"],"updated_at_ms":store.now.get()}),
    );
    assert!(
        run(authority.reserve_paid(
            &request,
            &PaidAuthorization::DirectPaidModel,
            &receipt,
            store.now.get()
        ))
        .is_err()
    );
    assert!(
        store
            .get(&keys::paid_attempt(
                &request.key_id.0,
                &request.attempt_id.0
            ))
            .is_some()
    );
    Ok(())
}

#[test]
fn coarse_union_never_authorizes_the_wrong_client_endpoint() -> TestResult {
    use gateway_authority::store::Authority;
    let store = Store::new();
    store.install(false, true);
    let host = Host::new(&store, false, true);
    let runtime = CatalogRuntime::new();
    host.queue(json!({"data":[{"id":"paid-only","supported_endpoint_types":["openai"]}]}));
    run(publish_paid_routing(&runtime, &store, &host))?;
    assert!(
        store
            .projection()
            .policy
            .models
            .contains_key(&"paid-only".into())
    );
    let request = paid_request(&store, PaidProvider::Metered, "paid-only");
    let authority =
        Authority::with_clock(store.clone(), Tokens(Cell::new(0)), Clock(store.now.get()));
    assert!(
        run(authority.authorize_paid_route(&request, &context("paid-only")?, store.now.get()))
            .is_err()
    );
    Ok(())
}

#[test]
fn ordinary_tool_request_cannot_use_a_coarse_non_tool_surplus_permission() -> TestResult {
    use gateway_authority::store::Authority;
    let store = Store::new();
    store.install(true, false);
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    let mut response = surplus("paid-only");
    response["data"][0]["supported_features"] = json!([]);
    host.queue(response);
    run(publish_paid_routing(&runtime, &store, &host))?;
    assert!(
        store
            .projection()
            .policy
            .models
            .contains_key(&"paid-only".into())
    );
    let request = paid_request(&store, PaidProvider::Surplus, "paid-only");
    let authority =
        Authority::with_clock(store.clone(), Tokens(Cell::new(0)), Clock(store.now.get()));
    let mut context = context("paid-only")?;
    set_source_field(
        &mut context.body,
        "tools",
        source_catalog::json(&json!([{"type":"function","name":"actual-tool"}])),
    );
    assert!(run(authority.authorize_paid_route(&request, &context, store.now.get())).is_err());
    Ok(())
}

#[test]
fn synthetic_override_requires_the_server_owned_hop_not_a_body_shortcut() -> TestResult {
    use gateway_authority::store::Authority;
    let store = Store::new();
    store.install(true, false);
    store.put(
        state::selection_key(),
        "selection",
        json!({"provider_ids":["ubiquity"],"updated_at_ms":store.now.get()}),
    );
    let host = Host::new(&store, true, false);
    let runtime = CatalogRuntime::new();
    host.queue(surplus("deepseek-v4.1-flash"));
    run(publish_paid_routing(&runtime, &store, &host))?;
    let request = paid_request(&store, PaidProvider::Surplus, "deepseek-v4.1-flash");
    let authority =
        Authority::with_clock(store.clone(), Tokens(Cell::new(0)), Clock(store.now.get()));
    let mut ordinary = context("deepseek-v4.1-flash")?;
    set_source_field(
        &mut ordinary.body,
        "synthetic_hop",
        source_catalog::json("surplus"),
    );
    assert!(run(authority.authorize_paid_route(&request, &ordinary, store.now.get())).is_err());
    let mut synthetic = context("ubiquity/deepseek-flash")?;
    synthetic.synthetic_hop = Some(PaidSyntheticHop::Surplus);
    assert!(run(authority.authorize_paid_route(&request, &synthetic, store.now.get())).is_ok());
    Ok(())
}
