use gateway_admin_contracts::{operations::AdminHost, routes::AdminOperation};
use gateway_admin_runtime::RuntimeConfig;
use gateway_admin_test_support::{
    runtime_fixtures::{WireBody, input, runtime},
    runtime_host::{Host, run},
    runtime_store::Store,
};
use gateway_auth::types::AuthHost;
use gateway_source::{error::GatewayError, js_string::JsString16};
use gateway_core::state::{AtomicTransition, AuthorityStore, CommitOutcome};
use serde_json::{Value, json};
type TestResult = Result<(), Box<dyn std::error::Error>>;

#[test]
fn claude_empty_access_is_authoritative_and_corruption_never_seeds() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig {
        claude_allowed_key_ids: vec!["owner-key".into()],
    };
    let service = runtime(&store, &host, &config);
    run(service.execute(
        AdminOperation::ClaudeAccessSet,
        input(json!({"allowed_key_ids":[]})),
    ))
    .unwrap();
    assert_eq!(
        run(service.execute(AdminOperation::ClaudeAccessGet, input(Value::Null)))
            .unwrap()
            .body
            .wire_json()["data"]["allowed_key_ids"],
        json!([])
    );
    store.put(
        json!(["uos_ai", "claude_code_access", "v1"]),
        json!({"v":2}),
    );
    assert_eq!(
        run(service.execute(AdminOperation::ClaudeAccessGet, input(Value::Null)))
            .unwrap_err()
            .status,
        503
    );
}
#[test]
fn claude_roster_rotation_preserves_identity_and_fences_prior_generations() -> TestResult {
    use gateway_admin_runtime::claude::{
        AccountUpdate, Bootstrap, current_binding, parse_credential,
        source_secret::ClaudeSourceSecret, update_account,
    };
    let store = Store::default();
    let host = Host::default();
    let seed = Bootstrap::default();
    let credential = parse_credential(
        &json!({"secret":"sk-ant-api-test-secret","kind":"api_key","updated_at_ms":900000}),
        "kv",
    )
    .ok_or("fixture credential missing")?;
    let first = run(update_account(
        &store,
        &host,
        &seed,
        &AccountUpdate {
            account_id: None,
            create_new: false,
            credential: Some(credential.clone()),
            workspace: None,
            activate: false,
            expected: None,
        },
    ))?;
    assert!(run(current_binding(&store, &first, host.now_ms()))?.is_some());
    let mut renamed = credential.clone();
    renamed.label = Some("renamed".into());
    let same = run(update_account(
        &store,
        &host,
        &seed,
        &AccountUpdate {
            account_id: Some(first.account_id.clone()),
            create_new: false,
            credential: Some(renamed),
            workspace: None,
            activate: false,
            expected: Some(first.clone()),
        },
    ))?;
    assert_eq!(first.account_cohort_id, same.account_cohort_id);
    assert_eq!(first.evidence_generation, same.evidence_generation);
    let mut rotated = credential;
    rotated.secret = std::sync::Arc::new(ClaudeSourceSecret::from_source(
        "sk-ant-api-rotated-secret".into(),
    ));
    let next = run(update_account(
        &store,
        &host,
        &seed,
        &AccountUpdate {
            account_id: Some(first.account_id.clone()),
            create_new: false,
            credential: Some(rotated),
            workspace: None,
            activate: false,
            expected: Some(first.clone()),
        },
    ))?;
    assert_eq!(first.account_id, next.account_id);
    assert_ne!(first.account_cohort_id, next.account_cohort_id);
    assert_ne!(first.evidence_generation, next.evidence_generation);
    assert!(run(current_binding(&store, &first, host.now_ms()))?.is_none());
    assert!(run(current_binding(&store, &next, host.now_ms()))?.is_some());
    let debug = format!("{next:?}");
    assert!(!debug.contains("sk-ant-api-rotated-secret"));
    Ok(())
}

#[test]
fn claude_tombstone_refuses_legacy_or_environment_resurrection() {
    use gateway_admin_runtime::claude::*;
    let store = Store::default();
    let host = Host::default();
    let credential = parse_credential(
        &json!({"secret":"sk-ant-api-legacy-secret","kind":"api_key","updated_at_ms":900000}),
        "kv",
    )
    .unwrap();
    store.rows.borrow_mut().insert(
        gateway_auth::store::key(json!(["ubq_ai", "claude_code_auth"])).0,
        gateway_admin_runtime::claude::encode_credential_record("fixture", &credential, None)
            .unwrap(),
    );
    let seed = Bootstrap {
        credential: Some(credential),
        workspace: None,
    };
    let read = run(load_roster(&store, &host, &seed)).unwrap();
    let account = active_binding(&read.roster).unwrap();
    assert!(read.revision.is_some());
    assert!(
        run(remove_account(
            &store,
            &host,
            &seed,
            Some(&account.account_id)
        ))
        .unwrap()
    );
    let after = run(load_roster(&store, &host, &seed)).unwrap();
    assert!(after.roster.accounts.is_empty());
    assert!(after.roster.active_account_id.is_none());
    assert!(store.get(json!(["ubq_ai", "claude_code_auth"])).is_some());
    store.put(
        json!(["uos_ai", "claude_code_accounts", "v1"]),
        json!({"v":99}),
    );
    assert_eq!(
        run(load_roster(&store, &host, &seed)).unwrap_err().status,
        503
    );
}

