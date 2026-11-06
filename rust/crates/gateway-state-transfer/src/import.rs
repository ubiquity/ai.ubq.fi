//! Resumable, idempotent import over the existing private staged protocol.
//! Every write is one `execute_staged` call, so the empty-target, ordered-key,
//! absent-key, digest-chain and one-transition-per-chunk rules are unchanged.
use crate::{decode, intent::Intent, manifest_key, progress_key, refused, root, validate_record};
use gateway_core::error::GatewayError;
use gateway_core::query::{QueryRequest, QuerySelector, QueryStore};
use gateway_migration::staged::{StageBatch, StageReceipt, StageRequest, execute_staged};
use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum TargetState {
    /// No row at all: only a `begin` frame is accepted.
    Empty,
    /// A begun, unsealed capture; `next_sequence` is the resume point.
    Staging,
    /// A sealed adoption manifest; data frames replay without writes.
    Sealed,
    /// Rows exist without a staged or sealed capture: every import refuses.
    Occupied,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ImportStatus {
    pub v: u32,
    pub state: TargetState,
    pub capture_id: Option<String>,
    pub next_sequence: u64,
    pub received_records: usize,
    pub expected_records: usize,
    pub digest: Option<String>,
    pub activation_authorized: bool,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Outcome {
    Applied,
    /// The exact frame was already committed; nothing was written.
    Replayed,
}

#[derive(Debug, Serialize)]
pub struct ImportReply {
    pub v: u32,
    pub stage: &'static str,
    pub outcome: Outcome,
    pub receipt: Option<StageReceipt>,
    pub status: ImportStatus,
}

/// Field mirror of the staged progress row; unknown extra fields are ignored
/// so the owner's private layout stays authoritative.
#[derive(Deserialize)]
struct Progress {
    header: Value,
    sequence: u64,
    received: usize,
    digest: String,
}

struct Header {
    capture_id: String,
    records: usize,
}
fn header_facts(header: &Value) -> Result<Header, GatewayError> {
    let capture_id = header
        .pointer("/manifest/capture_id")
        .and_then(Value::as_str)
        .filter(|id| crate::intent::hex(id, 64))
        .ok_or_else(|| refused("Staged header has no capture identity"))?;
    let records = header
        .get("records")
        .and_then(Value::as_u64)
        .and_then(|records| usize::try_from(records).ok())
        .ok_or_else(|| refused("Staged header has no record count"))?;
    Ok(Header {
        capture_id: capture_id.to_owned(),
        records,
    })
}

struct Observed {
    status: ImportStatus,
    header: Option<Value>,
}
fn status_of(
    state: TargetState,
    capture_id: Option<String>,
    progress: (u64, usize, usize),
    digest: Option<String>,
) -> ImportStatus {
    ImportStatus {
        v: 1,
        state,
        capture_id,
        next_sequence: progress.0,
        received_records: progress.1,
        expected_records: progress.2,
        digest,
        activation_authorized: false,
    }
}

async fn observe<S: QueryStore>(store: &S) -> Result<Observed, GatewayError> {
    if let Some(record) = store.read(&progress_key()).await? {
        let progress: Progress = decode(record.json_value()?)?;
        let header = header_facts(&progress.header)?;
        if progress.received > header.records {
            return Err(refused("Staged progress exceeds its header"));
        }
        return Ok(Observed {
            status: status_of(
                TargetState::Staging,
                Some(header.capture_id),
                (progress.sequence, progress.received, header.records),
                Some(progress.digest),
            ),
            header: Some(progress.header),
        });
    }
    if let Some(record) = store.read(&manifest_key()).await? {
        let manifest = record.json_value()?;
        let capture_id = manifest
            .get("capture_id")
            .and_then(Value::as_str)
            .filter(|id| record.revision == format!("capture:{id}"))
            .ok_or_else(|| refused("Sealed manifest identity is inconsistent"))?;
        let rows = manifest.get("source_rows").and_then(Value::as_u64);
        let derived = manifest
            .get("derived")
            .and_then(Value::as_object)
            .map(|derived| derived.len() as u64);
        let expected = rows
            .zip(derived)
            .and_then(|(rows, derived)| rows.checked_mul(2)?.checked_add(derived))
            .and_then(|expected| usize::try_from(expected).ok())
            .ok_or_else(|| refused("Sealed manifest counts are inconsistent"))?;
        return Ok(Observed {
            status: status_of(
                TargetState::Sealed,
                Some(capture_id.to_owned()),
                (0, expected, expected),
                None,
            ),
            header: None,
        });
    }
    let page = store
        .query(QueryRequest {
            selector: QuerySelector::Prefix {
                prefix: root()?,
                start: None,
                end: None,
            },
            reverse: false,
            limit: 1,
            cursor: None,
            now_ms: 0,
        })
        .await?;
    let state = if page.entries.is_empty() && page.next_cursor.is_none() {
        TargetState::Empty
    } else {
        TargetState::Occupied
    };
    Ok(Observed {
        status: status_of(state, None, (0, 0, 0), None),
        header: None,
    })
}

pub async fn status<S: QueryStore>(store: &S) -> Result<ImportStatus, GatewayError> {
    Ok(observe(store).await?.status)
}

pub fn stage_name(request: &StageRequest) -> &'static str {
    match request {
        StageRequest::Begin(_) => "begin",
        StageRequest::Append(_) => "append",
        StageRequest::Seal { .. } => "seal",
        StageRequest::BindOwner(_) => "bind_owner",
    }
}

fn capture_of(request: &StageRequest) -> Result<String, GatewayError> {
    Ok(match request {
        StageRequest::Begin(header) => {
            header_facts(
                &serde_json::to_value(header).map_err(|_| refused("Staged header is invalid"))?,
            )?
            .capture_id
        }
        StageRequest::Append(batch) => batch.capture_id.clone(),
        StageRequest::Seal { capture_id } => capture_id.clone(),
        StageRequest::BindOwner(binding) => binding.capture_id.clone(),
    })
}

/// Decide whether a frame is an exact replay of committed state (no write),
/// a frame to apply, or a refusal. Nothing here can relax a staged check.
fn replay(observed: &Observed, request: &StageRequest) -> Result<bool, GatewayError> {
    let status = &observed.status;
    match (status.state, request) {
        (TargetState::Occupied, _) => Err(refused(
            "Target holds rows without a staged capture; import requires a new empty authority",
        )),
        (TargetState::Empty, StageRequest::Begin(_)) => Ok(false),
        (TargetState::Empty, _) => Err(refused("No staged capture exists; send begin first")),
        (TargetState::Staging, StageRequest::Begin(header)) => {
            let presented =
                serde_json::to_value(header).map_err(|_| refused("Staged header is invalid"))?;
            if observed.header.as_ref() == Some(&presented) {
                Ok(true)
            } else {
                Err(refused("Target is staging a different manifest header"))
            }
        }
        (TargetState::Staging, StageRequest::Append(batch)) => {
            if batch.sequence == status.next_sequence {
                Ok(false)
            } else if batch.sequence.checked_add(1) == Some(status.next_sequence)
                && status.digest.as_deref() == Some(batch.digest.as_str())
            {
                Ok(true)
            } else {
                Err(refused(format!(
                    "Append chunk {} is out of order; resume from chunk {}",
                    batch.sequence, status.next_sequence
                )))
            }
        }
        (TargetState::Staging, StageRequest::Seal { .. }) => Ok(false),
        (TargetState::Staging, StageRequest::BindOwner(_)) => {
            Err(refused("Seal the staged capture before binding its owner"))
        }
        (TargetState::Sealed, StageRequest::BindOwner(_)) => Ok(false),
        (TargetState::Sealed, _) => Ok(true),
    }
}

/// Every record is checked before the one atomic transition is attempted.
fn validate_batch(batch: &StageBatch) -> Result<(), GatewayError> {
    if batch.records.is_empty() || !crate::intent::hex(&batch.digest, 64) {
        return Err(refused("Append chunk is empty or has no digest"));
    }
    for item in &batch.records {
        validate_record(&item.record)?;
    }
    Ok(())
}

pub async fn import<S: QueryStore>(
    store: &S,
    intent: &Intent,
    request: StageRequest,
) -> Result<ImportReply, GatewayError> {
    let capture_id = capture_of(&request)?;
    if intent.import_capture()? != capture_id {
        return Err(GatewayError::new(
            403,
            "The operator intent names a different capture",
            Some("state_transfer_intent_refused"),
        ));
    }
    let stage = stage_name(&request);
    let observed = observe(store).await?;
    if observed
        .status
        .capture_id
        .as_ref()
        .is_some_and(|current| current != &capture_id)
    {
        return Err(refused("Target holds a different capture"));
    }
    if replay(&observed, &request)? {
        return Ok(ImportReply {
            v: 1,
            stage,
            outcome: Outcome::Replayed,
            receipt: None,
            status: observed.status,
        });
    }
    if let StageRequest::Append(batch) = &request {
        validate_batch(batch)?;
    }
    let receipt = execute_staged(store, request).await?;
    if receipt.activation_authorized || receipt.capture_id != capture_id {
        return Err(refused("Staged receipt differs from the requested capture"));
    }
    Ok(ImportReply {
        v: 1,
        stage,
        outcome: Outcome::Applied,
        receipt: Some(receipt),
        status: status(store).await?,
    })
}
