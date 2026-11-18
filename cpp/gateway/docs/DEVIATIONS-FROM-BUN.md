# C++ gateway deviations from Bun

## BigInt

- A decimal sign requires digits. Bun 1.4.0 parses `BigInt("+")` and `BigInt("-")` as zero. The gateway reports `SyntaxError` for those inputs so incomplete signed values cannot become numeric zero.
- A binary, octal or hex prefix requires digits even when whitespace follows it. Bun 1.4.0 parses `BigInt("0x ")` as zero. The gateway reports `SyntaxError` and applies the same digit requirement to `'0x'`, `'0o'` and `'0b'` numeric tokens. Stored values and KV keys carry complete digits.
- A fixed-width unsigned cast may produce a value at the full 1,048,576-bit limit. For example, Bun 1.4.0 throws `RangeError` for `BigInt.asUintN(1048576, -1n)`, and the gateway returns a value of 1,048,576 one bits. This keeps the C++ runtime's existing bit limit and lets Boost mask the result within that limit.
