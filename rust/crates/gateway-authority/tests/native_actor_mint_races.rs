use gateway_auth_passkeys::passkeys;
use gateway_authority::native_control::actor::authenticate_admin_actor;
use gateway_authority_test_support::native_actor::{
    Host, Store, authority, config, context, request, run, session_setup, user,
};
use gateway_core::state::StoredRecord;
use std::sync::atomic::Ordering;
#[test]
fn demotion_or_revocation_during_auth_await_never_captures_a_new_revision_beside_old_grants() {
    run(async {
        let store = Store::default();
        let token = session_setup(&store, true, true);
        let host = Host::new(&store);
        let authority = self::authority(&store);
        let cfg = config();
        store.race(
            passkeys::key("users", "user-fixture"),
            Some(StoredRecord::json(
                "demotion",
                serde_json::to_value(user(false)).unwrap(),
                None,
            )),
        );
        assert!(
            authenticate_admin_actor(
                &authority,
                &host,
                &cfg,
                &request(&token),
                &context("passkey-user:user-fixture", None)
            )
            .await
            .is_err()
        );
        let store = Store::default();
        let token = api_setup(&store, -1);
        let host = Host::new(&store);
        let key = api_keys::hash_key(&sha256_base64url(token.as_bytes()));
        let mut replacement = store.rows.lock().unwrap().get(&key.0).unwrap().clone();
        replacement.revision = "revoked".into();
        replacement.value["revoked_at_ms"] = json!(1);
        store.race(key, Some(replacement));
        assert!(
            authenticate_client_actor(
                &self::authority(&store),
                &host,
                &cfg,
                &request(&token),
                &context("api-key:api-fixture", Some("api-fixture"))
            )
            .await
            .is_err()
        );
    });
}

#[test]
fn user_row_absolute_expiry_crossing_readonly_commit_prevents_actor_mint() {
    run(async {
        let store = Store::default();
        let token = session_setup(&store, true, true);
        let key = passkeys::key("users", "user-fixture");
        let mut row = store.rows.lock().unwrap().get(&key.0).unwrap().clone();
        row.expires_at_ms = Some(1_000_100);
        store.rows.lock().unwrap().insert(key.0, row);
        store.commit_time.store(1_000_100, Ordering::SeqCst);
        let host = Host::new(&store);
        assert!(
            authenticate_admin_actor(
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
