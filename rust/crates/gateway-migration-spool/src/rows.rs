//! Normalized-row DATA frames retain actual bytes and their inseparable facts.
use crate::raw::{Reader, Spool, Writer};
use gateway_admin_data::inventory::AuditInventory;
use gateway_migration_data::{MappingData, RowData, Transform, invalid};
use gateway_migration_ledger::rows::ObservedRow;
use gateway_source::error::GatewayError;
use serde::Deserialize;
use std::path::Path;
pub struct RowSpool(Spool<RowData>);
pub struct RowWriter(Writer<RowData>);
pub struct RowReader(Reader<RowData>);
/// Constructed only from a hash-checked frame of the sealed row spool.
pub struct RowFrame {
    bytes: Vec<u8>,
    key: String,
    expiry: Option<i64>,
    transform: Option<Transform>,
}
/// Actual source record token and frame facts, never accepted preparation/custody.
/// No public raw-parts constructor, setter, Clone or deserializer.
pub struct RowRecordFrame<'a> {
    pub(crate) key: String,
    pub(crate) expected_key: &'a str,
    pub(crate) record: &'a serde_json::value::RawValue,
    pub(crate) expiry: Option<i64>,
}
impl RowRecordFrame<'_> {
    pub fn key(&self) -> &str {
        &self.key
    }
    pub fn baseline_digest(&self) -> String {
        gateway_auth_primitives::crypto::sha256_hex(self.record.get().as_bytes())
    }
}
impl RowFrame {
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }
    pub fn canonical_parts(&self) -> Result<(RowRecordFrame<'_>, MappingData), GatewayError> {
        #[derive(Deserialize)]
        struct RawRow<'a> {
            key: String,
            #[serde(borrow)]
            record: &'a serde_json::value::RawValue,
            mapping: MappingData,
        }
        let row: RawRow<'_> = serde_json::from_slice(&self.bytes)
            .map_err(|_| invalid("Private canonical row cannot be decoded"))?;
        Ok((
            RowRecordFrame {
                key: row.key,
                expected_key: &self.key,
                record: row.record,
                expiry: self.expiry,
            },
            row.mapping,
        ))
    }
}
impl RowWriter {
    pub fn create(path: &Path) -> Result<Self, GatewayError> {
        Ok(Self(Writer::create(path)?))
    }
    pub fn push(&mut self, value: &RowData) -> Result<(), GatewayError> {
        self.0.push(value)
    }
    pub fn copy_frame(&mut self, frame: &RowFrame) -> Result<(), GatewayError> {
        self.0.push_frame(
            &frame.bytes,
            Some(frame.key.clone()),
            frame.expiry,
            frame.transform,
        )
    }
    pub fn seal(self) -> Result<RowSpool, GatewayError> {
        Ok(RowSpool(self.0.seal()?))
    }
}
impl RowSpool {
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn reader(&self) -> Result<RowReader, GatewayError> {
        Ok(RowReader(self.0.reader()?))
    }
    pub fn key(&self, index: usize) -> Result<&str, GatewayError> {
        self.0.key(index)
    }
    pub fn expiry(&self, index: usize) -> Option<i64> {
        self.0.expiry(index)
    }
    pub fn live(&self, index: usize, now: i64) -> bool {
        self.0.live(index, now)
    }
    pub fn transform(&self, index: usize) -> Option<Transform> {
        self.0.transform(index)
    }
}
impl RowReader {
    pub fn next_row(&mut self) -> Result<Option<RowData>, GatewayError> {
        self.0.next()
    }
    pub fn next_frame(&mut self) -> Result<Option<RowFrame>, GatewayError> {
        let index = self.0.position();
        let Some(bytes) = self.0.next_raw()? else {
            return Ok(None);
        };
        let spool = self.0.spool();
        Ok(Some(RowFrame {
            bytes,
            key: spool.key(index)?.to_owned(),
            expiry: spool.expiry(index),
            transform: spool.transform(index),
        }))
    }
    pub fn read(&mut self, index: usize) -> Result<RowData, GatewayError> {
        self.0.read(index)
    }
    pub fn verify_end(&mut self) -> Result<(), GatewayError> {
        self.0.verify_end()
    }
    pub fn verify_file_end(&mut self) -> Result<(), GatewayError> {
        self.0.verify_file_end()
    }
}
/// Complete normalized row inventory. Rows were already validated by the same
/// native validator before this spool was sealed, so the audit does not decode
/// or rebuild them a second time.
pub struct RowInventory<'a> {
    pub rows: &'a RowSpool,
    pub now: i64,
}
impl AuditInventory for RowInventory<'_> {
    fn visit(
        &self,
        callback: &mut dyn FnMut(&ObservedRow) -> Result<(), GatewayError>,
    ) -> Result<(), GatewayError> {
        let mut reader = self.rows.reader()?;
        while let Some(row) = reader.next_row()? {
            if row
                .record
                .expires_at_ms
                .is_none_or(|expiry| expiry > self.now)
            {
                callback(&ObservedRow {
                    key: row.key,
                    parts: row.parts,
                    record: row.record,
                })?;
            }
        }
        reader.verify_end()
    }
}
