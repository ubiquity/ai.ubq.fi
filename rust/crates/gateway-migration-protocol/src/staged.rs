//! Private offline operator data, never an HTTP/admin capability. The host must
//! hold a NEW empty target unpublished and quiescent until seal and reconciliation.
use gateway_authority_initial_owner::{InitialAdoptionAccountFacts, InitialAdoptionFacts};
use gateway_authority_paid_control_records::PaidControlOwner;
use gateway_core::state::StoredRecord;
use gateway_core::{
    query::{QueryRequest, QuerySelector, QueryStore},
    state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, RevisionCheck},
};
use gateway_kv_codec::key::RecordKey;
use gateway_kv_codec::tuple_key::ScalarKeyPart;
use gateway_kv_codec::tuple_key::TupleKey;
use gateway_migration_data::decode;
use gateway_migration_data::invalid;
use gateway_migration_data::json;
use gateway_migration_data::staged::{
    STAGED_BATCH_MAX_BYTES, StageHeader, StageOwnerBinding, StageReceipt, StageRequest, bounded,
    extend, initial,
};
use gateway_source::error::GatewayError;
use serde::Deserialize;
use serde::Serialize;
use std::collections::BTreeSet;

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Progress {
    header: StageHeader,
    sequence: u64,
    received: usize,
    digest: String,
    last_key: Option<String>,
}
fn progress_key() -> RecordKey {
    TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("migration"),
        ScalarKeyPart::String("v3"),
        ScalarKeyPart::String("staged"),
    ])
    .to_record_key()
}
pub fn decode_stage_request(bytes: &[u8]) -> Result<StageRequest, GatewayError> {
    if bytes.len() > STAGED_BATCH_MAX_BYTES {
        return Err(invalid("Private staged input exceeds its bound"));
    }
    serde_json::from_slice(bytes).map_err(|_| invalid("Private staged input is invalid"))
}
async fn transition<S: AuthorityStore>(
    store: &S,
    checks: Vec<RevisionCheck>,
    mutations: Vec<RecordMutation>,
) -> Result<(), GatewayError> {
    if store.commit(AtomicTransition { checks, mutations }).await? != CommitOutcome::Committed {
        return Err(invalid("Private staged adoption conflicted"));
    }
    Ok(())
}
fn check(key: RecordKey, expected_revision: Option<String>) -> RevisionCheck {
    RevisionCheck {
        key,
        expected_revision,
    }
}
fn progress_record(progress: &Progress) -> Result<StoredRecord, GatewayError> {
    Ok(StoredRecord::json(
        format!(
            "stage:{}:{}",
            progress.header.manifest.capture_id, progress.sequence
        ),
        json(progress)?,
        None,
    ))
}
fn receipt(p: &Progress, sealed: bool) -> StageReceipt {
    StageReceipt {
        capture_id: p.header.manifest.capture_id.clone(),
        source_rows: p.header.manifest.source_rows,
        derived_rows: p.header.manifest.derived.len(),
        accepted_records: p.received,
        atomic_batches: p.sequence,
        sealed,
        owner_bound: false,
        activation_authorized: false,
    }
}
async fn begin<S: QueryStore>(
    store: &S,
    header: StageHeader,
    manifest_key: RecordKey,
) -> Result<StageReceipt, GatewayError> {
    if header.manifest.v != 4
        || header.manifest.native_revision.is_none()
        || header.records
            != header
                .manifest
                .source_rows
                .checked_mul(2)
                .and_then(|v| v.checked_add(header.manifest.derived.len()))
                .ok_or_else(|| invalid("Stage count overflow"))?
        || header.digest.len() != 64
    {
        return Err(invalid("Private staged header is inconsistent"));
    }
    let page = store
        .query(QueryRequest {
            selector: QuerySelector::Prefix {
                prefix: TupleKey::from_parts(&[])?.to_record_key(),
                start: None,
                end: None,
            },
            reverse: false,
            limit: 1,
            cursor: None,
            now_ms: 0,
        })
        .await?;
    if !page.entries.is_empty() || page.next_cursor.is_some() {
        return Err(invalid("Staged target is not empty"));
    }
    let progress = Progress {
        digest: initial(&header.manifest.capture_id),
        header,
        sequence: 0,
        received: 0,
        last_key: None,
    };
    transition(
        store,
        vec![check(progress_key(), None), check(manifest_key, None)],
        vec![RecordMutation::Put {
            key: progress_key(),
            record: progress_record(&progress)?,
        }],
    )
    .await?;
    Ok(receipt(&progress, false))
}

