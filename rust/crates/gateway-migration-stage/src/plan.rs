//! Ordered bounded staged emission from complete prepared state; no deserializer.
use gateway_core::error::GatewayError;
use gateway_migration_data::{
    invalid,
    staged::{
        CanonicalRecord, STAGED_BATCH_MAX_BYTES, StageBatch, StageHeader, StageRequest, bounded,
        extend, initial,
    },
};
use gateway_migration_prepare::engine::PreparedAdoption;
use serde::Serialize;
use std::collections::VecDeque;
const PREFERRED_BATCH_BYTES: usize = 4 * 1024 * 1024;
#[derive(Debug, Serialize)]
pub struct StagePlanSummary {
    pub capture_id: String,
    pub source_git_sha: String,
    pub captured_at_ms: i64,
    pub source_rows: usize,
    pub derived_rows: usize,
    pub records: usize,
    pub digest: String,
}
/// Only constructed from a complete audited PreparedAdoption. No Deserialize.
/// Native preparation may retain the full capture; WASM receives bounded frames.
pub struct StagePlan {
    header: StageHeader,
    rows: StageRows,
    sequence: u64,
    digest: String,
    emission_failed: bool,
    #[cfg(all(unix, not(target_arch = "wasm32")))]
    last_emitted_key: Option<String>,
}
enum StageRows {
    Memory(VecDeque<(CanonicalRecord, usize)>),
    #[cfg(all(unix, not(target_arch = "wasm32")))]
    Disk {
        reader: gateway_migration_spool::canonical::CanonicalReader,
        order: VecDeque<(usize, usize)>,
    },
}
impl StageRows {
    fn front_size(&self) -> Option<usize> {
        match self {
            Self::Memory(rows) => rows.front().map(|(_, size)| *size),
            #[cfg(all(unix, not(target_arch = "wasm32")))]
            Self::Disk { order, .. } => order.front().map(|(_, size)| *size),
        }
    }
    fn pop(&mut self) -> Result<(CanonicalRecord, usize), GatewayError> {
        match self {
            Self::Memory(rows) => Ok(rows.pop_front().expect("front checked")),
            #[cfg(all(unix, not(target_arch = "wasm32")))]
            Self::Disk { reader, order } => {
                let (index, size) = order.pop_front().expect("front checked");
                Ok((reader.read(index)?, size))
            }
        }
    }
}
impl StagePlan {
    pub fn from_prepared(prepared: PreparedAdoption) -> Result<Self, GatewayError> {
        let (manifest, rows) = prepared.stage_records()?;
        let mut digest = initial(&manifest.capture_id);
        let mut encoded = VecDeque::new();
        for row in rows {
            let size = bounded(&row)?;
            if size + 1024 > STAGED_BATCH_MAX_BYTES {
                return Err(invalid("Staged row does not fit one bounded append"));
            }
            digest = extend(&digest, &row)?;
            encoded.push_back((row, size));
        }
        let header = StageHeader {
            manifest,
            records: encoded.len(),
            digest,
        };
        bounded(&StageRequest::Begin(header.clone()))?;
        Ok(Self {
            digest: initial(&header.manifest.capture_id),
            header,
            rows: StageRows::Memory(encoded),
            sequence: 0,
            emission_failed: false,
            #[cfg(all(unix, not(target_arch = "wasm32")))]
            last_emitted_key: None,
        })
    }
    #[cfg(all(unix, not(target_arch = "wasm32")))]
    pub(crate) fn from_spool(
        manifest: gateway_migration_data::manifest::Manifest,
        spool: gateway_migration_spool::canonical::CanonicalRows,
    ) -> Result<Self, GatewayError> {
        let mut reader = spool.reader()?;
        let mut keys = std::collections::BTreeMap::new();
        let mut ordinal = 0usize;
        while reader.next_raw()?.is_some() {
            if keys
                .insert(
                    spool.key(ordinal)?.to_owned(),
                    (ordinal, spool.encoded_size(ordinal)),
                )
                .is_some()
            {
                return Err(invalid("Staged canonical inventory has duplicate keys"));
            }
            ordinal += 1;
        }
        reader.verify_end()?;
        if ordinal != spool.len() {
            return Err(invalid("Staged canonical count changed"));
        }
        let order: VecDeque<_> = keys.into_values().collect();
        let mut digest = initial(&manifest.capture_id);
        for (index, size) in &order {
            if size + 1024 > STAGED_BATCH_MAX_BYTES {
                return Err(invalid("Staged row does not fit one bounded append"));
            }
            digest = extend_raw(&digest, &reader.read_raw(*index)?)?;
        }
        reader.verify_file_end()?;
        let header = StageHeader {
            manifest,
            records: order.len(),
            digest,
        };
        bounded(&StageRequest::Begin(header.clone()))?;
        Ok(Self {
            digest: initial(&header.manifest.capture_id),
            header,
            rows: StageRows::Disk { reader, order },
            sequence: 0,
            emission_failed: false,
            last_emitted_key: None,
        })
    }
    pub fn summary(&self) -> StagePlanSummary {
        StagePlanSummary {
            capture_id: self.header.manifest.capture_id.clone(),
            source_git_sha: self.header.manifest.source_git_sha.clone(),
            captured_at_ms: self.header.manifest.captured_at_ms,
            source_rows: self.header.manifest.source_rows,
            derived_rows: self.header.manifest.derived.len(),
            records: self.header.records,
            digest: self.header.digest.clone(),
        }
    }
    pub fn begin(&self) -> StageRequest {
        StageRequest::Begin(self.header.clone())
    }
    pub fn seal(&self) -> StageRequest {
        StageRequest::Seal {
            capture_id: self.header.manifest.capture_id.clone(),
        }
    }
    pub fn next_batch(&mut self) -> Result<Option<StageRequest>, GatewayError> {
        if self.emission_failed {
            return Err(invalid("Private staged emission already failed"));
        }
        if self.rows.front_size().is_none() {
            return Ok(None);
        }
        let previous_digest = self.digest.clone();
        let mut records = Vec::new();
        let mut size = 1024usize;
        while let Some(next) = self.rows.front_size() {
            if !records.is_empty() && size + next + 1 > PREFERRED_BATCH_BYTES {
                break;
            }
            let (row, next) = self.rows.pop()?;
            self.digest = extend(&self.digest, &row)?;
            #[cfg(all(unix, not(target_arch = "wasm32")))]
            {
                self.last_emitted_key = Some(row.key.0.clone());
            }
            size += next + 1;
            records.push(row);
        }
        let batch = StageRequest::Append(StageBatch {
            capture_id: self.header.manifest.capture_id.clone(),
            sequence: self.sequence,
            previous_digest,
            records,
            digest: self.digest.clone(),
        });
        bounded(&batch)?;
        self.sequence = self
            .sequence
            .checked_add(1)
            .ok_or_else(|| invalid("Stage sequence overflow"))?;
        Ok(Some(batch))
    }

