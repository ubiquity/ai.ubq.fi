# Owner-only follow-ups from #1132 and #1176

Recorded on 2026-10-31 against `0d7268741f5c61abe6ad493aa00ee93ffb21a60e` (PR #1156's merge).
Closing #1132 did not resolve the two owner-only items below. Both remain pending; merging the runner tests and
documentation corrections for #1176 does not decide them. Only the repository owner can complete these checkboxes.

## Retention hard cap (#1073 item 2; #139)

- [ ] Owner: confirm and durably record the reported 2026-10-16 decline in a comment on #139 or a dated entry in
      `docs/DECISIONS.md`. If appropriate, change #139's close reason from completed to not planned.

PR #1156 reported the decline, but neither #139 nor `docs/DECISIONS.md` records it at the revision above. No oldest-first
eviction was implemented: the 5 GiB budget is an alert threshold, not a hard cap. The reported rationale was ample VPS
disk and the retirement of per-GiB hosted-KV billing; this checklist does not establish an owner decision. See the
[retention follow-ups](../log-retention-and-quota-runway-2026-08-25.md#follow-ups).

## macOS non-loopback live e2e (#1077 item 2)

- [ ] Owner: explicitly accept host-loopback-only live e2e on macOS, or open a task for a destination-scoped CONNECT
      relay (or equivalent) for non-loopback destinations. Record the decision and any task reference here.

Darwin currently fails closed for every non-loopback `BASE_URL`, including the default `https://ai.ubq.fi`; its
Seatbelt profile permits all host-loopback ports, not only the selected destination's port. See the
[runner boundary documentation](../../README.md#runtime-and-checks). No relay implementation or policy relaxation is
authorized by this checklist, and the restriction stays unchanged pending the owner's decision.
