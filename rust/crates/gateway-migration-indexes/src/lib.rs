use gateway_auth_passkeys::{passkeys::PasskeyCredential, passkeys::PasskeyUser};
use gateway_authority_api_protocol::{ApiUsageRequest, ApiUsageState, ApiUsageWindow};
use gateway_authority_kernel_protocol::{KernelDefaults, KernelReservation, KernelWindow};
use gateway_authority_keys as keys;
use gateway_authority_paid_records::{
    PaidAttempt, PaidWindow, PendingPaid, ReconciliationGate,
    source_records::decode_paid_attempt_record,
};
use gateway_contracts::{identity::ApiKeyId, lifecycle::BillingState};
use gateway_core::state::StoredRecord;
use gateway_kv_codec::key::RecordKey;
use gateway_kv_codec::tuple_key::{ScalarKeyPart, TupleKey};
use gateway_migration_data::{RowData as Row, Transform, decode, invalid, json, parts, starts};
use gateway_source::error::GatewayError;
use serde_json::Value;
use serde_json::json;
use std::{
    borrow::Cow,
    collections::{BTreeMap, BTreeSet},
};

#[derive(Default)]
struct PaidAccounting {
    settled_microcredits: i64,
    reserved_microcredits: i64,
    pending_count: i64,
    updated_at_ms: i64,
    policy_version: String,
}

fn account_paid(
    accounting: &mut BTreeMap<(String, i64), PaidAccounting>,
    attempt: &PaidAttempt,
) -> Result<(), GatewayError> {
    let entry = accounting
        .entry((
            attempt.reservation.key_id.0.clone(),
            attempt.reservation.window_reset_at_ms,
        ))
        .or_default();
    if attempt.updated_at_ms >= entry.updated_at_ms {
        entry.updated_at_ms = attempt.updated_at_ms;
        entry.policy_version = attempt.policy_version.clone();
    }
    match attempt.billing_state {
        BillingState::Settled => {
            entry.settled_microcredits = entry
                .settled_microcredits
                .checked_add(attempt.spend_microcredits.unwrap_or(0))
                .ok_or_else(|| invalid("Paid settled total overflows"))?;
        }
        BillingState::Pending | BillingState::Unresolved => {
            entry.reserved_microcredits = entry
                .reserved_microcredits
                .checked_add(attempt.reservation.reserved_microcredits)
                .ok_or_else(|| invalid("Paid reserved total overflows"))?;
            entry.pending_count = entry
                .pending_count
                .checked_add(1)
                .ok_or_else(|| invalid("Paid pending count overflows"))?;
        }
        BillingState::NotBilled => {}
    }
    Ok(())
}

fn derive_paid_window(
    key_id: &str,
    window_reset_at_ms: i64,
    limit_microcredits: i64,
    accounting: &PaidAccounting,
    revision: &str,
) -> Result<(RecordKey, StoredRecord), GatewayError> {
    if accounting.policy_version.is_empty() || accounting.updated_at_ms <= 0 {
        return Err(invalid("Paid accounting lacks current policy evidence"));
    }
    let window = PaidWindow {
        v: 3,
        key_id: ApiKeyId(key_id.into()),
        policy_version: accounting.policy_version.clone(),
        window_reset_at_ms,
        limit_microcredits,
        settled_microcredits: accounting.settled_microcredits,
        reserved_microcredits: accounting.reserved_microcredits,
        pending_count: accounting.pending_count,
        updated_at_ms: accounting.updated_at_ms,
    };
    Ok((
        keys::paid_window(key_id, window_reset_at_ms),
        StoredRecord::json(revision, json(&window)?, None),
    ))
}

