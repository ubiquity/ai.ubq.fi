# Provider input recovery repair v1

This is new owned implementation based on recovery commit
`2bd07ecab87743253740931f72675422ed6d7cbe`, tree
`54d6445ecc4abec1f3ef30c1ea66b3fa4faee4eb`.
The original later postimages remain unknown. No historical gate receipt is adopted.

The six documented edit spans are three redundant `Ok(...?)` pairs:
`request::codex_replay_input`, `request::project_cerebras_schema`, and
`source_controls::map_effort`. Returning each existing Result directly retains the
same scalar construction, call order, values and errors. One pinned Rust 1.98.1
formatter pass covers exactly the two changed Rust files. The input crate owns
493 physical source lines after that pass; actual AST qualification is pending.
No package, lock, feature, release profile, test body or fixture changes here.
All 109 original integration case names and five fixture files are retained.
Fresh execution of them and the 15 added regressions remains required.

The approved closed-envelope producer is a separate next foundation milestone.
The actual existing UTF16 encoder is `JsString16::json_token(&self) -> String`;
it is crate-private both in this recovery Core source and in C's defining
`gateway-source` source. C must expose and freeze the approved existing encoder
before D can consume it. D does not copy it, use a lossy string, edit Core or hide
a parser assertion. `ClosedErrorBody` will remain opaque and limited to the three
fixed envelopes, with immutable wire bytes and the exact source message/optional
field observations required by actual consumers. Generic SourceJson stays fallible.

The producer's exact consumption map must be frozen with its own clean commit
before any unowned consumer body changes. Current crossings include schema
reasoning-message extraction; Worker direct byte rendering/content-type matches
and extras carriers; admin/defaults/state, catalog and extras source-JSON response
carriers; and provider/schema/routing error-body tests. The untrusted validated
SourceRefusalBody::Json carrier remains separate. Existing cleanup must snapshot
before diagnostic mutation, and private local-overflow identity must survive clone
and routing conversion without any serialized way to forge it. No caller body has
been changed for that future cutover.

Fresh qualification starts with gateway-quality fixtures and its current parser
report, then strict all-target input/provider Clippy with the six staged flags and
the unchanged comparator, all existing provider targets with nonzero execution,
and the matching wasm32 checks. Cargo remains locked/offline in the unique D
source-owned target. Every compile must use the persistent epoch's lease-exec.py
D wrapper after sourcing its D/environment.sh. Generation 1 is assigned to C;
no D compilation has run for this checkpoint. Cloud GNU x86_64 evidence remains
separate from canonical native confinement, Worker/client and Mac acceptance.

Persistent exact argv, pre/postimage hashes, formatter output, consumer dependency
map and fixture/case inventories accompany this checkpoint under
`/workspace/.ai-rust-output/D/recovery-20261027T143350Z/gap-repair-v1`.
The full provider/P1/private-refusal family remains unfinished across preserved turns.
