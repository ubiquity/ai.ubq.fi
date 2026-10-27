use serde_json::Value;
/// Source utils.isRecord deliberately includes arrays in the JSON domain.
pub fn is_record(value: &Value) -> bool {
    value.is_object() || value.is_array()
}

pub const DAY: i64 = 86_400_000;
pub fn js_whitespace(c: char) -> bool {
    matches!(c,'\u{0009}'..='\u{000d}'|' '|'\u{00a0}'|'\u{1680}'|'\u{2000}'..='\u{200a}'|'\u{2028}'|'\u{2029}'|'\u{202f}'|'\u{205f}'|'\u{3000}'|'\u{feff}')
}
pub fn trim(s: &str) -> &str {
    s.trim_matches(js_whitespace)
}
pub fn safe_i64(v: &Value) -> Option<i64> {
    v.as_f64()
        .filter(|n| {
            n.is_finite() && n.fract() == 0.0 && (0.0..=9_007_199_254_740_991.0).contains(n)
        })
        .map(|n| n as i64)
}
