use gateway_native_import_request::live::LiveImportCustody;

fn main() {
    let _forged: LiveImportCustody<'_> = serde_json::from_str("{}").unwrap();
}
