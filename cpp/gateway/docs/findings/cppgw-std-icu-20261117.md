# ICU case mapping and default collation

## Work contract v1

Replace the case-mapping and collation code from plan rows 15 and 16 with the ICU 78.3 already used by the native build. Base commit `093a9e0511e9d0deae5495dc324e53ff4dccd719`, tree `cb6c6415cfd603f5aac070d424bc42b4642349e1`.

| Surface                         | Required behavior                                                                                                                                   |
| ------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| `toLowerCase` and `toUpperCase` | ICU root casing, including expansions and contextual mappings. Preserve the WTF-8 string representation and accept embedded NULs.                   |
| `localeCompare`                 | ICU root collation with canonical normalization and tertiary strength. Preserve comparison signs and test the catalog and persisted-schema callers. |
| Arbitrary bytes                 | Use the existing WTF-8 conversion boundary and its replacement for malformed byte sequences. Check lengths before passing them to ICU.              |
| Application contracts           | Preserve authentication and validation decisions, wire formats, and stored-data formats.                                                            |

Ownership covers the two string operations, their declarations, the findings and deviation notes, the component regression and fuzz fixtures, and any required linkage registration. UTF conversion, the TypeScript oracle, other runtime components, CI workflows, hosted activation and production data are separate work.

Validate with Bun 1.4.0, the existing case and collation vectors, a comparison against the original string implementation, strict warning-fatal release and ASan/UBSan Linux ARM64 tests, and a 120-second libFuzzer campaign. Every registered test links `src/rt/string.cpp`, so the sweep includes the full registry. Measure allocation calls and wall time on matched old and current library builds. Format the changed documents and registry and run `git diff --check`.

Open questions at discovery are whether ICU changes any recorded collation signs, whether the historical SDK profile supplies ICU, and whether the runner can finish both full sweeps. Record the exact dependency and source versions, observed differences, terminal test counts and any remaining qualification before handing the committed lane back to the pool.
