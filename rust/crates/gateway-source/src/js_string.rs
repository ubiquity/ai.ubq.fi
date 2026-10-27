//! A JavaScript string is a sequence of UTF16 code units, not necessarily scalar text.
use crate::{error::GatewayError, source_json::SourceJson};
use serde::{Deserialize, Deserializer, Serialize, Serializer, de::Error};
use serde_json::value::RawValue;

#[derive(Clone, Default, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct JsString16(Vec<u16>);

impl std::fmt::Debug for JsString16 {
    fn fmt(&self, out: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        out.debug_tuple("JsString16").field(&self.0).finish()
    }
}

impl JsString16 {
    pub fn from_units(units: Vec<u16>) -> Self {
        Self(units)
    }
    pub fn from_utf8(text: &str) -> Self {
        Self(text.encode_utf16().collect())
    }
    pub fn as_units(&self) -> &[u16] {
        &self.0
    }
    pub fn into_units(self) -> Vec<u16> {
        self.0
    }
    pub fn prefix_units(&self, limit: usize) -> Self {
        Self(self.0[..self.0.len().min(limit)].to_vec())
    }
    pub fn try_utf8(&self) -> Result<String, GatewayError> {
        String::from_utf16(&self.0).map_err(|_| invalid())
    }
    /// Source replace(/\s+/g, " ").trim(), followed by slice(0, limit).
    pub fn sanitize_js_whitespace_prefix(&self, limit: usize) -> Option<Self> {
        let mut units = Vec::new();
        let mut pending = false;
        for &unit in &self.0 {
            if whitespace(unit) {
                pending = !units.is_empty();
            } else {
                if pending {
                    units.push(32);
                }
                units.push(unit);
                pending = false;
            }
        }
        if units.is_empty() {
            return None;
        }
        units.truncate(limit);
        Some(Self(units))
    }
    /// The safe RawValue parser has already checked the surrounding JSON grammar.
    pub fn from_json_token(raw: &RawValue) -> Result<Self, GatewayError> {
        let text = raw.get();
        let bytes = text.as_bytes();
        if bytes.len() < 2 || bytes.first() != Some(&b'"') || bytes.last() != Some(&b'"') {
            return Err(invalid());
        }
        let mut units = Vec::new();
        let mut at = 1;
        let end = bytes.len() - 1;
        while at < end {
            if bytes[at] == b'\\' {
                at += 1;
                let unit = match bytes.get(at).copied().ok_or_else(invalid)? {
                    b'"' => 34,
                    b'\\' => 92,
                    b'/' => 47,
                    b'b' => 8,
                    b'f' => 12,
                    b'n' => 10,
                    b'r' => 13,
                    b't' => 9,
                    b'u' => {
                        let digits = text.get(at + 1..at + 5).ok_or_else(invalid)?;
                        let value = u16::from_str_radix(digits, 16).map_err(|_| invalid())?;
                        at += 4;
                        value
                    }
                    _ => return Err(invalid()),
                };
                units.push(unit);
                at += 1;
            } else {
                let ch = text[at..end].chars().next().ok_or_else(invalid)?;
                let mut encoded = [0; 2];
                units.extend_from_slice(ch.encode_utf16(&mut encoded));
                at += ch.len_utf8();
            }
        }
        Ok(Self(units))
    }
    pub fn to_source_json(&self) -> Result<SourceJson, GatewayError> {
        SourceJson::parse(&self.json_token())
    }
    /// Encodes every source UTF16 sequence as one JSON string token.
    /// This returns bytes only; it does not construct SourceJson or an authority.
    pub fn json_token(&self) -> String {
        fn append_hex(out: &mut String, unit: u16) {
            const HEX: &[u8; 16] = b"0123456789abcdef";
            out.push_str("\\u");
            for shift in [12, 8, 4, 0] {
                out.push(char::from(HEX[usize::from((unit >> shift) & 15)]));
            }
        }
        let mut out = String::from("\"");
        for decoded in char::decode_utf16(self.0.iter().copied()) {
            match decoded {
                Ok('\u{0008}') => out.push_str("\\b"),
                Ok('\t') => out.push_str("\\t"),
                Ok('\n') => out.push_str("\\n"),
                Ok('\u{000c}') => out.push_str("\\f"),
                Ok('\r') => out.push_str("\\r"),
                Ok('"') => out.push_str("\\\""),
                Ok('\\') => out.push_str("\\\\"),
                Ok(value @ '\0'..='\u{001f}') => append_hex(&mut out, value as u16),
                Ok(value) => out.push(value),
                Err(unpaired) => append_hex(&mut out, unpaired.unpaired_surrogate()),
            }
        }
        out.push('"');
        out
    }
}

impl From<&str> for JsString16 {
    fn from(value: &str) -> Self {
        Self::from_utf8(value)
    }
}
impl std::str::FromStr for JsString16 {
    type Err = std::convert::Infallible;
    fn from_str(value: &str) -> Result<Self, Self::Err> {
        Ok(Self::from_utf8(value))
    }
}
impl Serialize for JsString16 {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        self.to_source_json()
            .map_err(|_| serde::ser::Error::custom("Invalid source JSON string"))?
            .serialize(serializer)
    }
}
impl<'de> Deserialize<'de> for JsString16 {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let raw = Box::<RawValue>::deserialize(deserializer)?;
        Self::from_json_token(&raw).map_err(|_| D::Error::custom("Expected a source JSON string"))
    }
}

fn whitespace(unit: u16) -> bool {
    matches!(unit, 0x0009..=0x000d | 0x0020 | 0x00a0 | 0x1680 | 0x2000..=0x200a | 0x2028 | 0x2029 | 0x202f | 0x205f | 0x3000 | 0xfeff)
}
fn invalid() -> GatewayError {
    GatewayError::unavailable(
        "Source JSON string is invalid or cannot be represented as UTF8",
        "source_json_string_invalid",
    )
}
