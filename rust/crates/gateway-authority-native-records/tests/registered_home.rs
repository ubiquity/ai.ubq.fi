use gateway_authority_native_records::owner::RegisteredRenewalOwner;
#[test]
fn native_registered_home_keeps_absolute_syntax_and_rejects_relative_metadata() {
    assert!(
        RegisteredRenewalOwner::configured("owner".into(), "/tmp/native-profile".into()).is_ok()
    );
    for home in ["relative-profile", "./profile", "../profile", ""] {
        assert!(RegisteredRenewalOwner::configured("owner".into(), home.into()).is_err());
    }
}
