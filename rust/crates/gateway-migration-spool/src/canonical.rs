//! Finite canonical DATA frames with private generic storage and exact source tokens.
use crate::raw::{Reader, Spool, Writer};
use crate::rows::RowRecordFrame;
use gateway_migration_data::{invalid, staged::CanonicalRecord};
use gateway_source::error::GatewayError;
use std::path::Path;
pub struct CanonicalRows(Spool<CanonicalRecord>);
pub struct CanonicalWriter(Writer<CanonicalRecord>);
pub struct CanonicalReader(Reader<CanonicalRecord>);
impl CanonicalWriter {
    pub fn create(path: &Path) -> Result<Self, GatewayError> {
        Ok(Self(Writer::create(path)?))
    }
    pub fn push(&mut self, value: &CanonicalRecord) -> Result<(), GatewayError> {
        self.0.push(value)
    }
    /// Only a verified row frame can supply this exact serialized record token.
    pub fn push_source_row(&mut self, row: &RowRecordFrame<'_>) -> Result<(), GatewayError> {
        if row.key != row.expected_key {
            return Err(invalid("Private canonical key changed"));
        }
        let bytes = format!(
            "{{\"key\":{},\"record\":{}}}",
            serde_json::to_string(&row.key)
                .map_err(|_| invalid("Private native spool is changed, truncated or unreadable"))?,
            row.record.get()
        );
        self.0
            .push_frame(bytes.as_bytes(), Some(row.key.clone()), row.expiry, None)
    }
    pub fn seal(self) -> Result<CanonicalRows, GatewayError> {
        Ok(CanonicalRows(self.0.seal()?))
    }
}
impl CanonicalRows {
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn reader(&self) -> Result<CanonicalReader, GatewayError> {
        Ok(CanonicalReader(self.0.reader()?))
    }
    pub fn key(&self, index: usize) -> Result<&str, GatewayError> {
        self.0.key(index)
    }
    pub fn encoded_size(&self, index: usize) -> usize {
        self.0.encoded_size(index)
    }
}
impl CanonicalReader {
    pub fn len(&self) -> usize {
        self.0.spool().len()
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn key(&self, index: usize) -> Result<&str, GatewayError> {
        self.0.spool().key(index)
    }
    pub fn encoded_size(&self, index: usize) -> usize {
        self.0.spool().encoded_size(index)
    }
    pub fn next_raw(&mut self) -> Result<Option<Vec<u8>>, GatewayError> {
        self.0.next_raw()
    }
    pub fn read(&mut self, index: usize) -> Result<CanonicalRecord, GatewayError> {
        self.0.read(index)
    }
    pub fn read_raw(&mut self, index: usize) -> Result<Vec<u8>, GatewayError> {
        self.0.read_raw(index)
    }
    pub fn verify_end(&mut self) -> Result<(), GatewayError> {
        self.0.verify_end()
    }
    pub fn verify_file_end(&mut self) -> Result<(), GatewayError> {
        self.0.verify_file_end()
    }
}
