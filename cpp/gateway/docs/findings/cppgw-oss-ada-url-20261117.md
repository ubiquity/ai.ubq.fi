# Ada 4.0.0 URL replacement findings

## Contract v1

Qualify OSS replacement plan rows 4 and 5 against the C++ port at `0edaf83367bbb29eaf50076afd428818f4b3e4bb` and the Bun 1.4.0 oracle. Preserve the public C++ API, constructor and relative-resolution behavior, setter results and errors, IDNA, and form encoding. Preserve search params ordering, live iteration, and the two-way link to the URL. Row 34's Node POSIX file URL conversion is in the parity checks. Application routing, providers, credentials, deployment, and TypeScript changes are outside the scope.

Writes are limited to the assigned URL sources, Ada vendor directory, URL test additions, equivalence and fuzz files, related test links and CMake registration, and this report. The acceptance commands cover the unchanged recorded vectors, fixed and seeded old/new comparisons, a 120-second fuzz smoke, and strict plain and ASan/UBSan builds for the affected closures. The release benchmark compares old and candidate at identical optimization settings. The candidate must meet parity with a thin adapter and have no runtime regression on the representative workloads. If that requires reimplementing substantial generic behavior, retain the existing production component and record the evidence.

## Discovery

The runner is Ubuntu 24.04 ARM64 with GCC 14.2.0 and Bun 1.4.0. The release tag was published on 2026-07-27. Both single-header asset hashes matched the task pins after download from the release. [Ada v4.0.0](https://github.com/ada-url/ada/releases/tag/v4.0.0)

```sh
curl -fsSL https://github.com/ada-url/ada/releases/download/v4.0.0/ada.h -o ada.h
curl -fsSL https://github.com/ada-url/ada/releases/download/v4.0.0/ada.cpp -o ada.cpp
printf '%s\n' \
  '0010b17b3ff489e13998e86d921b16e1643e370ee3276c584f1b0c4b72bdf628  ada.h' \
  '1c722f8f5355d6bf2bf3601870684edc875aa8f56b4d4735440d96263dd0207c  ada.cpp' | sha256sum --check
```

## Qualification checkpoint

The unchanged port with ICU 78.3 passed `rt/url_test.cpp` (6/6) and `rt/url_idna_test.cpp` (2/2) under GCC 14.2.0. The latter includes 1,260 curated rows and 2,228,224 scalar observations. For the same scalar inputs, a live Bun run and the unchanged port produced identical accepted URL output byte for byte. Their shared SHA-256 was `10f60d88344eb5530adaf8775005928f7ce385c07663eb8b58e32689d79c0bde`.

Raw Ada agreed with the Bun constructor fixture on 2,072 of 2,081 rows, and with the curated IDNA fixture on 1,180 of 1,260 rows. The scalar run had 411 extra acceptances across 401 code points, with identical hrefs on all 313,554 accepted Bun inputs. Examples include leading U+1AC1 and U+0870 inside a Latin label. Rejecting either character wherever it occurs would discard valid Bun URLs. Ada also accepts invalid ASCII ACE labels and some invalid bidirectional domains and joiner sequences. The corresponding baseline and live Bun results agree on those inputs.

Ada 4.0.0 deliberately passes ASCII domains through without validating ACE labels. Its Unicode validity checks operate per label and its ContextJ loop can return success before checking the remainder of a label. Full Bun parity therefore needs version-specific Unicode data and validation across labels in addition to URL-level adaptation. The remaining work is to document reproducible controls, performance, and the shipping decision. No production lines have changed.
