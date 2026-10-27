#![cfg(not(target_arch = "wasm32"))]
use gateway_core::state::AuthorityStore;
use gateway_migration::{
    engine::adopt_empty,
    graph_transport,
    inverse::ArtifactContext,
    native_capture::{NativeCapture, NativeEntry, NativeValue, prepare_native},
    native_custody::{NativeCustody, OwnedNativeSnapshot},
    native_inverse::{export_native_latest, export_native_rollback_bundle},
    native_operator, native_stage,
    staged::*,
};
use gateway_migration_test_support::fixture::{Directory, run};
use gateway_migration_test_support::sqlite_store::SqliteStore;
fn fixture() -> NativeCapture {
    serde_json::from_str(include_str!("fixtures/native-source.json")).unwrap()
}
fn copy(request: &StageRequest) -> StageRequest {
    decode_stage_request(&serde_json::to_vec(request).unwrap()).unwrap()
}
#[test]
fn private_staging_seals_exact_native_state_without_claiming_one_atomic_commit() {
    let directory = Directory::new();
    let path = directory.file("staged.sqlite3");
    let source = fixture();
    let mut plan = StagePlan::from_prepared(prepare_native(&source).unwrap()).unwrap();
    let store = SqliteStore::open(&path);
    let begin = plan.begin();
    assert!(!run(execute_staged(&store, copy(&begin))).unwrap().sealed);
    assert!(run(execute_staged(&store, copy(&begin))).is_err());
    assert!(run(execute_staged(&store, plan.seal())).is_err());
    assert!(run(export_native_latest(&store, 2300)).is_err());
    while let Some(batch) = plan.next_batch().unwrap() {
        assert!(serde_json::to_vec(&batch).unwrap().len() <= STAGED_BATCH_MAX_BYTES);
        if let StageRequest::Append(mut wrong) = copy(&batch) {
            wrong.sequence += 1;
            assert!(run(execute_staged(&store, StageRequest::Append(wrong))).is_err());
        }
        if let StageRequest::Append(mut wrong) = copy(&batch) {
            wrong.digest.replace_range(
                ..1,
                if wrong.digest.starts_with('0') {
                    "1"
                } else {
                    "0"
                },
            );
            assert!(run(execute_staged(&store, StageRequest::Append(wrong))).is_err());
        }
        assert!(!run(execute_staged(&store, copy(&batch))).unwrap().sealed);
        assert!(run(execute_staged(&store, batch)).is_err());
    }
    let receipt = run(execute_staged(&store, plan.seal())).unwrap();
    assert!(receipt.sealed && !receipt.activation_authorized);
    assert_eq!(receipt.source_rows, source.entries.len());
    assert!(
        serde_json::to_value(&receipt)
            .unwrap()
            .get("atomic_committed")
            .is_none()
    );
    drop(store);
    let store = SqliteStore::open(&path);
    let latest = run(export_native_latest(&store, 2300)).unwrap();
    let map = |capture: &NativeCapture| {
        capture
            .entries
            .iter()
            .map(|e| (e.raw_key.clone(), serde_json::to_string(e).unwrap()))
            .collect::<std::collections::BTreeMap<_, _>>()
    };
    // The established inverse intentionally normalizes source graph wire tokens.
    // Staging must match the ENTIRE existing atomic adoption/inverse result,
    // not demand a new raw-JSON spelling guarantee from that codec.
    let atomic = SqliteStore::open(&directory.file("atomic-baseline.sqlite3"));
    run(adopt_empty(&atomic, &prepare_native(&source).unwrap())).unwrap();
    let baseline = run(export_native_latest(&atomic, 2300)).unwrap();
    assert_eq!(map(&baseline), map(&latest));
    assert!(run(execute_staged(&store, begin)).is_err());
    assert!(
        run(store.read(&gateway_auth::api_keys::id_key("offline-key")))
            .unwrap()
            .is_some()
    );
}
#[test]
fn bounded_closed_frames_and_global_source_audit_are_not_per_batch_guesses() {
    assert!(
        decode_stage_request(
            b"{\"stage\":\"seal\",\"data\":{\"capture_id\":\"x\"},\"extra\":true}"
        )
        .is_err()
    );
    assert!(decode_stage_request(b"{\"stage\":\"append\",\"data\":").is_err());
    assert!(decode_stage_request(&vec![b' '; STAGED_BATCH_MAX_BYTES + 1]).is_err());
    let mut source = fixture();
    source.entries.push(source.entries[0].clone());
    assert!(prepare_native(&source).is_err());
}

