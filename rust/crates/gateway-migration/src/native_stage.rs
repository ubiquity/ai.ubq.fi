//! Private stopped-file preparation. No full-capture payload collection, VM,
//! database, public import route, or capability is created by these spool files.
use crate::core_state::{CoreStateCapture, CoreStateManifest};
use crate::native_custody::NativeCustody;
use crate::{Transform, digest, invalid, json, starts};
use gateway_core::error::GatewayError;
use gateway_core::source_json::SourceJson;
use gateway_core::state::StoredRecord;
use gateway_core::tuple_key::TupleKey;
use gateway_migration_data::error::MigrationError;
use gateway_migration_data::native_capture::NativeCapture;
use gateway_migration_data::native_capture::NativeEntry;
use gateway_migration_spool::canonical::CanonicalWriter;
use gateway_migration_spool::native_entries::{
    NativeEntries, NativeEntryReader, NativeEntryWriter,
};
use gateway_migration_spool::rows::{RowFrame, RowInventory, RowWriter};
use serde::de::{DeserializeSeed, Error, MapAccess, SeqAccess, Visitor};
use serde_json::Value;
use std::{
    collections::BTreeMap,
    fs::File,
    io::BufReader,
    path::{Path, PathBuf},
};

/// Each replay reads the same verified frames in original order. This header
/// contains no entry payload; a separate sealed spool retains every original row.
pub struct Captured {
    header: NativeCapture,
    entries: NativeEntries,
    directory: PathBuf,
}
impl Captured {
    pub fn read(path: &Path, directory: &Path) -> Result<Self, GatewayError> {
        let mut writer = NativeEntryWriter::create(&directory.join("native-entries.spool"))?;
        let file = private_file(path)?;
        let mut decoder = serde_json::Deserializer::from_reader(BufReader::new(file));
        let header = CaptureSeed(&mut writer)
            .deserialize(&mut decoder)
            .map_err(|_| invalid("Private native source document is malformed"))?;
        decoder
            .end()
            .map_err(|_| invalid("Private native source has trailing data"))?;
        header.validate_header()?;
        Ok(Self {
            header,
            entries: writer.seal()?,
            directory: directory.into(),
        })
    }
    pub fn owned(mut source: NativeCapture, directory: &Path) -> Result<Self, GatewayError> {
        source.validate_header()?;
        let mut writer = NativeEntryWriter::create(&directory.join("native-entries.spool"))?;
        for entry in std::mem::take(&mut source.entries) {
            writer.push(&entry)?;
        }
        Ok(Self {
            header: source,
            entries: writer.seal()?,
            directory: directory.into(),
        })
    }
    pub(crate) fn from_parts(
        header: NativeCapture,
        entries: NativeEntries,
        directory: &Path,
    ) -> Self {
        Self {
            header,
            entries,
            directory: directory.into(),
        }
    }
    pub(crate) fn replay(&self, directory: &Path) -> Self {
        Self {
            header: self.header.clone(),
            entries: self.entries.clone(),
            directory: directory.into(),
        }
    }
    pub fn count(&self) -> usize {
        self.entries.len()
    }
    pub(crate) fn metadata(&self) -> &NativeCapture {
        &self.header
    }
    pub fn visit_entries(
        &self,
        callback: &mut dyn FnMut(&NativeEntry) -> Result<(), GatewayError>,
    ) -> Result<(), GatewayError> {
        self.entries.visit(&mut |entry| callback(&entry))
    }
    pub fn entry(&self, index: usize) -> Result<NativeEntry, GatewayError> {
        self.entries.get(index)
    }
    /// Select the bounded operational core without touching a target or opening
    /// a credential. The returned capture still needs the existing preparation
    /// and custody gates before any adoption.
    pub fn core_capture(&self) -> Result<CoreStateCapture, MigrationError> {
        crate::core_state::select_spool(&self.header, &self.entries)
    }
    /// Select and prepare the bounded core in one private offline operation.
    /// The returned plan still needs the existing staged adoption and activation gates.
    pub async fn prepare_core<H: NativeCustody>(
        self,
        host: &H,
    ) -> Result<(CoreStateManifest, crate::staged::StagePlan), MigrationError> {
        let CoreStateCapture { capture, manifest } = self.core_capture()?;
        let directory = gateway_migration_spool::directory(&self.directory)?;
        let plan = Captured::owned(capture, &directory)?.prepare(host).await?;
        Ok((manifest, plan))
    }
    pub(crate) fn reader(&self) -> Result<NativeEntryReader, GatewayError> {
        self.entries.reader()
    }
    pub async fn verify_configured(
        &self,
        custody: &crate::native_operator::ConfiguredNativeCustody,
    ) -> Result<(), MigrationError> {
        let mut reader = self.entries.reader()?;
        while let Some(entry) = reader.next()? {
            custody.verify_access_only_entry(&entry).await?;
        }
        reader.verify_end()?;
        Ok(())
    }
    pub async fn prepare<H: NativeCustody>(
        self,
        host: &H,
    ) -> Result<crate::staged::StagePlan, MigrationError> {
        let version = self.header.validate_header()?;
        let mut writer = RowWriter::create(&self.directory.join("normalized-rows.spool"))?;
        let mut seen = std::collections::BTreeSet::new();
        // Every complete source row, including expired data, passes the original
        // tuple/version/expiry/typed graph/counter and current-custody validators.
        // One sequential pass validates and seals the normalized row inventory.
        let mut reader = self.entries.reader()?;
        while let Some(mut entry) = reader.next()? {
            crate::native_custody::normalize_entry(&mut entry, host).await?;
            let key = TupleKey::from_sort_bytes(entry.raw_key.clone())?.to_record_key();
            if !seen.insert(key.0) {
                return Err(invalid(
                    "Native source key is duplicated or attempts to mint private state",
                )
                .into());
            }
            let row = gateway_migration_data::native_capture::validate_entry(&entry, version)?;
            writer.push(row.as_data())?;
        }
        reader.verify_end()?;
        let normalized = writer.seal()?;
        let audited = gateway_source_audit::driver::inspect_inventory(
            &RowInventory {
                rows: &normalized,
                now: self.header.captured_at_ms,
            },
            self.header.captured_at_ms,
        )?;
        if !audited.errors.is_empty() {
            return Err(MigrationError::SourceInventory(audited));
        }
        // Identity binds the complete original ordered private entry inventory and
        // header. It does not authorize that captured state as current ownership.
        let capture_id = gateway_auth::crypto::sha256_hex(
            &serde_json::to_vec(&(&self.header, self.entries.digest()))
                .map_err(|_| invalid("Private source identity cannot be encoded"))?,
        );
        let revision = format!("capture:{capture_id}");
        let mut projected = RowWriter::create(&self.directory.join("projected-rows.spool"))?;
        let mut reader = normalized.reader()?;
        while let Some(mut row) = reader.next()? {
            let p = &row.parts;
            // The original generic retained graph branch is an identity return.
            // Its full typed validators and complete audit already ran above.
            if !(gateway_migration_projection::projection::retained_typed(p)
                && gateway_migration_projection::projection::known(p)
                && !gateway_migration_projection::source_metadata::named_key(p)
                && (starts(p, &["agent_messages"])
                    || starts(p, &["embeddings"])
                    || starts(p, &["key"])
                    || (starts(p, &["default"])
                        && !matches!(
                            p.get(1).and_then(Value::as_str),
                            Some("kernel_policy_limit_requests" | "kernel_policy_window_ms")
                        ))))
            {
                gateway_migration_projection::projection::forward(&mut row)?;
            }
            projected.push(&row)?;
        }
        reader.verify_end()?;
        let projected = projected.seal()?;
        let (bound, derived) = crate::indexes::disk::bind_spooled(
            &projected,
            self.header.captured_at_ms,
            &revision,
            &self.directory.join("bound-rows.spool"),
        )?;
        let mut canonical = CanonicalWriter::create(&self.directory.join("canonical-rows.spool"))?;
        let mut mapping_digests = BTreeMap::new();
        let mut keys = std::collections::BTreeSet::new();
        let mut reader = bound.reader()?;
        while let Some(frame) = reader.next_frame()? {
            canonical_row(
                &frame,
                (&capture_id, &revision),
                &mut canonical,
                &mut mapping_digests,
                &mut keys,
            )?;
        }
        reader.verify_end()?;
        let mut derived_digests = BTreeMap::new();
        for (key, record) in derived {
            if !keys.insert(key.0.clone())
                || derived_digests
                    .insert(key.0.clone(), digest(&record)?)
                    .is_some()
            {
                return Err(invalid("Derived schema key collides with source state").into());
            }
            canonical.push(&gateway_migration_data::staged::CanonicalRecord { key, record })?;
        }
        let entries: Vec<_> = mapping_digests.into_iter().collect();
        let manifest = gateway_migration_data::manifest::Manifest {
            v: 4,
            capture_id,
            source_git_sha: self.header.source_git_sha,
            captured_at_ms: self.header.captured_at_ms,
            source_rows: normalized.len(),
            mapping_checksum: gateway_auth::crypto::sha256_hex(
                &serde_json::to_vec(&entries)
                    .map_err(|_| invalid("Mapping checksum cannot be encoded"))?,
            ),
            derived: derived_digests,
            native_revision: Some(self.header.data_revision),
        };
        Ok(crate::staged::StagePlan::from_spool(
            manifest,
            canonical.seal()?,
        )?)
    }
}
fn canonical_row(
    frame: &RowFrame,
    identity: (&str, &str),
    canonical: &mut CanonicalWriter,
    mapping_digests: &mut BTreeMap<String, String>,
    keys: &mut std::collections::BTreeSet<String>,
) -> Result<(), MigrationError> {
    let (capture_id, revision) = identity;
    let (source, mut mapping_data) = frame.canonical_parts()?;
    mapping_data.baseline_digest = source.baseline_digest();
    if mapping_data.transform == Transform::Identity {
        mapping_data.source.value = SourceJson::parse("null")?;
        mapping_data.source.key.clear();
    }
    let mapping = gateway_migration_data::manifest::mapping_key(capture_id, source.key());
    let record = StoredRecord::json(revision, json(&mapping_data)?, None);
    mapping_digests.insert(mapping.0.clone(), digest(&record)?);
    if !keys.insert(source.key().to_owned()) {
        return Err(invalid("Staged canonical inventory has duplicate keys").into());
    }
    canonical.push(&gateway_migration_data::staged::CanonicalRecord {
        key: mapping,
        record,
    })?;
    canonical.push_source_row(&source)?;
    Ok(())
}

