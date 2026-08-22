# UbiquityOS AI Gateway

UbiquityOS AI Gateway, available at [AI.UBQ.FI](https://ai.ubq.fi/), is an OpenAI-compatible AI inference service for
the UbiquityOS ecosystem. It gives approved applications and agents one stable base URL for model discovery, Chat
Completions, and the Responses API. The gateway is not the public OpenAI API and it requires a UbiquityOS gateway token
for inference requests.

## When to use this service

Use this gateway when an approved UbiquityOS application or autonomous agent needs OpenAI-compatible text inference,
streaming Responses or Chat Completions, runtime model discovery, or documented function tools. Before an inference
request, query `GET /v1/models` and choose a returned model instead of assuming a public OpenAI model alias exists. Use
the `/v1` base URL in an OpenAI-compatible SDK and pass the approved token as its API key.

Do not use this service to create credentials, administer keys, or probe private health and administration routes. Those
actions require a separately authorized operator workflow. Never put a bearer token in a prompt, a source repository, a
browser URL, or an agent transcript.

## Start here

- [Developer portal](https://ai.ubq.fi/developers) — quickstart, API behavior, and integration guidance.
- [OpenAPI contract](https://ai.ubq.fi/openapi.json) — machine-readable endpoint and function-tool schemas.
- [Agent instructions](https://ai.ubq.fi/llms.txt) and [full guide](https://ai.ubq.fi/llms-full.txt).
- `GET https://ai.ubq.fi/v1/models` — authenticated model discovery.
- `POST https://ai.ubq.fi/v1/responses` or `POST https://ai.ubq.fi/v1/chat/completions` — inference.

The gateway does not forward client bearer tokens upstream. See the developer portal and full guide for authentication,
error envelopes, streaming, and rate-limit behavior.
