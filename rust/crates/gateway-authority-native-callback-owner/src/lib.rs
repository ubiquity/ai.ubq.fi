//! Fixed callback resource observations. A selected configured owner is not effect authority.
mod snapshot;
use gateway_auth_credentials::provider_credentials::{
    codex_source_pool_key, parse_codex_source_pool,
};
use gateway_auth_native_wire::control::{NativeControlEnvelope, NativeControlRequest};
use gateway_authority_contracts::{AuthorityClock, text, unavailable, validate_time};
use gateway_authority_keys as keys;
use gateway_authority_native_records::{
    owner::RegisteredRenewalOwner,
    rows::{self, ActionRow, OperationRow, fail},
    source_rows::SourceRenewalActionRowV3,
};
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_core::{source_control::decode_source_control_record, state::AuthorityStore};
use gateway_native_authority_request::NativeRenewalAuthorityRequest as Request;
use gateway_source::{error::GatewayError, source_serialization as source};
use snapshot::Snapshot;

#[derive(Clone, Copy, Eq, PartialEq, Debug)]
pub enum RenewalActionVersion {
    Legacy2,
    Source3,
}
pub struct SelectedCallbackOwner<'a> {
    owner: &'a RegisteredRenewalOwner,
    version: RenewalActionVersion,
}
impl<'a> SelectedCallbackOwner<'a> {
    pub fn owner(&self) -> &'a RegisteredRenewalOwner {
        self.owner
    }
    pub fn version(&self) -> RenewalActionVersion {
        self.version
    }
}
pub struct CallbackOwnerSelector<'s, 'o, S> {
    store: &'s S,
    clock: Option<&'s dyn AuthorityClock>,
    owners: &'o [RegisteredRenewalOwner],
}
enum Action {
    Legacy(ActionRow),
    Source(SourceRenewalActionRowV3),
}
impl Action {
    fn reference(&self) -> &str {
        match self {
            Self::Legacy(row) => &row.action_ref,
            Self::Source(row) => &row.action_ref,
        }
    }
    fn operation(&self) -> Option<&str> {
        match self {
            Self::Legacy(row) => row.operation_id.as_deref(),
            Self::Source(row) => row.operation_id.as_deref(),
        }
    }
    fn account(&self) -> &str {
        match self {
            Self::Legacy(row) => &row.fence.account_id_hash,
            Self::Source(row) => &row.fence.account_id_hash,
        }
    }
    fn owner(&self) -> &str {
        match self {
            Self::Legacy(row) => &row.fence.native_owner_id,
            Self::Source(row) => &row.fence.native_owner_id,
        }
    }
    fn version(&self) -> RenewalActionVersion {
        match self {
            Self::Legacy(_) => RenewalActionVersion::Legacy2,
            Self::Source(_) => RenewalActionVersion::Source3,
        }
    }
}
impl<'s, 'o, S: AuthorityStore> CallbackOwnerSelector<'s, 'o, S> {
    pub fn new(
        store: &'s S,
        clock: Option<&'s dyn AuthorityClock>,
        owners: &'o [RegisteredRenewalOwner],
    ) -> Self {
        Self {
            store,
            clock,
            owners,
        }
    }
    fn time(&self, anchor: i64) -> Result<i64, GatewayError> {
        let now = self
            .clock
            .ok_or_else(|| {
                unavailable(
                    "authority_clock_unavailable",
                    "Time-sensitive authority actions require a trusted current clock.",
                )
            })?
            .now_ms()?;
        validate_time(now)?;
        if now < anchor {
            return Err(unavailable(
                "invalid_clock",
                "Authority clock moved backwards.",
            ));
        }
        Ok(now)
    }
    async fn action(
        &self,
        tx: &mut Snapshot<'_, S>,
        reference: &str,
        paired: bool,
    ) -> Result<Action, GatewayError> {
        let key = rows::key("action", reference);
        if paired {
            let raw = tx.record(&key).await?.ok_or_else(fail)?;
            let document = decode_source_control_record(&raw)?;
            if source::member(&document, "v").and_then(|v| v.deserialize::<u8>().ok()) == Some(3) {
                let row = SourceRenewalActionRowV3::from_source_json(&document)?;
                if row.v != 3 || row.action_ref != reference || row.fence.v != 3 {
                    return Err(fail());
                }
                return Ok(Action::Source(row));
            }
        }
        let row: ActionRow = tx.read(&key).await?.ok_or_else(fail)?;
        if row.v != 2
            || row.action_ref != reference
            || row.fence.v != gateway_native_authority_wire::renewal::NATIVE_RENEWAL_FENCE_VERSION
        {
            return Err(fail());
        }
        Ok(Action::Legacy(row))
    }
    async fn account(
        &self,
        tx: &mut Snapshot<'_, S>,
        hash: &str,
        required_owner: &str,
    ) -> Result<&'o RegisteredRenewalOwner, GatewayError> {
        let source_key = codex_source_pool_key();
        let raw: serde_json::Value = tx.read(&source_key).await?.ok_or_else(fail)?;
        let source = parse_codex_source_pool(&raw)?.ok_or_else(fail)?;
        let normalized: SubscriptionPool = tx.read(&keys::pool()).await?.ok_or_else(fail)?;
        gateway_authority_routing_evidence::validation::validate_pool(&normalized)?;
        if normalized.pool_versionstamp.0 != tx.revision(&source_key).ok_or_else(fail)?
            || normalized.accounts.len() != source.accounts.len()
        {
            return Err(fail());
        }
        let (slot, account) = source
            .accounts
            .iter()
            .enumerate()
            .find(|(_, a)| a.identity().routing_account_hash.0 == hash)
            .ok_or_else(fail)?;
        let identity = account.identity();
        let projected = &normalized.accounts[slot];
        if projected.account_id_hash != identity.routing_account_hash
            || projected.admin_account_hash != identity.administrative_account_hash
            || projected.credential_version != identity.credential_version
        {
            return Err(fail());
        }
        let native = account.native_owner.as_ref().ok_or_else(fail)?;
        let owner = self
            .owners
            .iter()
            .find(|o| o.registered_home() == native.codex_home)
            .ok_or_else(fail)?;
        if required_owner != owner.native_owner_id() {
            return Err(fail());
        }
        Ok(owner)
    }
    async fn select_action(
        &self,
        reference: &str,
        paired: bool,
    ) -> Result<SelectedCallbackOwner<'o>, GatewayError> {
        text(reference)?;
        for _ in 0..3 {
            let mut tx = Snapshot::new(self.store, self.time(0)?);
            let action = self.action(&mut tx, reference, paired).await?;
            let owner = self
                .account(&mut tx, action.account(), action.owner())
                .await?;
            self.time(tx.now)?;
            if tx.commit().await? {
                return Ok(SelectedCallbackOwner {
                    owner,
                    version: action.version(),
                });
            }
        }
        Err(fail())
    }
    async fn select_operation(
        &self,
        operation_id: &str,
        claim_ref: &str,
        paired: bool,
        owned_cleanup: bool,
    ) -> Result<SelectedCallbackOwner<'o>, GatewayError> {
        text(operation_id)?;
        text(claim_ref)?;
        for _ in 0..3 {
            let mut tx = Snapshot::new(self.store, self.time(0)?);
            let operation: OperationRow = tx
                .read(&rows::key("operation", operation_id))
                .await?
                .ok_or_else(fail)?;
            if operation.v != 1
                || operation.operation_id != operation_id
                || operation.claim_ref != claim_ref
            {
                return Err(fail());
            }
            let action = self.action(&mut tx, &operation.action_ref, paired).await?;
            if action.reference() != operation.action_ref
                || action.operation() != Some(operation_id)
            {
                return Err(fail());
            }
            let owner = if owned_cleanup && action.version() == RenewalActionVersion::Source3 {
                self.owners
                    .iter()
                    .find(|owner| owner.native_owner_id() == action.owner())
                    .ok_or_else(fail)?
            } else {
                self.account(&mut tx, action.account(), action.owner())
                    .await?
            };
            self.time(tx.now)?;
            if tx.commit().await? {
                return Ok(SelectedCallbackOwner {
                    owner,
                    version: action.version(),
                });
            }
        }
        Err(fail())
    }
    pub async fn legacy_action(
        &self,
        reference: &str,
    ) -> Result<&'o RegisteredRenewalOwner, GatewayError> {
        Ok(self.select_action(reference, false).await?.owner())
    }
    pub async fn legacy_operation(
        &self,
        operation_id: &str,
        claim_ref: &str,
    ) -> Result<&'o RegisteredRenewalOwner, GatewayError> {
        Ok(self
            .select_operation(operation_id, claim_ref, false, false)
            .await?
            .owner())
    }
    pub async fn callback(
        &self,
        request: &Request,
    ) -> Result<SelectedCallbackOwner<'o>, GatewayError> {
        request.validate()?;
        match request {
            Request::Claim { .. } => {
                let envelope = NativeControlEnvelope::parse(&request.original_control_body()?)?;
                let NativeControlRequest::CodexRenewAccess { action_ref } = envelope.request;
                self.select_action(&action_ref, true).await
            }
            Request::Unknown {
                operation_id,
                claim_ref,
                ..
            } => {
                self.select_operation(operation_id, claim_ref, true, true)
                    .await
            }
            Request::Start {
                operation_id,
                claim_ref,
                ..
            }
            | Request::Recheck {
                operation_id,
                claim_ref,
                ..
            }
            | Request::Published {
                operation_id,
                claim_ref,
                ..
            } => {
                self.select_operation(operation_id, claim_ref, true, false)
                    .await
            }
        }
    }
}
