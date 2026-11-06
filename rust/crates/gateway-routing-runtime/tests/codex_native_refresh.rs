//! The Codex subscription tier's credential-refresh fence at the routing boundary. A 401 asks
//! the native owner to renew the exact dispatched credential once; only a refresh the owner
//! could not publish, or a second 401 on an already-renewed version, classifies the credential
//! invalid and lets the durable global active subscription move. Transient upstream faults
//! neither refresh nor move the active account.
use gateway_authority::{keys, routing_types::RoutingState};
use gateway_core::lifecycle::TransportObservation;
use gateway_providers::policy::HopFailure;
use gateway_routing_runtime::{
    session::RoutingSession,
    types::{
        AttemptFailure, CredentialFence, FailureOrigin, PreparedAttempt, RefreshOutcome,
        RefusalDecision, TransportState,
    },
};
use gateway_routing_runtime_test_support::{exec::*, host::*, input::*, seed::*, store::*};
use std::collections::BTreeMap;

type Session = RoutingSession<Store, Store, Store, Host>;
type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;

fn provider_failure(status: u16) -> AttemptFailure {
    AttemptFailure {
        origin: FailureOrigin::Provider,
        status: Some(status),
        headers: BTreeMap::new(),
        body: Vec::new(),
        body_complete: true,
        failure: HopFailure::Status(status),
        transport: TransportState::Started,
        semantic_committed: false,
        cancelled: false,
    }
}

fn begin(store: &Store, id: &str, outcome: RefreshOutcome) -> TestResult<(Session, Host)> {
    let host = Host::new(store);
    host.codex_refresh.lock().map_err(|_| "poisoned")?.0 = outcome;
    let session = run(Session::begin(
        input(id, "gpt-test", &[]),
        store.clone(),
        host.clone(),
        store.clone(),
        store.clone(),
    ))?;
    Ok((session, host))
}

async fn dispatched(session: &Session) -> TestResult<(PreparedAttempt, String, String)> {
    let attempt = session.next_attempt().await?.ok_or("a Codex attempt")?;
    let CredentialFence::Codex { admission, .. } = &attempt.credential else {
        return Err("the subscription tier serves first".into());
    };
    let account = admission.binding.account_id_hash.0.clone();
    let version = admission.binding.credential_version.0.clone();
    session.before_dispatch(&attempt).await?;
    session.transport_started(&attempt).await?;
    Ok((attempt, account, version))
}

fn invalid_versions(store: &Store) -> TestResult<Vec<Option<String>>> {
    let state: RoutingState = serde_json::from_value(store.value(keys::routing()))?;
    Ok(state
        .slots
        .iter()
        .map(|slot| {
            slot.invalid_credential_version
                .as_ref()
                .map(|v| v.0.clone())
        })
        .collect())
}

fn refreshed(host: &Host) -> TestResult<Vec<CredentialFence>> {
    Ok(host.codex_refresh.lock().map_err(|_| "poisoned")?.1.clone())
}

#[test]
fn a_published_renewal_retries_the_same_active_account_without_invalidating_it() -> TestResult {
    let store = Store::new();
    seed_key(&store);
    seed_pool(&store);
    let (session, host) = begin(&store, "refresh-published", RefreshOutcome::Published)?;
    run(async {
        let (first, account, version) = dispatched(&session).await?;
        let decision = session.refusal(&first, provider_failure(401)).await?;
        assert_eq!(decision, RefusalDecision::Continue);
        // Exactly the dispatched fence: account hash, credential version, source revision
        // and native generation are what the native owner must prove it renewed.
        assert_eq!(refreshed(&host)?, vec![first.credential.clone()]);
        assert!(invalid_versions(&store)?.iter().all(Option::is_none));

        let (second, retried, retried_version) = dispatched(&session).await?;
        assert_eq!(
            retried, account,
            "a renewal never moves the global active subscription"
        );
        assert_eq!(retried_version, version);
        // The same credential version is renewed at most once per request; a repeated 401
        // is now a classified credential invalidity and the pool may advance.
        session.refusal(&second, provider_failure(401)).await?;
        assert_eq!(refreshed(&host)?.len(), 1);
        assert!(invalid_versions(&store)?.contains(&Some(version)));
        let (_, advanced, _) = dispatched(&session).await?;
        assert_ne!(advanced, account);
        session.finish(TransportObservation::default()).await?;
        Ok(())
    })
}

#[test]
fn an_unpublished_renewal_classifies_only_that_credential_invalid() -> TestResult {
    let store = Store::new();
    seed_key(&store);
    seed_pool(&store);
    let (session, host) = begin(&store, "refresh-unavailable", RefreshOutcome::Unavailable)?;
    run(async {
        let (first, account, version) = dispatched(&session).await?;
        assert_eq!(
            session.refusal(&first, provider_failure(401)).await?,
            RefusalDecision::Continue
        );
        assert_eq!(refreshed(&host)?.len(), 1);
        let invalid = invalid_versions(&store)?;
        assert_eq!(invalid.iter().filter(|slot| slot.is_some()).count(), 1);
        assert!(invalid.contains(&Some(version)));
        let (_, advanced, _) = dispatched(&session).await?;
        assert_ne!(advanced, account);
        session.finish(TransportObservation::default()).await?;
        Ok(())
    })
}

#[test]
fn transient_upstream_faults_neither_refresh_nor_move_the_active_account() -> TestResult {
    let store = Store::new();
    seed_key(&store);
    seed_pool(&store);
    let mut active = None;
    for (index, status) in [500, 502, 503].into_iter().enumerate() {
        let (session, host) = begin(
            &store,
            &format!("transient-{index}"),
            RefreshOutcome::Published,
        )?;
        run(async {
            let (attempt, account, _) = dispatched(&session).await?;
            assert_eq!(*active.get_or_insert(account.clone()), account);
            session.refusal(&attempt, provider_failure(status)).await?;
            session.finish(TransportObservation::default()).await?;
            assert!(refreshed(&host)?.is_empty());
            assert!(invalid_versions(&store)?.iter().all(Option::is_none));
            TestResult::Ok(())
        })?;
    }
    Ok(())
}
