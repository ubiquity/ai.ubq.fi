# C++ gateway deviations from Bun

## Fetch and WebSocket-client HTTP responses with llhttp 9.4.4

- **Status line and line endings.** Bun 1.4.0 fetch accepts `HTTP/1.1  200 OK`, `HTTP/1.2 200 OK`, and response heads with bare LF line endings. Strict llhttp rejects these heads. The gateway uses standard HTTP/1.0 and HTTP/1.1 response lines and CRLF delimiters.
- **Reason phrase spacing.** Bun fetch reads `HTTP/1.1 200  OK` as status text `OK`. llhttp preserves the extra space in the reason phrase. The gateway uses the numeric status for decisions.
- **Duplicate and conflicting lengths.** Bun accepts identical duplicate `Content-Length` fields and responses that contain both `Content-Length` and `Transfer-Encoding`, including `identity`. Strict llhttp rejects those responses before exposing a head. A single message boundary avoids ambiguity.
- **Malformed length errors.** For `Content-Length: abc`, Bun fetch reports `InvalidContentLength`. The gateway reports `Malformed_HTTP_Response` when llhttp rejects the head. Both reject fetch before exposing a response.
- **Chunk delimiters and trailers.** Bun fetch accepts bare-LF chunks, spaces before a chunk size's CRLF, and malformed trailer field names such as `X Trailer`. Strict llhttp validates chunk metadata and trailers. Fetch reports `InvalidHTTPResponse` when the body read reaches invalid chunk syntax.
- **Chunk size padding.** Bun rejects `00000000000000002` as a chunk size because the field exceeds 16 digits. llhttp accepts it as size two and rejects numeric overflow. llhttp parses the digits incrementally with fixed parser storage.
- **Transfer coding order.** Bun fetch accepts `Transfer-Encoding: chunked,` and decodes the chunks. llhttp identifies the response as EOF-delimited. The gateway rejects this disagreement before streaming the body so chunk delimiters cannot reach clients as payload bytes.
