use gateway_native_confinement::file_observation::Facts;
use gateway_native_custody_wire::owner_file_policy::{
    require_epoch_file_shape, require_generation_file, require_prepared_file,
    require_stopped_auth_file,
};

fn observed_data() -> Facts {
    Facts {
        device: 1,
        inode: 2,
        uid: 501,
        gid: 20,
        mode: 0o100600,
        links: 1,
        size: 2048,
        ctime: 1,
        ctime_nsec: 0,
    }
}

#[test]
fn prepared_files_require_private_owned_regular_single_link_data() {
    let good = observed_data();
    require_prepared_file(&good, 501).unwrap();
    let mut changed = good;
    changed.mode = 0o100640;
    assert!(require_prepared_file(&changed, 501).is_err());
    changed = good;
    changed.uid = 502;
    assert!(require_prepared_file(&changed, 501).is_err());
    changed = good;
    changed.mode = 0o040700;
    assert!(require_prepared_file(&changed, 501).is_err());
    changed = good;
    changed.links = 2;
    assert!(require_prepared_file(&changed, 501).is_err());
    changed = good;
    changed.size = u64::MAX;
    require_prepared_file(&changed, 501).unwrap();
}

#[test]
fn stopped_auth_retains_original_permission_and_size_domain() {
    let mut good = observed_data();
    good.mode = 0o100666;
    good.size = u64::MAX;
    require_stopped_auth_file(&good, 501).unwrap();
    let mut changed = good;
    changed.uid = 502;
    assert!(require_stopped_auth_file(&changed, 501).is_err());
    changed = good;
    changed.mode = 0o040700;
    assert!(require_stopped_auth_file(&changed, 501).is_err());
    changed = good;
    changed.links = 2;
    assert!(require_stopped_auth_file(&changed, 501).is_err());
}

#[test]
fn generation_size_boundary_does_not_add_a_private_mode_check() {
    let mut good = observed_data();
    good.mode = 0o100666;
    good.size = 1_048_576;
    require_generation_file(&good, 501).unwrap();
    let mut changed = good;
    changed.size += 1;
    assert!(require_generation_file(&changed, 501).is_err());
    changed = good;
    changed.uid = 502;
    assert!(require_generation_file(&changed, 501).is_err());
    changed = good;
    changed.mode = 0o040700;
    assert!(require_generation_file(&changed, 501).is_err());
    changed = good;
    changed.links = 2;
    assert!(require_generation_file(&changed, 501).is_err());
}

#[test]
fn epoch_shape_boundary_leaves_parent_uid_observation_to_b() {
    let mut good = observed_data();
    good.uid = 502;
    require_epoch_file_shape(&good).unwrap();
    let mut changed = good;
    changed.size = 2049;
    assert!(require_epoch_file_shape(&changed).is_err());
    changed = good;
    changed.mode = 0o100640;
    assert!(require_epoch_file_shape(&changed).is_err());
    changed = good;
    changed.mode = 0o040700;
    assert!(require_epoch_file_shape(&changed).is_err());
    changed = good;
    changed.links = 2;
    assert!(require_epoch_file_shape(&changed).is_err());
}
