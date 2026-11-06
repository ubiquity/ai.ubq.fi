//! Import -> Worker write -> export -> archive -> native latest inverse, over the
//! real staged protocol and a real SQLite authority store. Synthetic data only.
#![cfg(all(unix, not(target_arch = "wasm32")))]
use gateway_auth::crypto::sha256_hex;
use gateway_core::state::{
    AtomicTransition, AuthorityStore, RecordKey, RecordMutation, RevisionCheck, StoredRecord,
};
use gateway_core::tuple_key::TupleKey;
use gateway_core::value::PortableValue;
use gateway_migration::engine::adopt_empty;
use gateway_migration::inverse::ArtifactContext;
use gateway_migration::native_capture::{NativeCapture, NativeValue, prepare_native};
use gateway_migration::native_inverse::export_native_latest;
use gateway_migration::staged::StagePlan;
use gateway_migration_test_support::fixture::{Directory, run};
use gateway_migration_test_support::sqlite_store::SqliteStore;
use gateway_state_reverse::archive::ArchiveStore;
use gateway_state_reverse::reverse::{NoCustody, reverse};
use gateway_state_transfer::http::{
    EXPORT_COUNT_HEADER, EXPORT_CURSOR_HEADER, EXPORT_SHA256_HEADER, Operation, Reply, handle,
};
use gateway_state_transfer::intent::{Scope, token};
use serde_json::{Value, json};
use std::collections::BTreeMap;
use std::fmt::Debug;

type Outcome = Result<(), String>;
trait Check<T> {
    fn check(self, context: &str) -> Result<T, String>;
}
impl<T, E: Debug> Check<T> for Result<T, E> {
    fn check(self, context: &str) -> Result<T, String> {
        self.map_err(|error| format!("{context}: {error:?}"))
    }
}
impl<T> Check<T> for Option<T> {
    fn check(self, context: &str) -> Result<T, String> {
        self.ok_or_else(|| format!("{context}: missing"))
    }
}

const NONCE: &str = "5555555555555555555555555555555555555555555555555555555555555555";

fn fixture() -> Result<NativeCapture, String> {
    let path = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../gateway-migration/tests/fixtures/native-source.json"
    );
    let text = std::fs::read_to_string(path).check("fixture")?;
    serde_json::from_str(&text).check("fixture json")
}

/// The fixture plus rows large enough that staging needs several chunks.
fn multi_chunk_fixture() -> Result<NativeCapture, String> {
    use gateway_core::tuple_key::KeyPart;
    use gateway_migration::native_capture::{NativeEntry, NativeRevision};
    let mut capture = fixture()?;
    for name in ["big-a", "big-b"] {
        let parts = [
            KeyPart::String("default".into()),
            KeyPart::String(name.into()),
        ];
        capture.entries.push(NativeEntry {
            raw_key: TupleKey::from_parts(&parts)
                .check("key")?
                .sort_bytes()
                .to_vec(),
            value: NativeValue::Bytes(vec![7; 1_100_000]),
            revision: NativeRevision {
                version: "49".into(),
                sequence: "0".into(),
            },
            expiration_ms: 4_102_444_800_000,
        });
    }
    Ok(capture)
}

struct Plan {
    capture_id: String,
    frames: Vec<Vec<u8>>,
}
fn plan(capture: &NativeCapture) -> Result<Plan, String> {
    let mut plan =
        StagePlan::from_prepared(prepare_native(capture).check("prepare")?).check("plan")?;
    let capture_id = plan.summary().capture_id;
    let mut frames = vec![serde_json::to_vec(&plan.begin()).check("begin")?];
    while let Some(batch) = plan.next_batch().check("batch")? {
        frames.push(serde_json::to_vec(&batch).check("append")?);
    }
    frames.push(serde_json::to_vec(&plan.seal()).check("seal")?);
    Ok(Plan { capture_id, frames })
}

