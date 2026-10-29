use gateway_native_import_store::durability::DurableNativeImport;

fn main() {
    let _forged: DurableNativeImport = serde_json::from_str("{}").unwrap();
}