#[test]
fn source_claude_submission_limits_and_workspace_parser_are_preserved() {
    let accepted = gateway_admin_runtime::parse::claude(
        &json!({"credential":"sk-ant-api-valid","workspace_id":"wrkspc_a.b-c"}),
    )
    .unwrap();
    assert_eq!(accepted.workspace_id, Some(Some("wrkspc_a.b-c".into())));
    assert_eq!(
        gateway_admin_runtime::parse::claude(
            &json!({"credential":format!("sk-ant-{}","x".repeat(507))})
        )
        .err()
        .unwrap()
        .status,
        400
    );
    assert_eq!(gateway_admin_runtime::parse::claude(&json!({"credential":"sk-ant-api-valid","create_new":true,"account_id":"12345678-1234-4123-8123-123456789abc"})).err().unwrap().status,400);
}

#[test]
fn shared_claude_access_loader_returns_exact_fence_and_does_not_reseed_explicit_deny() {
    use gateway_admin_runtime::claude_access::*;
    let store = Store::default();
    let host = Host::default();
    store.conflicts.set(2);
    let loaded = run(load_policy(
        &store,
        &host,
        &["trusted-seed".into()],
        &|a, b| Ok(a.cmp(b)),
    ))
    .unwrap();
    assert_eq!(loaded.policy.source, PolicySource::Env);
    assert_eq!(
        loaded.policy.allowed_key_ids,
        vec![JsString16::from_utf8("trusted-seed")]
    );
    let actual = run(store.read(&policy_key())).unwrap().unwrap();
    assert_eq!(loaded.check.expected_revision, Some(actual.revision));
    store.put(
        json!(["uos_ai", "claude_code_access", "v1"]),
        json!({"v":1,"source":"kv","allowed_key_ids":[],"updated_at_ms":100}),
    );
    let denied = run(load_policy(
        &store,
        &host,
        &["must-not-resurrect".into()],
        &|a, b| Ok(a.cmp(b)),
    ))
    .unwrap();
    assert!(denied.policy.allowed_key_ids.is_empty());
    assert_eq!(denied.policy.source, PolicySource::Kv);
    assert_eq!(
        run(store.commit(AtomicTransition {
            checks: vec![loaded.check],
            mutations: vec![]
        }))
        .unwrap(),
        CommitOutcome::Conflict
    );
    store.put(
        json!(["uos_ai", "claude_code_access", "v1"]),
        json!({"v":1,"source":"kv","allowed_key_ids":null,"updated_at_ms":100}),
    );
    assert!(
        run(load_policy(
            &store,
            &host,
            &["forbidden-fallback".into()],
            &|a, b| Ok(a.cmp(b))
        ))
        .is_err()
    );
}
#[test]
fn shared_claude_access_parser_preserves_stable_locale_ties_and_missing_host_refuses() {
    use gateway_admin_runtime::claude_access::*;
    let ids = parse_ids(&json!(["é", "e\u{0301}", "é"]), &|_, _| {
        Ok(std::cmp::Ordering::Equal)
    })
    .unwrap();
    assert_eq!(
        ids,
        vec![
            JsString16::from_utf8("é"),
            JsString16::from_utf8("e\u{0301}")
        ]
    );
    assert!(
        parse_policy(
            &json!({"v":1,"source":"kv","allowed_key_ids":[],"updated_at_ms":9007199254740992_i64}),
            &|a, b| Ok(a.cmp(b))
        )
        .is_err()
    );
    let store = Store::default();
    let host = Host::default();
    let error = run(load_policy(
        &store,
        &host,
        &["a".into(), "b".into()],
        &|_, _| {
            Err(GatewayError::unavailable(
                "locale host missing",
                "source_collation_unavailable",
            ))
        },
    ))
    .unwrap_err();
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("source_collation_unavailable")
    );
    assert!(
        store
            .get(json!(["uos_ai", "claude_code_access", "v1"]))
            .is_none()
    );
    store.conflicts.set(8);
    assert_eq!(
        run(load_policy(&store, &host, &[], &|a, b| Ok(a.cmp(b))))
            .unwrap_err()
            .status,
        409
    );
    assert!(
        store
            .get(json!(["uos_ai", "claude_code_access", "v1"]))
            .is_none()
    );
}
