//! Shared key lifecycle fixtures with explicit denied provider capabilities.
use crate::{contract_host::Host, source_store::Store};
use gateway_admin_contracts::{
    operations::{AdminHost, AdminInput, PaidKeyFields, missing_capability},
    routes::AdminOperation,
};
use gateway_admin_keys::keys::{ApiKeyRecord, decode_record, deletion_key};
use gateway_auth_contracts::types::{
    AuthHost, AuthMethod, AuthPrincipal, AuthResponse, AuthResponseBody,
};
use gateway_auth_primitives::crypto::Secret;
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, RevisionCheck, StoredRecord,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde_json::{Value, json};
use std::{cell::RefCell, collections::BTreeMap};

pub fn source(value: &Value) -> SourceJson {
    SourceJson::from_serializable(value)
        .unwrap_or_else(|error| panic!("Invalid fixture source: {error}"))
}
pub fn wire_json(body: &AuthResponseBody) -> Value {
    serde_json::from_slice(
        &body
            .to_bytes()
            .unwrap_or_else(|error| panic!("Invalid fixture wire body: {error}")),
    )
    .unwrap_or_else(|error| panic!("Invalid fixture wire JSON: {error}"))
}

pub struct Capabilities {
    pub pending: bool,
}
impl AdminHost for Capabilities {
    async fn execute(
        &self,
        operation: AdminOperation,
        _: AdminInput,
    ) -> Result<AuthResponse, GatewayError> {
        Err(missing_capability(operation))
    }
    async fn prepare_key_deletion(&self, _: &str) -> Result<Vec<RevisionCheck>, GatewayError> {
        if self.pending {
            Err(GatewayError::new(
                409,
                "Cannot delete API key while metered billing is pending or unresolved",
                Some("paid_fallback_billing_outstanding"),
            ))
        } else {
            Ok(Vec::new())
        }
    }
    async fn paid_key_policy(&self, _: &SourceJson) -> Result<PaidKeyFields, GatewayError> {
        Err(GatewayError::unavailable(
            "No pricing fixture",
            "paid_fallback_pricing_unavailable",
        ))
    }
}
pub fn actor() -> AuthPrincipal {
    AuthPrincipal {
        token: Some(Secret::new("private-credential".into())),
        method: AuthMethod::AdminAllowlist,
        is_admin: true,
        is_super_admin: true,
    }
}

pub struct Pricing {
    pub inputs: RefCell<Vec<Value>>,
}
impl AdminHost for Pricing {
    async fn execute(
        &self,
        operation: AdminOperation,
        _: AdminInput,
    ) -> Result<AuthResponse, GatewayError> {
        Err(missing_capability(operation))
    }
    async fn prepare_key_deletion(&self, _: &str) -> Result<Vec<RevisionCheck>, GatewayError> {
        Err(missing_capability(AdminOperation::ApiKeysDelete))
    }
    async fn paid_key_policy(&self, raw: &SourceJson) -> Result<PaidKeyFields, GatewayError> {
        let raw: Value = raw.deserialize()?;
        self.inputs.borrow_mut().push(raw.clone());
        assert_eq!(raw["paid_fallback_enabled"], true);
        let Some(limit) = raw["paid_fallback_limit_credits"].as_f64() else {
            panic!("Fixture pricing requires a numeric credit limit");
        };
        Ok(PaidKeyFields {
            enabled: true,
            limit_microcredits: if limit == -1.0 {
                -1
            } else {
                (limit * 1_000_000.0).round() as i64
            },
            model_ids: vec!["paid-model".into()],
            quota_per_credit: 1_000_000,
            max_exposure_microcredits: BTreeMap::from([("paid-model".into(), 1000)]),
            pricing_checked_at_ms: Some(1_000_000),
        })
    }
}

pub struct ClaimedDeletion<'a> {
    pub store: &'a Store,
    pub host: &'a Host,
}
impl AdminHost for ClaimedDeletion<'_> {
    async fn execute(
        &self,
        operation: AdminOperation,
        _: AdminInput,
    ) -> Result<AuthResponse, GatewayError> {
        Err(missing_capability(operation))
    }
    async fn paid_key_policy(&self, _: &SourceJson) -> Result<PaidKeyFields, GatewayError> {
        Err(missing_capability(AdminOperation::ApiKeysUpdate))
    }
    async fn prepare_key_deletion(&self, id: &str) -> Result<Vec<RevisionCheck>, GatewayError> {
        let key = deletion_key(id);
        let before = self.store.read(&key).await?;
        let revision =
            gateway_auth_primitives::crypto::uuid_from_random(&self.host.random_bytes(16)?)?;
        let guard = StoredRecord::json(&revision, json!({"created_at_ms":1_000_000}), None);
        let transition = AtomicTransition {
            checks: vec![gateway_auth_primitives::store::check(&key, before.as_ref())],
            mutations: vec![RecordMutation::Put {
                key: key.clone(),
                record: guard,
            }],
        };
        assert_eq!(
            self.store.commit(transition).await?,
            CommitOutcome::Committed
        );
        // This finite fixture's only possible paid ledger is the racing attempt.
        assert!(
            self.store
                .read(&gateway_authority::keys::paid_attempt(id, "racing-attempt"))
                .await?
                .is_none()
        );
        Ok(vec![RevisionCheck {
            key,
            expected_revision: Some(revision),
        }])
    }
    async fn finish_key_deletion(
        &self,
        id: &str,
        prepared: &[RevisionCheck],
    ) -> Result<(), GatewayError> {
        let key = gateway_auth_primitives::api_keys::id_key(id);
        let row = self.store.read(&key).await?;
        assert!(
            row.is_none(),
            "Finish must run only after the committed ID deletion"
        );
        let mut checks = prepared.to_vec();
        checks.push(gateway_auth_primitives::store::check(&key, row.as_ref()));
        let history = gateway_auth_primitives::store::key(json!([
            "uos_ai",
            "paid_fallback",
            "ledger",
            id,
            1,
            "fixture-history"
        ]));
        let result = self
            .store
            .commit(AtomicTransition {
                checks,
                mutations: vec![RecordMutation::Delete { key: history }],
            })
            .await?;
        if result != CommitOutcome::Committed {
            return Err(gateway_auth_primitives::error::conflict(
                "Fixture cleanup ownership changed",
            ));
        }
        Ok(())
    }
    async fn abort_key_deletion(
        &self,
        id: &str,
        prepared: &[RevisionCheck],
    ) -> Result<(), GatewayError> {
        let Some(row) = self
            .store
            .read(&gateway_auth_primitives::api_keys::id_key(id))
            .await?
        else {
            panic!("Fixture deletion requires its retained revoked key");
        };
        let record: ApiKeyRecord = decode_record(&row)?;
        assert!(record.policy.revoked_at_ms.is_some());
        assert!(prepared.iter().any(|check| check.key == deletion_key(id)));
        // Source ordinary guards persist; this fixture never claims the special local key.
        Ok(())
    }
}
