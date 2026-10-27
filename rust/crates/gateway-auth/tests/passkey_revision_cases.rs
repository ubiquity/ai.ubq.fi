use gateway_auth::resolve::AuthService;
use gateway_auth_contracts::types::{AuthConfig, AuthHost};
use gateway_auth_passkeys::{passkeys::PasskeyService, session_index};
use gateway_core::state::*;
mod revision_fixture;
use revision_fixture::*;

#[test]
fn actual_session_creation_and_logout_publish_and_delete_one_atomic_pair()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        user(&store, true, "policy-1")?;
        let service = PasskeyService {
            store: &store,
            host: &host,
            config: &config,
        };
        let first = create_session(&store, &host, &config).await?;
        let second = create_session(&store, &host, &config).await?;
        let first_digest = session_index::digest(&first.token);
        let second_digest = session_index::digest(&second.token);
        assert_ne!(first_digest, second_digest);
        let index = session_index::read_current(&store, &first_digest, host.now_ms())
            .await?
            .ok_or("missing expected session fixture")?;
        let source = store
            .read(&gateway_auth_passkeys::passkeys::key(
                "sessions",
                &first.token,
            ))
            .await?
            .ok_or("missing expected session fixture")?;
        assert_eq!(index.fence.session_revision, source.revision);
        assert_eq!(index.fence.user_id, "fixture-user");
        let create = store
            .history
            .borrow()
            .iter()
            .find(|tx| tx.checks.len() == 2 && tx.mutations.len() == 2)
            .ok_or("missing paired session transition")?
            .clone();
        assert_eq!(create.checks.len(), 2);
        assert_eq!(create.mutations.len(), 2);
        let auth = AuthService::new(&store, &host, &config);
        assert!(
            auth.authenticate_admin(&request(&first.token))
                .await?
                .is_admin
        );
        let result = service.logout(&request(&first.token), None).await?;
        assert_eq!(result.status, 204);
        assert_eq!(
            service.logout(&request(&first.token), None).await?.status,
            204
        );
        assert!(
            store
                .read(&gateway_auth_passkeys::passkeys::key(
                    "sessions",
                    &first.token
                ))
                .await?
                .is_none()
        );
        assert!(
            session_index::read_current(&store, &first_digest, host.now_ms())
                .await?
                .is_none()
        );
        assert!(
            session_index::read_current(&store, &second_digest, host.now_ms())
                .await?
                .is_some()
        );
        let delete = store
            .history
            .borrow()
            .last()
            .ok_or("missing expected session fixture")?
            .clone();
        assert_eq!(delete.checks.len(), 2);
        assert_eq!(delete.mutations.len(), 2);
        Ok(())
    })
}

#[test]
fn creation_conflicts_and_unknown_writes_never_detach_a_valid_index()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        for fault in [0, 1, 2] {
            let store = Store::default();
            let host = Host::default();
            let config = AuthConfig::default();
            match fault {
                0 => store.conflict.set(true),
                1 => store.unknown_before.set(true),
                _ => store.unknown_after.set(true),
            }
            assert!(create_session(&store, &host, &config).await.is_err());
            let tx = store
                .history
                .borrow()
                .iter()
                .find(|tx| tx.checks.len() == 2 && tx.mutations.len() == 2)
                .ok_or("missing paired session transition")?
                .clone();
            assert_eq!(tx.checks.len(), 2);
            assert_eq!(tx.mutations.len(), 2);
            let all_rows = store.rows.borrow();
            let rows = all_rows.iter().filter(|(key,_)|tx.mutations.iter().any(|mutation|matches!(mutation,RecordMutation::Put{key:selected,..} if &selected.0==*key))).map(|(key,row)|(key.clone(),row.clone())).collect::<std::collections::BTreeMap<_,_>>();
            assert!(rows.is_empty() || rows.len() == 2);
            for mutation in tx.mutations {
                let RecordMutation::Put { key, record } = mutation else {
                    panic!("create was not a put");
                };
                assert_eq!(
                    rows.get(&key.0),
                    if fault == 2 { Some(&record) } else { None }
                );
            }
        }
        Ok(())
    })
}

