//! Fixed same-authority derived control metadata; these DTOs grant no write capability.
use backend::ReadTransaction;
use gateway_authority_contracts::{positive, text, unavailable};
use gateway_authority_keys as keys;
use gateway_authority_paid_control_records::PaidSourceCheck;
use gateway_authority_paid_control_records::{PAID_CONTROL_CONFIG_VERSION, PaidSyntheticHop};
use gateway_authority_paid_control_records::{PaidControlConfig, PaidRoutingPolicy};
use gateway_authority_paid_control_records::{PaidControlOwner, PaidProviderObservation};
use gateway_authority_paid_control_records::{PaidRequestContext, PaidRoutingProjection};
use gateway_authority_paid_records::PaidRequest;
use gateway_core::provider::PaidProvider;
use gateway_core::{state::AuthorityStore, state::RecordKey};
use gateway_core::{state::RevisionCheck, tuple_key::ScalarKeyPart, tuple_key::TupleKey};
use gateway_providers::policy::PaidRoutingInput;
use gateway_providers::{policy, policy::DeepseekHop, types::ClientSurface};
use gateway_schema::{catalog_store, selection::ProviderSelection};
use gateway_source::source_serialization as source;
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde_json::Value;
use std::collections::BTreeSet;
mod backend;
mod original;

pub(crate) fn control_error(message: &str) -> GatewayError {
    unavailable("paid_control_unavailable", message)
}

pub(crate) fn source_key(parts: &[&str]) -> RecordKey {
    TupleKey::from_scalar_parts(
        &parts
            .iter()
            .map(|part| ScalarKeyPart::String(part))
            .collect::<Vec<_>>(),
    )
    .to_record_key()
}

/// Fixed durable facts used by the coarse catalog projection, including checked absence.
pub fn paid_projection_source_keys() -> Vec<RecordKey> {
    vec![
        catalog_store::runtime_config_key(),
        catalog_store::codex_snapshot_key(),
        source_key(&["uos_ai", "provider_selection", "v1"]),
        catalog_store::auth_generation_key(),
        gateway_auth::provider_credentials::codex_source_pool_key(),
        keys::pool(),
    ]
}

fn validity(observed: i64, expires: Option<i64>, now: i64) -> Result<(), GatewayError> {
    positive(observed)?;
    if observed > now
        || expires.is_some_and(|expires| {
            positive(expires).is_err() || expires <= observed || expires <= now
        })
    {
        return Err(control_error(
            "Paid source validity is future, expired, or malformed.",
        ));
    }
    Ok(())
}

async fn source_checks<S: AuthorityStore>(
    tx: &mut ReadTransaction<'_, S>,
    checks: &[PaidSourceCheck],
    own: &RecordKey,
) -> Result<(), GatewayError> {
    let mut seen = BTreeSet::new();
    for check in checks {
        let key = RecordKey(check.key.clone());
        TupleKey::from_record_key(&key)?.validate_stored_key()?;
        let controls = [
            keys::paid_routing(),
            keys::paid_control_owner(),
            keys::paid_control_config(),
            keys::paid_control_observation(PaidProvider::Surplus),
            keys::paid_control_observation(PaidProvider::Metered),
        ];
        if key == *own || controls.contains(&key) || !seen.insert(&check.key) {
            return Err(control_error(
                "Paid source checks are duplicated or refer to derived controls.",
            ));
        }
        if let Some(revision) = &check.expected_revision {
            text(revision)?;
        }
        tx.observe_revision(&key).await?;
        if !tx.expect(&key, &check.expected_revision) {
            return Err(control_error("Paid source provenance changed."));
        }
    }
    Ok(())
}

struct ControlView {
    projection: PaidRoutingProjection,
    config: PaidControlConfig,
    surplus: Option<Vec<SourceJson>>,
    openlux: Option<Vec<SourceJson>>,
}

