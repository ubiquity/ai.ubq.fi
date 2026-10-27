pub fn decimal_key(negative: bool, magnitude: &[u8]) -> String {
    let mut digits = vec![0_u32];
    for byte in magnitude {
        let mut carry = u32::from(*byte);
        for digit in &mut digits {
            let value = *digit * 256 + carry;
            *digit = value % 10;
            carry = value / 10;
        }
        while carry > 0 {
            digits.push(carry % 10);
            carry /= 10;
        }
    }
    let mut decimal = digits
        .into_iter()
        .rev()
        // Both digit writes above are modulo ten; this is an ASCII index in 0..10.
        .map(|d| char::from(b"0123456789"[d as usize]))
        .collect::<String>();
    if negative && decimal != "0" {
        decimal.insert(0, '-');
    }
    decimal
}
