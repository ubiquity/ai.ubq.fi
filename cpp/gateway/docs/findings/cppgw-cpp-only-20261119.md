# C++-only gateway branch

## Checklist

- [x] Start the report on the supplied Gemini integration tree.
- [x] Merge the residuals branch and record the request-allowlist revert under STATE A.
- [ ] Build and run the selected State A request tests.
- [ ] Inventory tracked language sources, toolchain commands and runtime dependencies.
- [ ] Remove the historical comparison and formatter.
- [ ] Remove unnecessary package runtime artifacts and offline tooling.
- [ ] Update CI and current operating documents.
- [ ] Add the ARM64 cutover checklist and runtime environment table.
- [ ] Verify registrations, tracked files, workflows and whitespace.
- [ ] Build the gateway and run the selected release tests in native isolation.
- [ ] Build and verify the portable package.
- [ ] Record final results and leave the checkout clean.

## STATE A

Starting tree: `13502438` (`agent-jobs/cppgw-gemini-merge-20261119-44556239421-1`).
`git fetch origin agent-jobs/cppgw-residuals-1-20261119-44477130790-1` fetched `a5b0e14a36b228272161ab71def2269b34e17d68`. `git merge --no-ff --no-edit FETCH_HEAD` created `0ced7f44c3fdf77323201dd969b102248bf72133` without conflicts.

**STATE A commit:** `ca045a83ebaab6c50c20567d1a825638abac0891`, tree `07cded5948418115e3080701f7be15c16a1857ba`. `git revert --no-edit d35a8a8bbc9a986bded5bba2d9c0f0f1a11c4ea1` conflicted only in the residuals findings. The resolution preserved the merged observations for the other fixes. `git revert --continue` removed both ingress allowlists and their three tests. Both ingress model-length guards and the oversized-model regression from `b58afd3d1` remained as merged.

## Scope

Local verification uses qualified native dependencies and the repository's loopback-only fixture driver. Build outputs and logs stay outside the checkout. The hosted matrix is the full-suite verification path.
