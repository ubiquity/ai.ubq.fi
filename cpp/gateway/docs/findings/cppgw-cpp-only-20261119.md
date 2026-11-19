# C++-only gateway branch

## Checklist

- [x] Start the report on the supplied Gemini integration tree.
- [x] Merge the residuals branch and record the request-allowlist revert under STATE A.
- [ ] Build and run the selected State A request tests.
- [x] Inventory tracked language sources, toolchain commands and runtime dependencies.
- [x] Remove the historical comparison and formatter.
- [x] Call the linked Linux confinement implementation directly.
- [x] Retire the offline legacy conversion executables and helper.
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

## Inventory and dependency decisions

- `git ls-files | rg -i '\.(ts|tsx|js|jsx|mjs|cjs|rs)$|Cargo|package\.json|bun\.lock|bunfig|prettier|native-kv|legacy-codec|\.node$|rust-toolchain'` found three TypeScript files, the offline `v8-helper.mjs`, one Cargo lock, two Bun configs, root formatter files and 14 browser scripts under `static/`. No tracked Rust source, add-on binary, or `tests/helpers/*.mjs` remained. `git grep -Il -i -E '(bun|deno|nodejs|cargo|rustc|rustup|typescript|npm|native.kv|legacy.codec|conformance|prettier)'` and targeted searches for artifact paths identified the active callers, packaging, launcher and docs. The historical findings and recorded C++ fixtures are evidence and test inputs.
- `src/storage/native_addon.cpp` verifies the external add-on and returns C++ placeholders. `src/storage/kv.cpp` opens `rt::SqliteKv` directly, and `src/rt/kv_sqlite.cpp` implements the store. `src/rt/kv_jsc*.cpp` implement the persisted value codec. The `serve.cpp` bootstrap gate checks preparation, which only verified that unused artifact. Keep the marked database path, marker and data format.
- `src/runtime/permissions_linux.cpp` hashes the external helper before returning function pointers to its own Landlock and seccomp implementation. `runtime/startup.cpp` verifies both artifacts before opening the store or binding the listener. Neither artifact is loaded. Remove their build and verification paths and keep the linked confinement and manifest checks for the remaining payload.
- `runtime/legacy-codec/typed-value.ts` is imported by `v8-helper.mjs`. The C++ `kv_conversion/legacy_codec.cpp` starts the pinned codec subprocess only for offline conversion and the round-trip proof. Native storage and serving have no call path to that helper. Native snapshot reads and portable typed-value handling are independent and remain covered.
- The existing opt-in Gemini provider executes the owner-installed `agy` binary, and confines child execution to that file and the system ARM64 loader. The gateway, workflow and package do not install that executable or a language runtime for it.

Removed the paired harness and pins, comparison modes from the smoke and benchmark drivers, and the root formatter dependencies. The native HTTP smoke and JSON, regex and request-path measurements remain. The hook uses Git whitespace checks. Parsed all affected Python with `ast.parse` and ran `git diff --check`.

The confinement change keeps the same Landlock and seccomp implementations and policy. It removes the lookup of a separate, never-loaded helper from the confinement API. Three helper-file-only cases were retired. The kernel, strict and reduced boundary, and Gemini child checks remain. Clang 21 strict syntax checks and the registration gate passed.

Removed the pinned offline V8 subprocess, its converter and round-trip proof entry points, and their exclusively offline CLI, conversion and proof tests. The file-digest case, private SQLite snapshot API and tests, schema and queue checks, native profile export, typed values, and cursor fixtures remain. The executable and package target lists now build the gateway only. The CMake inventory and native registration gates passed after the removal.