fn private_file(path: &Path) -> Result<File, GatewayError> {
    use std::os::unix::fs::MetadataExt;
    let before =
        std::fs::symlink_metadata(path).map_err(|_| invalid("Private source cannot be opened"))?;
    if !path.is_absolute() || !before.is_file() || before.mode() & 0o077 != 0 {
        return Err(invalid(
            "Private native capture file is not owner-only/regular",
        ));
    }
    let file = File::open(path).map_err(|_| invalid("Private source cannot be opened"))?;
    let opened = file
        .metadata()
        .map_err(|_| invalid("Private source cannot be opened"))?;
    if before.dev() != opened.dev() || before.ino() != opened.ino() || before.uid() != opened.uid()
    {
        return Err(invalid("Private native capture file changed before read"));
    }
    Ok(file)
}
struct CaptureSeed<'a>(&'a mut NativeEntryWriter);
impl<'de> DeserializeSeed<'de> for CaptureSeed<'_> {
    type Value = NativeCapture;
    fn deserialize<D: serde::Deserializer<'de>>(self, d: D) -> Result<NativeCapture, D::Error> {
        d.deserialize_map(self)
    }
}
impl<'de> Visitor<'de> for CaptureSeed<'_> {
    type Value = NativeCapture;
    fn expecting(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("complete native capture")
    }
    fn visit_map<M: MapAccess<'de>>(self, mut map: M) -> Result<NativeCapture, M::Error> {
        let mut header = serde_json::Map::new();
        while let Some(key) = map.next_key::<String>()? {
            if header.contains_key(&key) {
                return Err(M::Error::custom("Duplicate native capture field"));
            }
            if key == "entries" {
                map.next_value_seed(EntriesSeed(self.0))?;
                header.insert(key, serde_json::json!([]));
            } else {
                if ![
                    "v",
                    "source_git_sha",
                    "captured_at_ms",
                    "source_codec",
                    "native_schema_version",
                    "data_revision",
                    "queue_entries",
                    "running_queue_entries",
                ]
                .contains(&key.as_str())
                {
                    return Err(M::Error::custom("Unknown native capture field"));
                }
                header.insert(key, map.next_value()?);
            }
        }
        serde_json::from_value(Value::Object(header)).map_err(M::Error::custom)
    }
}
struct EntriesSeed<'a>(&'a mut NativeEntryWriter);
impl<'de> DeserializeSeed<'de> for EntriesSeed<'_> {
    type Value = ();
    fn deserialize<D: serde::Deserializer<'de>>(self, d: D) -> Result<(), D::Error> {
        d.deserialize_seq(self)
    }
}
impl<'de> Visitor<'de> for EntriesSeed<'_> {
    type Value = ();
    fn expecting(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("native entry sequence")
    }
    fn visit_seq<S: SeqAccess<'de>>(self, mut entries: S) -> Result<(), S::Error> {
        while let Some(entry) = entries.next_element::<NativeEntry>()? {
            self.0.push(&entry).map_err(S::Error::custom)?;
        }
        Ok(())
    }
}
