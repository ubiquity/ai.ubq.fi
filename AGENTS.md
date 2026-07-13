# Project Guidance

- Keep OpenAI-compatible endpoints and request bodies aligned with the official OpenAI API schema. Do not add
  gateway-only aliases, sentinel values, or alternate wire formats.
- Treat the uploaded Codex CLI model catalog as the source of truth for reasoning tier strings. Preserve every non-empty
  advertised tier and do not enforce a hard-coded tier allowlist or tier membership check.
- Represent no reasoning publicly as `none`. Normalize null efforts in upstream model metadata to `none`, and omit the
  reasoning field only when translating `none` at the Codex upstream request boundary; never use `null` as the public
  no-reasoning wire value.