async fn current_projection<S: AuthorityStore>(
    tx: &mut ReadTransaction<'_, S>,
) -> Result<ControlView, GatewayError> {
    let raw: SourceJson = tx
        .read_source_control(&keys::paid_routing())
        .await?
        .ok_or_else(|| control_error("Paid routing projection is missing."))?;
    if source::member(&raw, "v").and_then(|value| value.deserialize::<u32>().ok()) != Some(2)
        || source::member(&raw, "provenance").is_none()
    {
        return Err(control_error(
            "A fenced v2 paid routing projection is required.",
        ));
    }
    let projection: PaidRoutingProjection = raw
        .deserialize()
        .map_err(|_| control_error("Paid routing projection is malformed."))?;
    let provenance = &projection.provenance;
    if provenance.v != 1 {
        return Err(control_error(
            "Paid projection provenance version is unsupported.",
        ));
    }
    validity(
        provenance.observed_at_ms,
        provenance.source_valid_until_ms,
        tx.now,
    )?;
    for key in paid_projection_source_keys() {
        if !provenance
            .source_checks
            .iter()
            .any(|check| check.key == key.0)
        {
            return Err(control_error(
                "Paid projection omits a required durable source fence.",
            ));
        }
    }
    source_checks(tx, &provenance.source_checks, &keys::paid_routing()).await?;
    let owner: PaidControlOwner = tx
        .read(&keys::paid_control_owner())
        .await?
        .ok_or_else(|| control_error("Authenticated paid control owner is not installed."))?;
    if owner.v != 1 {
        return Err(control_error("Paid control owner version is unsupported."));
    }
    text(&owner.owner_id)?;
    text(&owner.owner_epoch)?;
    text(&owner.key_fingerprint)?;
    if tx.revision(&keys::paid_control_owner()).as_deref()
        != Some(provenance.owner_revision.as_str())
    {
        return Err(control_error("Paid control owner revision changed."));
    }
    let config: PaidControlConfig = tx
        .read(&keys::paid_control_config())
        .await?
        .ok_or_else(|| control_error("Authenticated paid configuration is not published."))?;
    if config.v != PAID_CONTROL_CONFIG_VERSION
        || config.owner_id != owner.owner_id
        || config.owner_epoch != owner.owner_epoch
        || tx.revision(&keys::paid_control_config()).as_deref()
            != Some(provenance.config_revision.as_str())
    {
        return Err(control_error("Paid configuration custody changed."));
    }
    text(&config.generation)?;
    text(&config.configuration_fingerprint)?;
    for binding in [
        &config.surplus_binding,
        &config.openlux_discovery_binding,
        &config.openlux_binding,
    ]
    .into_iter()
    .flatten()
    {
        text(binding)?;
    }
    source_checks(tx, &config.source_checks, &keys::paid_control_config()).await?;
    let mut catalogs = Vec::new();
    for (provider, binding, expected) in [
        (
            PaidProvider::Surplus,
            &config.surplus_binding,
            &provenance.surplus_observation_revision,
        ),
        (
            PaidProvider::Metered,
            &config.openlux_discovery_binding,
            &provenance.openlux_observation_revision,
        ),
    ] {
        let key = keys::paid_control_observation(provider);
        let observation: Option<PaidProviderObservation> = tx.read_source_control(&key).await?;
        if &tx.revision(&key) != expected {
            return Err(control_error("Paid provider observation revision changed."));
        }
        let models = if let Some(observation) = observation {
            if observation.v != 1
                || observation.provider != provider
                || observation.owner_id != owner.owner_id
                || observation.owner_epoch != owner.owner_epoch
                || observation.configuration_generation != config.generation
                || observation.configuration_fingerprint != config.configuration_fingerprint
                || binding.as_ref() != Some(&observation.credential_binding)
            {
                return Err(control_error(
                    "Paid provider observation belongs to a superseded credential or configuration.",
                ));
            }
            validity(
                observation.observed_at_ms,
                observation.source_valid_until_ms,
                tx.now,
            )?;
            source_checks(tx, &observation.source_checks, &key).await?;
            // Retained source-stale routes stay usable; refresh TTL is not route expiry.
            Some(observation.models)
        } else {
            None
        };
        catalogs.push(models);
    }
    let openlux = catalogs
        .pop()
        .ok_or_else(|| control_error("Paid observation read failed."))?;
    let surplus = catalogs
        .pop()
        .ok_or_else(|| control_error("Paid observation read failed."))?;
    Ok(ControlView {
        projection,
        config,
        surplus,
        openlux,
    })
}

struct RouteView {
    policy: PaidRoutingPolicy,
    owner_id: String,
    owner_epoch: String,
    configuration_generation: String,
    credential_binding: String,
}

