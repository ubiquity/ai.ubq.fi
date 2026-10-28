use super::*;
use gateway_admin_test_support::claude_source_fixtures::{credential, roster};

#[test]
fn finite_stored_ids_and_empty_tombstones_are_unchanged() {
    let value = roster(credential(JsString16::from_utf8("sk-ant-synthetic")), None);
    let document = roster_source(&value);
    let altered = document
        .as_str()
        .replace("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa", r"\ud800");
    assert!(parse_source_roster(&SourceJson::parse(&altered).unwrap()).is_err());
    let empty = empty(100);
    let record = encode_roster_record("tombstone", &empty, None).unwrap();
    assert_eq!(decode_roster_record(&record, 100).unwrap(), Some(empty));
}
