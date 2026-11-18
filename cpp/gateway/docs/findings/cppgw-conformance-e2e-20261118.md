# Native and Bun gateway functional conformance

## Work contract v1

Exercise the two real gateway entrypoints over loopback using declarative requests and a shared synthetic upstream. Compare HTTP status, client-visible wire data and headers, stream event order and terminals, and durable accounting and key state. Record functional differences before cosmetic differences, with request and response evidence. Use Bun 1.4.0 and the native release build of base `b54357a92c0f2669335d83f04047c3f7de32f925` on Linux ARM64.

| Surface                   | Measurement                                                                                                                   |
| ------------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| DeepSeek                  | Existing benchmark Chat and Responses fixture, with buffered and streamed cases                                               |
| Codex                     | Existing fixture transport and smoke provider, with Models, Responses and Realtime coverage subject to mock dispatch evidence |
| Other providers           | Discover local routing hooks and cover availability or validation when the packaged fixture cannot dispatch                   |
| Access and administration | Authentication decisions, validation, CORS and health, admin reads and synthetic API-key state                                |
| Settlement                | Usage and accounting readback after delivery and restart, with rate and quota refusals                                        |

Own `cpp/gateway/conformance/`, `cpp/gateway/docs/CONFORMANCE.md`, and this report. Gateway fixes, benchmark edits, workflow changes, production state and real provider access are outside this run. The supplied base contains the production-smoke harness. The requested request-path benchmark lives on the port branch, so use its pinned files in scratch and keep the baseline source and other checkout paths intact.

The runner must prove isolated loopback and use fresh private state, home and temp directories with an allowlisted environment. Verify the selected native release build, run every case against both gateways and retain the JSON receipt and full result table, then run changed-file formatting and `git diff --check`. The launcher and provider discovery will determine which successful routes can be observed. Stop after complete local evidence and bounded branch handoff, with any external delivery blocker recorded.

## Work contract v2

The fixture map supports DeepSeek and Codex, including the Codex catalog and Realtime. Add cached Voyage response and job cases via two synthetic vector rows imported through the authenticated admin HTTP route. This exercises output, idempotency and principal ownership in the real durable store. Retain the unconfigured Voyage cases in the first phase, and enable a synthetic cache-only credential after restart. Required checks include exact wire status and media type, matching gateway request and accounting evidence, dispatch provenance, header and stream expectations, and the independently specified validation decisions. Discovery results before this extension are diagnostic.
