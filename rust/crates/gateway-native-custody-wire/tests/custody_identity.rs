use gateway_native_custody_wire::identity_policy::same_identity_data;

#[test]
fn a_stopped_session_rejects_new_kernel_epoch_or_root_identity() {
    let expected = ("a".repeat(64), 1, 2);
    for actual in [
        ("b".repeat(64), 1, 2),
        ("a".repeat(64), 3, 2),
        ("a".repeat(64), 1, 4),
    ] {
        assert!(
            same_identity_data(
                (&expected.0, expected.1, expected.2),
                (&actual.0, actual.1, actual.2),
            )
            .is_err()
        );
    }
}
mod owner_file_policy;
mod stopped_replay;