struct Operator {
    token: String,
    configured: String,
}
impl Operator {
    fn new(scope: &Scope) -> Result<Self, String> {
        let token = token(scope, NONCE).check("token")?;
        Ok(Self {
            configured: sha256_hex(token.as_bytes()),
            token,
        })
    }
    fn send(&self, store: &SqliteStore, operation: Operation, body: &[u8]) -> Result<Reply, u16> {
        run(handle(
            store,
            operation,
            Some(&self.configured),
            Some(&self.token),
            body,
        ))
        .map_err(|error| error.status)
    }
    fn json(
        &self,
        store: &SqliteStore,
        operation: Operation,
        body: &[u8],
    ) -> Result<Value, String> {
        let reply = self.send(store, operation, body).check("reply")?;
        serde_json::from_slice(&reply.body).check("reply json")
    }
}

fn header<'a>(reply: &'a Reply, name: &str) -> Option<&'a str> {
    reply
        .headers
        .iter()
        .find(|(key, _)| *key == name)
        .map(|(_, value)| value.as_str())
}

/// Pages the export with a small page so the cursor path is exercised.
fn export_archive(store: &SqliteStore) -> Result<(Vec<u8>, usize), String> {
    let operator = Operator::new(&Scope::Export)?;
    let mut archive = Vec::new();
    let mut records = 0usize;
    let mut cursor: Option<String> = None;
    loop {
        let body = serde_json::to_vec(&json!({"v":1,"cursor":cursor,"limit":3})).check("body")?;
        let reply = operator
            .send(store, Operation::ExportPage, &body)
            .check("export")?;
        assert_eq!(
            header(&reply, EXPORT_SHA256_HEADER),
            Some(sha256_hex(&reply.body).as_str())
        );
        let count: usize = header(&reply, EXPORT_COUNT_HEADER)
            .check("count")?
            .parse()
            .check("n")?;
        let page: Vec<Value> = serde_json::from_slice(&reply.body).check("page")?;
        assert_eq!(page.len(), count);
        records += count;
        archive.extend_from_slice(&reply.body);
        archive.push(b'\n');
        cursor = header(&reply, EXPORT_CURSOR_HEADER).map(str::to_owned);
        if cursor.is_none() {
            return Ok((archive, records));
        }
    }
}

fn entries(capture: &NativeCapture) -> Result<BTreeMap<Vec<u8>, String>, String> {
    capture
        .entries
        .iter()
        .map(|e| Ok((e.raw_key.clone(), serde_json::to_string(e).check("entry")?)))
        .collect()
}

fn counter_key() -> Result<RecordKey, String> {
    use gateway_core::tuple_key::KeyPart;
    let parts = [
        KeyPart::String("default".into()),
        KeyPart::String("native-counter".into()),
    ];
    Ok(TupleKey::from_parts(&parts).check("key")?.to_record_key())
}

/// Applies every frame, then replays each one as a lost-response retry would.
fn import_all(target: &SqliteStore, plan: &Plan) -> Outcome {
    let operator = Operator::new(&Scope::Import {
        capture_id: plan.capture_id.clone(),
    })?;
    let status = operator.json(target, Operation::ImportStatus, b"")?;
    assert_eq!(status["state"], "empty");
    for (index, frame) in plan.frames.iter().enumerate() {
        let applied = operator.json(target, Operation::ImportFrame, frame)?;
        assert_eq!(applied["outcome"], "applied", "frame {index}");
        assert_eq!(applied["status"]["activation_authorized"], false);
        // A lost response is retried with the exact same frame: no second write.
        let replayed = operator.json(target, Operation::ImportFrame, frame)?;
        assert_eq!(replayed["outcome"], "replayed", "frame {index}");
        assert_eq!(replayed["status"], applied["status"]);
    }
    let sealed = operator.json(target, Operation::ImportStatus, b"")?;
    assert_eq!(sealed["state"], "sealed");
    assert_eq!(sealed["received_records"], sealed["expected_records"]);
    Ok(())
}

