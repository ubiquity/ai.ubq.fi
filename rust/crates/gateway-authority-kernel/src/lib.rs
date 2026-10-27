//! Complete kernel reserve/renew/intent/settlement operations; no dispatch/delivery accounting changes.
mod backend;
use backend::Transaction;
use gateway_authority_contracts::{
    QUOTA_RETENTION_MS, REQUEST_LEASE_MS, TokenSource, add, invalid, nonnegative, positive,
    quota_error, retention, text, unavailable,
};
use gateway_authority_kernel_protocol::{
    KernelDefaults, KernelPolicy, KernelReservation, KernelTerminal, KernelTerminalDraft,
    KernelWindow, decode_kernel_policy, decode_kernel_window, kernel_identity,
    validate_kernel_window,
};
use gateway_authority_keys as keys;
use gateway_contracts::identity::RequestId;
use gateway_core::state::AuthorityStore;
use gateway_source::{error::GatewayError, source_json::SourceJson};
use std::collections::BTreeMap;

pub struct KernelService<'a, S, I> {
    store: &'a S,
    tokens: &'a I,
}
impl<S, I> Copy for KernelService<'_, S, I> {}
impl<S, I> Clone for KernelService<'_, S, I> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<'a, S: AuthorityStore, I: TokenSource> KernelService<'a, S, I> {
    pub fn new(store: &'a S, tokens: &'a I) -> Self {
        Self { store, tokens }
    }
    fn transaction(&self, now: i64) -> Result<backend::Transaction<'_, S, I>, GatewayError> {
        backend::Transaction::new(self.store, self.tokens, now)
    }
}
async fn read_kernel_policy<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    key: &gateway_kv_codec::key::RecordKey,
    owner: &str,
    repo: Option<&str>,
) -> Result<Option<KernelPolicy<String, f64>>, GatewayError> {
    let raw = tx.read_source_control::<SourceJson>(key).await?;
    decode_kernel_policy(raw.as_ref(), owner, repo)
}
async fn read_kernel_window<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    key: &gateway_kv_codec::key::RecordKey,
    owner: &str,
    repo: Option<&str>,
) -> Result<Option<KernelWindow>, GatewayError> {
    let raw = tx.read_source_control::<SourceJson>(key).await?;
    decode_kernel_window(raw.as_ref(), owner, repo)
}
async fn validate_kernel_index<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    window: &KernelWindow,
) -> Result<(), GatewayError> {
    for (request_id, expiry) in &window.reservations {
        let key = keys::kernel_request(
            &window.owner,
            window.repo.as_deref(),
            window.created_at_ms,
            request_id,
        );
        let row: KernelReservation = tx
            .read(&key)
            .await?
            .ok_or_else(|| invalid("Active kernel reservation has no durable row."))?;
        row.validate()?;
        if row.owner != window.owner
            || row.repo != window.repo
            || row.request_id.0 != *request_id
            || row.window_created_at_ms != window.created_at_ms
            || row.window_reset_at_ms != window.usage_reset_at_ms
            || row.state.is_some()
            || row.lease_expires_at_ms != *expiry
        {
            return Err(invalid(
                "Kernel reservation/window identity or aggregate is inconsistent.",
            ));
        }
    }
    Ok(())
}

async fn stage_kernel_terminal_intent<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    expected: &KernelReservation,
    terminal: KernelTerminal,
    now: i64,
) -> Result<bool, GatewayError> {
    let key = keys::kernel_request(
        &expected.owner,
        expected.repo.as_deref(),
        expected.window_created_at_ms,
        &expected.request_id.0,
    );
    let row: KernelReservation = tx
        .read(&key)
        .await?
        .ok_or_else(|| invalid("Kernel reservation is missing."))?;
    let draft = KernelTerminalDraft::prepare(expected, row, terminal, now)?;
    let window = if draft.needs_window() {
        let row = draft.row();
        let window_key = keys::kernel_window(&row.owner, row.repo.as_deref());
        read_kernel_window(tx, &window_key, &row.owner, row.repo.as_deref()).await?
    } else {
        None
    };
    let (row, window, was_chosen) = draft.finish(window)?.into_parts();
    if let Some(window) = window {
        let window_key = keys::kernel_window(&window.owner, window.repo.as_deref());
        tx.put(
            &window_key,
            &window,
            Some(retention(window.usage_reset_at_ms, QUOTA_RETENTION_MS)?),
        )?;
    }
    if let Some(row) = row {
        tx.put(
            &key,
            &row,
            Some(retention(row.window_reset_at_ms, QUOTA_RETENTION_MS)?),
        )?;
    }
    Ok(was_chosen)
}

