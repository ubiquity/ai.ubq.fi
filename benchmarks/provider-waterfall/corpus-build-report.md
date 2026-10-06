# corpus-build-report.md — DeepSeek V4.1 Flash provider waterfall (corpus-v1)

Produced by `corpus-build.ts` (m1-corpus lane) from recorded DeepSeek V4.1 Flash subagent sessions under
`/Users/nv/.codex/sessions` (2026/10); September harvested only if October could not fill a class: false.

Frozen artifact: `corpus/corpus-v1.jsonl` (5832678 bytes, sha256
`6aa1248dac25135f6fe2dfe8444f33f8f357d369940d296cd692b68ba1cdb288`), 30 entries, class counts small=6 medium=9 large=9
xlarge=6.

## Selection method

Candidate sessions: 41 (session_meta.thread_source=subagent or source.subagent present; turn_context model
deepseek-ai/DeepSeek-V4.1-Flash*, deepseek-flash, or deepseek-v4-flash*). Each candidate file was read once; a boundary
is a user/developer message, tool result, or agent_message whose next recorded item is assistant-side, never splitting a
tool result from its call. Class sizes come from the boundary turn's recorded input tokens.

Selection order: xlarge → small → large → medium. xlarge: largest in-window boundary per session; sessions ranked by
largest in-window recorded input tokens desc. small: closest-recorded-request. large: first in-window boundary per
session; sessions in path order. medium: first in-window boundary per session; sessions in path order. One entry per
session; ties resolve by session path and ordinal, so the same inputs produce identical bytes.

Small-class fallback: no DeepSeek V4.1 Flash session under 2026/10 has a boundary inside 2000-10000 recorded input
tokens; the six recorded requests closest to the window were used.

## Class distribution (recorded input tokens per entry)