fn derive_basics<'a>(
    get: &mut impl FnMut(&str) -> Result<Option<Cow<'a, StoredRecord>>, GatewayError>,
    revision: &str,
    derived: &mut Vec<(RecordKey, StoredRecord)>,
) -> Result<(), GatewayError> {
    let mut defaults = KernelDefaults::default();
    let mut explicit_defaults = false;
    if let Some(row) = get(&TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("default"),
        ScalarKeyPart::String("kernel_policy_limit_requests"),
    ])
    .to_record_key()
    .0)?
    {
        defaults.usage_limit_requests =
            gateway_admin_contracts::fields::limit(row.json_value()?).unwrap_or(-1);
        explicit_defaults = true;
    }
    if let Some(row) = get(&TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("default"),
        ScalarKeyPart::String("kernel_policy_window_ms"),
    ])
    .to_record_key()
    .0)?
    {
        defaults.window_ms =
            gateway_admin_contracts::fields::positive(row.json_value()?).unwrap_or(604800000);
        explicit_defaults = true;
    }
    if explicit_defaults {
        derived.push((
            keys::kernel_defaults(),
            StoredRecord::json(revision, json(&defaults)?, None),
        ));
    }
    // The raw source pool and the authority routing projection are distinct
    // records. Derive the latter here, inside the same atomic adoption, using
    // the canonical account identities and the exact retained source revision.
    if let Some(source) =
        get(&gateway_auth_credentials::provider_credentials::codex_source_pool_key().0)?
    {
        let pool = gateway_auth_credentials::provider_credentials::parse_codex_source_pool(
            source.json_value()?,
        )?
        .ok_or_else(|| invalid("Source Codex pool is malformed"))?;
        let accounts = pool
            .accounts
            .iter()
            .map(|account| {
                let identity = account.identity();
                gateway_authority::routing_types::PoolAccount {
                    account_id_hash: identity.routing_account_hash,
                    admin_account_hash: identity.administrative_account_hash,
                    credential_version: identity.credential_version,
                    // Same absent-projection initialization as access publication;
                    // current provider selection is rechecked by the routing reader.
                    enabled: true,
                }
            })
            .collect();
        let normalized = gateway_authority::routing_types::SubscriptionPool {
            v: 1,
            pool_versionstamp: gateway_contracts::identity::PoolVersionstamp(
                source.revision.clone(),
            ),
            accounts,
        };
        derived.push((
            keys::pool(),
            StoredRecord::json(revision, json(&normalized)?, None),
        ));
    }
    Ok(())
}

fn bind_identity<'a>(
    row: &Row,
    now: i64,
    revision: &str,
    get: &mut impl FnMut(&str) -> Result<Option<Cow<'a, StoredRecord>>, GatewayError>,
    derived: &mut Vec<(RecordKey, StoredRecord)>,
) -> Result<(), GatewayError> {
    if starts(&row.parts, &["ubq_ai", "api_keys", "id"]) {
        let id: gateway_admin_keys::keys::ApiKeyRecord = decode(row.record.json_value()?)?;
        let hash_key = gateway_auth_primitives::api_keys::hash_key(&id.hash);
        let hash = get(&hash_key.0)?.ok_or_else(|| invalid("API key hash index is missing"))?;
        let hash: gateway_auth_primitives::api_keys::ApiKeyHashRecord = decode(hash.json_value()?)?;
        if hash != id.policy {
            return Err(invalid("API ID/hash policy pair conflicts"));
        }
        let policy = gateway_admin_keys::keys::authority_policy(&id, now);
        derived.push((
            keys::api_policy(&id.policy.id),
            StoredRecord::json(revision, json(&policy)?, None),
        ));
    } else if starts(&row.parts, &["ubq_ai", "api_keys", "hash"]) {
        let hash: gateway_auth_primitives::api_keys::ApiKeyHashRecord =
            decode(row.record.json_value()?)?;
        let id = get(&gateway_auth_primitives::api_keys::id_key(&hash.id).0)?
            .ok_or_else(|| invalid("Orphan API hash index"))?;
        if id.json_value()?["hash"] != row.parts[3] {
            return Err(invalid("API hash names another ID"));
        }
    } else if starts(&row.parts, &["uos_ai", "auth", "users"]) {
        let user: PasskeyUser = decode(row.record.json_value()?)?;
        let handle = get(&gateway_auth_passkeys::passkeys::key("handles", &user.handle).0)?;
        if handle
            .as_ref()
            .map(|r| r.json_value())
            .transpose()?
            .and_then(Value::as_str)
            != Some(user.id.as_str())
        {
            return Err(invalid("Passkey handle index conflicts"));
        }
        for id in &user.credential_ids {
            let credential = get(&gateway_auth_passkeys::passkeys::key("credentials", id).0)?
                .ok_or_else(|| invalid("Passkey credential index missing"))?;
            let credential: PasskeyCredential = decode(credential.json_value()?)?;
            if credential.user_id != user.id {
                return Err(invalid("Passkey credential belongs to another user"));
            }
        }
    } else if starts(&row.parts, &["uos_ai", "auth", "credentials"]) {
        let credential: PasskeyCredential = decode(row.record.json_value()?)?;
        let user = get(&gateway_auth_passkeys::passkeys::key("users", &credential.user_id).0)?
            .ok_or_else(|| invalid("Passkey credential owner missing"))?;
        let user: PasskeyUser = decode(user.json_value()?)?;
        if !user.credential_ids.contains(&credential.credential_id) {
            return Err(invalid("Passkey user omits credential index"));
        }
    } else if starts(&row.parts, &["uos_ai", "auth", "sessions"]) {
        let session: gateway_auth_passkeys::passkeys::PasskeySession =
            decode(row.record.json_value()?)?;
        derived.push((
            gateway_auth_passkeys::session_index::index_key(
                &gateway_auth_passkeys::session_index::digest(&session.token),
            )?,
            gateway_auth_passkeys::session_index::record_for_session(
                &session,
                &row.record.revision,
                row.record.expires_at_ms,
            )?,
        ));
    }
    Ok(())
}

