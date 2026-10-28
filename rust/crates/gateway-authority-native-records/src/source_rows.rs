//! Explicit source actor2 action rows. These records never reconstruct a live phase.
use crate::{import_facts::ImportFacts, required::required_nullable};
use gateway_auth_native_contracts::native_import::{
    NativeImportKind, NativeImportMetadata, NativeImportTarget,
};
use gateway_auth_native_wire::publication_data::ImportPublicationDataV1;
use gateway_authority_actor_records::{context, source::StoredNativeActorV2, versions};
use gateway_contracts::identity::RequestContext;
use gateway_native_authority_wire::source_fence::SourceNativeRenewalFenceV3;
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde::{Deserialize, Serialize};
#[derive(Clone)]
pub struct SourceRenewalActionRowV3 {
    pub v: u8,
    pub action_ref: String,
    pub actor: StoredNativeActorV2,
    pub context: RequestContext,
    pub created_at_ms: i64,
    pub deadline_ms: i64,
    pub fence: SourceNativeRenewalFenceV3,
    pub checks_digest: String,
    pub control_checks_digest: String,
    pub account_digests: Vec<String>,
    pub operation_id: Option<String>,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct RenewalData {
    v: u8,
    action_ref: String,
    actor: SourceJson,
    context: SourceJson,
    created_at_ms: i64,
    deadline_ms: i64,
    fence: SourceNativeRenewalFenceV3,
    checks_digest: String,
    control_checks_digest: String,
    account_digests: Vec<String>,
    operation_id: Option<String>,
}
impl SourceRenewalActionRowV3 {
    pub fn to_source_json(&self) -> Result<SourceJson, GatewayError> {
        self.validate_data()?;
        SourceJson::from_serializable(&RenewalData {
            v: self.v,
            action_ref: self.action_ref.clone(),
            actor: self.actor.to_source_json()?,
            context: context::to_source_json(&self.context)?,
            created_at_ms: self.created_at_ms,
            deadline_ms: self.deadline_ms,
            fence: self.fence.clone(),
            checks_digest: self.checks_digest.clone(),
            control_checks_digest: self.control_checks_digest.clone(),
            account_digests: self.account_digests.clone(),
            operation_id: self.operation_id.clone(),
        })
    }
    pub fn from_source_json(value: &SourceJson) -> Result<Self, GatewayError> {
        let raw: RenewalData = value.deserialize()?;
        if raw.v != versions::RENEWAL_ACTION_V3 {
            return Err(invalid());
        }
        let row = Self {
            v: raw.v,
            action_ref: raw.action_ref,
            actor: StoredNativeActorV2::from_source_json(&raw.actor)?,
            context: context::from_source_json(&raw.context)?,
            created_at_ms: raw.created_at_ms,
            deadline_ms: raw.deadline_ms,
            fence: raw.fence,
            checks_digest: raw.checks_digest,
            control_checks_digest: raw.control_checks_digest,
            account_digests: raw.account_digests,
            operation_id: raw.operation_id,
        };
        row.validate_data()?;
        Ok(row)
    }
    fn validate_data(&self) -> Result<(), GatewayError> {
        versions::validate_renewal_pair(
            self.actor.v,
            self.v,
            self.fence.v,
            versions::AUTHORITY_RESPONSE_V2,
        )?;
        if self.actor.principal != self.context.principal
            || self.actor.principal.as_source() != &self.fence.principal
            || self.context.request_id.0 != self.fence.original_request_id
            || self.actor.policy_digest()? != self.fence.principal_policy_digest
        {
            return Err(invalid());
        }
        Ok(())
    }
}
#[derive(Clone)]
pub struct SourceImportActionRowV4 {
    pub v: u8,
    pub kind: NativeImportKind,
    pub metadata: NativeImportMetadata,
    pub target: NativeImportTarget,
    pub actor: StoredNativeActorV2,
    pub context: RequestContext,
    pub created_at_ms: i64,
    pub deadline_ms: i64,
    pub facts: ImportFacts,
    pub publication: Option<ImportPublicationDataV1>,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct ImportData {
    v: u8,
    kind: NativeImportKind,
    metadata: NativeImportMetadata,
    target: NativeImportTarget,
    actor: SourceJson,
    context: SourceJson,
    created_at_ms: i64,
    deadline_ms: i64,
    facts: ImportFacts,
    #[serde(deserialize_with = "required_nullable")]
    publication: Option<ImportPublicationDataV1>,
}
impl SourceImportActionRowV4 {
    pub fn to_source_json(&self) -> Result<SourceJson, GatewayError> {
        self.validate_data()?;
        SourceJson::from_serializable(&ImportData {
            v: self.v,
            kind: self.kind,
            metadata: self.metadata.clone(),
            target: self.target.clone(),
            actor: self.actor.to_source_json()?,
            context: context::to_source_json(&self.context)?,
            created_at_ms: self.created_at_ms,
            deadline_ms: self.deadline_ms,
            facts: self.facts.clone(),
            publication: self.publication.clone(),
        })
    }
    pub fn from_source_json(value: &SourceJson) -> Result<Self, GatewayError> {
        let raw: ImportData = value.deserialize()?;
        if raw.v != versions::IMPORT_ACTION_V4 {
            return Err(invalid());
        }
        let row = Self {
            v: raw.v,
            kind: raw.kind,
            metadata: raw.metadata,
            target: raw.target,
            actor: StoredNativeActorV2::from_source_json(&raw.actor)?,
            context: context::from_source_json(&raw.context)?,
            created_at_ms: raw.created_at_ms,
            deadline_ms: raw.deadline_ms,
            facts: raw.facts,
            publication: raw.publication,
        };
        row.validate_data()?;
        Ok(row)
    }
    fn validate_data(&self) -> Result<(), GatewayError> {
        versions::validate_import_pair(self.actor.v, self.v)?;
        if self.actor.principal != self.context.principal {
            return Err(invalid());
        }
        if let Some(publication) = &self.publication {
            publication.validate()?;
        }
        Ok(())
    }
}
#[derive(Clone)]
pub struct SourceLiveActionRowV2 {
    pub v: u8,
    pub action_ref: String,
    pub request_id: String,
    pub actor: StoredNativeActorV2,
    pub context: RequestContext,
    pub call_id: String,
    pub account_id: String,
    pub mapping_revision: String,
    pub created_at_ms: i64,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct LiveData {
    v: u8,
    action_ref: String,
    request_id: String,
    actor: SourceJson,
    context: SourceJson,
    call_id: String,
    account_id: String,
    mapping_revision: String,
    created_at_ms: i64,
}
impl SourceLiveActionRowV2 {
    pub fn to_source_json(&self) -> Result<SourceJson, GatewayError> {
        self.validate_data()?;
        SourceJson::from_serializable(&LiveData {
            v: self.v,
            action_ref: self.action_ref.clone(),
            request_id: self.request_id.clone(),
            actor: self.actor.to_source_json()?,
            context: context::to_source_json(&self.context)?,
            call_id: self.call_id.clone(),
            account_id: self.account_id.clone(),
            mapping_revision: self.mapping_revision.clone(),
            created_at_ms: self.created_at_ms,
        })
    }
    pub fn from_source_json(value: &SourceJson) -> Result<Self, GatewayError> {
        let raw: LiveData = value.deserialize()?;
        if raw.v != versions::LIVE_ACTION_V2 {
            return Err(invalid());
        }
        let row = Self {
            v: raw.v,
            action_ref: raw.action_ref,
            request_id: raw.request_id,
            actor: StoredNativeActorV2::from_source_json(&raw.actor)?,
            context: context::from_source_json(&raw.context)?,
            call_id: raw.call_id,
            account_id: raw.account_id,
            mapping_revision: raw.mapping_revision,
            created_at_ms: raw.created_at_ms,
        };
        row.validate_data()?;
        Ok(row)
    }
    fn validate_data(&self) -> Result<(), GatewayError> {
        versions::validate_live_pair(
            self.actor.v,
            self.v,
            versions::LiveFenceForm::SourceV2,
            versions::LIVE_RESPONSE_V2,
        )?;
        if self.actor.principal != self.context.principal
            || self.request_id != self.context.request_id.0
        {
            return Err(invalid());
        }
        Ok(())
    }
}
fn invalid() -> GatewayError {
    GatewayError::unavailable(
        "Versioned native action DATA is invalid.",
        "native_action_data_invalid",
    )
}
