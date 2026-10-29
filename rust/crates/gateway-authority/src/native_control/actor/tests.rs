use super::*;
type TestResult = Result<(), Box<dyn std::error::Error>>;
mod native_import;
use gateway_auth_primitives::crypto::{Secret, sha256_base64url};
use gateway_authority_test_support::native_actor::{
    Host, Store, Tokens, api_setup, authority, config, context, request, run, session_setup, user,
};
use gateway_core::state::{AtomicTransition, CommitOutcome, StoredRecord};
use std::collections::BTreeMap;
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicUsize, Ordering},
};
async fn current(
    authority: &Authority<Store, Tokens>,
    actor: &NativeRenewalActor,
    cfg: &AuthConfig,
) -> Result<CurrentNativeActor, GatewayError> {
    let mut tx = authority.transaction(authority.fresh_time(0)?)?;
    let result = read_current(&mut tx, actor.stored(), cfg).await?;
    validate_current_time(actor.stored(), authority.fresh_time(tx.now)?)?;
    if !tx.commit().await? {
        return Err(fail());
    }
    Ok(result)
}

#[test]
fn actual_client_and_admin_session_auth_mint_without_serializing_bearer_keys() {
    run(async {
        let store = Store::default();
        let token = session_setup(&store, true, true);
        let host = Host::new(&store);
        let authority = self::authority(&store);
        let cfg = config();
        let req = request(&token);
        let ctx = context("passkey-user:user-fixture", None);
        let client = authenticate_client_actor(&authority, &host, &cfg, &req, &ctx)
            .await
            .unwrap();
        let admin = authenticate_admin_actor(&authority, &host, &cfg, &req, &ctx)
            .await
            .unwrap();
        assert_eq!(client.scope(), NativeActorScope::Client);
        assert_eq!(admin.scope(), NativeActorScope::Admin);
        assert_eq!(client.context(), &ctx);
        assert_eq!(client.principal(), "passkey-user:user-fixture");
        let serialized = serde_json::to_string(client.stored()).unwrap();
        assert!(!serialized.contains(&token));
        assert!(!serialized.contains(&passkeys::key("sessions", &token).0));
        let before = current(&authority, &client, &cfg).await.unwrap();
        assert_eq!(before.principal, client.principal());
        assert_eq!(before.policy_digest, client.policy_digest());
        let mut tx = authority.transaction(host.now_ms()).unwrap();
        client.bind_preparation(&mut tx).await.unwrap();
        assert!(
            tx.checks()
                .iter()
                .any(|check| check.key == passkeys::key("sessions", &token))
        );
        store.seed(
            passkeys::key("users", "user-fixture"),
            "demoted",
            user(false),
        );
        assert!(current(&authority, &admin, &cfg).await.is_err());
        assert!(
            authenticate_admin_actor(&authority, &host, &cfg, &req, &ctx)
                .await
                .is_err()
        );
        assert_eq!(host.fetches.load(Ordering::SeqCst), 0);
    });
}

#[test]
fn session_revocation_replacement_expiry_and_missing_index_refuse() {
    run(async {
        for change in 0..5 {
            let store = Store::default();
            let token = session_setup(&store, true, true);
            let host = Host::new(&store);
            let authority = self::authority(&store);
            let cfg = config();
            let req = request(&token);
            let ctx = context("passkey-user:user-fixture", None);
            let actor = authenticate_client_actor(&authority, &host, &cfg, &req, &ctx)
                .await
                .unwrap();
            let key = session_index::index_key(&session_index::digest(&token)).unwrap();
            match change {
                0 => {
                    store.rows.lock().unwrap().remove(&key.0);
                }
                1 => {
                    store.seed(
                        passkeys::key("users", "user-fixture"),
                        "replacement",
                        user(true),
                    );
                }
                2 => {
                    store.now.store(1_100_000, Ordering::SeqCst);
                }
                3 => {
                    let mut row = store.rows.lock().unwrap().get(&key.0).unwrap().clone();
                    row.revision = "replacement".into();
                    store.rows.lock().unwrap().insert(key.0, row);
                }
                _ => {
                    store
                        .rows
                        .lock()
                        .unwrap()
                        .remove(&passkeys::key("users", "user-fixture").0);
                }
            }
            assert!(current(&authority, &actor, &cfg).await.is_err());
        }
        let store = Store::default();
        let token = session_setup(&store, true, false);
        let host = Host::new(&store);
        assert!(
            authenticate_client_actor(
                &self::authority(&store),
                &host,
                &config(),
                &request(&token),
                &context("passkey-user:user-fixture", None)
            )
            .await
            .is_err()
        );
    });
}

