# Lint toolchain

Dev-only tooling for ai.ubq.fi, ported from the Ubiquity `ts-template` lint stack as documented in the gpt-pro-skill
`.guidebook/LINTING.md`. Nothing here ships: the deployed service is Deno-only and never imports anything from this
directory.

## Layout

| Path                           | Owns                                                                                   |
| ------------------------------ | -------------------------------------------------------------------------------------- |
| `package.json` (root)          | Project metadata for tool discovery. **No dependencies.**                              |
| `tools/lint/package.json`      | The real dev toolchain: ESLint, typescript-eslint, sonarjs, knip, Prettier, TypeScript |
| `tools/lint/eslint.config.mjs` | Flat config (here, so its plugin imports resolve locally)                              |
| `tools/lint/node_modules/`     | Installed by bun or npm; gitignored                                                    |
| `../../tsconfig.lint.json`     | Type-aware program, deliberately not named `tsconfig.json`                             |
| `../../knip.json`              | Unused files/exports/dependencies config                                               |
| `../../.prettierrc`            | `printWidth: 160`, the template's formatting                                           |
| `../../scripts/*.sh`           | Wrappers the deno tasks and `npm run` scripts call                                     |

## Why the toolchain is not at the repository root

`@simplewebauthn/server` (a JSR dependency) transitively requires npm packages such as `npm:@hexagon/base64`. A root
`package.json` that declares npm dependencies makes Deno resolve that graph through `node_modules`, and CI runs
`deno task build` and `deno task test` -- both `--frozen`, neither with an install step -- so a fresh checkout then dies
with:

```
error: Could not find a matching package for 'npm:@hexagon/base64@^1.1.27' in the node_modules directory.
Ensure you have all your JSR and npm dependencies listed in your deno.json or package.json, then run `deno install`.
Alternatively, turn on auto-install by specifying "nodeModulesDir": "auto" in your deno.json file.
```

Measured with `node_modules` absent (a fresh-checkout simulation) and `nodeModulesDir` unset:

| Root `package.json`                                    | `deno task build` |
| ------------------------------------------------------ | ----------------- |
| absent                                                 | **passes**        |
| present, `devDependencies` empty                       | fails             |
| present, with dependencies                             | fails             |
| present, with dependencies, `nodeModulesDir: "manual"` | fails             |

`nodeModulesDir: "auto"` would "fix" CI by letting Deno own `node_modules` and populate it with symlinks into its own
cache -- the two-owners trap the guidebook documents in section 2.3 -- and would require committing a `deno.lock`
listing the entire JS toolchain, which every CI run would then download.

Two things follow, and both are deliberate:

1. **`deno.json` sets `"nodeModulesDir": "none"`.** Deno resolves its own JSR/npm graph from the global cache exactly as
   it did before this tooling existed, and never reads a `node_modules` directory. This is what makes a root
   `package.json` safe: with zero dependencies declared, Deno has nothing to resolve there, `deno.lock` is unchanged,
   and `deno task build` / `deno task test` pass on a fresh checkout with no install step. Verified.
2. **The dev dependencies live in `tools/lint/`.** Declaring them at the root would pull the toolchain into Deno's
   dependency graph (lockfile churn plus eager npm resolution on every build). Deno never looks inside `tools/lint/`:
   module resolution walks _up_ from each source file (`src/x.ts` -> `src/node_modules` -> `node_modules`), never
   sideways.

`tools/lint/node_modules` has no symlinks (`find node_modules -maxdepth 1 -type l` -> 0) and no `node_modules/.deno`, so
the stale-core trap from section 2.3 cannot occur.

## Running the gates

```sh
sh scripts/verify.sh     # every gate; reports all failures, not just the first
sh scripts/lint.sh       # ESLint only; --fix to apply safe fixes
sh scripts/format.sh     # Prettier write; --check to verify
sh scripts/knip.sh       # unused files/exports/deps
```

`deno task verify`, `deno task lint:eslint`, `deno task format`, `deno task knip` and `deno task types` are wrappers
over the same scripts. The pre-commit hook routes staged TypeScript to Prettier and staged JSON/Markdown to `deno fmt`.

## Formatter ownership

Exactly one formatter owns each file type, so the two can never fight:

- **Prettier** -- `*.ts`, `*.mjs` (see `.prettierignore`).
- **`deno fmt`** -- everything else it supports: JSON, Markdown, CSS, HTML, `static/*.js`.