#[derive(Default)]
struct ReservationIndexes {
    api: BTreeMap<String, BTreeMap<String, i64>>,
    kernel: BTreeMap<String, BTreeMap<String, i64>>,
    paid: BTreeMap<String, (i64, i64)>,
    paid_accounting: BTreeMap<(String, i64), PaidAccounting>,
    pending: BTreeMap<String, PendingPaid>,
    has_gate: bool,
}

impl ReservationIndexes {
    fn observe<'a>(
        &mut self,
        row: &Row,
        now: i64,
        paid_limits: &BTreeMap<String, i64>,
        get: &mut impl FnMut(&str) -> Result<Option<Cow<'a, StoredRecord>>, GatewayError>,
        exists: &impl Fn(&str) -> bool,
    ) -> Result<(), GatewayError> {
        match row.mapping.transform {
            Transform::ApiRequest => {
                let request: ApiUsageRequest = decode(row.record.json_value()?)?;
                if request.state == ApiUsageState::Reserved {
                    let start = row.parts[6]
                        .as_f64()
                        .ok_or_else(|| invalid("API window tuple start missing"))?
                        as i64;
                    let window = keys::api_window(
                        &request.key_id.0,
                        row.parts[5]
                            .as_str()
                            .ok_or_else(|| invalid("API policy version missing"))?,
                        start,
                    );
                    if !exists(&window.0) {
                        return Err(invalid("API request window missing"));
                    }
                    self.api
                        .entry(window.0)
                        .or_default()
                        .insert(request.request_id.0, request.lease_expires_at_ms);
                }
            }
            Transform::KernelRequest => {
                let request: KernelReservation = decode(row.record.json_value()?)?;
                let expected = keys::kernel_request(
                    &request.owner,
                    request.repo.as_deref(),
                    request.window_created_at_ms,
                    &request.request_id.0,
                );
                if expected != row.key {
                    return Err(invalid("Kernel request tuple/identity conflict"));
                }
                if request.state.is_none() {
                    let window = keys::kernel_window(&request.owner, request.repo.as_deref());
                    let source =
                        get(&window.0)?.ok_or_else(|| invalid("Kernel request window missing"))?;
                    let source = source.json_value()?;
                    if decode::<i64>(&source["created_at_ms"])? != request.window_created_at_ms
                        || decode::<i64>(&source["usage_reset_at_ms"])?
                            != request.window_reset_at_ms
                    {
                        return Err(invalid("Kernel reservation belongs to another window"));
                    }
                    self.kernel
                        .entry(window.0)
                        .or_default()
                        .insert(request.request_id.0, request.lease_expires_at_ms);
                }
            }
            _ => self.observe_paid(row, now, paid_limits, get, exists)?,
        }
        Ok(())
    }
    fn observe_paid<'a>(
        &mut self,
        row: &Row,
        now: i64,
        paid_limits: &BTreeMap<String, i64>,
        get: &mut impl FnMut(&str) -> Result<Option<Cow<'a, StoredRecord>>, GatewayError>,
        exists: &impl Fn(&str) -> bool,
    ) -> Result<(), GatewayError> {
        match row.mapping.transform {
            Transform::PaidRequest => {
                let attempt = decode_paid_attempt_record(&row.key, &row.record)?;
                if attempt.reservation.window_reset_at_ms > now {
                    account_paid(&mut self.paid_accounting, &attempt)?;
                }
                if gateway_migration_projection::projection::outstanding(&attempt) {
                    if !exists(
                        &keys::paid_pending(
                            &attempt.reservation.key_id.0,
                            &attempt.reservation.attempt_id.0,
                        )
                        .0,
                    ) {
                        return Err(invalid(
                            "Outstanding paid request lacks its persistent pending marker",
                        ));
                    }
                    // Unlimited keys do not require a paid window aggregate;
                    // their current request rows are the accounting authority.
                    if paid_limits.get(&attempt.reservation.key_id.0).copied() != Some(-1) {
                        let window = keys::paid_window(
                            &attempt.reservation.key_id.0,
                            attempt.reservation.window_reset_at_ms,
                        );
                        let count = self.paid.entry(window.0).or_default();
                        count.0 = count
                            .0
                            .checked_add(1)
                            .ok_or_else(|| invalid("Paid pending count overflows"))?;
                        count.1 = count
                            .1
                            .checked_add(attempt.reservation.reserved_microcredits)
                            .ok_or_else(|| invalid("Paid exposure overflows"))?;
                    }
                }
            }
            Transform::PaidPending => {
                let marker: PendingPaid = decode(row.record.json_value()?)?;
                let attempt_key = keys::paid_attempt(&marker.key_id.0, &marker.attempt_id.0);
                let attempt = get(&attempt_key.0)?
                    .ok_or_else(|| invalid("Pending paid marker has missing or expired request"))?;
                let attempt = decode_paid_attempt_record(&attempt_key, &attempt)?;
                if !matches!(
                    attempt.billing_state,
                    BillingState::Pending | BillingState::Unresolved
                ) {
                    return Err(invalid(
                        "Settled paid request retains conflicting pending marker",
                    ));
                }
                self.pending.insert(
                    json!([marker.key_id.0, marker.attempt_id.0]).to_string(),
                    marker,
                );
            }
            _ => {}
        }
        Ok(())
    }
    fn bind_window(&mut self, row: &mut Row) -> Result<(), GatewayError> {
        match row.mapping.transform {
            Transform::ApiWindow => {
                let mut window: ApiUsageWindow = decode(row.record.json_value()?)?;
                window.reservations = self.api.remove(&row.key.0).unwrap_or_default();
                if window.reserved_requests != window.reservations.len() as i64 {
                    return Err(invalid("API reserved aggregate lacks exact source rows"));
                }
                row.record.value = json(&window)?;
            }
            Transform::KernelWindow => {
                let reservations = self.kernel.remove(&row.key.0).unwrap_or_default();
                let mut bound = row.record.json_value()?.clone();
                bound["reservations"] = json(&reservations)?;
                let window: KernelWindow = decode(&bound)?;
                if keys::kernel_window(&window.owner, window.repo.as_deref()) != row.key {
                    return Err(invalid("Kernel window tuple identity conflict"));
                }
                if window.reserved_requests != window.reservations.len() as i64 {
                    return Err(invalid("Kernel reserved aggregate lacks exact source rows"));
                }
                // Preserve source-only fields for m12/source admin readers and the inverse.
                row.record.value["reservations"] = json(&window.reservations)?;
            }
            Transform::PaidGate => {
                self.has_gate = true;
                let old: ReconciliationGate = decode(row.record.json_value()?)?;
                let minimum = self
                    .pending
                    .values()
                    .map(|p| p.next_reconciliation_at_ms)
                    .min();
                let next = match (old.next_due_at_ms, minimum) {
                    (Some(a), Some(b)) => Some(a.min(b)),
                    (a, b) => a.or(b),
                };
                row.record.value = json(&ReconciliationGate {
                    next_due_at_ms: next,
                    pending: self.pending.clone(),
                })?;
            }
            _ => {
                if starts(&row.parts, &["uos_ai", "paid_fallback", "v3", "window"]) {
                    let window: PaidWindow = decode(row.record.json_value()?)?;
                    let (count, exposure) = self.paid.remove(&row.key.0).unwrap_or_default();
                    if count != window.pending_count || exposure != window.reserved_microcredits {
                        return Err(invalid(
                            "Paid window outstanding count/exposure conflicts with imported requests",
                        ));
                    }
                }
            }
        }
        Ok(())
    }
    fn finish(
        &mut self,
        source_paid_windows: &BTreeSet<String>,
        paid_limits: &BTreeMap<String, i64>,
        revision: &str,
        derived: &mut Vec<(RecordKey, StoredRecord)>,
    ) -> Result<(), GatewayError> {
        for ((key_id, reset_at_ms), accounting) in std::mem::take(&mut self.paid_accounting) {
            if accounting.settled_microcredits == 0
                && accounting.reserved_microcredits == 0
                && accounting.pending_count == 0
            {
                continue;
            }
            let window_key = keys::paid_window(&key_id, reset_at_ms).0;
            if source_paid_windows.contains(&window_key) {
                continue;
            }
            let limit_microcredits = paid_limits
                .get(&key_id)
                .copied()
                .ok_or_else(|| invalid("Paid accounting key is missing"))?;
            derived.push(derive_paid_window(
                &key_id,
                reset_at_ms,
                limit_microcredits,
                &accounting,
                revision,
            )?);
            self.paid.remove(&window_key);
        }
        if !self.api.is_empty() || !self.kernel.is_empty() || !self.paid.is_empty() {
            return Err(invalid("Reservation index has an orphan window"));
        }
        if !self.has_gate && !self.pending.is_empty() {
            let gate = ReconciliationGate {
                next_due_at_ms: self
                    .pending
                    .values()
                    .map(|p| p.next_reconciliation_at_ms)
                    .min(),
                pending: std::mem::take(&mut self.pending),
            };
            derived.push((
                keys::paid_gate(),
                StoredRecord::json(revision, json(&gate)?, None),
            ));
        }
        Ok(())
    }
}