#[test]
fn actual_api_auth_binds_current_hash_policy_and_absolute_source_expiry() {
    run(async {
        let store = Store::default();
        let token = api_setup(&store, -1);
        let hashkey = api_keys::hash_key(&sha256_base64url(token.as_bytes()));
        let mut row = store.rows.lock().unwrap().get(&hashkey.0).unwrap().clone();
        row.expires_at_ms = Some(1_000_100);
        store.rows.lock().unwrap().insert(hashkey.0.clone(), row);
        let host = Host::new(&store);
        let authority = self::authority(&store);
        let cfg = config();
        let ctx = context("api-key:api-fixture", Some("api-fixture"));
        let actor = authenticate_client_actor(&authority, &host, &cfg, &request(&token), &ctx)
            .await
            .unwrap();
        assert!(
            authenticate_admin_actor(&authority, &host, &cfg, &request(&token), &ctx)
                .await
                .is_err()
        );
        assert!(
            !serde_json::to_string(actor.stored())
                .unwrap()
                .contains(&token)
        );
        store.now.store(1_000_100, Ordering::SeqCst);
        assert!(validate_current_time(actor.stored(), authority.fresh_time(0).unwrap()).is_err());
        store.now.store(1_000_000, Ordering::SeqCst);
        store.commit_time.store(1_000_100, Ordering::SeqCst);
        assert!(
            authenticate_client_actor(&authority, &host, &cfg, &request(&token), &ctx)
                .await
                .is_err()
        );
        store.now.store(1_000_000, Ordering::SeqCst);
        let mut row = store.rows.lock().unwrap().get(&hashkey.0).unwrap().clone();
        row.revision = "changed-policy".into();
        row.value["usage_limit_requests"] = json!(2);
        store.rows.lock().unwrap().insert(hashkey.0, row);
        assert!(current(&authority, &actor, &cfg).await.is_err());
    });
}

#[test]
fn allowlists_local_and_deno_bind_actual_current_configuration() {
    run(async {
        for admin in [false, true] {
            let store = Store::default();
            let host = Host::new(&store);
            let authority = self::authority(&store);
            let mut cfg = config();
            let token = "SYNTHETIC_ALLOWLIST_SECRET";
            if admin {
                cfg.admin_tokens.push(Secret::new(token.into()));
            } else {
                cfg.auth_tokens.push(Secret::new(token.into()));
            }
            let name = if admin {
                "auth-method:admin_allowlist"
            } else {
                "auth-method:auth_tokens_allowlist"
            };
            let ctx = context(name, None);
            let req = request(token);
            let actor = if admin {
                authenticate_admin_actor(&authority, &host, &cfg, &req, &ctx)
                    .await
                    .unwrap()
            } else {
                authenticate_client_actor(&authority, &host, &cfg, &req, &ctx)
                    .await
                    .unwrap()
            };
            assert!(
                !serde_json::to_string(actor.stored())
                    .unwrap()
                    .contains(token)
            );
            assert!(current(&authority, &actor, &cfg).await.is_ok());
            cfg.auth_tokens.clear();
            cfg.admin_tokens.clear();
            assert!(current(&authority, &actor, &cfg).await.is_err());
        }
        let store = Store::default();
        let host = Host::new(&store);
        let authority = self::authority(&store);
        let cfg = AuthConfig::default();
        let mut req = AuthRequest {
            method: "POST".into(),
            url: "http://localhost/v1/responses".into(),
            ..Default::default()
        };
        let ctx = context("local-auth-disabled", None);
        let actor = authenticate_client_actor(&authority, &host, &cfg, &req, &ctx)
            .await
            .unwrap();
        assert!(current(&authority, &actor, &config()).await.is_err());
        req.url = "https://ai.ubq.fi/v1/responses".into();
        assert!(
            authenticate_client_actor(&authority, &host, &cfg, &req, &ctx)
                .await
                .is_err()
        );
        let mut cfg = config();
        cfg.deno_app_slug = Some("fixture-app".into());
        let token = "ddw_SYNTHETIC_DENO_SECRET";
        let actor = authenticate_admin_actor(
            &authority,
            &host,
            &cfg,
            &request(token),
            &context("auth-method:deno_deploy_token", None),
        )
        .await
        .unwrap();
        assert!(host.fetches.load(Ordering::SeqCst) > 0);
        assert!(
            !serde_json::to_string(actor.stored())
                .unwrap()
                .contains(token)
        );
        store.now.store(1_300_000, Ordering::SeqCst);
        assert!(current(&authority, &actor, &cfg).await.is_err());
    });
}

#[test]
fn context_without_auth_wrong_principal_or_key_and_readonly_health_cannot_mint() {
    run(async {
        let store = Store::default();
        let token = api_setup(&store, -1);
        let host = Host::new(&store);
        let authority = self::authority(&store);
        let cfg = config();
        let ctx = context("api-key:api-fixture", Some("api-fixture"));
        assert!(
            authenticate_client_actor(&authority, &host, &cfg, &AuthRequest::default(), &ctx)
                .await
                .is_err()
        );
        assert!(
            authenticate_client_actor(
                &authority,
                &host,
                &cfg,
                &request(&token),
                &context("other", Some("api-fixture"))
            )
            .await
            .is_err()
        );
        assert!(
            authenticate_client_actor(
                &authority,
                &host,
                &cfg,
                &request(&token),
                &context("api-key:api-fixture", None)
            )
            .await
            .is_err()
        );
        let mut req = request(&token);
        req.method = "GET".into();
        req.url = "https://ai.ubq.fi/health".into();
        assert!(
            authenticate_client_actor(&authority, &host, &cfg, &req, &ctx)
                .await
                .is_err()
        );
        let actor = authenticate_client_actor(&authority, &host, &cfg, &request(&token), &ctx)
            .await
            .unwrap();
        assert_eq!(actor.context().request_id.0, "original-request");
        assert!(
            store
                .rows
                .lock()
                .unwrap()
                .keys()
                .all(|key| !key.contains("routing_runtime"))
        );
        let mut stored = serde_json::to_value(actor.stored()).unwrap();
        stored["v"] = json!(99);
        let stored: StoredNativeActor = serde_json::from_value(stored).unwrap();
        let mut tx = authority.transaction(1_000_000).unwrap();
        assert!(read_current(&mut tx, &stored, &cfg).await.is_err());
    });
}