`deno.json`'s `fmt.exclude` keeps TypeScript and `.mjs` away from `deno fmt`, which also keeps CI's existing
`deno fmt --check` gate meaningful for the files it still owns.

Two traps worth knowing, both measured on Deno 2.9.6 and both contrary to the guidebook's section 2.6 as written:

1. `"*.ts"` in `fmt.exclude` is **root-relative**. It silently excluded nothing nested -- which is every file in this
   repo. `"**/*.ts"` is the pattern that works.
2. Glob `*` does not match a leading dot, so the generated `.deno-types.d.ts` needs its own explicit entry in both
   `fmt.exclude` and `lint.exclude`.

## Generated inputs

`deno task types` writes `.deno-types.d.ts` (~23k lines, gitignored) from `deno types`. The type-aware rules need it:
Deno does not ship `node:` or Web API typings any other way, and when the file is missing those rules lose type
information and report **fewer** findings without erroring. `scripts/verify.sh` regenerates it before linting.

`@types/node` is pinned to `^26` because Deno 2.9.6's Node compatibility level is 26.3.0.

## knip

knip is configured for this repo's real entry points (`serve.ts`, the scripts and ops CLIs, the benchmark harness, and
the `*_test.ts` / `*.e2e.ts` files) with `includeEntryExports: true`.

Two accommodations, both with evidence:

- `ignoreDependencies` lists the Deno import-map specifiers (`@std/yaml`, `@deno/kv-utils`, `@deno/kv-utils/json`,
  `@simplewebauthn/server`). knip does not read `deno.json#imports`, so it would otherwise report them as unlisted.
- `ignoreIssues` suppresses `exports` findings in the five files whose exports are loaded through
  `await import(new URL("src/kv.ts", release).href)` in `scripts/serve-vps.ts` and `scripts/serve-mac.ts`. knip cannot
  follow a URL built at runtime, so every symbol those serve scripts destructure looks unused. `initializeKv` is the
  worked example.

## Known false positives in the type-aware rules

The lint program's type environment (`.deno-types.d.ts` + `ES2022` lib) is close to, but not identical with, Deno's own
checker. Autofixes that rely on type inference can therefore disagree with `deno check`:

- `@typescript-eslint/no-unnecessary-type-assertion` is **off** for this reason. Its autofix removed 406 assertions, and
  7 of those removals failed `deno check` / `deno test`.
- `@typescript-eslint/consistent-type-definitions` is set to `"type"`. At the preset default of `"interface"` its
  autofix converted exported record aliases into interfaces, and an interface has no implicit index signature where an
  object-literal type alias does, which broke two type checks.
- `@typescript-eslint/dot-notation` is on, but three sites needed the file's own
  `const recorded = recordedBody as Record<string, unknown>` idiom: a closure-assigned `let` narrows to `never` after
  `assert.ok(...)`, and `never["x"]` is accepted where `never.x` is not.

Always run `deno task build` and `deno task test` after a `--fix` run; `deno check` is the authority on whether the
result is still correct.

## Rule divergences from the template

Each is marked `DIVERGENCE` in `tools/lint/eslint.config.mjs` with its measurement:

| Rule                                                           | Decision                                                                                                                                               |
| -------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `func-style`                                                   | `allowArrowFunctions: true`. The template's ban produced 2944 findings, all of them the repo's `const fn = () => {}` idiom.                            |
| `@typescript-eslint/naming-convention` (boolean selector)      | Off. 323 findings across 173 distinct names spanning noun phrases, verbs and bare states; any regex accepting them accepts every camelCase identifier. |
| `@typescript-eslint/naming-convention` (`variableLike` filter) | `^_` instead of the bare `_`, matching what the sibling `no-unused-vars` rule already accepts via `argsIgnorePattern`.                                 |
| `@typescript-eslint/restrict-template-expressions`             | `allowNumber`/`allowBoolean`. They were 479 of 613 findings; objects, symbols, nullish and `any`/`unknown` stay reported.                              |
| `sonarjs/no-empty-test-file`                                   | Off. It looks for `describe`/`it`/`test`; all 1148 of this repo's tests are `Deno.test`, so it flagged 97 real test files as empty.                    |
| `no-empty`                                                     | `allowEmptyCatch`, and `deno lint`'s `no-empty` is excluded to match (section 2.6).                                                                    |
| `no-nested-ternary`                                            | Dropped; `sonarjs/no-nested-conditional` reports the same lines.                                                                                       |
