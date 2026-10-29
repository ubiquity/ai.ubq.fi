use gateway_native_custody_engine::exclusive_owner::ExclusiveManagedOwner;

fn main() {
    let _forged: ExclusiveManagedOwner = serde_json::from_str("{}").unwrap();
}