pub trait KernelFactory {
    type Store: AuthorityStore;
    type Tokens: TokenSource;
    fn kernel(&self) -> KernelService<'_, Self::Store, Self::Tokens>;
}

pub trait KernelOperations: KernelFactory {
    fn reserve_kernel(
        &self,
        owner: &str,
        repo: &str,
        request_id: &RequestId,
        route: &str,
        now: i64,
    ) -> impl std::future::Future<Output = Result<KernelReservation, GatewayError>> {
        async move {
            let context = self.kernel();
            text(owner)?;
            text(repo)?;
            text(&request_id.0)?;
            text(route)?;
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                let repo_policy = read_kernel_policy(
                    &mut tx,
                    &keys::kernel_policy(owner, Some(repo)),
                    owner,
                    Some(repo),
                )
                .await?;
                let org_policy = if repo_policy.is_none() {
                    read_kernel_policy(&mut tx, &keys::kernel_policy(owner, None), owner, None)
                        .await?
                } else {
                    None
                };
                let defaults: KernelDefaults =
                    tx.read(&keys::kernel_defaults()).await?.unwrap_or_default();
                let cutover: Option<serde_json::Value> = tx.read(&keys::kernel_cutover()).await?;
                let scope_repo = repo_policy.as_ref().map(|_| repo.to_owned());
                let policy = repo_policy.or(org_policy);
                if let Some(policy) = &policy
                    && (policy.v != 2 || policy.owner != owner || policy.repo != scope_repo)
                {
                    return Err(invalid("Kernel policy scope conflicts."));
                }
                if policy.is_none() && cutover.is_some() {
                    return Err(unavailable(
                        "kernel_quota_unavailable",
                        "Kernel defaults are changing.",
                    ));
                }
                let limit = policy
                    .as_ref()
                    .map_or(defaults.usage_limit_requests, |policy| {
                        policy.usage_limit_requests
                    });
                let duration = policy
                    .as_ref()
                    .map_or(defaults.window_ms, |policy| policy.window_ms);
                positive(duration)?;
                if limit != -1 {
                    nonnegative(limit)?;
                }
                if limit == 0
                    || policy.as_ref().is_some_and(|policy| {
                        policy.expires_at_ms != -1.0 && policy.expires_at_ms <= now as f64
                    })
                {
                    return Err(quota_error(
                        "Kernel usage limit is zero or expired.",
                        "rate_limit_exceeded",
                    ));
                }
                let window_key = keys::kernel_window(owner, scope_repo.as_deref());
                let prior =
                    read_kernel_window(&mut tx, &window_key, owner, scope_repo.as_deref()).await?;
                if let Some(window) = &prior {
                    validate_kernel_window(window, owner, scope_repo.as_deref())?;
                    validate_kernel_index(&mut tx, window).await?;
                }
                let mut window = match prior {
                    Some(window)
                        if window.usage_reset_at_ms > now
                            && window.applied_window_ms == duration =>
                    {
                        window
                    }
                    _ => KernelWindow {
                        v: 2,
                        owner: owner.to_owned(),
                        repo: scope_repo.clone(),
                        usage_requests: 0,
                        reserved_requests: 0,
                        created_at_ms: now,
                        usage_reset_at_ms: add(now, duration)?,
                        applied_window_ms: duration,
                        updated_at_ms: now,
                        reservations: BTreeMap::new(),
                    },
                };
                let row_key = keys::kernel_request(
                    owner,
                    scope_repo.as_deref(),
                    window.created_at_ms,
                    &request_id.0,
                );
                if tx.read::<KernelReservation>(&row_key).await?.is_some() {
                    return Err(unavailable(
                        "kernel_quota_unavailable",
                        "Kernel request already has a reservation.",
                    ));
                }
                if limit != -1 && add(window.usage_requests, window.reserved_requests)? >= limit {
                    let expired = window
                        .reservations
                        .iter()
                        .find(|(_, expiry)| **expiry <= now)
                        .map(|(id, _)| id.clone());
                    if let Some(id) = expired {
                        let key = keys::kernel_request(
                            owner,
                            scope_repo.as_deref(),
                            window.created_at_ms,
                            &id,
                        );
                        let mut row: KernelReservation = tx
                            .read(&key)
                            .await?
                            .ok_or_else(|| invalid("Kernel reservation is missing."))?;
                        row.validate()?;
                        if row.state.is_some() || row.lease_expires_at_ms > now {
                            return Err(invalid("Kernel expiry index is malformed."));
                        }
                        let terminal = row.terminal_intent.unwrap_or(KernelTerminal::Released);
                        row.state = Some(terminal);
                        row.terminal_intent = Some(terminal);
                        row.terminal_at_ms = Some(now);
                        row.release_reason = if terminal == KernelTerminal::Released {
                            Some("lease_expired".to_owned())
                        } else {
                            None
                        };
                        if terminal == KernelTerminal::Committed {
                            window.usage_requests = add(window.usage_requests, 1)?;
                        }
                        window.reserved_requests -= 1;
                        window.reservations.remove(&id);
                        tx.put(
                            &key,
                            &row,
                            Some(retention(row.window_reset_at_ms, QUOTA_RETENTION_MS)?),
                        )?;
                        tx.put(
                            &window_key,
                            &window,
                            Some(retention(window.usage_reset_at_ms, QUOTA_RETENTION_MS)?),
                        )?;
                        tx.commit().await?;
                        continue;
                    }
                    if tx.commit().await? {
                        return Err(quota_error(
                            "Kernel usage limit exceeded.",
                            "rate_limit_exceeded",
                        ));
                    } else {
                        continue;
                    }
                }
                let lease = add(now, REQUEST_LEASE_MS)?;
                let row = KernelReservation {
                    v: 2,
                    owner: owner.to_owned(),
                    repo: scope_repo.clone(),
                    request_id: request_id.clone(),
                    route: route.to_owned(),
                    window_created_at_ms: window.created_at_ms,
                    window_reset_at_ms: window.usage_reset_at_ms,
                    reserved_at_ms: now,
                    lease_expires_at_ms: lease,
                    terminal_intent: None,
                    state: None,
                    terminal_at_ms: None,
                    release_reason: None,
                };
                window.reserved_requests = add(window.reserved_requests, 1)?;
                window.reservations.insert(request_id.0.clone(), lease);
                window.updated_at_ms = now;
                let expiry = Some(retention(window.usage_reset_at_ms, QUOTA_RETENTION_MS)?);
                tx.put(&row_key, &row, expiry)?;
                tx.put(&window_key, &window, expiry)?;
                if tx.commit().await? {
                    return Ok(row);
                }
            }
            Err(unavailable(
                "kernel_quota_unavailable",
                "Kernel quota changed concurrently.",
            ))
        }
    }

    fn renew_kernel(
        &self,
        expected: &KernelReservation,
        now: i64,
    ) -> impl std::future::Future<Output = Result<i64, GatewayError>> {
        async move {
            let context = self.kernel();
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                let key = keys::kernel_request(
                    &expected.owner,
                    expected.repo.as_deref(),
                    expected.window_created_at_ms,
                    &expected.request_id.0,
                );
                let mut row: KernelReservation = tx.read(&key).await?.ok_or_else(|| {
                    unavailable("kernel_quota_unavailable", "Kernel reservation is missing.")
                })?;
                if !kernel_identity(&row, expected)
                    || row.state.is_some()
                    || (row.lease_expires_at_ms <= now && row.terminal_intent.is_none())
                {
                    return Err(unavailable(
                        "kernel_quota_unavailable",
                        "Kernel reservation lease expired or settled.",
                    ));
                }
                let lease = add(now, REQUEST_LEASE_MS)?;
                row.lease_expires_at_ms = lease;
                let window_key = keys::kernel_window(&row.owner, row.repo.as_deref());
                let window =
                    read_kernel_window(&mut tx, &window_key, &row.owner, row.repo.as_deref())
                        .await?;
                if let Some(mut window) = window {
                    validate_kernel_window(&window, &row.owner, row.repo.as_deref())?;
                    if window.created_at_ms == row.window_created_at_ms {
                        window.reservations.insert(row.request_id.0.clone(), lease);
                        tx.put(
                            &window_key,
                            &window,
                            Some(retention(window.usage_reset_at_ms, QUOTA_RETENTION_MS)?),
                        )?;
                    }
                }
                tx.put(
                    &key,
                    &row,
                    Some(retention(row.window_reset_at_ms, QUOTA_RETENTION_MS)?),
                )?;
                if tx.commit().await? {
                    return Ok(lease);
                }
            }
            Err(unavailable(
                "kernel_quota_unavailable",
                "Kernel renewal changed concurrently.",
            ))
        }
    }

    fn record_kernel_terminal_intent(
        &self,
        expected: &KernelReservation,
        terminal: KernelTerminal,
        now: i64,
    ) -> impl std::future::Future<Output = Result<(), GatewayError>> {
        async move {
            let context = self.kernel();
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                stage_kernel_terminal_intent(&mut tx, expected, terminal, now).await?;
                if tx.commit().await? {
                    return Ok(());
                }
            }
            Err(unavailable(
                "kernel_quota_unavailable",
                "Kernel terminal intent changed concurrently.",
            ))
        }
    }

    /// Caller supplies the semantic terminal decision; downstream delivery never changes it.
    fn settle_kernel(
        &self,
        expected: &KernelReservation,
        terminal: KernelTerminal,
        reason: &str,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.kernel();
            self.record_kernel_terminal_intent(expected, terminal, now)
                .await?;
            for _ in 0..3 {
                let mut tx = context.transaction(now)?;
                let key = keys::kernel_request(
                    &expected.owner,
                    expected.repo.as_deref(),
                    expected.window_created_at_ms,
                    &expected.request_id.0,
                );
                let mut row: KernelReservation = tx
                    .read(&key)
                    .await?
                    .ok_or_else(|| invalid("Kernel reservation is missing."))?;
                if !kernel_identity(&row, expected)
                    || row.terminal_intent != Some(terminal)
                    || row.state.is_some_and(|state| state != terminal)
                {
                    return Err(invalid("Kernel terminal state conflicts."));
                }
                if row.state == Some(terminal) {
                    return Ok(false);
                }
                let window_key = keys::kernel_window(&row.owner, row.repo.as_deref());
                if let Some(mut window) =
                    read_kernel_window(&mut tx, &window_key, &row.owner, row.repo.as_deref())
                        .await?
                {
                    validate_kernel_window(&window, &row.owner, row.repo.as_deref())?;
                    if window.created_at_ms == row.window_created_at_ms {
                        if window.reserved_requests < 1
                            || window.reservations.remove(&row.request_id.0).is_none()
                        {
                            return Err(invalid("Missing kernel reserved capacity."));
                        }
                        window.reserved_requests -= 1;
                        if terminal == KernelTerminal::Committed {
                            window.usage_requests = add(window.usage_requests, 1)?;
                        }
                        window.updated_at_ms = now;
                        tx.put(
                            &window_key,
                            &window,
                            Some(retention(window.usage_reset_at_ms, QUOTA_RETENTION_MS)?),
                        )?;
                    }
                }
                row.state = Some(terminal);
                row.terminal_at_ms = Some(now);
                if terminal == KernelTerminal::Released {
                    let reason = reason.chars().take(120).collect::<String>();
                    row.release_reason = Some(if reason.trim().is_empty() {
                        "request_incomplete".to_owned()
                    } else {
                        reason
                    });
                }
                tx.put(
                    &key,
                    &row,
                    Some(retention(row.window_reset_at_ms, QUOTA_RETENTION_MS)?),
                )?;
                if tx.commit().await? {
                    return Ok(true);
                }
            }
            Err(unavailable(
                "kernel_quota_unavailable",
                "Kernel settlement changed concurrently.",
            ))
        }
    }
}
impl<T: KernelFactory + ?Sized> KernelOperations for T {}
