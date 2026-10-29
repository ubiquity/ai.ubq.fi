//! Exact data schemas only. Legacy proof-only rows never become effect capabilities.
use crate::OWNER_RENEWAL_MAX_MS;
use gateway_auth_native_contracts::native_import::{
    NativeImportKind, NativeImportMetadata, NativeImportTarget,
};
use gateway_authority_actor_records::{legacy::StoredNativeActor, scope::NativeActorScope};
use gateway_authority_native_records::import_action::ImportActionRow;
use gateway_source::error::GatewayError;
use gateway_source::source_json::SourceJson;
use serde::{Deserialize, Serialize};

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct LegacyFacts {
    owner_id: String,
    owner_epoch: String,
    native_owner_id: String,
    slot: u64,
    account_id_hash: String,
    credential_version: String,
    native_generation: String,
    checks_digest: String,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct LegacyAction {
    v: u8,
    kind: NativeImportKind,
    metadata: NativeImportMetadata,
    target: NativeImportTarget,
    actor: StoredNativeActor,
    context: gateway_contracts::identity::RequestContext,
    created_at_ms: i64,
    deadline_ms: i64,
    facts: LegacyFacts,
}
fn exact<T: Serialize>(value: &SourceJson, row: &T) -> Result<(), GatewayError> {
    let scalar: serde_json::Value = value.deserialize().map_err(|_| unavailable())?;
    if serde_json::to_value(row).map_err(|_| unavailable())? != scalar {
        return Err(unavailable());
    }
    crate::renewal::actor_shape(&scalar["actor"])
}
fn common(
    kind: (u8, NativeImportKind),
    selection: (&NativeImportMetadata, &NativeImportTarget),
    original: (
        &StoredNativeActor,
        &gateway_contracts::identity::RequestContext,
    ),
    lifetime: (i64, i64),
    id: &str,
) -> Result<(), GatewayError> {
    let (version, kind) = kind;
    let (metadata, target) = selection;
    let (actor, context) = original;
    let (created, deadline) = lifetime;
    metadata.validate()?;
    target.validate()?;
    if !matches!(version, 1..=3)
        || kind != NativeImportKind::CodexAuth
        || metadata.action_ref != id
        || actor.scope() != NativeActorScope::Admin
        || context
            .principal
            .try_scalar()
            .map_err(|_| unavailable())?
            .as_str()
            != actor.principal()
        || deadline != gateway_authority_contracts::add(created, OWNER_RENEWAL_MAX_MS)?
    {
        return Err(unavailable());
    }
    for text in [&context.request_id.0, &context.route] {
        gateway_authority_contracts::text(text)?;
    }
    for time in [created, deadline, context.started_at_ms] {
        gateway_authority_contracts::validate_time(time)?;
    }
    Ok(())
}
pub(crate) fn validate_custody(value: &SourceJson, id: &str) -> Result<(), GatewayError> {
    let version = gateway_source::source_serialization::member(value, "v")
        .and_then(|v| v.deserialize::<u8>().ok())
        .ok_or_else(unavailable)?;
    if version == 1 {
        let row: LegacyAction = value.deserialize().map_err(|_| unavailable())?;
        common(
            (row.v, row.kind),
            (&row.metadata, &row.target),
            (&row.actor, &row.context),
            (row.created_at_ms, row.deadline_ms),
            id,
        )?;
        let NativeImportTarget::ExistingAccount { account_id_hash } = &row.target else {
            return Err(unavailable());
        };
        if row.facts.account_id_hash != *account_id_hash
            || row.facts.slot > gateway_contracts::identity::MAX_SAFE_INTEGER as u64
        {
            return Err(unavailable());
        }
        for hash in [
            id,
            account_id_hash,
            &row.facts.credential_version,
            &row.facts.native_generation,
            &row.facts.checks_digest,
        ] {
            crate::renewal::hash(hash)?;
        }
        for text in [
            &row.facts.owner_id,
            &row.facts.owner_epoch,
            &row.facts.native_owner_id,
        ] {
            gateway_authority_contracts::text(text)?;
        }
        return exact(value, &row);
    }
    let row: ImportActionRow = value.deserialize().map_err(|_| unavailable())?;
    common(
        (row.v, row.kind),
        (&row.metadata, &row.target),
        (&row.actor, &row.context),
        (row.created_at_ms, row.deadline_ms),
        id,
    )?;
    let NativeImportTarget::ExistingAccount { account_id_hash } = &row.target else {
        return Err(unavailable());
    };
    let facts = &row.facts;
    if !matches!(row.v, 2 | 3)
        || facts.account_id_hash != *account_id_hash
        || facts.slot >= facts.account_digests.len() as u64
    {
        return Err(unavailable());
    }
    for hash in [
        id,
        account_id_hash,
        &facts.credential_version,
        &facts.access_generation,
        &facts.source_native_generation,
        &facts.checks_digest,
        &facts.control_checks_digest,
    ]
    .into_iter()
    .chain(facts.account_digests.iter().map(String::as_str))
    {
        crate::renewal::hash(hash)?;
    }
    for text in [
        &facts.owner_id,
        &facts.owner_epoch,
        &facts.native_owner_id,
        &facts.owner_revision,
        &facts.source_revision,
        &facts.normalized_revision,
    ]
    .into_iter()
    .chain(facts.runtime_revision.iter())
    {
        gateway_authority_contracts::text(text)?;
    }
    exact(value, &row)
}

fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native import preparation is unavailable, changed, expired, or unsupported.",
        "native_import_authority_unavailable",
    )
}