#[test]
fn moved_complete_audit_retains_expired_typed_metadata_and_rejects_orphan_coupling() {
    use gateway_kv_codec::{tuple_key::KeyPart, tuple_key::TupleKey, value::PortableValue};
    let source = fixture();
    let mut corrupt = source.clone();
    corrupt.entries.retain(|entry| {
        TupleKey::from_sort_bytes(entry.raw_key.clone())
            .unwrap()
            .to_record_key()
            != gateway_auth::api_keys::id_key("offline-key")
    });
    assert_eq!(corrupt.entries.len(), source.entries.len() - 1);
    assert!(
        prepare_native(&corrupt).is_err(),
        "the same complete hash/ID audit must reject an orphan"
    );
    let mut source = source;
    let tuple = TupleKey::from_parts(&[
        KeyPart::String("default".into()),
        KeyPart::String("expired-audit-owned".into()),
    ])
    .unwrap();
    let revision = source.data_revision.clone();
    let expiry = source.captured_at_ms - 1;
    source.entries.insert(
        1,
        NativeEntry {
            raw_key: tuple.sort_bytes().to_vec(),
            value: NativeValue::CounterLe64(u64::MAX.to_le_bytes().to_vec()),
            revision: revision.clone(),
            expiration_ms: expiry,
        },
    );
    let directory = Directory::new();
    let store = SqliteStore::open(&directory.file("owned-audit.sqlite3"));
    let result = run(adopt_empty(&store, &prepare_native(&source).unwrap())).unwrap();
    assert_eq!(result.source_rows, source.entries.len());
    let retained = run(store.read(&tuple.to_record_key())).unwrap().unwrap();
    assert_eq!(
        retained.revision,
        format!("native-v1:{}:{}", revision.version, revision.sequence)
    );
    assert_eq!(retained.expires_at_ms, Some(expiry));
    assert_eq!(
        retained.portable_value().unwrap(),
        PortableValue::KvU64(u64::MAX.to_string())
    );
}

