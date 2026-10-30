use gateway_native_custody_engine::custodian_runtime::CustodianRuntime;

fn main() {
    let _forged: CustodianRuntime = serde_json::from_str("{}").unwrap();
}