/// Caller must be the existing trusted private host with exclusive unpublished
/// target custody. A deserialized frame grants no public/live authority.
pub async fn execute_staged<S: QueryStore>(
    store: &S,
    request: StageRequest,
) -> Result<StageReceipt, GatewayError> {
    bounded(&request)?;
    if let StageRequest::BindOwner(binding) = &request {
        return bind_owner(store, binding).await;
    }
    let manifest_key = gateway_migration_data::manifest::manifest_key();
    if store.read(&manifest_key).await?.is_some() {
        return Err(invalid("Target already adopted"));
    }
    if let StageRequest::Begin(header) = request {
        return begin(store, header, manifest_key).await;
    }
    let previous = store
        .read(&progress_key())
        .await?
        .ok_or_else(|| invalid("No private staged target"))?;
    let mut progress: Progress = decode(previous.json_value()?)?;
    let mut checks = vec![
        check(progress_key(), Some(previous.revision)),
        check(manifest_key.clone(), None),
    ];
    let mut mutations = Vec::new();
    match request {
        StageRequest::Append(batch) => {
            if batch.capture_id != progress.header.manifest.capture_id
                || batch.sequence != progress.sequence
                || batch.previous_digest != progress.digest
                || batch.records.is_empty()
                || progress
                    .received
                    .checked_add(batch.records.len())
                    .is_none_or(|n| n > progress.header.records)
            {
                return Err(invalid("Private staged batch sequence/count differs"));
            }
            for item in batch.records {
                TupleKey::from_record_key(&item.key)?.validate_stored_key()?;
                item.record.validate_version()?;
                if item.key == manifest_key
                    || item.key == progress_key()
                    || progress
                        .last_key
                        .as_ref()
                        .is_some_and(|last| last >= &item.key.0)
                {
                    return Err(invalid("Private staged key order/collision differs"));
                }
                progress.digest = extend(&progress.digest, &item)?;
                progress.last_key = Some(item.key.0.clone());
                progress.received += 1;
                checks.push(check(item.key.clone(), None));
                mutations.push(RecordMutation::Put {
                    key: item.key,
                    record: item.record,
                });
            }
            if progress.digest != batch.digest {
                return Err(invalid("Private staged batch digest differs"));
            }
            progress.sequence = progress
                .sequence
                .checked_add(1)
                .ok_or_else(|| invalid("Stage sequence overflow"))?;
            mutations.push(RecordMutation::Put {
                key: progress_key(),
                record: progress_record(&progress)?,
            });
            transition(store, checks, mutations).await?;
            Ok(receipt(&progress, false))
        }
        StageRequest::Seal { capture_id } => {
            if capture_id != progress.header.manifest.capture_id
                || progress.received != progress.header.records
                || progress.digest != progress.header.digest
            {
                return Err(invalid("Private staged target is incomplete or corrupt"));
            }
            mutations.push(RecordMutation::Put {
                key: manifest_key,
                record: StoredRecord::json(
                    format!("capture:{capture_id}"),
                    json(&progress.header.manifest)?,
                    None,
                ),
            });
            mutations.push(RecordMutation::Delete {
                key: progress_key(),
            });
            transition(store, checks, mutations).await?;
            Ok(receipt(&progress, true))
        }
        StageRequest::Begin(_) | StageRequest::BindOwner(_) => {
            unreachable!("begin/owner handled above")
        }
    }
}

