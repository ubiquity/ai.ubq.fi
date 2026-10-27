use crate::scalar::error;

use gateway_source::error::GatewayError;
use gateway_schema::util::js_trim;
use serde_json::Value;

pub fn js_string(value: &Value) -> String {
    match value {
        Value::Null => "null".into(),
        Value::Bool(v) => v.to_string(),
        Value::String(s) => s.clone(),
        Value::Number(v) => v
            .as_f64()
            .map(|n| ryu_js::Buffer::new().format(n).to_owned())
            .unwrap_or_else(|| "NaN".into()),
        Value::Array(v) => v
            .iter()
            .map(|v| {
                if v.is_null() {
                    String::new()
                } else {
                    js_string(v)
                }
            })
            .collect::<Vec<_>>()
            .join(","),
        Value::Object(_) => "[object Object]".into(),
    }
}
pub fn js_number(value: &Value) -> f64 {
    match value {
        Value::Null => 0.0,
        Value::Bool(v) => {
            if *v {
                1.0
            } else {
                0.0
            }
        }
        Value::Number(v) => v.as_f64().unwrap_or(f64::NAN),
        _ => {
            let text = js_string(value);
            let text = js_trim(&text);
            if text.is_empty() {
                return 0.0;
            }
            match text {
                "Infinity" | "+Infinity" => f64::INFINITY,
                "-Infinity" => f64::NEG_INFINITY,
                "NaN" => f64::NAN,
                _ => {
                    let radix = if text.starts_with("0x") || text.starts_with("0X") {
                        Some(16)
                    } else if text.starts_with("0b") || text.starts_with("0B") {
                        Some(2)
                    } else if text.starts_with("0o") || text.starts_with("0O") {
                        Some(8)
                    } else {
                        None
                    };
                    if let Some(radix) = radix {
                        if text.len() == 2 {
                            return f64::NAN;
                        }
                        return text[2..]
                            .chars()
                            .try_fold(0.0, |a, c| {
                                c.to_digit(radix)
                                    .map(|d| a * f64::from(radix) + f64::from(d))
                            })
                            .unwrap_or(f64::NAN);
                    }
                    if text
                        .chars()
                        .any(|c| !c.is_ascii_digit() && !".+-eE".contains(c))
                    {
                        return f64::NAN;
                    }
                    text.parse().unwrap_or(f64::NAN)
                }
            }
        }
    }
}
pub fn normalize_bigint(text: &str) -> Result<(bool, String, Vec<u8>), GatewayError> {
    let text = text.trim();
    let (negative, digits) = if let Some(v) = text.strip_prefix('-') {
        (true, v)
    } else {
        (false, text.strip_prefix('+').unwrap_or(text))
    };
    let (radix, digits) = if digits.starts_with("0x") || digits.starts_with("0X") {
        (16, &digits[2..])
    } else if digits.starts_with("0b") || digits.starts_with("0B") {
        (2, &digits[2..])
    } else if digits.starts_with("0o") || digits.starts_with("0O") {
        (8, &digits[2..])
    } else {
        (10, digits)
    };
    if (negative || text.starts_with('+')) && radix != 10 {
        return Err(error("Signed nondecimal bigint is invalid."));
    }
    if digits.is_empty() && radix != 10 {
        return Err(error("Bigint digits are missing."));
    }
    let mut decimal = vec![0u8];
    let mut magnitude = vec![0u8];
    for c in digits.chars() {
        let digit = c
            .to_digit(radix)
            .ok_or_else(|| error("Invalid bigint digits."))?;
        let mut carry = digit;
        for d in &mut decimal {
            let n = u32::from(*d) * radix + carry;
            *d = (n % 10) as u8;
            carry = n / 10;
        }
        while carry > 0 {
            decimal.push((carry % 10) as u8);
            carry /= 10;
        }
        let mut carry = digit;
        for d in &mut magnitude {
            let n = u32::from(*d) * radix + carry;
            *d = (n % 256) as u8;
            carry = n / 256;
        }
        while carry > 0 {
            magnitude.push((carry % 256) as u8);
            carry /= 256;
        }
    }
    while decimal.len() > 1 && decimal.last() == Some(&0) {
        decimal.pop();
    }
    while magnitude.last() == Some(&0) {
        magnitude.pop();
    }
    let negative = negative && !magnitude.is_empty();
    let mut output = if negative {
        "-".to_owned()
    } else {
        String::new()
    };
    output.extend(decimal.iter().rev().map(|d| (b'0' + d) as char));
    magnitude.reverse();
    Ok((negative, output, magnitude))
}

/// Node Buffer's base64/base64url decoder ignores nonalphabet bytes and trailing orphan bits.
pub fn decode_base64(value: &str) -> Vec<u8> {
    let mut out = vec![];
    let mut bits = 0u32;
    let mut count = 0;
    for b in value.bytes() {
        if b == b'=' {
            break;
        }
        let digit = match b {
            b'A'..=b'Z' => b - b'A',
            b'a'..=b'z' => b - b'a' + 26,
            b'0'..=b'9' => b - b'0' + 52,
            b'+' | b'-' => 62,
            b'/' | b'_' => 63,
            _ => continue,
        };
        bits = (bits << 6) | u32::from(digit);
        count += 6;
        if count >= 8 {
            count -= 8;
            out.push((bits >> count) as u8);
            bits &= (1 << count) - 1;
        }
    }
    out
}
pub fn encode_base64(value: &[u8]) -> String {
    const A: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    let mut out = String::new();
    let mut bits = 0u32;
    let mut count = 0;
    for b in value {
        bits = (bits << 8) | u32::from(*b);
        count += 8;
        while count >= 6 {
            count -= 6;
            out.push(A[((bits >> count) & 63) as usize] as char);
        }
        bits &= (1 << count) - 1;
    }
    if count > 0 {
        out.push(A[((bits << (6 - count)) & 63) as usize] as char);
    }
    out
}