#[test]
fn several_chunks_import_with_exact_replay_of_each_committed_chunk() -> Outcome {
    let directory = Directory::new();
    let target = SqliteStore::open(&directory.file("target.sqlite3"));
    let plan = plan(&multi_chunk_fixture()?)?;
    assert!(plan.frames.len() >= 4, "several append chunks");
    import_all(&target, &plan)
}

#[test]
fn import_resume_write_export_and_reverse_preserve_every_latest_row() -> Outcome {
    let directory = Directory::new();
    let target = SqliteStore::open(&directory.file("target.sqlite3"));
    let source = fixture()?;
    let plan = plan(&source)?;
    import_all(&target, &plan)?;
    let staged = run(export_native_latest(&target, 2300)).check("staged inverse")?;
    // The established inverse normalizes graph wire tokens, so the staged
    // result must equal the existing atomic adoption's inverse exactly, and
    // keep every raw key, original revision and absolute expiry of the source.
    let atomic = SqliteStore::open(&directory.file("atomic.sqlite3"));
    run(adopt_empty(
        &atomic,
        &prepare_native(&source).check("prepare")?,
    ))
    .check("adopt")?;
    let baseline = run(export_native_latest(&atomic, 2300)).check("atomic inverse")?;
    assert_eq!(entries(&staged)?, entries(&baseline)?);
    let identity = |capture: &NativeCapture| {
        let rows = capture.entries.iter();
        rows.map(|e| (e.raw_key.clone(), (e.revision.clone(), e.expiration_ms)))
            .collect::<BTreeMap<_, _>>()
    };
    assert_eq!(identity(&staged), identity(&source));

    // A post-adoption Worker write must survive the reverse path.
    let key = counter_key()?;
    let current = run(target.read(&key)).check("read")?.check("counter")?;
    let written = StoredRecord::profile("worker-write-1", PortableValue::KvU64("7".into()), None)
        .check("record")?;
    let transition = AtomicTransition {
        checks: vec![RevisionCheck {
            key: key.clone(),
            expected_revision: Some(current.revision),
        }],
        mutations: vec![RecordMutation::Put {
            key,
            record: written,
        }],
    };
    run(target.commit(transition)).check("worker write")?;
    let latest = run(export_native_latest(&target, 2400)).check("latest")?;

    let (archive, records) = export_archive(&target)?;
    let sha = sha256_hex(&archive);
    let store = ArchiveStore::from_bytes(&archive, &sha, records).check("archive")?;
    let mirrored = run(export_native_latest(&store, 2400)).check("archive inverse")?;
    assert_eq!(entries(&mirrored)?, entries(&latest)?);
    let counter = mirrored.entries.iter().find(
        |e| matches!(&e.value, NativeValue::CounterLe64(bytes) if bytes == &7u64.to_le_bytes()),
    );
    assert!(
        counter.is_some(),
        "the Worker write is in the reverse capture"
    );
    assert_ne!(mirrored.data_revision, source.data_revision);

    let artifacts = ArtifactContext {
        rust_git_sha: "a".repeat(40),
        rust_git_tree: "b".repeat(40),
        wasm_sha256: "c".repeat(64),
        native_sha256: "d".repeat(64),
        native_nonce_capture_sha256: None,
    };
    let output = directory.file("reverse");
    let (count, written, _) =
        run(reverse(&store, 2400, &NoCustody, artifacts, &output)).check("reverse")?;
    let bytes = std::fs::read(&written.path).check("source file")?;
    assert_eq!(sha256_hex(&bytes), written.sha256);
    let reversed: NativeCapture = serde_json::from_slice(&bytes).check("reversed")?;
    assert_eq!(count, reversed.entries.len());
    assert_eq!(entries(&reversed)?, entries(&latest)?);
    Ok(())
}

