#![cfg(target_os = "macos")]
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_authority_native_records::owner::RegisteredRenewalOwner;
use gateway_native_import_flow_process::fixture::{Home, deadline, helper, owner};
use gateway_native_import_store::store::ExistingAccountImportStore;
use gateway_native_owner_inspector::inspector::VerifiedInspector;
use std::{fs, os::unix::fs::PermissionsExt};
static FOUNDATION_FIXTURES: std::sync::Mutex<()> = std::sync::Mutex::new(());
#[test]
fn actual_principal_path_inode_mode_and_executable_mismatches_never_grant_mutation() {
    let _fixture_family = FOUNDATION_FIXTURES.lock().unwrap();
    let home = Home::new();
    let (store, registered) = home.store();
    let (helper, hash) = helper();
    let uid = rustix::process::getuid().as_raw();
    assert!(
        store
            .prepare_exclusive_owner(&registered, uid, &[uid], &helper, &hash, deadline())
            .is_err()
    );
    assert!(
        store
            .prepare_exclusive_owner(&registered, uid + 1, &[uid], &helper, &hash, deadline())
            .is_err()
    );
    assert!(
        store
            .prepare_exclusive_owner(
                &registered,
                uid,
                &[uid + 1],
                &helper,
                &sha256_hex(b"wrong"),
                deadline()
            )
            .is_err()
    );
    let owner = owner(&home);
    assert!(store.require_mutation_owner().is_err());
    fs::set_permissions(&home.0, fs::Permissions::from_mode(0o777)).unwrap();
    assert!(owner.recheck(deadline()).is_err());
    fs::set_permissions(&home.0, fs::Permissions::from_mode(0o700)).unwrap();
    let renamed = home.0.with_extension("retained");
    fs::rename(&home.0, &renamed).unwrap();
    fs::create_dir(&home.0).unwrap();
    fs::set_permissions(&home.0, fs::Permissions::from_mode(0o700)).unwrap();
    assert!(owner.recheck(deadline()).is_err());
    fs::remove_dir(&home.0).unwrap();
    fs::rename(renamed, &home.0).unwrap();
    let link = home.0.join("alias");
    std::os::unix::fs::symlink(&helper, &link).unwrap();
    assert!(VerifiedInspector::prepare(&link, &hash).is_err());
}
#[test]
fn insecure_nonsticky_parent_and_hardlinked_auth_refuse_runtime() {
    let _fixture_family = FOUNDATION_FIXTURES.lock().unwrap();
    let home = Home::new();
    let owner = owner(&home);
    fs::hard_link(home.0.join("auth.json"), home.0.join("auth-copy")).unwrap();
    assert!(owner.prepare_runtime().is_err());
    let outer = Home::new();
    fs::set_permissions(&outer.0, fs::Permissions::from_mode(0o777)).unwrap();
    let inner = outer.0.join("owner");
    fs::create_dir(&inner).unwrap();
    fs::set_permissions(&inner, fs::Permissions::from_mode(0o700)).unwrap();
    fs::write(inner.join("auth.json"), b"SYNTHETIC").unwrap();
    fs::set_permissions(inner.join("auth.json"), fs::Permissions::from_mode(0o600)).unwrap();
    let registered =
        RegisteredRenewalOwner::configured("fixture".into(), inner.to_str().unwrap().into())
            .unwrap();
    let store =
        ExistingAccountImportStore::configured(&registered, sha256_hex(b"account")).unwrap();
    let (helper, hash) = helper();
    let uid = rustix::process::getuid().as_raw();
    assert!(
        store
            .prepare_exclusive_owner(&registered, uid, &[uid + 1], &helper, &hash, deadline())
            .is_err()
    );
}
