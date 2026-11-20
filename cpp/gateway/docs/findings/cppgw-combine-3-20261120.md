# Native gateway follow-up combination 3

## Checklist

- [ ] Fetch and inspect the three reviewed branches from `claude/cpp-gateway-port` (`0c545781`).
- [ ] Merge `agent-jobs/cppgw-macos-final-20261119-44637373710-1` (Apple Silicon confirmation).
- [ ] Merge `agent-jobs/cppgw-stream-retry-20261119-44629153076-1` (pre-output stream retry).
- [ ] Merge `agent-jobs/cppgw-build-cache-20261119-44628404886-1` (compiler cache).
- [ ] Reconcile release commit binding with the compiler launcher in packaging and tooling.
- [ ] Restrict `UOS_PACKAGE_GIT_COMMIT` to translation units that read it.
- [ ] Show two package-style builds at different commits share non-identity cache hits.
- [ ] Run `python3 cpp/gateway/tools/links.py --check`.
- [ ] Run `actionlint` on both workflows.
- [ ] Build release with the launcher and run release-identity, health, startup, Codex routing/stream-retry and Gemini tests.
- [ ] Portable package build + verifier (Docker) with health bound to the built commit.
- [ ] Record evidence and leave the checkout clean.

## State

Starting commit: `0c545781` (`claude/cpp-gateway-port`, delivered native gateway).

## Log