fn add_expired_and_utf16_entries(
    capture: &mut NativeCapture,
) -> Result<(gateway_kv_codec::tuple_key::TupleKey, i64), Box<dyn std::error::Error>> {
    use gateway_kv_codec::tuple_key::{KeyPart, TupleKey};
    let expired_key = TupleKey::from_parts(&[
        KeyPart::String("default".into()),
        KeyPart::String("expired-native-replay".into()),
    ])?;
    let expiry = capture.captured_at_ms - 1;
    capture.entries.insert(
        1,
        NativeEntry {
            raw_key: expired_key.sort_bytes().to_vec(),
            value: NativeValue::CounterLe64(u64::MAX.to_le_bytes().to_vec()),
            revision: capture.data_revision.clone(),
            expiration_ms: expiry,
        },
    );
    capture.entries.push(NativeEntry {
        raw_key: TupleKey::from_parts(&[
            KeyPart::String("default".into()),
            KeyPart::String("native-frame-utf16".into()),
        ])?
        .sort_bytes()
        .to_vec(),
        value: NativeValue::SourceGraphV1(graph_transport::to_source_graph(
            &gateway_source_graph::graph::SourceGraph {
                root: gateway_source_graph::graph::GraphAtom::String(
                    gateway_source::js_string::JsString16::from_units(vec![0xd800, 0, 0xdc00]),
                ),
                nodes: vec![],
            },
        )?),
        revision: capture.data_revision.clone(),
        expiration_ms: -1,
    });
    Ok((expired_key, expiry))
}
#[test]
fn sealed_file_inventory_matches_slice_audit_and_rejects_truncated_native_document()
-> Result<(), Box<dyn std::error::Error>> {
    use gateway_kv_codec::{tuple_key::TupleKey, value::PortableValue};
    struct Host;
    impl NativeCustody for Host {
        async fn observe(
            &self,
            _: &str,
        ) -> Result<OwnedNativeSnapshot, gateway_source::error::GatewayError> {
            Err(gateway_source::error::GatewayError::unavailable(
                "Unexpected custody request",
                "fixture",
            ))
        }
    }
    let directory = Directory::new();
    let mut capture = fixture();
    let mut corrupt = capture.clone();
    let mut retained = Vec::new();
    for entry in corrupt.entries {
        if TupleKey::from_sort_bytes(entry.raw_key.clone())?.to_record_key()
            != gateway_auth::api_keys::id_key("offline-key")
        {
            retained.push(entry);
        }
    }
    corrupt.entries = retained;
    let corrupt_dir = native_stage::directory(&std::env::temp_dir())?;
    assert!(
        run(native_stage::Captured::owned(corrupt, &corrupt_dir)?.prepare(&Host)).is_err(),
        "sealed native replay must retain the complete orphan-pair audit"
    );
    std::fs::remove_dir_all(corrupt_dir)?;
    let (expired_key, expiry) = add_expired_and_utf16_entries(&mut capture)?;
    let dir = native_stage::directory(&std::env::temp_dir())?;
    let mut plan = run(native_stage::Captured::owned(capture.clone(), &dir)?.prepare(&Host))?;
    let wire_dir = native_stage::directory(&std::env::temp_dir())?;
    let mut wire_plan =
        run(native_stage::Captured::owned(capture.clone(), &wire_dir)?.prepare(&Host))?;
    assert_eq!(
        serde_json::to_vec(&wire_plan.begin())?,
        serde_json::to_vec(&plan.begin())?
    );
    let store = SqliteStore::open(&directory.file("spooled.sqlite3"));
    run(execute_staged(&store, plan.begin()))?;
    while let Some(batch) = plan.next_batch()? {
        let mut bytes = Vec::new();
        assert!(wire_plan.write_next_batch(&mut bytes)?);
        assert_eq!(bytes, serde_json::to_vec(&batch)?);
        assert!(bytes.len() <= STAGED_BATCH_MAX_BYTES);
        run(execute_staged(&store, decode_stage_request(&bytes)?))?;
    }
    assert!(!wire_plan.write_next_batch(&mut Vec::new())?);
    assert_eq!(
        serde_json::to_vec(&wire_plan.seal())?,
        serde_json::to_vec(&plan.seal())?
    );
    run(execute_staged(&store, plan.seal()))?;
    let retained =
        run(store.read(&expired_key.to_record_key()))?.ok_or("Expected retained fixture row")?;
    assert_eq!(
        retained.revision,
        format!(
            "native-v1:{}:{}",
            capture.data_revision.version, capture.data_revision.sequence
        )
    );
    assert_eq!(retained.expires_at_ms, Some(expiry));
    assert_eq!(
        retained.portable_value()?,
        PortableValue::KvU64(u64::MAX.to_string())
    );
    let latest = run(export_native_latest(&store, 2300))?;
    let ordinary = SqliteStore::open(&directory.file("ordinary.sqlite3"));
    run(adopt_empty(&ordinary, &prepare_native(&capture)?))?;
    let expected = run(export_native_latest(&ordinary, 2300))?;
    assert_eq!(serde_json::to_vec(&latest)?, serde_json::to_vec(&expected)?);
    let artifacts = ArtifactContext {
        rust_git_sha: "1".repeat(40),
        rust_git_tree: "2".repeat(40),
        wasm_sha256: "3".repeat(64),
        native_sha256: "4".repeat(64),
        native_nonce_capture_sha256: None,
    };
    let bundle = run(export_native_rollback_bundle(
        &store,
        2300,
        artifacts.clone(),
    ))?;
    let mut source_sink = Vec::new();
    let mut custody_sink = Vec::new();
    let count = run(native_operator::write_private_latest(
        &store,
        2300,
        &Host,
        artifacts,
        &mut source_sink,
        &mut custody_sink,
    ))?;
    assert_eq!(count, bundle.source.entries.len());
    assert_eq!(source_sink, serde_json::to_vec(&bundle.source)?);
    assert_eq!(
        serde_json::from_slice::<serde_json::Value>(&custody_sink)?,
        serde_json::json!({"rust_controls":bundle.rust_controls,"artifacts":bundle.artifacts})
    );
    let path = directory.file("native.json");
    std::fs::write(&path, b"{\"entries\":[")?;
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600))?;
    let second = native_stage::directory(&std::env::temp_dir())?;
    assert!(native_stage::Captured::read(&path, &second).is_err());
    std::fs::remove_dir_all(dir)?;
    std::fs::remove_dir_all(wire_dir)?;
    std::fs::remove_dir_all(second)?;
    Ok(())
}

