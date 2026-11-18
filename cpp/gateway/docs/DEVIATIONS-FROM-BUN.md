# C++ gateway deviations from Bun

## BigInt

- A decimal sign requires digits. Bun 1.4.0 parses `BigInt("+")` and `BigInt("-")` as zero. The gateway reports `SyntaxError` for those inputs so incomplete signed values cannot become numeric zero.
- A binary, octal or hex prefix requires digits even when whitespace follows it. Bun 1.4.0 parses `BigInt("0x ")` as zero. The gateway reports `SyntaxError` and applies the same digit requirement to `'0x'`, `'0o'` and `'0b'` numeric tokens. Stored values and KV keys carry complete digits.
