//! Synthetic fixture composition through the real staged and provisioning reducers.
//! Not a shipping entrypoint, native custody claim, or public import capability.
use gateway_auth::crypto::sha256_hex;
use gateway_authority_provisioning::{
    intent::{Intent, Scope, authorization_digest, token},
    model::*,
    policy,
};
use gateway_core::error::GatewayError;
use gateway_migration::{
    native_capture::{NativeCapture, NativeRevision, prepare_native},
    staged::{StagePlan, StageRequest},
};
use gateway_worker_authority::client::RemoteAuthorityStore;

async fn status(store: &RemoteAuthorityStore) -> Result<Status, GatewayError> {
    match store.coordination(Operation::Status).await? {
        Reply::Status { status } => Ok(status),
        _ => Err(super::invalid()),
    }
}
fn entropy() -> Result<String, GatewayError> {
    Ok(sha256_hex(
        gateway_worker::transport::random::opaque_token()?.as_bytes(),
    ))
}
async fn authorized(
    store: &RemoteAuthorityStore,
    authorization: Authorization,
) -> Result<Reply, GatewayError> {
    let purpose = match &authorization {
        Authorization::FinalizeImport(_) => Purpose::FinalizeImport,
        Authorization::Activate(_) => Purpose::Activate,
        Authorization::Drain(_) => Purpose::Drain,
    };
    let value = token(
        &Scope::Control {
            purpose,
            authorization_digest: authorization_digest(&authorization)?,
        },
        &"6".repeat(64),
    )?;
    let digest = sha256_hex(value.as_bytes());
    let verified = Intent::verify(Some(&value), Some(&digest))?;
    store
        .coordination(Operation::Control {
            intent_digest: verified.require_control(&authorization)?.into(),
            authorization,
        })
        .await
}
async fn frame(
    store: &RemoteAuthorityStore,
    intent: &Intent,
    import: &ImportState,
    request: StageRequest,
) -> Result<(), GatewayError> {
    let capability = entropy()?;
    let scoped = store
        .acquire(
            Acquire {
                id: entropy()?,
                capability_sha256: sha256_hex(capability.as_bytes()),
                kind: LeaseKind::ImportFrame(import.clone()),
                parent: None,
            },
            capability,
        )
        .await?;
    let result = gateway_state_transfer::import::import(&scoped, intent, request).await;
    scoped
        .finish_scope(result.as_ref().err().map(|_| Blocker::RequiredSettlement))
        .await?;
    result.map(drop)
}
async fn empty_import(
    store: &RemoteAuthorityStore,
    gate: &Gate,
) -> Result<FrozenRef, GatewayError> {
    let capture = NativeCapture {
        v: 2,
        source_git_sha: "1".repeat(40),
        captured_at_ms: 1,
        source_codec: "bun-jsc-v1".into(),
        native_schema_version: 3,
        data_revision: NativeRevision {
            version: "0".into(),
            sequence: "0".into(),
        },
        queue_entries: 0,
        running_queue_entries: 0,
        entries: Vec::new(),
    };
    let prepared = prepare_native(&capture).map_err(|_| super::invalid())?;
    let mut plan = StagePlan::from_prepared(prepared)?;
    let summary = plan.summary();
    let begin = plan.begin();
    let value = serde_json::to_value(&begin).map_err(|_| super::invalid())?;
    let header = value.get("data").ok_or_else(super::invalid)?;
    let value = token(
        &Scope::Import {
            capture_id: summary.capture_id.clone(),
        },
        &"5".repeat(64),
    )?;
    let digest = sha256_hex(value.as_bytes());
    let intent = Intent::verify(Some(&value), Some(&digest))?;
    let import = ImportState {
        capture_id: summary.capture_id.clone(),
        intent_digest: digest,
        header_digest: sha256_hex(&serde_json::to_vec(header).map_err(|_| super::invalid())?),
        records: summary.records as u64,
        chain_digest: summary.digest.clone(),
    };
    frame(store, &intent, &import, begin).await?;
    while let Some(batch) = plan.next_batch()? {
        frame(store, &intent, &import, batch).await?;
    }
    frame(store, &intent, &import, plan.seal()).await?;
    let authorization = Authorization::FinalizeImport(FinalizePlan {
        binding: Binding {
            identity: gate.identity.clone(),
            serving_epoch: gate.serving_epoch,
        },
        capture_id: summary.capture_id.clone(),
        staged_records: summary.records as u64,
        final_chain_digest: summary.digest,
        manifest_revision: format!("capture:{}", summary.capture_id),
        expected_owner: OwnerExpectation::Absent,
    });
    match authorized(store, authorization).await? {
        Reply::Frozen { reference } => Ok(reference),
        _ => Err(super::invalid()),
    }
}

/// Returns a tracked setup root after explicit synthetic import/finalize/activation.
pub async fn begin(
    store: &RemoteAuthorityStore,
) -> Result<Option<RemoteAuthorityStore>, GatewayError> {
    let reader = store.operator_read();
    let current = status(&reader).await?;
    match current.gate.phase {
        Phase::LegacyRunning => return Ok(Some(store.clone())),
        Phase::Provisioning | Phase::Importing => {
            let reference = empty_import(&reader, &current.gate).await?;
            authorized(&reader, Authorization::Activate(reference)).await?;
        }
        Phase::Frozen => {
            if current.gate.activation_digest.is_some() {
                return Ok(None);
            }
            let reference = current.gate.frozen.ok_or_else(super::invalid)?;
            authorized(&reader, Authorization::Activate(reference)).await?;
        }
        Phase::Running => {}
        Phase::Draining => return Ok(None),
    }
    let capability = entropy()?;
    reader
        .acquire(
            Acquire {
                id: entropy()?,
                capability_sha256: sha256_hex(capability.as_bytes()),
                kind: LeaseKind::Root {
                    request_id: "fixture-setup".into(),
                },
                parent: None,
            },
            capability,
        )
        .await
        .map(Some)
}

pub async fn finish(store: &RemoteAuthorityStore, failed: bool) -> Result<(), GatewayError> {
    if matches!(store.access(), Access::Permit(_)) {
        store
            .finish_scope(failed.then_some(Blocker::RequiredSettlement))
            .await?;
    }
    Ok(())
}