fn hex(value: &str, length: usize) -> bool {
    value.len() == length
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}
fn identifier(value: &str, max: usize) -> bool {
    !value.is_empty()
        && value.len() <= max
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
}
fn decimal(value: &str) -> bool {
    value
        .parse::<i64>()
        .ok()
        .is_some_and(|number| number >= 0 && number.to_string() == value)
}
fn opaque(value: &str) -> bool {
    !value.trim().is_empty() && value.len() <= 4096
}
fn owner_refused() -> GatewayError {
    invalid("Private staged control-owner binding is inconsistent or stale")
}
fn validate_owner_binding(binding: &StageOwnerBinding) -> Result<(), GatewayError> {
    let mut accounts = BTreeSet::new();
    if !hex(&binding.capture_id, 64)
        || !hex(&binding.source_git_sha, 40)
        || !decimal(&binding.source_native_version)
        || !decimal(&binding.source_native_sequence)
        || !opaque(&binding.source_pool_revision)
        || !opaque(&binding.normalized_pool_revision)
        || !(1..=2).contains(&binding.accounts.len())
        || binding.accounts.iter().any(|account| {
            !hex(&account.account_id_hash, 64)
                || !hex(&account.access_generation, 64)
                || !accounts.insert(account.account_id_hash.clone())
        })
        || !(0..=gateway_contracts::identity::MAX_SAFE_INTEGER)
            .contains(&binding.source_captured_at_ms)
        || !(0..=gateway_contracts::identity::MAX_SAFE_INTEGER)
            .contains(&binding.custody_observed_at_ms)
        || !(0..=gateway_contracts::identity::MAX_SAFE_INTEGER).contains(&binding.valid_through_ms)
        || !(0..=gateway_contracts::identity::MAX_SAFE_INTEGER).contains(&binding.bound_at_ms)
        || binding.source_captured_at_ms > binding.custody_observed_at_ms
        || binding.valid_through_ms < binding.custody_observed_at_ms
        || binding.bound_at_ms < binding.custody_observed_at_ms
        || binding.bound_at_ms > binding.valid_through_ms
        || !identifier(&binding.owner_id, 128)
        || !identifier(&binding.owner_epoch, 128)
        || !hex(&binding.key_fingerprint, 64)
        || !opaque(&binding.owner_revision)
    {
        return Err(owner_refused());
    }
    Ok(())
}
fn owner_from_binding(binding: &StageOwnerBinding) -> PaidControlOwner {
    PaidControlOwner {
        v: 1,
        owner_id: binding.owner_id.clone(),
        owner_epoch: binding.owner_epoch.clone(),
        key_fingerprint: binding.key_fingerprint.clone(),
    }
}
async fn sealed_owner_facts<S: QueryStore>(
    store: &S,
    binding: &StageOwnerBinding,
) -> Result<
    (
        gateway_migration_data::manifest::Manifest,
        StoredRecord,
        StoredRecord,
        StoredRecord,
    ),
    GatewayError,