#[test]
fn logout_cannot_report_success_on_conflict_or_unknown_write()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        for fault in [0, 1, 2] {
            let store = Store::default();
            let host = Host::default();
            let config = AuthConfig::default();
            user(&store, true, "policy")?;
            let service = PasskeyService {
                store: &store,
                host: &host,
                config: &config,
            };
            let value = create_session(&store, &host, &config).await?;
            match fault {
                0 => store.conflict.set(true),
                1 => store.unknown_before.set(true),
                _ => store.unknown_after.set(true),
            }
            assert!(service.logout(&request(&value.token), None).await.is_err());
            let source = store
                .read(&gateway_auth_passkeys::passkeys::key(
                    "sessions",
                    &value.token,
                ))
                .await?;
            let index = session_index::read_current(
                &store,
                &session_index::digest(&value.token),
                host.now_ms(),
            )
            .await?;
            assert_eq!(source.is_some(), index.is_some());
            assert_eq!(source.is_none(), fault == 2);
        }
        Ok(())
    })
}

#[test]
fn expiry_cleanup_deletes_both_and_source_ttl_bounds_the_metadata_lifetime()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        user(&store, true, "policy")?;
        let value = session("uos_ai_session_expired_fixture", host.now_ms() + 1);
        install(&store, &value, "source-1", None, true)?;
        host.now.set(value.expires_at_ms);
        let service = PasskeyService {
            store: &store,
            host: &host,
            config: &config,
        };
        assert!(
            service
                .session(&request(&value.token), None)
                .await?
                .is_none()
        );
        assert!(
            store
                .read(&gateway_auth_passkeys::passkeys::key(
                    "sessions",
                    &value.token
                ))
                .await?
                .is_none()
        );
        assert!(
            store
                .read(&session_index::index_key(&session_index::digest(
                    &value.token
                ))?)
                .await?
                .is_none()
        );
        let future = session("uos_ai_session_short_ttl_fixture", host.now_ms() + 100);
        install(&store, &future, "source-2", Some(host.now_ms() + 10), true)?;
        host.now.set(host.now_ms() + 10);
        assert!(
            service
                .session(&request(&future.token), None)
                .await?
                .is_none()
        );
        assert!(
            session_index::read_current(
                &store,
                &session_index::digest(&future.token),
                host.now_ms()
            )
            .await?
            .is_none()
        );
        Ok(())
    })
}

#[test]
fn replacement_and_current_user_revision_fence_prepared_actions()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        user(&store, true, "policy-1")?;
        let value = create_session(&store, &host, &config).await?;
        let digest = session_index::digest(&value.token);
        let index_key = session_index::index_key(&digest)?;
        let old = session_index::read_current(&store, &digest, host.now_ms())
            .await?
            .ok_or("missing expected session fixture")?;
        let user_key = gateway_auth_passkeys::passkeys::key("users", "fixture-user");
        user(&store, false, "policy-2")?;
        let auth = AuthService::new(&store, &host, &config);
        assert_eq!(
            auth.authenticate_admin(&request(&value.token))
                .await
                .err()
                .ok_or("expected administrator authorization refusal")?
                .status,
            403
        );
        let stale_user = AtomicTransition {
            checks: vec![
                RevisionCheck {
                    key: index_key.clone(),
                    expected_revision: Some(old.revision.clone()),
                },
                RevisionCheck {
                    key: user_key,
                    expected_revision: Some("policy-1".into()),
                },
            ],
            mutations: vec![],
        };
        assert_eq!(store.commit(stale_user).await?, CommitOutcome::Conflict);
        let source_key = gateway_auth_passkeys::passkeys::key("sessions", &value.token);
        let replacement_revision = "replacement-session-revision";
        let replacement = AtomicTransition {
            checks: vec![
                RevisionCheck {
                    key: source_key.clone(),
                    expected_revision: Some(old.fence.session_revision.clone()),
                },
                RevisionCheck {
                    key: index_key.clone(),
                    expected_revision: Some(old.revision.clone()),
                },
            ],
            mutations: vec![
                RecordMutation::Put {
                    key: source_key,
                    record: StoredRecord::json(
                        replacement_revision,
                        serde_json::to_value(&value)?,
                        Some(value.expires_at_ms),
                    ),
                },
                RecordMutation::Put {
                    key: index_key.clone(),
                    record: session_index::record_for_session(
                        &value,
                        replacement_revision,
                        Some(value.expires_at_ms),
                    )?,
                },
            ],
        };
        assert_eq!(store.commit(replacement).await?, CommitOutcome::Committed);
        assert_eq!(
            store
                .commit(AtomicTransition {
                    checks: vec![RevisionCheck {
                        key: index_key,
                        expected_revision: Some(old.revision)
                    }],
                    mutations: vec![]
                })
                .await?,
            CommitOutcome::Conflict
        );
        assert_ne!(
            session_index::read_current(&store, &digest, host.now_ms())
                .await?
                .ok_or("missing expected session fixture")?
                .fence
                .session_revision,
            old.fence.session_revision
        );
        Ok(())
    })
}
