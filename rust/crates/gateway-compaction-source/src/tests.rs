use crate::text::{self, Text};
use gateway_compaction_test_support::{cases, decode, field};
#[test]
fn source_json_stringify_retains_utf16_numbers_last_members_and_integer_property_order() {
    for case in cases("canonical") {
        let raw: Text = decode(&field(&case, "raw"));
        let expected: Text = decode(&field(&case, "expected"));
        let actual = text::parse_text(&raw).unwrap();
        assert_eq!(Text::from(actual.as_str()), expected);
    }
}