async fn route_current<S: AuthorityStore>(
    tx: &mut ReadTransaction<'_, S>,
    request: &PaidRequest,
    context: &PaidRequestContext,
    collator: &dyn gateway_schema::aliases::SourceStringCollator,
) -> Result<RouteView, GatewayError> {
    if source::member(&context.body, "model")
        .as_ref()
        .and_then(source::string)
        .as_ref()
        != Some(context.original_model.as_js_string())
    {
        return Err(control_error(
            "Paid context must retain the normalized logical original model.",
        ));
    }
    let endpoint = match context.surface {
        ClientSurface::Responses if request.path == "/v1/responses" => "openai-response",
        ClientSurface::Chat if request.path == "/v1/chat/completions" => "openai",
        _ => {
            return Err(control_error(
                "This paid client surface/path is not supported by the source router.",
            ));
        }
    };
    if context.synthetic_hop.is_none() && request.model != context.original_model {
        return Err(control_error(
            "An ordinary paid request cannot substitute a logical model.",
        ));
    }
    if context.synthetic_hop.is_some()
        && !gateway_schema::aliases::is_synthetic(&context.original_model)
    {
        return Err(control_error(
            "Only the source synthetic alias can use a server-owned paid hop.",
        ));
    }
    let ControlView {
        projection,
        config,
        surplus: surplus_catalog,
        openlux: openlux_catalog,
    } = current_projection(tx).await?;
    if let Some(hop) = context.synthetic_hop {
        let day = gateway_authority_reset_records::utc_day(tx.now)?;
        let year = day
            .split('-')
            .next()
            .and_then(|year| year.parse::<u32>().ok())
            .ok_or_else(|| {
                control_error("Paid source clock cannot resolve the synthetic alias.")
            })?;
        let aliases = gateway_schema::aliases::resolve_flash_aliases(
            None,
            None,
            surplus_catalog.as_deref(),
            openlux_catalog.as_deref(),
            year,
            collator,
        )?;
        let expected = match hop {
            PaidSyntheticHop::Surplus => aliases.surplus,
            PaidSyntheticHop::Openlux => aliases.openlux,
        };
        if request.model != expected {
            return Err(control_error(
                "The synthetic paid logical model does not match its current source alias.",
            ));
        }
    }
    let selection_raw: Option<Value> = tx
        .read(&source_key(&["uos_ai", "provider_selection", "v1"]))
        .await?;
    let selection = selection_raw
        .as_ref()
        .and_then(ProviderSelection::normalize);
    let codex_enabled = ProviderSelection::enabled(selection.as_ref(), "codex");
    if context.synthetic_hop.is_some()
        && !ProviderSelection::enabled(selection.as_ref(), "ubiquity")
    {
        return Err(control_error("The source synthetic route is disabled."));
    }
    if projection.policy.codex_enabled != codex_enabled {
        return Err(control_error(
            "Paid projection disagrees with current provider selection.",
        ));
    }
    let pinned = context.synthetic_hop.map(|hop| match hop {
        PaidSyntheticHop::Surplus => DeepseekHop::Surplus,
        PaidSyntheticHop::Openlux => DeepseekHop::Openlux,
    });
    let allowed = context.synthetic_hop.map(|hop| {
        vec![match hop {
            PaidSyntheticHop::Surplus => PaidProvider::Surplus,
            PaidSyntheticHop::Openlux => PaidProvider::Metered,
        }]
    });
    let plan = policy::resolve_paid_route(&PaidRoutingInput {
        model: request.model.clone(),
        endpoint_type: endpoint.to_owned(),
        request_uses_tools: source::member(&context.body, "tools")
            .as_ref()
            .and_then(source::array)
            .is_some_and(|tools| !tools.is_empty()),
        codex_model_known: projection.policy.codex_models.contains(&request.model),
        codex_enabled,
        surplus_enabled: ProviderSelection::enabled(selection.as_ref(), "surplus"),
        openlux_enabled: ProviderSelection::enabled(selection.as_ref(), "openlux"),
        surplus_configured: config.surplus_binding.is_some(),
        openlux_configured: config.openlux_binding.is_some(),
        surplus_catalog,
        openlux_catalog,
        allowed_paid_providers: allowed,
        synthetic_hop: pinned,
        ubiquity_enabled: ProviderSelection::enabled(selection.as_ref(), "ubiquity"),
    });
    if !plan.providers.contains(&request.provider) {
        return Err(control_error(
            "The exact source endpoint/tools/synthetic plan does not permit this paid provider.",
        ));
    }
    let credential_binding = match request.provider {
        PaidProvider::Surplus => config.surplus_binding,
        PaidProvider::Metered => config.openlux_binding,
    }
    .ok_or_else(|| control_error("Selected paid credential binding is missing."))?;
    Ok(RouteView {
        policy: projection.policy,
        owner_id: config.owner_id,
        owner_epoch: config.owner_epoch,
        configuration_generation: config.generation,
        credential_binding,
    })
}

/// Closed observations from the fixed source reader. This value grants no effect.
pub struct PaidRouteEvidence {
    route: RouteView,
    checks: Vec<RevisionCheck>,
}
impl PaidRouteEvidence {
    pub fn policy(&self) -> &PaidRoutingPolicy {
        &self.route.policy
    }
    pub fn owner_id(&self) -> &str {
        &self.route.owner_id
    }
    pub fn owner_epoch(&self) -> &str {
        &self.route.owner_epoch
    }
    pub fn configuration_generation(&self) -> &str {
        &self.route.configuration_generation
    }
    pub fn credential_binding(&self) -> &str {
        &self.route.credential_binding
    }
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
}
pub async fn capture_route<S: AuthorityStore>(
    store: &S,
    request: &PaidRequest,
    context: &PaidRequestContext,
    now: i64,
    collator: &dyn gateway_schema::aliases::SourceStringCollator,
) -> Result<PaidRouteEvidence, GatewayError> {
    let mut tx = ReadTransaction::new(store, now)?;
    let route = route_current(&mut tx, request, context, collator).await?;
    Ok(PaidRouteEvidence {
        route,
        checks: tx.checks(),
    })
}
/// Fixed original journal/API/kernel reads; revisions must join the caller's existing effect CAS.
pub async fn capture_original<S: AuthorityStore>(
    store: &S,
    request: &PaidRequest,
    original: &gateway_core::identity::RequestContext,
    existing: Option<&gateway_authority_paid_records::PaidReservation>,
    now: i64,
) -> Result<Vec<RevisionCheck>, GatewayError> {
    let mut tx = ReadTransaction::new(store, now)?;
    original::require_current(&mut tx, request, original, existing).await?;
    Ok(tx.checks())
}
