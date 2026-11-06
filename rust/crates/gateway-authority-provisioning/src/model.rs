use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Identity {
    pub object_id: String,
    pub source_sha: String,
    pub source_tree: String,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Phase {
    Provisioning,
    Importing,
    Frozen,
    Running,
    Draining,
    LegacyRunning,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ImportState {
    pub capture_id: String,
    pub intent_digest: String,
    pub header_digest: String,
    pub records: u64,
    pub chain_digest: String,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct OwnerBinding {
    pub owner_id: String,
    pub owner_epoch: String,
    pub key_fingerprint: String,
    pub owner_revision: String,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(
    tag = "kind",
    content = "binding",
    rename_all = "snake_case",
    deny_unknown_fields
)]
pub enum OwnerExpectation {
    Absent,
    Bound(OwnerBinding),
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Binding {
    pub identity: Identity,
    pub serving_epoch: u64,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct FinalizePlan {
    pub binding: Binding,
    pub capture_id: String,
    pub staged_records: u64,
    pub final_chain_digest: String,
    pub manifest_revision: String,
    pub expected_owner: OwnerExpectation,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct FrozenRef {
    pub identity: Identity,
    pub serving_epoch: u64,
    pub freeze_id: String,
    pub freeze_revision: String,
    pub capture_id: String,
    pub finalization_digest: String,
    pub roots: u64,
    pub native_operations: u64,
    pub native_joins: u64,
    pub import_frames: u64,
    pub blocked: u64,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(
    tag = "purpose",
    content = "data",
    rename_all = "snake_case",
    deny_unknown_fields
)]
pub enum Authorization {
    FinalizeImport(FinalizePlan),
    Drain(Binding),
    Activate(FrozenRef),
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Purpose {
    FinalizeImport,
    Drain,
    Activate,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Gate {
    pub v: u32,
    pub identity: Identity,
    pub serving_epoch: u64,
    pub phase: Phase,
    pub import: Option<ImportState>,
    pub frozen: Option<FrozenRef>,
    pub activation_digest: Option<String>,
    pub drain_digest: Option<String>,
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Permit {
    pub id: String,
    pub serving_epoch: u64,
    pub capability: String,
}

impl std::fmt::Debug for Permit {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Permit")
            .field("id", &self.id)
            .field("serving_epoch", &self.serving_epoch)
            .finish_non_exhaustive()
    }
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct NativeBinding {
    pub operation_id: String,
    pub action_ref: String,
    pub owner_id: String,
    pub owner_epoch: String,
    pub key_fingerprint: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct NativeSelector {
    pub operation_id: Option<String>,
    pub action_ref: Option<String>,
    pub owner_id: String,
    pub owner_epoch: String,
    pub key_fingerprint: String,
}

impl NativeSelector {
    pub fn matches(&self, binding: &NativeBinding) -> bool {
        (self.operation_id.is_some() || self.action_ref.is_some())
            && self
                .operation_id
                .as_ref()
                .is_none_or(|id| id == &binding.operation_id)
            && self
                .action_ref
                .as_ref()
                .is_none_or(|id| id == &binding.action_ref)
            && self.owner_id == binding.owner_id
            && self.owner_epoch == binding.owner_epoch
            && self.key_fingerprint == binding.key_fingerprint
    }
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(
    tag = "kind",
    content = "binding",
    rename_all = "snake_case",
    deny_unknown_fields
)]
pub enum LeaseKind {
    Root { request_id: String },
    NativeOperation(NativeBinding),
    NativeJoin(NativeBinding),
    ImportFrame(ImportState),
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Blocker {
    UncertainCommit,
    RequiredSettlement,
    NativeEffectUnknown,
    TaskAbandoned,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Lease {
    pub v: u32,
    pub id: String,
    pub serving_epoch: u64,
    pub capability_sha256: String,
    pub parent_id: Option<String>,
    pub kind: LeaseKind,
    pub blocker: Option<Blocker>,
    pub terminal: bool,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Acquire {
    pub id: String,
    pub capability_sha256: String,
    pub kind: LeaseKind,
    pub parent: Option<Permit>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(
    tag = "kind",
    content = "value",
    rename_all = "snake_case",
    deny_unknown_fields
)]
pub enum Access {
    Legacy,
    OperatorRead,
    Permit(Permit),
    Frozen(FrozenRef),
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Counts {
    pub roots: u64,
    pub native_operations: u64,
    pub native_joins: u64,
    pub import_frames: u64,
    pub blocked: u64,
}

impl Counts {
    pub fn empty(&self) -> bool {
        self.roots + self.native_operations + self.native_joins + self.import_frames + self.blocked
            == 0
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Status {
    pub gate: Gate,
    pub counts: Counts,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
pub enum Operation {
    Acquire {
        request: Acquire,
    },
    JoinNative {
        selector: NativeSelector,
        id: String,
        capability_sha256: String,
    },
    NativePublished {
        join: Permit,
    },
    Finish {
        permit: Permit,
        blocker: Option<Blocker>,
    },
    Control {
        authorization: Authorization,
        intent_digest: String,
    },
    Status,
    AwaitFrozen {
        reference_id: String,
    },
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
pub enum Reply {
    Acquired {
        serving_epoch: u64,
    },
    Finished {
        present: bool,
        status: Status,
    },
    Status {
        status: Status,
    },
    Frozen {
        reference: FrozenRef,
    },
    DrainTimeout {
        status: Status,
    },
    Activated {
        serving_epoch: u64,
        replayed: bool,
        status: Status,
    },
}

pub struct FinalizeFacts {
    pub progress_present: bool,
    pub manifest_capture_id: String,
    pub manifest_revision: String,
    pub canonical_records: u64,
    pub owner: Option<OwnerBinding>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
pub enum ControlRequest {
    FinalizeImport { plan: FinalizePlan },
    Drain { binding: Binding },
    Activate { reference: FrozenRef },
    AwaitFrozen { binding: Binding },
    Status,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ControlEnvelope {
    pub v: u32,
    pub request: ControlRequest,
}
