# C++ gateway deviations from Bun

## WebSocket

- The receiver always validates UTF-8 in text messages and close reasons. With `skipUTF8Validation: true`, `ws@8.18.3` under Bun 1.4.0 delivers text bytes `c3 28` and accepts close 1000 with reason byte `ff`. The C++ receiver rejects both with close 1007. Beast enforces the RFC 6455 UTF-8 requirement and gateway callers use validation. The option still compiles for existing callers.