| id               | class  | recorded input tokens | items | boundary                    | source session                                                                      |
| ---------------- | ------ | --------------------- | ----- | --------------------------- | ----------------------------------------------------------------------------------- |
| corpus-small-01  | small  | 18789                 | 6     | message/developer @ord 10   | `2026/10/03/rollout-2026-10-03T06-31-11-01a10151-5b28-7043-a626-1642835bebfc.jsonl` |
| corpus-small-02  | small  | 18797                 | 6     | message/developer @ord 10   | `2026/10/03/rollout-2026-10-03T06-37-08-01a10156-ce18-7860-8f0d-c0099b5474c4.jsonl` |
| corpus-small-03  | small  | 18779                 | 6     | message/developer @ord 10   | `2026/10/03/rollout-2026-10-03T06-45-34-01a1015e-861f-7fb0-8b31-892353386559.jsonl` |
| corpus-small-04  | small  | 18689                 | 6     | message/developer @ord 10   | `2026/10/03/rollout-2026-10-03T06-51-57-01a10164-5e2e-7e20-ab5d-5346f36d5f24.jsonl` |
| corpus-small-05  | small  | 18779                 | 7     | message/developer @ord 16   | `2026/10/03/rollout-2026-10-03T06-52-27-01a10164-d448-7a82-9282-943ac02b49aa.jsonl` |
| corpus-small-06  | small  | 18769                 | 7     | message/developer @ord 16   | `2026/10/03/rollout-2026-10-03T06-53-25-01a10165-b643-7752-bb76-d21cb7209b3c.jsonl` |
| corpus-medium-01 | medium | 18835                 | 6     | message/developer @ord 10   | `2026/10/03/rollout-2026-10-03T06-48-47-01a10161-777e-77c3-9d3a-9b7a439f263d.jsonl` |
| corpus-medium-02 | medium | 18832                 | 6     | message/developer @ord 10   | `2026/10/03/rollout-2026-10-03T06-48-56-01a10161-9c64-7f43-9bcf-158bbbeaf78d.jsonl` |
| corpus-medium-03 | medium | 18835                 | 6     | message/developer @ord 10   | `2026/10/03/rollout-2026-10-03T06-49-57-01a10162-895d-71c1-bbd0-08634ee18895.jsonl` |
| corpus-medium-04 | medium | 18832                 | 6     | message/developer @ord 10   | `2026/10/03/rollout-2026-10-03T06-50-03-01a10162-a31d-78e1-a91c-bfc996526eca.jsonl` |
| corpus-medium-05 | medium | 18902                 | 7     | message/developer @ord 16   | `2026/10/03/rollout-2026-10-03T06-50-37-01a10163-26d6-7c63-835b-f3f6f1193db6.jsonl` |
| corpus-medium-06 | medium | 22927                 | 6     | message/developer @ord 10   | `2026/10/03/rollout-2026-10-03T07-22-42-01a10180-8407-7bc0-a047-c02fdc395da7.jsonl` |
| corpus-medium-07 | medium | 20106                 | 6     | message/developer @ord 10   | `2026/10/04/rollout-2026-10-04T21-50-31-01a109c1-60d8-7291-bcc2-5c8aac97766c.jsonl` |
| corpus-medium-08 | medium | 20106                 | 6     | message/developer @ord 10   | `2026/10/04/rollout-2026-10-04T22-24-39-01a109e0-a267-7a01-b0bb-f110c5b8f202.jsonl` |
| corpus-medium-09 | medium | 20106                 | 6     | message/developer @ord 10   | `2026/10/04/rollout-2026-10-04T22-24-40-01a109e0-a508-7a72-893b-1a6133ed9ce6.jsonl` |
| corpus-large-01  | large  | 48329                 | 10    | message/user @ord 24        | `2026/10/03/rollout-2026-10-03T06-46-09-01a1015f-10b9-7482-af8d-a6b615d6ea17.jsonl` |
| corpus-large-02  | large  | 48850                 | 12    | message/developer @ord 23   | `2026/10/03/rollout-2026-10-03T07-05-29-01a10170-c156-76d1-9bad-a1d651ac4ed3.jsonl` |
| corpus-large-03  | large  | 46455                 | 10    | message/developer @ord 20   | `2026/10/03/rollout-2026-10-03T07-31-02-01a10188-28be-71f1-8923-b9742ffc8d19.jsonl` |
| corpus-large-04  | large  | 43604                 | 10    | message/developer @ord 20   | `2026/10/04/rollout-2026-10-04T21-37-30-01a109b5-767b-7f33-bc31-56a612fac4eb.jsonl` |
| corpus-large-05  | large  | 44483                 | 10    | message/developer @ord 20   | `2026/10/04/rollout-2026-10-04T21-41-09-01a109b8-d1fc-78f2-8924-04c3ce5eeaf8.jsonl` |
| corpus-large-06  | large  | 43363                 | 10    | message/developer @ord 20   | `2026/10/04/rollout-2026-10-04T21-41-10-01a109b8-d49a-7bc2-b2f1-f59613905f1a.jsonl` |
| corpus-large-07  | large  | 43379                 | 10    | message/developer @ord 20   | `2026/10/04/rollout-2026-10-04T21-47-30-01a109be-a16f-7491-b5e9-ba353b52863f.jsonl` |
| corpus-large-08  | large  | 44243                 | 10    | message/developer @ord 20   | `2026/10/04/rollout-2026-10-04T21-47-31-01a109be-a4e8-7d82-a60a-529be359e372.jsonl` |
| corpus-large-09  | large  | 42428                 | 10    | message/developer @ord 20   | `2026/10/04/rollout-2026-10-04T21-50-30-01a109c1-5de4-7882-b956-67ba797e9f27.jsonl` |
| corpus-xlarge-01 | xlarge | 591585                | 65    | message/developer @ord 151  | `2026/10/03/rollout-2026-10-03T07-05-01-01a10170-5509-74c0-9141-1ad2082252f0.jsonl` |
| corpus-xlarge-02 | xlarge | 565649                | 63    | message/developer @ord 154  | `2026/10/03/rollout-2026-10-03T07-43-31-01a10193-9329-7611-9df0-94bf4ccfc93f.jsonl` |
| corpus-xlarge-03 | xlarge | 211377                | 18    | message/developer @ord 40   | `2026/10/04/rollout-2026-10-04T21-37-29-01a109b5-72d5-7b42-bf65-6d96b0b090fe.jsonl` |
| corpus-xlarge-04 | xlarge | 598401                | 943   | message/developer @ord 2310 | `2026/10/04/rollout-2026-10-04T23-55-02-01a10a33-61f0-7a03-820d-3ed8ca216912.jsonl` |
| corpus-xlarge-05 | xlarge | 573544                | 49    | message/developer @ord 108  | `2026/10/05/rollout-2026-10-05T20-57-43-01a10eb7-6a21-7342-b3e8-c313c80ce40f.jsonl` |
| corpus-xlarge-06 | xlarge | 507010                | 52    | message/developer @ord 105  | `2026/10/05/rollout-2026-10-05T20-57-44-01a10eb7-6cad-7ba2-9a5c-5334a508b826.jsonl` |

