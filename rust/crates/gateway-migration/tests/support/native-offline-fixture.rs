//! Fixed synthetic source -> one persisted authority -> latest native inverse.
use gateway_core::error::GatewayError;
#[cfg(not(target_arch = "wasm32"))]
use gateway_migration_test_support::fixture;
#[cfg(not(target_arch = "wasm32"))]
use gateway_migration_test_support::sqlite_store;
#[cfg(not(target_arch = "wasm32"))]
fn main() -> Result<(), Box<dyn std::error::Error>> {
    assert_eq!(
        std::env::args_os().len(),
        1,
        "The fixture accepts no source, credential or target arguments"
    );
    use fixture::{Directory, run};
    use gateway_auth::store::key;
    use gateway_core::{source_graph::GraphNode, state::*};
    use gateway_migration::{
        engine::adopt_empty,
        native_capture::{NativeCapture, prepare_native},
        native_inverse::export_native_latest,
    };
    use sqlite_store::SqliteStore;
    struct Tokens(std::sync::atomic::AtomicU64);
    impl gateway_authority_contracts::TokenSource for Tokens {
        fn next_token(&self) -> Result<String, GatewayError> {
            Ok(format!(
                "native-fixture-{}",
                self.0.fetch_add(1, std::sync::atomic::Ordering::SeqCst)
            ))
        }
    }
    struct Clock;
    impl gateway_authority_contracts::AuthorityClock for Clock {
        fn now_ms(&self) -> Result<i64, GatewayError> {
            Ok(2300)
        }
    }
    let source: NativeCapture =
        serde_json::from_str(include_str!("../fixtures/native-source.json"))?;
    let directory = Directory::new();
    let path = directory.file("source-candidate.sqlite3");
    let store = SqliteStore::open(&path);
    run(adopt_empty(&store, &prepare_native(&source)?))?;
    drop(store);
    let store = SqliteStore::open(&path);
    let token = format!("u_{}", "1".repeat(64));
    assert_eq!(
        run(gateway_auth::api_keys::authenticate(&store, &token, 2300))?.key_id,
        "offline-key"
    );
    let authority = gateway_authority::store::Authority::with_clock(
        store.clone(),
        Tokens(std::sync::atomic::AtomicU64::new(1)),
        Clock,
    );
    run(authority.api_quota().reserve_api_key(
        &gateway_core::identity::ApiKeyId("offline-key".into()),
        &gateway_core::identity::RequestId("native-post-restart-admission".into()),
        "responses",
        2300,
    ))?;
    drop(authority);
    let key = key(serde_json::json!(["default", "native-graph", 0]));
    let original = run(store.read(&key))?.ok_or("Expected retained fixture row")?;
    let mut graph = original.source_graph_value()?;
    for node in &mut graph.nodes {
        if let GraphNode::ArrayBuffer { bytes } = node {
            bytes[4] = 99;
        }
    }
    store.put(
        key,
        StoredRecord::source_graph("latest-alias-write", graph, Some(9500))?,
    );
    drop(store);
    let store = SqliteStore::open(&path);
    let latest = run(export_native_latest(&store, 2300))?;
    let inverse = SqliteStore::open(&directory.file("new-inverse.sqlite3"));
    run(adopt_empty(&inverse, &prepare_native(&latest)?))?;
    println!("{}", serde_json::to_string(&latest)?);
    Ok(())
}
#[cfg(target_arch = "wasm32")]
fn main() {}
