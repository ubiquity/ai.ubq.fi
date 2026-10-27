//! Global serial subscription selection; source routing-serial.ts and routing-evaluation.ts.
use crate::{invalid, store::Transaction};
use gateway_authority_contracts::TokenSource;
use gateway_authority_routing_evidence::{capacity::CapacityStore, models::ModelAvailability};
use gateway_authority_routing_records::{types::RoutingState, types::SubscriptionPool};
use gateway_core::{error::GatewayError, identity::ActiveSelection, state::AuthorityStore};
use std::collections::BTreeMap;

// Only finite ASCII class comparisons consume this projection; original model data is untouched.

pub(crate) struct RoutingView {
    pub pool: SubscriptionPool,
    pub state: RoutingState,
    pub active: Option<ActiveSelection>,
    models: ModelAvailability,
    pub(crate) capacity: CapacityStore,
    pub(crate) overage: BTreeMap<String, bool>,
}

pub(crate) async fn read_view<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
) -> Result<RoutingView, GatewayError> {
    read_view_for(tx, true).await
}

/// Source slot-pinned callers hydrate the same state without consulting the global election.
pub(crate) async fn read_pinned_view<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
) -> Result<RoutingView, GatewayError> {
    read_view_for(tx, false).await
}

async fn read_view_for<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    read_active: bool,
) -> Result<RoutingView, GatewayError> {
    let now = tx.now;
    let mode = if read_active {
        gateway_authority_routing_evidence::reader::RoutingReadMode::GlobalSelection
    } else {
        gateway_authority_routing_evidence::reader::RoutingReadMode::PinnedObservation
    };
    let evidence = {
        let reader = tx.routing_read_store();
        gateway_authority_routing_evidence::reader::RoutingEvidenceReader::new(&reader, now)?
            .read(mode)
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(invalid(
                "Routing evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(RoutingView {
        pool: evidence.pool().clone(),
        state: evidence.state().clone(),
        active: evidence.active().cloned(),
        models: evidence.models().clone(),
        capacity: evidence.capacity().clone(),
        overage: evidence.overage().clone(),
    })
}

impl RoutingView {
    pub(crate) fn policy(&self) -> gateway_authority_routing_policy::RoutingPolicyData<'_> {
        gateway_authority_routing_policy::RoutingPolicyData {
            pool: &self.pool,
            state: &self.state,
            active: self.active.as_ref(),
            models: &self.models,
            capacity: &self.capacity,
            overage: &self.overage,
        }
    }
}