    /// Native operator output only. The sealed spool comes exclusively from the
    /// complete genuine-row audit; this accepts neither caller JSON nor an inventory.
    #[cfg(all(unix, not(target_arch = "wasm32")))]
    pub fn write_next_batch<W: std::io::Write>(
        &mut self,
        sink: &mut W,
    ) -> Result<bool, GatewayError> {
        if self.emission_failed {
            return Err(invalid("Private staged emission already failed"));
        }
        let result = self.next_native_frame().and_then(|frame| {
            let Some(frame) = frame else {
                return Ok(false);
            };
            sink.write_all(&frame)
                .map_err(|_| invalid("Private staged frame sink failed"))?;
            Ok(true)
        });
        if result.is_err() {
            // A partial write has an unknown delivery outcome. Neither another
            // sink call nor the typed adapter may continue this operator plan.
            self.emission_failed = true;
        }
        result
    }

    #[cfg(all(unix, not(target_arch = "wasm32")))]
    fn next_native_frame(&mut self) -> Result<Option<Vec<u8>>, GatewayError> {
        if matches!(&self.rows, StageRows::Memory(_)) {
            return self
                .next_batch()?
                .map(|batch| {
                    serde_json::to_vec(&batch)
                        .map_err(|_| invalid("Private staged frame cannot be encoded"))
                })
                .transpose();
        }
        let StageRows::Disk { reader, order } = &mut self.rows else {
            unreachable!("memory adapter returned above");
        };
        reader.verify_file_end()?;
        if reader.len() != self.header.records || order.len() > self.header.records {
            return Err(invalid("Staged canonical count changed"));
        }
        if order.is_empty() {
            if self.digest != self.header.digest {
                return Err(invalid("Staged canonical digest changed"));
            }
            return Ok(None);
        }
        let next_sequence = self
            .sequence
            .checked_add(1)
            .ok_or_else(|| invalid("Stage sequence overflow"))?;
        let mut digest = self.digest.clone();
        let mut last_key = self.last_emitted_key.clone();
        let mut records = vec![b'['];
        let mut count = 0;
        let mut size = 1024usize;
        for (index, encoded_size) in order.iter() {
            if count != 0 && size + encoded_size + 1 > PREFERRED_BATCH_BYTES {
                break;
            }
            if *encoded_size != reader.encoded_size(*index)
                || *encoded_size + 1024 > STAGED_BATCH_MAX_BYTES
            {
                return Err(invalid("Staged canonical frame length changed"));
            }
            let key = reader.key(*index)?.to_owned();
            if last_key
                .as_deref()
                .is_some_and(|previous| previous >= key.as_str())
            {
                return Err(invalid("Staged canonical order changed"));
            }
            let raw = reader.read_raw(*index)?;
            digest = extend_raw(&digest, &raw)?;
            if count != 0 {
                records.push(b',');
            }
            records.extend_from_slice(&raw);
            last_key = Some(key);
            size += encoded_size + 1;
            count += 1;
        }
        records.push(b']');
        reader.verify_file_end()?;
        if count == order.len() && digest != self.header.digest {
            return Err(invalid("Staged canonical digest changed"));
        }
        // These bytes were serialized from actual validated CanonicalRecords and
        // authenticated above. RawValue checks only the unchanged JSON grammar;
        // it never substitutes for the complete semantic audit or grants authority.
        let records = serde_json::value::RawValue::from_string(
            String::from_utf8(records).map_err(|_| invalid("Staged canonical bytes changed"))?,
        )
        .map_err(|_| invalid("Staged canonical bytes changed"))?;
        #[derive(Serialize)]
        struct WireBatch<'a> {
            capture_id: &'a str,
            sequence: u64,
            previous_digest: &'a str,
            records: &'a serde_json::value::RawValue,
            digest: &'a str,
        }
        #[derive(Serialize)]
        struct WireFrame<'a> {
            stage: &'static str,
            data: WireBatch<'a>,
        }
        let frame = serde_json::to_vec(&WireFrame {
            stage: "append",
            data: WireBatch {
                capture_id: &self.header.manifest.capture_id,
                sequence: self.sequence,
                previous_digest: &self.digest,
                records: &records,
                digest: &digest,
            },
        })
        .map_err(|_| invalid("Private staged frame cannot be encoded"))?;
        if frame.len() > STAGED_BATCH_MAX_BYTES {
            return Err(invalid("Private staged frame exceeds its bound"));
        }
        order.drain(..count);
        self.sequence = next_sequence;
        self.digest = digest;
        self.last_emitted_key = last_key;
        Ok(Some(frame))
    }
}

#[cfg(all(unix, not(target_arch = "wasm32")))]
fn extend_raw(previous: &str, record: &[u8]) -> Result<String, GatewayError> {
    let mut bytes =
        serde_json::to_vec(previous).map_err(|_| invalid("Staged digest cannot be encoded"))?;
    bytes.insert(0, b'[');
    bytes.push(b',');
    bytes.extend_from_slice(record);
    bytes.push(b']');
    Ok(gateway_auth::crypto::sha256_hex(&bytes))
}
