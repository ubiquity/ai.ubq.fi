//! Calibrated UTF16 word-piece token estimator shared by compaction and prompt admission.
use gateway_core::js_string::JsString16;
fn letter(unit: u16) -> bool {
    (65..=90).contains(&unit) || (97..=122).contains(&unit)
}
fn digit(unit: u16) -> bool {
    (48..=57).contains(&unit)
}
/// Source `estimateTokens`: letter runs cost one per six letters, digit runs half per digit.
pub fn estimate_tokens(text: &JsString16) -> usize {
    let units = text.as_units();
    let (mut at, mut tokens) = (0, 0.0_f64);
    while at < units.len() {
        let (begin, unit) = (at, units[at]);
        at += 1;
        if crate::text::whitespace(unit) {
            continue;
        }
        let run: fn(u16) -> bool = if letter(unit) { letter } else { digit };
        if !run(unit) {
            tokens += 0.9;
            continue;
        }
        while at < units.len() && run(units[at]) {
            at += 1;
        }
        let len = at - begin;
        tokens += if digit(unit) {
            len as f64 / 2.0
        } else {
            (1 + (len - 1) / 6) as f64
        };
    }
    tokens.ceil() as usize
}
