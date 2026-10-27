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
        .map(|d| char::from_digit(d, 10).expect("decimal digit"))
        .collect::<String>();
    if negative && decimal != "0" {
        decimal.insert(0, '-');
    }
    decimal
}
