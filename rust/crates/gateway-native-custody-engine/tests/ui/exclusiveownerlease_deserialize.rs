use gateway_native_custody_engine::owner_runtime::ExclusiveOwnerLease;

fn main() {
    let _forged: ExclusiveOwnerLease<'_> = serde_json::from_str("{}").unwrap();
}
