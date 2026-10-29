use gateway_native_import_request::live::LiveImportCustody;

fn resolves_public_type<T: ?Sized>() {}

#[test]
fn public_defining_path_and_serde_resolve_without_native_construction() {
    resolves_public_type::<LiveImportCustody<'static>>();
    let _from_str = serde_json::from_str::<serde_json::Value>;
}
