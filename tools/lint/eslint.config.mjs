// @ts-check
// Ubiquity ts-template lint ruleset, ported to ai.ubq.fi from the gpt-pro-skill
// reference implementation. Deliberate local changes are marked DIVERGENCE.
// The in-house @ubiquity-os/eslint-plugin-no-empty-strings rule is intentionally omitted.
import eslint from "@eslint/js";
import tsEslint from "typescript-eslint";
import sonarjs from "eslint-plugin-sonarjs";
import checkFile from "eslint-plugin-check-file";
import path from "node:path";

// DIVERGENCE: the stock template's boolean-variable naming rule is disabled in
// this repo -- see the naming-convention block below for the measurement. The
// template's improved booleanNamePattern (34/34 idiomatic names accepted on the
// reference repo) was tried here first and still rejected 323 variables across
// 173 distinct names, so its arrays and pattern are deliberately absent rather
// than left in place as dead code.

export default tsEslint.config(
  {
    // Stale copies, generated output, and dev scratch directories.
    // lib/ is the vendored openai/codex submodule; .data/ holds immutable release
    // copies of this whole repo; .codex-worktrees/ holds ~139 live worktrees.
    // None of them are our source, and the last two would otherwise be linted as
    // thousands of duplicate files.
    ignores: [
      "node_modules/**",
      "**/*.d.ts", // Trap 2.4: .deno-types.d.ts is generated and ~23k lines
      "eslint.config.mjs",
      "lib/**",
      ".codex-worktrees/**",
      ".data/**",
      ".release/**",
      ".kv-migration/**",
      ".sentinel/**",
      ".diagnostics/**",
      "logs/**",
      "benchmark-runs/**",
    ],
  },
  {
    files: ["**/*.ts"],
    plugins: {
      "@typescript-eslint": tsEslint.plugin,
      "check-file": checkFile,
    },
    extends: [
      eslint.configs.recommended,
      ...tsEslint.configs.recommended,
      ...tsEslint.configs.strictTypeChecked,
      ...tsEslint.configs.stylisticTypeChecked,
      sonarjs.configs.recommended,
    ],
    languageOptions: {
      parser: tsEslint.parser,
      parserOptions: {
        project: ["./tsconfig.lint.json"],
        // This config lives in tools/lint/ but lints the repository root, so the
        // tsconfig paths above resolve against the repo root, not this directory.
        tsconfigRootDir: path.resolve(import.meta.dirname, "../.."),
      },
    },
    rules: {
      "check-file/filename-naming-convention": [
        "error",
        {
          "**/*.{js,ts}": "+([-._a-z0-9])",
        },
      ],
      "prefer-arrow-callback": ["warn", { allowNamedFunctions: true }],
      // DIVERGENCE: the template forbids arrow functions entirely
      // ({ allowArrowFunctions: false }) and reported 2944 findings here -- every
      // one of them the repo's established `const fn = () => {}` idiom, which is
      // also Deno's own convention. Satisfying it would mean rewriting 2944
      // functions with no behavioural gain, so the rule keeps its real intent
      // (prefer declarations or arrows over `function` expressions assigned to a
      // variable) and stops policing the arrow-vs-declaration style choice.
      "func-style": ["warn", "declaration", { allowArrowFunctions: true }],
      "@typescript-eslint/no-floating-promises": "error",
      "@typescript-eslint/no-non-null-assertion": "error",
      "constructor-super": "error",
      "no-invalid-this": "off",
      "@typescript-eslint/no-invalid-this": "error",
      "no-restricted-syntax": ["error", "ForInStatement"],
      "use-isnan": "error",
      "no-unneeded-ternary": "error",
      // DIVERGENCE: the stylistic preset defaults to "interface". Left at that
      // default, its autofix converted this repo's exported record aliases
      // (`export type PasskeyCredentialRecord = {...}`) into interfaces, and an
      // interface has NO implicit index signature where an object-literal type
      // alias does. That silently broke two type checks (`Spread types may only
      // be created from object types`, and `{}` no longer assignable to the
      // record) under `deno check`, which is the authority here. The repo's
      // established style is type aliases, so the rule now enforces that.
      "@typescript-eslint/consistent-type-definitions": ["error", "type"],
      // DIVERGENCE: strictTypeChecked forbids numbers and booleans inside template
      // literals, which produced 479 of 613 findings here (`Invalid type "number"
      // of template literal expression`). Interpolating a number or a boolean is
      // unambiguous and safe; what this rule exists to catch is objects, symbols,
      // null/undefined and `any`/`unknown`, and those stay reported.
      "@typescript-eslint/restrict-template-expressions": [
        "error",
        { allowNumber: true, allowBoolean: true },
      ],
      // DIVERGENCE: OFF, with evidence. Its autofix removed 406 "unnecessary"
      // assertions; 7 of those removals then failed `deno check` / `deno test`,
      // because this lint project's type environment (.deno-types.d.ts + ES2022
      // lib) does not reproduce Deno's checker exactly -- e.g.
      // `lease.quota_class as CodexQuotaClass` is load-bearing for Deno and
      // "unnecessary" for us. Since the autofix is not per-site opt-out-able, the
      // rule stays off until the lint project's lib matches Deno's. Revisit as
      // follow-up; do not simply re-enable without re-running the suite.
      "@typescript-eslint/no-unnecessary-type-assertion": "off",
      // DIVERGENCE: OFF. The rule looks for describe/it/test calls; this repo's
      // 1148 tests are all `Deno.test(...)`, so it reported 97 real test files as
      // empty. It cannot see Deno's test API, so every finding here is a false
      // positive.
      "sonarjs/no-empty-test-file": "off",
      // DIVERGENCE: dropped "no-nested-ternary". sonarjs/no-nested-conditional already
      // ships in the sonarjs recommended preset and reported the identical lines,
      // producing two errors for one problem.
      // DIVERGENCE: allowEmptyCatch, matching the workaround work.ubq.fi already
      // carried locally. All 11 no-empty hits here were intentional `} catch {}`
      // cleanup/teardown blocks.
      "no-empty": ["error", { allowEmptyCatch: true }],
      "@typescript-eslint/no-unused-vars": [
        "error",
        {
          args: "after-used",
          ignoreRestSiblings: true,
          vars: "all",
          varsIgnorePattern: "^_",
          argsIgnorePattern: "^_",
        },
      ],
      "@typescript-eslint/await-thenable": "error",
      "@typescript-eslint/no-misused-new": "error",
      "@typescript-eslint/restrict-plus-operands": "error",
      // ---------------------------------------------------------------------
      // RESTORE 1: sonarjs rules that v4 still ships but sets to "off" in its
      // recommended preset (they were "error" in v2). Mostly security hotspots.
      // ---------------------------------------------------------------------
      "sonarjs/os-command": "error",
      "sonarjs/no-unsafe-unzip": "error",
      "sonarjs/confidential-information-logging": "error",
      "sonarjs/no-ip-forward": "error",
      "sonarjs/frame-ancestors": "error",
      "sonarjs/no-mixed-content": "error",
      "sonarjs/hidden-files": "error",
      "sonarjs/no-intrusive-permissions": "error",
      "sonarjs/no-commented-code": "error",
      // ---------------------------------------------------------------------
      // RESTORE 2: rules sonarjs v4 deleted, recovered from ESLint core.
      // (their typescript-eslint equivalents arrive via strictTypeChecked,
      //  so core duplicates are deliberately not enabled.)
      // ---------------------------------------------------------------------
      "no-extend-native": "error",
      "new-cap": "error",
      "default-case": "error",
      "no-var": "error",
      "no-self-compare": "error",
      "no-useless-escape": "error",
      "max-lines": ["warn", { max: 1000 }],
      // ---------------------------------------------------------------------
      // SONARJS/TS OVERLAP: the type-aware TS rule supersedes the sonarjs one,
      // so turn the sonarjs copy off to avoid reporting one problem twice.
      // ---------------------------------------------------------------------
      "sonarjs/prefer-regexp-exec": "off",
      "sonarjs/deprecation": "off",
      // ---------------------------------------------------------------------
      // strictTypeChecked's no-unsafe-* family needs the `any` sources cleaned
      // up first; enable these once the codebase is `any`-free.
      // ---------------------------------------------------------------------
      "@typescript-eslint/no-unsafe-assignment": "off",
      "@typescript-eslint/no-unsafe-member-access": "off",
      "@typescript-eslint/no-unsafe-call": "off",
      "@typescript-eslint/no-unsafe-return": "off",
      "@typescript-eslint/no-unsafe-argument": "off",
      "sonarjs/no-all-duplicated-branches": "error",
      "sonarjs/no-collection-size-mischeck": "error",
      "sonarjs/no-duplicated-branches": "error",
      "sonarjs/no-element-overwrite": "error",
      "sonarjs/no-identical-conditions": "error",
      "sonarjs/no-identical-expressions": "error",
      "@typescript-eslint/naming-convention": [
        "error",
        {
          selector: "interface",
          format: ["StrictPascalCase"],
          custom: { regex: "^I[A-Z]", match: false },
        },
        {
          selector: "memberLike",
          modifiers: ["private"],
          format: ["strictCamelCase"],
          leadingUnderscore: "require",
        },
        {
          selector: "typeLike",
          format: ["StrictPascalCase"],
        },
        {
          selector: "typeParameter",
          format: ["StrictPascalCase"],
          prefix: ["T"],
        },
        {
          selector: "variable",
          format: ["strictCamelCase", "UPPER_CASE"],
          leadingUnderscore: "allow",
          trailingUnderscore: "allow",
        },
        {
          selector: "variable",
          modifiers: ["destructured"],
          format: null,
        },
        // DIVERGENCE: the template's boolean-variable selector (and this repo's
        // inherited copy of the improved booleanNamePattern above) is DISABLED
        // here, measured rather than assumed. It reported 323 boolean variables
        // covering 173 distinct names: noun-phrase states (`coreMissing`,
        // `duplicatedKeys`, `dispatchBoundary`), verb forms (`classificationsAgree`,
        // `correlationMatches`), bare states (`complete`, `dryRun`, `due`, `escape`)
        // and UPPER_CASE constants (`CANONICAL_TOOL_DEFAULT_STRICTNESS`). Any regex
        // that accepts those names accepts every camelCase identifier, so keeping it
        // would add noise without enforcing a real convention. This is a third data
        // point for the guidebook's org-level finding that the boolean rule does not
        // survive contact with real code. The rest of naming-convention stays active.
        {
          selector: "variableLike",
          format: ["strictCamelCase"],
          // DIVERGENCE: exempt ALL leading-underscore names, not just the bare `_`
          // placeholder the template filtered. ts-template dropped the filter
          // ubiquibot's older .eslintrc carried, so `(_, reject) => {}` was reported
          // as not strictCamelCase; here the same class was 205 findings, almost all
          // of them `_ctx`, `_input`, `_signal`-style deliberately-unused parameters.
          // `^_` is exactly what the sibling no-unused-vars rule already accepts via
          // argsIgnorePattern, so the two rules now agree instead of contradicting.
          filter: { regex: "^_", match: false },
        },
        {
          selector: ["function", "variable"],
          format: ["strictCamelCase"],
        },
      ],
    },
  },
  {
    // ---------------------------------------------------------------------
    // Test doubles legitimately need no-op stubs: fake clocks, `save` callbacks
    // for paths that intentionally do not persist, stream sinks that only record
    // what they receive. @typescript-eslint/no-empty-function exists to catch
    // ACCIDENTALLY empty implementations, so it earns nothing on arrow-function
    // stubs in tests, and rewriting them as `() => Promise.resolve()` is
    // contortion. Narrower than turning the rule off: empty function
    // DECLARATIONS and empty methods in tests are still reported.
    // ---------------------------------------------------------------------
    files: ["**/tests/**/*.ts", "**/benchmarks/**/*.ts"],
    rules: {
      "@typescript-eslint/no-empty-function": ["error", { allow: ["methods", "arrowFunctions"] }],
    },
  }
);