> {
    let manifest_record = store
        .read(&gateway_migration_data::manifest::manifest_key())
        .await?
        .ok_or_else(owner_refused)?;
    if manifest_record.revision != format!("capture:{}", binding.capture_id) {
        return Err(owner_refused());
    }
    let manifest: gateway_migration_data::manifest::Manifest =
        decode(manifest_record.json_value()?)?;
    let native = manifest
        .native_revision
        .as_ref()
        .ok_or_else(owner_refused)?;
    if manifest.v != 4
        || manifest.capture_id != binding.capture_id
        || manifest.source_git_sha != binding.source_git_sha
        || manifest.captured_at_ms != binding.source_captured_at_ms
        || native.version != binding.source_native_version
        || native.sequence != binding.source_native_sequence
        || binding.bound_at_ms < manifest.captured_at_ms
    {
        return Err(owner_refused());
    }
    let source = store
        .read(&gateway_auth_credentials::provider_credentials::codex_source_pool_key())
        .await?
        .ok_or_else(owner_refused)?;
    let normalized = store
        .read(&gateway_authority_keys::pool())
        .await?
        .ok_or_else(owner_refused)?;
    if source.revision != binding.source_pool_revision
        || normalized.revision != binding.normalized_pool_revision
    {
        return Err(owner_refused());
    }
    let pool = gateway_auth_credentials::provider_credentials::parse_codex_source_pool(
        source.json_value()?,
    )?
    .ok_or_else(owner_refused)?;
    let routing: gateway_authority_routing_records::types::SubscriptionPool =
        decode(normalized.json_value()?)?;
    if routing.v != 1
        || routing.pool_versionstamp.0 != source.revision
        || routing.accounts.len() != pool.accounts.len()
    {
        return Err(owner_refused());
    }
    let mut accounts = Vec::new();
    for (account, projection) in pool.accounts.iter().zip(&routing.accounts) {
        if !account.refresh_token.expose().is_empty() {
            return Err(owner_refused());
        }
        let owner = account.native_owner.as_ref().ok_or_else(owner_refused)?;
        let identity = account.identity();
        if projection.account_id_hash != identity.routing_account_hash
            || projection.admin_account_hash != identity.administrative_account_hash
            || projection.credential_version != identity.credential_version
            || !projection.enabled
        {
            return Err(owner_refused());
        }
        accounts.push(InitialAdoptionAccountFacts {
            account_id_hash: identity.routing_account_hash.0,
            projection_generation: owner.generation_hash.clone(),
            access_generation: identity.native_generation_hash,
        });
    }
    let actual = InitialAdoptionFacts {
        capture_id: manifest.capture_id.clone(),
        source_git_sha: manifest.source_git_sha.clone(),
        source_captured_at_ms: manifest.captured_at_ms,
        source_native_version: native.version.clone(),
        source_native_sequence: native.sequence.clone(),
        source_pool_revision: source.revision.clone(),
        normalized_pool_revision: normalized.revision.clone(),
        accounts,
    };
    let expected = InitialAdoptionFacts {
        capture_id: binding.capture_id.clone(),
        source_git_sha: binding.source_git_sha.clone(),
        source_captured_at_ms: binding.source_captured_at_ms,
        source_native_version: binding.source_native_version.clone(),
        source_native_sequence: binding.source_native_sequence.clone(),
        source_pool_revision: binding.source_pool_revision.clone(),
        normalized_pool_revision: binding.normalized_pool_revision.clone(),
        accounts: binding
            .accounts
            .iter()
            .map(|account| InitialAdoptionAccountFacts {
                account_id_hash: account.account_id_hash.clone(),
                projection_generation: account.access_generation.clone(),
                access_generation: account.access_generation.clone(),
            })
            .collect(),
    };
    if actual != expected {
        return Err(owner_refused());
    }
    Ok((manifest, manifest_record, source, normalized))
}
fn owner_receipt(
    binding: &StageOwnerBinding,
    manifest: &gateway_migration_data::manifest::Manifest,
) -> Result<StageReceipt, GatewayError> {
    Ok(StageReceipt {
        capture_id: binding.capture_id.clone(),
        source_rows: manifest.source_rows,
        derived_rows: manifest.derived.len(),
        accepted_records: manifest
            .source_rows
            .checked_mul(2)
            .and_then(|value| value.checked_add(manifest.derived.len()))
            .ok_or_else(owner_refused)?,
        atomic_batches: 1,
        sealed: true,
        owner_bound: true,
        activation_authorized: false,
    })
}
async fn bind_owner<S: QueryStore>(
    store: &S,
    binding: &StageOwnerBinding,
) -> Result<StageReceipt, GatewayError> {
    validate_owner_binding(binding)?;
    let (manifest, manifest_record, source, normalized) =
        sealed_owner_facts(store, binding).await?;
    let owner_key = gateway_authority_keys::paid_control_owner();
    let expected =
        serde_json::to_value(owner_from_binding(binding)).map_err(|_| owner_refused())?;
    let exact = |record: &StoredRecord| {
        record.revision == binding.owner_revision
            && record.expires_at_ms.is_none()
            && record.json_value().is_ok_and(|value| value == &expected)
    };
    if let Some(current) = store.read(&owner_key).await? {
        if exact(&current) {
            // Exact retry after a lost response reconciles the already-bound row.
            return owner_receipt(binding, &manifest);
        }
        return Err(invalid("Target already has a different control owner"));
    }
    let record = StoredRecord::json(&binding.owner_revision, expected.clone(), None);
    let transition = AtomicTransition {
        checks: vec![
            check(
                gateway_migration_data::manifest::manifest_key(),
                Some(manifest_record.revision),
            ),
            check(
                gateway_auth_credentials::provider_credentials::codex_source_pool_key(),
                Some(source.revision),
            ),
            check(gateway_authority_keys::pool(), Some(normalized.revision)),
            check(owner_key.clone(), None),
        ],
        mutations: vec![RecordMutation::Put {
            key: owner_key.clone(),
            record,
        }],
    };
    if store.commit(transition).await? != CommitOutcome::Committed {
        return Err(invalid("Private staged owner binding conflicted"));
    }
    let bound = store
        .read(&owner_key)
        .await?
        .ok_or_else(|| invalid("Private staged owner binding did not settle"))?;
    if !exact(&bound) {
        return Err(invalid("Private staged owner binding differs after commit"));
    }
    owner_receipt(binding, &manifest)
}