#[test]
fn verified_kernel_actor_needs_no_fake_inference_admission_and_rechecks_source_keys() {
    run(async {
        let fixture: Value =
            serde_json::from_str(include_str!("actor_kernel_fixture.json")).unwrap();
        let token = fixture["token"].as_str().unwrap();
        let store = Store::default();
        let host = Host::new(&store);
        let authority = self::authority(&store);
        let mut cfg = config();
        cfg.kernel_public_keys_pem
            .push(fixture["public_key"].as_str().unwrap().into());
        let mut req = request(token);
        req.url = "https://ai.ubq.fi/v1/live/calls".into();
        req.headers.insert(
            "x-ubiquity-kernel-token".into(),
            fixture["jwt"].as_str().unwrap().into(),
        );
        req.headers
            .insert("x-github-owner".into(), "FixtureOrg".into());
        req.headers
            .insert("x-github-repo".into(), "fixture-repo".into());
        let ctx = context("github-repo:fixtureorg/fixture-repo", None);
        let actor = authenticate_client_actor(&authority, &host, &cfg, &req, &ctx)
            .await
            .unwrap();
        assert_eq!(actor.principal(), "github-repo:fixtureorg/fixture-repo");
        assert_eq!(host.fetches.load(Ordering::SeqCst), 1);
        assert!(store.rows.lock().unwrap().is_empty());
        assert!(current(&authority, &actor, &cfg).await.is_ok());
        let serialized = serde_json::to_string(actor.stored()).unwrap();
        assert!(!serialized.contains(token));
        assert!(!serialized.contains(fixture["jwt"].as_str().unwrap()));
        store.seed(
            upstream::kernel_public_keys_key(),
            "keys-changed",
            json!([]),
        );
        assert!(current(&authority, &actor, &cfg).await.is_err());
    });
}

#[test]
fn transient_source_session_cas_is_not_a_serialized_policy_digest_input() {
    run(async {
        let store = Store::default();
        let token = session_setup(&store, true, true);
        let host = Host::new(&store);
        let authority = self::authority(&store);
        let cfg = config();
        let actor = authenticate_client_actor(
            &authority,
            &host,
            &cfg,
            &request(&token),
            &context("passkey-user:user-fixture", None),
        )
        .await
        .unwrap();
        let baseline = actor.policy_digest().to_owned();
        let mut tx = authority.transaction(1_000_000).unwrap();
        let facts = read_current(&mut tx, actor.stored(), &cfg).await.unwrap();
        assert_eq!(facts.policy_digest, baseline);
        assert_eq!(facts.scope, NativeActorScope::Client);
        actor.bind_preparation(&mut tx).await.unwrap();
        let source_key = passkeys::key("sessions", &token);
        let mut row = store
            .rows
            .lock()
            .unwrap()
            .get(&source_key.0)
            .unwrap()
            .clone();
        row.revision = "session-raced".into();
        store.rows.lock().unwrap().insert(source_key.0.clone(), row);
        assert!(!tx.commit().await.unwrap());
        assert!(
            !serde_json::to_string(actor.stored())
                .unwrap()
                .contains(&source_key.0)
        );
    });
}

#[test]
fn live_api_actor_does_not_require_or_create_normalized_quota_projection() {
    run(async {
        let store = Store::default();
        let token = api_setup(&store, -1);
        store
            .rows
            .lock()
            .unwrap()
            .remove(&keys::api_policy("api-fixture").0);
        let host = Host::new(&store);
        let authority = self::authority(&store);
        let cfg = config();
        let mut req = request(&token);
        req.url = "https://ai.ubq.fi/v1/live/calls".into();
        let actor = authenticate_client_actor(
            &authority,
            &host,
            &cfg,
            &req,
            &context("api-key:api-fixture", Some("api-fixture")),
        )
        .await
        .unwrap();
        assert!(current(&authority, &actor, &cfg).await.is_ok());
        assert!(
            !store
                .rows
                .lock()
                .unwrap()
                .contains_key(&keys::api_policy("api-fixture").0)
        );
        assert!(
            store
                .rows
                .lock()
                .unwrap()
                .keys()
                .all(|key| !key.contains("routing_runtime"))
        );
    });
}