## Sanitizer

Revision `sanitizer-v1`; substitutions counted per rule. The email rule redacts addresses only inside strings that
matched a credential rule, and decoded-string residuals are 0.

| rule                 | pattern                                                                                 | substitutions |
| -------------------- | --------------------------------------------------------------------------------------- | ------------- |
| authorization_header | `(Authorization:\s*)[A-Za-z0-9._~+/=-]{16,}`                                            | 0             |
| assignment           | ``([A-Z0-9_]*(?:API_KEY\|TOKEN\|SECRET\|PASSWORD)[A-Z0-9_]*)=(?!\[REDACTED:)[^\s'"`]+`` | 0             |
| bearer               | `\bBearer\s+[A-Za-z0-9._-]{20,}`                                                        | 0             |
| lith_sk              | `lith_sk_[A-Za-z0-9]{16,}`                                                              | 0             |
| fernet               | `gAAAAA[A-Za-z0-9_-]{40,}`                                                              | 2             |
| slack_bot            | `xoxb-[A-Za-z0-9-]{10,}`                                                                | 0             |
| github_pat           | `ghp_[A-Za-z0-9]{30,}`                                                                  | 0             |
| aws_access_key       | `AKIA[0-9A-Z]{16}`                                                                      | 0             |
| hex_ak_sk            | `(?:AK\|SK)[0-9a-f]{32}`                                                                | 0             |
| sk                   | `sk-[A-Za-z0-9_-]{16,}`                                                                 | 1             |
| email (conditional)  | addresses inside credential-matched strings                                             | 0             |

Total substitutions: 3. Serialized-line scan artifact: 1 escape-folded match with no decoded-string match (a real
newline renders as \n, so an empty NAME= at end of line folds into the following lines); recorded in the manifest.

## Dropped item types

Dropped from every included prefix and never replayed: `reasoning` (288 items), `web_search_call` (0 items). Removed
`encrypted_content` entries: 40; top-level `internal_chat_message_metadata_passthrough` removed from every item.

## Tools fixture

`fixtures/codex-tools-0.160.1.json`: sha256 `e48b8f1e1533e676ba43f1f15660a8f5e35d2a775c607b94cb4d46a2116e29ab`, 102688
bytes, 23 tools, used verbatim and in order for every entry. DESIGN.md quotes
`e48b8f1e1533e676ba43f1f15660a8f5e35d2a775c607b94cb4d46a2116e29ab`; the committed fixture hash matches DESIGN.md: true.

## Known limitations

- The small window (2000-10000 tokens) cannot be filled from the available recorded DeepSeek V4.1 Flash sessions: the
  candidate pool's smallest recorded request is 18689 tokens, so the six small entries are the closest recorded requests
  (18689-18797) per the corpus spec's fallback; they are labeled `small` but their recorded size is above the window.
- Entry sources cluster by task: cwd coverage is `/Users/nv/repos/0x4007/codex-mid-task-stop` (10),
  `/private/tmp/subgate-smoke` (9), `/private/tmp` (5), `/Users/nv/.codex` (3), `/Users/nv/repos/ubiquity/ai.ubq.fi`
  (3). The single largest group shares near-identical synthetic worker prompts, so prompt-content diversity is limited;
  selection still follows the deterministic path-order and largest-in-window rules.
- 4 of the selected entries come from candidate files (of 6 such candidates) that also carry a parent exec session_meta
  record; candidate detection accepts a file when any session_meta matches and the first matching (subagent) record
  supplies session identity and instructions.
- Recorded input tokens include each session's own tools and instructions; the frozen fixture's tool array may differ in
  size from what the recorded session used, so replayed request sizes can differ from the recorded values.
- The `sk-…` rule matches that shape anywhere, including substrings of identifiers such as `task-<id>`; substitutions
  are small, uniform across providers, and counted above.
- Encrypted agent payloads and reasoning items cannot be replayed and are removed; the corpus replays only plaintext
  items plus tool calls/outputs.
- All source sessions are from 2026-10-03..2026-10-05 on one host; no cross-host or cross-month diversity was available
  for this class mix.

## Verification

- `corpus-build.ts verify` recomputes the sha256, re-validates every line as a `CorpusEntry` (ids, ordering, class
  windows, tool fixture equality, call/output pairing, no passthrough/encrypted fields) and re-checks 6/9/9/6 counts
  without reading sessions.
- `corpus-build.ts selfcheck` feeds one fixture string per credential shape through the sanitizer and asserts redaction,
  counting and idempotence.
