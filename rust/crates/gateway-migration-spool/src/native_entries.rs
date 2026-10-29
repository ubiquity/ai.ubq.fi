//! Sealed native-entry DATA frames, not stopped-source or custody authority.
use crate::raw::{Reader, Spool, Writer};
use gateway_migration_data::native_capture::NativeEntry;
use gateway_source::error::GatewayError;
use std::path::Path;
#[derive(Clone)]
pub struct NativeEntries(Spool<NativeEntry>);
pub struct NativeEntryWriter(Writer<NativeEntry>);
pub struct NativeEntryReader(Reader<NativeEntry>);
impl NativeEntryWriter {
    pub fn create(path: &Path) -> Result<Self, GatewayError> {
        Ok(Self(Writer::create(path)?))
    }
    pub fn push(&mut self, entry: &NativeEntry) -> Result<(), GatewayError> {
        self.0.push(entry)
    }
    pub fn seal(self) -> Result<NativeEntries, GatewayError> {
        Ok(NativeEntries(self.0.seal()?))
    }
}
impl NativeEntries {
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn digest(&self) -> String {
        self.0.digest()
    }
    pub fn reader(&self) -> Result<NativeEntryReader, GatewayError> {
        Ok(NativeEntryReader(self.0.reader()?))
    }
    pub fn get(&self, index: usize) -> Result<NativeEntry, GatewayError> {
        self.0.get(index)
    }
    pub fn visit(
        &self,
        callback: &mut dyn FnMut(NativeEntry) -> Result<(), GatewayError>,
    ) -> Result<(), GatewayError> {
        self.0.visit(callback)
    }
}
impl NativeEntryReader {
    pub fn next_entry(&mut self) -> Result<Option<NativeEntry>, GatewayError> {
        self.0.next()
    }
    pub fn read(&mut self, index: usize) -> Result<NativeEntry, GatewayError> {
        self.0.read(index)
    }
    pub fn verify_end(&mut self) -> Result<(), GatewayError> {
        self.0.verify_end()
    }
    pub fn verify_file_end(&mut self) -> Result<(), GatewayError> {
        self.0.verify_file_end()
    }
}
