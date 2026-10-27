//! Same index predicates, with native-private independent cursor lookups.
use super::{ReservationIndexes, bind_identity, derive_basics};
use crate::native_spool::{Spool, Writer};
use crate::{Row, Transform, decode, invalid, parts, starts};
use gateway_core::{
    error::GatewayError,
    state::{RecordKey, StoredRecord},
};
use std::{
    borrow::Cow,
    collections::{BTreeMap, BTreeSet},
    path::Path,
};
type BoundRows = (Spool<Row>, Vec<(RecordKey, StoredRecord)>);
pub(crate) fn bind_spooled(
    rows: &Spool<Row>,
    now: i64,
    revision: &str,
    path: &Path,
) -> Result<BoundRows, GatewayError> {
    let mut output = Writer::create(path)?;
    let mut snapshot = BTreeMap::new();
    for index in 0..rows.len() {
        if rows.live(index, now) {
            snapshot.insert(rows.key(index)?.to_owned(), index);
        }
    }
    // One persistent directed reader for coupled-key lookups, never one open per row.
    let mut references = rows.reader()?;
    let mut get = |key: &str| -> Result<Option<Cow<'_, StoredRecord>>, GatewayError> {
        snapshot
            .get(key)
            .map(|i| references.read(*i).map(|row| Cow::Owned(row.record)))
            .transpose()
    };
    let mut paid_limits = BTreeMap::new();
    let mut source_paid_windows = BTreeSet::new();
    for key in snapshot.keys() {
        let p = parts(&RecordKey(key.clone()))?;
        if starts(&p, &["ubq_ai", "api_keys", "id"]) {
            let record: gateway_admin_keys::keys::ApiKeyRecord = decode(
                get(key)?
                    .ok_or_else(|| invalid("API key ID row is missing"))?
                    .json_value()?,
            )?;
            paid_limits.insert(
                record.policy.id,
                record.policy.paid_fallback_limit_microcredits,
            );
        } else if starts(&p, &["uos_ai", "paid_fallback", "v3", "window"]) {
            source_paid_windows.insert(key.clone());
        }
    }
    let mut derived = Vec::new();
    derive_basics(&mut get, revision, &mut derived)?;
    let mut reader = rows.reader()?;
    let mut index = 0usize;
    while let Some(raw) = reader.next_raw()? {
        let p = parts(&RecordKey(rows.key(index)?.to_owned()))?;
        let live = rows.live(index, now);
        index += 1;
        if !live
            || !(starts(&p, &["ubq_ai", "api_keys"])
                || starts(&p, &["uos_ai", "auth"])
                || starts(&p, &["uos_ai", "api_key_usage"])
                || starts(&p, &["uos_ai", "kernel_quota"])
                || starts(&p, &["uos_ai", "paid_fallback"]))
        {
            continue;
        }
        let row: Row = serde_json::from_slice(&raw)
            .map_err(|_| invalid("Private bound row cannot be decoded"))?;
        bind_identity(&row, now, revision, &mut get, &mut derived)?;
    }
    reader.verify_end()?;
    let mut indexes = ReservationIndexes::default();
    let mut reader = rows.reader()?;
    let mut index = 0usize;
    while let Some(raw) = reader.next_raw()? {
        let p = parts(&RecordKey(rows.key(index)?.to_owned()))?;
        let live = rows.live(index, now);
        index += 1;
        if !live
            || !(starts(&p, &["ubq_ai", "api_keys"])
                || starts(&p, &["uos_ai", "auth"])
                || starts(&p, &["uos_ai", "api_key_usage"])
                || starts(&p, &["uos_ai", "kernel_quota"])
                || starts(&p, &["uos_ai", "paid_fallback"]))
        {
            continue;
        }
        let row: Row = serde_json::from_slice(&raw)
            .map_err(|_| invalid("Private bound row cannot be decoded"))?;
        indexes.observe(&row, now, &paid_limits, &mut get, &|key| {
            snapshot.contains_key(key)
        })?;
    }
    reader.verify_end()?;
    let mut reader = rows.reader()?;
    let mut index = 0usize;
    while let Some(raw) = reader.next_raw()? {
        let key = rows.key(index)?.to_owned();
        let p = parts(&RecordKey(key.clone()))?;
        let transform = rows.transform(index);
        let expiry = rows.expiry(index);
        let live = rows.live(index, now);
        index += 1;
        if !live
            || !(matches!(
                transform,
                Some(Transform::ApiWindow | Transform::KernelWindow | Transform::PaidGate)
            ) || starts(&p, &["uos_ai", "paid_fallback", "v3", "window"]))
        {
            output.push_frame(&raw, Some(key), expiry, transform)?;
            continue;
        }
        let mut row: Row = serde_json::from_slice(&raw)
            .map_err(|_| invalid("Private bound row cannot be decoded"))?;
        indexes.bind_window(&mut row)?;
        output.push(&row)?;
    }
    reader.verify_end()?;
    indexes.finish(&source_paid_windows, &paid_limits, revision, &mut derived)?;
    Ok((output.seal()?, derived))
}
