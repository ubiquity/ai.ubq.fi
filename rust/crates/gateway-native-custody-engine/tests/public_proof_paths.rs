use gateway_native_custody_engine::custodian_runtime::CustodianRuntime;
use gateway_native_custody_engine::exclusive_owner::ExclusiveManagedOwner;
use gateway_native_custody_engine::owner_runtime::ExclusiveOwnerLease;

fn resolves_public_type<T: ?Sized>() {}

#[test]
fn public_defining_paths_resolve_without_constructing_native_proofs() {
    resolves_public_type::<ExclusiveManagedOwner>();
    resolves_public_type::<ExclusiveOwnerLease<'static>>();
    resolves_public_type::<CustodianRuntime>();
    let _from_str = serde_json::from_str::<serde_json::Value>;
}
