//! One calibrated UTF16 estimator shared by local compaction and prompt admission.
//! Ported from fast-jev-compaction c1eab5fdbd6bde7d67f2da070116496558836884 (MIT).
fn whitespace(unit: u16) -> bool {
    matches!(unit, 0x0009..=0x000d | 0x0020 | 0x00a0 | 0x1680 | 0x2000..=0x200a | 0x2028 | 0x2029 | 0x202f | 0x205f | 0x3000 | 0xfeff)
}

/// Matches the source word/digit/symbol pieces, including individual surrogate units.
pub fn estimate_tokens(units: &[u16]) -> usize {
    let mut at = 0;
    let mut tokens = 0.0_f64;
    while at < units.len() {
        let unit = units[at];
        if whitespace(unit) {
            at += 1;
            continue;
        }
        let letter = |value: u16| (65..=90).contains(&value) || (97..=122).contains(&value);
        let digit = |value: u16| (48..=57).contains(&value);
        if letter(unit) || digit(unit) {
            let begin = at;
            at += 1;
            while at < units.len()
                && if letter(unit) {
                    letter(units[at])
                } else {
                    digit(units[at])
                }
            {
                at += 1;
            }
            let len = at - begin;
            tokens += if digit(unit) {
                len as f64 / 2.0
            } else {
                (1 + (len - 1) / 6) as f64
            };
        } else {
            tokens += 0.9;
            at += 1;
        }
    }
    tokens.ceil() as usize
}
