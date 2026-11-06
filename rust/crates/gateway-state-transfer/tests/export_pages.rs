//! Export page bounds: the byte budget trims a page and resumes exactly after
//! the last returned key, so no row is skipped or duplicated.
#![cfg(not(target_arch = "wasm32"))]
use gateway_core::state::StoredRecord;
use gateway_core::tuple_key::{KeyPart, TupleKey};
use gateway_migration_test_support::fixture::{Directory, run};
use gateway_migration_test_support::sqlite_store::SqliteStore;
use gateway_state_transfer::export::{EXPORT_PAGE_MAX_BYTES, ExportRequest, export_page};
use serde_json::json;

fn key(parts: &[&str]) -> Result<gateway_core::state::RecordKey, String> {
    let parts: Vec<KeyPart> = parts.iter().map(|p| KeyPart::String((*p).into())).collect();
    TupleKey::from_parts(&parts)
        .map(|key| key.to_record_key())
        .map_err(|e| format!("{e:?}"))
}

#[test]
fn oversized_pages_trim_and_resume_after_the_last_returned_key() -> Result<(), String> {
    let directory = Directory::new();
    let store = SqliteStore::open(&directory.file("export.sqlite3"));
    let manifest = key(&["uos_ai", "migration", "v3", "manifest"])?;
    store.put(manifest, StoredRecord::json("capture:x", json!({}), None));
    let large = "x".repeat(EXPORT_PAGE_MAX_BYTES / 3 + 1);
    for name in ["a", "b", "c", "d"] {
        let record = StoredRecord::json("r1", json!(large), Some(4_102_444_800_000));
        store.put(key(&["default", name])?, record);
    }
    let mut cursor = None;
    let mut counts = Vec::new();
    let mut keys = Vec::new();
    loop {
        let request = ExportRequest {
            v: 1,
            cursor: cursor.take(),
            limit: 256,
        };
        let page = run(export_page(&store, request)).map_err(|e| format!("{e:?}"))?;
        assert!(page.body.len() <= EXPORT_PAGE_MAX_BYTES || page.count == 1);
        assert_eq!(page.sha256, gateway_auth::crypto::sha256_hex(&page.body));
        let rows: Vec<serde_json::Value> =
            serde_json::from_slice(&page.body).map_err(|e| e.to_string())?;
        keys.extend(rows.iter().map(|row| row["key"].clone()));
        let mut large_rows = rows.iter().filter(|row| row["record"]["revision"] == "r1");
        assert!(large_rows.all(|row| row["record"]["expires_at_ms"] == 4_102_444_800_000i64));
        counts.push(page.count);
        cursor = page.next_cursor;
        if cursor.is_none() {
            break;
        }
    }
    assert_eq!(counts, vec![2, 3]);
    let mut unique = keys.clone();
    unique.dedup();
    assert_eq!(unique.len(), 5);
    let bad = ExportRequest {
        v: 1,
        cursor: None,
        limit: 0,
    };
    assert!(run(export_page(&store, bad)).is_err());
    Ok(())
}