pub fn bind(
    rows: &mut [Row],
    now: i64,
    revision: &str,
) -> Result<Vec<(RecordKey, StoredRecord)>, GatewayError> {
    let live = |row: &Row| row.record.expires_at_ms.is_none_or(|n| n > now);
    let snapshot: BTreeMap<String, &StoredRecord> = rows
        .iter()
        .filter(|r| live(r))
        .map(|r| (r.key.0.clone(), &r.record))
        .collect();
    let mut derived = Vec::new();
    let mut paid_limits = BTreeMap::new();
    for row in rows.iter().filter(|row| live(row)) {
        if starts(&row.parts, &["ubq_ai", "api_keys", "id"]) {
            let record: gateway_admin_keys::keys::ApiKeyRecord = decode(row.record.json_value()?)?;
            paid_limits.insert(
                record.policy.id,
                record.policy.paid_fallback_limit_microcredits,
            );
        }
    }
    let mut source_paid_windows = BTreeSet::new();
    for name in snapshot.keys() {
        let p = parts(&RecordKey(name.clone()))?;
        if starts(&p, &["uos_ai", "paid_fallback", "v3", "window"]) {
            source_paid_windows.insert(name.clone());
        }
    }
    let mut get = |name: &str| Ok(snapshot.get(name).copied().map(Cow::Borrowed));
    derive_basics(&mut get, revision, &mut derived)?;
    for row in rows.iter().filter(|r| live(r)) {
        bind_identity(row, now, revision, &mut get, &mut derived)?;
    }
    let mut indexes = ReservationIndexes::default();
    for row in rows.iter().filter(|r| live(r)) {
        indexes.observe(row, now, &paid_limits, &mut get, &|key| {
            snapshot.contains_key(key)
        })?;
    }
    for row in rows.iter_mut().filter(|r| live(r)) {
        indexes.bind_window(row)?;
    }
    indexes.finish(&source_paid_windows, &paid_limits, revision, &mut derived)?;
    Ok(derived)
}

#[cfg(all(unix, not(target_arch = "wasm32")))]
pub mod disk;
