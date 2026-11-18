# Native gateway side-by-side operator kit

Base commit `b54357a92c0f2669335d83f04047c3f7de32f925` on `agent-jobs/cppgw-std-icu-20261117-44188151110-1`.

## Work contract v1

Deliver a Linux systemd template, bounded loopback request replay, and a runbook from verified package to the owner's cutover decision. The side-by-side lane uses isolated synthetic state and credentials. The existing production ingress stays on Bun. Deployment, live credentials, live data conversion, and real upstream traffic are outside this job.

Only `cpp/gateway/ops/side-by-side/`, `cpp/gateway/docs/SIDE-BY-SIDE.md` and this report belong to the job. Application, packaging, build, existing operations, and workflow paths are outside its scope.

The replay lane covers only audited reads. Provider probes, inference, account changes, and accounting belong to the existing isolated mock harness. The runbook will bind health, authentication, settlement, storage, and rollback evidence to the actual artifacts and state. Linux ARM64 package startup and host resource headroom require target-host evidence. Apple Silicon service startup and Raspberry Pi qualification have separate platform gates.

The final checks are `bash -n`, ShellCheck, `systemd-analyze verify` of the rendered unit, changed-document formatting, `git diff --check`, and bounded loopback controls for the replay script with an empty home and fresh temporary directory. This job ends with a committed, validated kit and an explicit handoff for deployment and cutover decisions.
