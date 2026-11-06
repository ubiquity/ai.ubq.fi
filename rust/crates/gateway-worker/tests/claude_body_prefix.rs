//! The Claude error-body prefix matches the source `response.text()` UTF-16 slice.
use gateway_worker::transport::claude_body::{PREFIX_UNITS, Utf16Prefix};

#[test]
fn segmented_utf8_bom_and_astral_character_match_response_text() {
    let mut prefix = Utf16Prefix::default();
    for bytes in [
        &[0xef][..],
        &[0xbb, 0xbf, 0xf0],
        &[0x9f, 0x98],
        &[0x80, b'X'],
    ] {
        prefix.push(bytes);
    }
    prefix.finish();
    assert_eq!(prefix.units, "😀X".encode_utf16().collect::<Vec<_>>());
}

#[test]
fn exact_js_slice_keeps_a_boundary_cut_high_surrogate() {
    let mut prefix = Utf16Prefix::default();
    prefix.push("A".repeat(PREFIX_UNITS - 1).as_bytes());
    prefix.push("😀tail".as_bytes());
    prefix.push(b"more drained bytes");
    prefix.finish();
    assert_eq!(prefix.units.len(), PREFIX_UNITS);
    assert_eq!(prefix.units[PREFIX_UNITS - 1], 0xd83d);
}

#[test]
fn invalid_utf8_and_partial_eof_use_replacement_characters() {
    let mut prefix = Utf16Prefix::default();
    prefix.push(&[0xff, b'A', 0xf0, 0x9f]);
    prefix.finish();
    assert_eq!(prefix.units, "�A�".encode_utf16().collect::<Vec<_>>());
}
