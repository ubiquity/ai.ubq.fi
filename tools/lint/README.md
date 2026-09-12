# Lint toolchain

Dev-only tooling for ai.ubq.fi, ported from the Ubiquity `ts-template` lint stack as documented in the gpt-pro-skill
`.guidebook/LINTING.md`. Nothing here ships: the deployed service is Deno-only and never imports anything from this
directory.

## Why this lives in `tools/lint/` and not at the repository root

Every other repo on this stack puts `package.json` and `node_modules` at its root. This one cannot, and the reason is
not stylistic:

`@simplewebauthn/server` (a JSR dependency) transitively requires npm packages. **The mere presence of a root
`package.json`** — even one whose `devDependencies` is empty — flips Deno 2.9 into node_modules-based npm resolution. CI
runs `deno task build` and `deno task test`, both with `--frozen` and neither with an install step, so a fresh checkout
then dies with:

```
error: Could not find a matching package for 'npm:@hexagon/base64@^1.1.27' in the node_modules directory.
Ensure you have all your JSR and npm dependencies listed in your deno.json or package.json,
then run `deno install`. Alternatively, turn on auto-install by specifying
"nodeModulesDir": "auto" in your deno.json file.
```

Measured, four ways, with `node_modules` absent (a fresh-checkout simulation):

| Root `package.json`        | `nodeModulesDir` | `deno task build` |
| -------------------------- | ---------------- | ----------------- |
| absent (today)             | unset            | **passes**        |
| present, no dependencies   | unset            | fails             |
| present, with dependencies | unset            | fails             |
| present, with dependencies | `manual`         | fails             |

`nodeModulesDir: "auto"` would "fix" CI by letting Deno own `node_modules` and populate it with symlinks into its own
cache — the two-owners trap the guidebook documents in §2.3, and the one that makes ESLint load a stale core. It would
also require committing a `deno.lock` that contains the whole JS toolchain.

So the toolchain is isolated here instead. Deno never looks inside this directory: module resolution walks **up** from
each source file (`src/x.ts` → `src/node_modules` → `node_modules`), never sideways into `tools/`.

Everything outside `tools/lint/` is unchanged and shared with the reference stack:

| Path                       | Purpose                                                    |
| -------------------------- | ---------------------------------------------------------- |
| `eslint.config.mjs`        | flat config (here, so its plugin imports resolve locally)  |
| `../../tsconfig.lint.json` | type-aware program, deliberately not named `tsconfig.json` |
| `../../knip.json`          | unused files/exports/dependencies                          |
| `../../.prettierrc`        | `printWidth: 160`, the template's formatting               |
| `../../scripts/*.sh`       | wrappers the tasks and `npm run` scripts call              |

## Running the gates

```sh
sh scripts/verify.sh     # every gate; reports all failures, not just the first
sh scripts/lint.sh       # ESLint only; --fix to apply safe fixes
sh scripts/format.sh     # Prettier write; --check to verify
sh scripts/knip.sh       # unused files/exports/deps
```

`deno task verify`, `deno task lint:eslint`, `deno task format`, `deno task knip` and `deno task types` are thin
wrappers over the same scripts.

## Formatter ownership

Exactly one formatter owns each file type, so the two can never fight:

- **Prettier** — `*.ts`, `*.mjs` (see `.prettierignore`).
- **`deno fmt`** — everything else it supports: JSON, Markdown, CSS, HTML, `static/*.js`.

`deno.json`'s `fmt.exclude` keeps TypeScript and `.mjs` away from `deno fmt`, which also keeps CI's existing
`deno fmt --check` gate meaningful for the files it still owns.

Two traps worth knowing, both measured on Deno 2.9.6 and both contrary to the guidebook's §2.6 as written:

1. `"*.ts"` in `fmt.exclude` is **root-relative**. It silently excludes nothing that is nested, which is every file in
   this repo. `"**/*.ts"` is the pattern that works.
2. Glob `*` does not match a leading dot, so the generated `.deno-types.d.ts` needs its own explicit entry in both
   `fmt.exclude` and `lint.exclude`.

## Generated inputs

`deno task types` writes `.deno-types.d.ts` (~23k lines, gitignored) from `deno types`. The type-aware rules need it:
Deno does not ship `node:` or Web API typings any other way, and when the file is missing those rules lose type
information and report **fewer** findings without erroring. `scripts/verify.sh` regenerates it before linting.

`@types/node` is pinned to `^26` because Deno 2.9.6's Node compatibility level is 26.3.0.

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
