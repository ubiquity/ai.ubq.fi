# Project Guidance

- Keep OpenAI-compatible endpoints and request bodies aligned with the official OpenAI API schema. Do not add
  gateway-only aliases, sentinel values, or alternate wire formats.
- For reasoning controls, use OpenAI's public string values exactly: `none`, `minimal`, `low`, `medium`, `high`, and
  `xhigh`. Do not use `null` to mean no reasoning unless the official OpenAI API documentation explicitly changes to
  require it.
- When upstream model metadata represents no reasoning as a null effort, normalize it at ingestion to OpenAI's public
  `none` value before storing, validating, or rendering it.