#[test]
fn refusals_happen_before_any_write() -> Outcome {
    let directory = Directory::new();
    let source = fixture()?;
    let plan = plan(&source)?;
    let scope = Scope::Import {
        capture_id: plan.capture_id.clone(),
    };
    let operator = Operator::new(&scope)?;
    let target = SqliteStore::open(&directory.file("target.sqlite3"));
    let begin = &plan.frames[0];
    let first = &plan.frames[1];

    // Closed Worker, wrong scope and wrong capture refuse without reading data.
    let closed = run(handle(
        &target,
        Operation::ImportFrame,
        None,
        Some(&operator.token),
        begin,
    ));
    assert_eq!(closed.err().map(|e| e.status), Some(404));
    let export = Operator::new(&Scope::Export)?;
    assert_eq!(
        export.send(&target, Operation::ImportFrame, begin).err(),
        Some(403)
    );
    assert_eq!(
        operator.send(&target, Operation::ExportPage, b"{}").err(),
        Some(403)
    );
    let other = Operator::new(&Scope::Import {
        capture_id: "f".repeat(64),
    })?;
    assert_eq!(
        other.send(&target, Operation::ImportFrame, begin).err(),
        Some(403)
    );
    // Appends before begin refuse; begin then an out-of-order chunk refuses.
    assert_eq!(
        operator.send(&target, Operation::ImportFrame, first).err(),
        Some(409)
    );
    operator.json(&target, Operation::ImportFrame, begin)?;
    let mut skipped: Value = serde_json::from_slice(first).check("frame")?;
    skipped["data"]["sequence"] = json!(1);
    let skipped = serde_json::to_vec(&skipped).check("skip")?;
    assert_eq!(
        operator
            .send(&target, Operation::ImportFrame, &skipped)
            .err(),
        Some(409)
    );
    // A negative absolute expiry is rejected before the chunk's transition.
    let mut expired: Value = serde_json::from_slice(first).check("frame")?;
    expired["data"]["records"][0]["record"]["expires_at_ms"] = json!(-5);
    let expired = serde_json::to_vec(&expired).check("expired")?;
    assert_eq!(
        operator
            .send(&target, Operation::ImportFrame, &expired)
            .err(),
        Some(409)
    );
    // A partial target cannot export as an accepted inverse.
    let page = serde_json::to_vec(&json!({"v":1,"cursor":null,"limit":3})).check("page")?;
    assert_eq!(
        export.send(&target, Operation::ExportPage, &page).err(),
        Some(409)
    );
    let status = operator.json(&target, Operation::ImportStatus, b"")?;
    assert_eq!(status["state"], "staging");
    assert_eq!(status["next_sequence"], 0);
    assert_eq!(status["received_records"], 0);
    // Resume from the reported chunk and finish.
    for frame in &plan.frames[1..] {
        operator.json(&target, Operation::ImportFrame, frame)?;
    }

    // An occupied target with no staged capture refuses import.
    let occupied = SqliteStore::open(&directory.file("occupied.sqlite3"));
    occupied.put(counter_key()?, StoredRecord::json("x", json!(1), None));
    assert_eq!(
        operator
            .send(&occupied, Operation::ImportFrame, begin)
            .err(),
        Some(409)
    );
    let status = operator.json(&occupied, Operation::ImportStatus, b"")?;
    assert_eq!(status["state"], "occupied");

    // A tampered, reordered or miscounted archive is refused.
    let (archive, records) = export_archive(&target)?;
    let sha = sha256_hex(&archive);
    assert!(ArchiveStore::from_bytes(&archive, &sha, records + 1).is_err());
    assert!(ArchiveStore::from_bytes(&archive, &"0".repeat(64), records).is_err());
    let mut lines: Vec<&[u8]> = archive
        .split(|b| *b == b'\n')
        .filter(|l| !l.is_empty())
        .collect();
    lines.reverse();
    let reordered = lines.join(&b'\n');
    let mut reordered = reordered;
    reordered.push(b'\n');
    let sha = sha256_hex(&reordered);
    assert!(ArchiveStore::from_bytes(&reordered, &sha, records).is_err());
    Ok(())
}
