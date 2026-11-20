# Deterministic regex limits on Linux ARM64

Base `claude/cpp-gateway-port` at `0c54578151b88da9afe6d014007dbffe1176214b`, tree `914c34ba615cd5de878ba8454595980668f3619d`. For a resumed run, read this checklist and `git log` before continuing.

- [x] Locate the execution, compile and allocation hooks and confirm the ARM64 runner supports Linux namespaces.
- [ ] Prepare the pinned CI dependencies and measure matcher callbacks in the existing fixtures.
- [ ] Implement the work limits and give wall-clock backstops distinct errors.
- [ ] Add regressions with an injected clock and audit other wall-clock limits.
- [ ] Run the release and sanitizer gates and the four-copy compaction repeat. Bind results to the verified commit and tree.