#[test]
fn sealed_native_frame_emitter_refuses_spool_and_sink_faults()
-> Result<(), Box<dyn std::error::Error>> {
    use std::{
        io::{Seek, SeekFrom, Write},
        os::unix::fs::PermissionsExt,
    };
    struct Host;
    impl NativeCustody for Host {
        async fn observe(
            &self,
            _: &str,
        ) -> Result<OwnedNativeSnapshot, gateway_source::error::GatewayError> {
            Err(gateway_source::error::GatewayError::unavailable(
                "Unexpected custody request",
                "fixture",
            ))
        }
    }
    let plan = || -> Result<_, Box<dyn std::error::Error>> {
        let directory = native_stage::directory(&std::env::temp_dir())?;
        let plan = run(native_stage::Captured::owned(fixture(), &directory)?.prepare(&Host))?;
        Ok((directory, plan))
    };
    for fault in 0..4 {
        let (directory, mut plan) = plan()?;
        let path = directory.join("canonical-rows.spool");
        let original = std::fs::read(&path)?;
        match fault {
            0 => {
                let mut file = std::fs::OpenOptions::new().append(true).open(&path)?;
                file.write_all(b" ")?;
                file.sync_all()?;
            }
            1 => {
                let mut file = std::fs::OpenOptions::new().write(true).open(&path)?;
                file.seek(SeekFrom::Start(0))?;
                file.write_all(b"[")?;
                file.sync_all()?;
            }
            2 => {
                std::fs::OpenOptions::new()
                    .write(true)
                    .open(&path)?
                    .set_len(original.len() as u64 - 1)?;
            }
            _ => {
                std::fs::rename(&path, directory.join("original.spool"))?;
                std::fs::write(&path, original)?;
                std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600))?;
            }
        }
        let mut sink = Vec::new();
        assert!(plan.write_next_batch(&mut sink).is_err());
        assert!(sink.is_empty());
        assert!(plan.next_batch().is_err());
        std::fs::remove_dir_all(directory)?;
    }
    struct PartialSink(bool);
    impl Write for PartialSink {
        fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
            if self.0 {
                return Err(std::io::Error::other("injected partial sink failure"));
            }
            self.0 = true;
            Ok(bytes.len().min(1))
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }
    let (directory, mut plan) = plan()?;
    assert!(plan.write_next_batch(&mut PartialSink(false)).is_err());
    assert!(plan.write_next_batch(&mut Vec::new()).is_err());
    assert!(plan.next_batch().is_err());
    std::fs::remove_dir_all(directory)?;
    Ok(())
}
