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

## Decision

Retain the existing production URL, ICU IDNA, URLSearchParams, and POSIX file URL code under the task's stop rule. Production lines deleted and added were both **0**. The 1,828-line `url.cpp` and 190-line public `url.hpp` retain the baseline bytes. The pinned Ada assets and the candidate remained in runner scratch. All recorded expected values were preserved.

The candidate would need its own domain-wide Bidi policy, ContextJ validation, compatible Unicode properties, and ASCII ACE validation to replace ICU. Ada 4.0.0's ASCII path deliberately skips ACE validation. For non-ASCII input, its Bidi decision is per label and its joiner check can return success before checking the rest of the label. Correcting those behaviors independently of ICU would reimplement the core IDNA validation being replaced. Retaining ICU as the authority for every affected domain would leave that part of the replacement incomplete. See `from_ascii_to_ascii`, `to_ascii`, and `is_label_valid` in the published amalgamation. [Ada source asset](https://github.com/ada-url/ada/releases/download/v4.0.0/ada.cpp)

A separate baseline conflict prevents satisfying both oracles for arbitrary inputs. Bun 1.4.0 accepts `"http://" + "a".repeat(2040) + ".é/"`, whose mapped hostname is 2,048 bytes. It throws `TypeError`, `Invalid URL`, `ERR_INVALID_URL` for the same input with 2,041 `a` characters. The current port with ICU 78.3 and Ada 4.0.0 both accept the 2,052-byte input and serialize a hostname of 2,049 bytes. An exact old/new comparator and live Bun parity cannot both pass on that input. The existing recorded corpus remains valid.

## Results

| Unchanged native test        |     Baseline `-O2` |           Baseline ASan/UBSan | Ada adapter trial `-O2` |
| ---------------------------- | -----------------: | ----------------------------: | ----------------------: |
| `tests/rt/url_test.cpp`      | 6 passed, 0 failed |   6 passed, 0 failed (GCC 14) |      2 passed, 4 failed |
| `tests/rt/url_idna_test.cpp` | 2 passed, 0 failed | 2 passed, 0 failed (Clang 18) |      0 passed, 2 failed |

The GCC-sanitized IDNA binary reached the scalar test and was stopped after 12 minutes once the Clang retry had passed the full suite in 47.18 seconds. A separate GCC run with `malloc_context_size=1` timed out at 120 seconds. The sanitized successes had zero sanitizer reports. The strict release builds for both suites and the trial used GCC 14.2.0.

The IDNA test includes 1,260 curated rows and 2,228,224 scalar observations in 1,088 blocks. Live Bun and the baseline produced identical accepted URL output byte for byte in the scalar sweep. Their shared SHA-256 was `10f60d88344eb5530adaf8775005928f7ce385c07663eb8b58e32689d79c0bde`. Raw Ada accepted all 313,554 of those inputs with identical hrefs and accepted 411 additional inputs across 401 distinct code points. The Ada adapter trial differed in 17 scalar blocks and 72 curated IDNA rows.

| Raw Ada comparison with recorded Bun values                 | Records | Differing records |
| ----------------------------------------------------------- | ------: | ----------------: |
| Constructor and original IDNA limit vectors, all getters    |   2,081 |                 9 |
| Setter vectors, href only                                   |   2,475 |               108 |
| Curated IDNA constructors, host, hostname, and href setters |   1,260 |                80 |

The constructor differences include two extra acceptances and origin differences for `blob:` and `ftps:`. The raw setter failures were 102 `host`, four `hostname`, and two `pathname` results. The trial used Ada parsing and form encoding/decoding behind the existing API. It retained the existing URL record shape and serialization, WebKit setters and origin, URLSearchParams storage and live linkage, and Node POSIX conversion. Its boundary converted WTF-8 strings to USVString, preserved a query's literal leading `?`, and replaced invalid UTF-8 after percent-decoding. The Bun-recorded form/linkage/Node test passed the trial.

The trial differed from the baseline on 2,191 of 8,192 seeded inputs. Each input exercised five operations, with separate hashes for getters, errors, and search params. Differences were 0 on raw absolute input, 887 on constructed absolute input, 1,358 on raw input with a base, 121 on setter and linked mutations, and 0 on standalone form operations. The generator used `std::mt19937(0xada400)`, 0–80 arbitrary bytes, and combinations of URL and query delimiters, ASCII/Unicode hosts, and special, custom, opaque, and file URLs. The permanent old/new comparator, fuzz target and two-minute smoke, and all-consumer plain and sanitized runs remain open for a candidate that passes the parity gate. Every normal test closure links `src/rt/url.cpp`.

### Confirmed examples

The `\u` sequences below represent Unicode characters in the input. Rejections from Bun and the baseline carry the same `Invalid URL` TypeError and `ERR_INVALID_URL` code.

| Input or operation                       | Bun 1.4.0 and baseline          | Raw Ada 4.0.0                     |
| ---------------------------------------- | ------------------------------- | --------------------------------- |
| `http://xn--a/` or `http://%78%6e--a/`   | Rejects                         | `http://xn--a/`                   |
| `http://a\u0870b.example/p?q#f`          | Rejects                         | `http://xn--ab-h8e.example/p?q#f` |
| `http://\u0870.example/p?q#f`            | `http://xn--cxb.example/p?q#f`  | Same                              |
| `http://\u1ac1.example/`                 | Rejects                         | `http://xn--brf.example/`         |
| `http://a\u1ac1b.example/`               | `http://xn--ab-eqq.example/`    | Same                              |
| `http://1.\u05d0\u05d1/p?q#f`            | Rejects                         | `http://1.xn--4dbc/p?q#f`         |
| `http://\u0915\u094d\u200c\u200da/`      | Rejects                         | `http://xn--a-tud2ow90gfa/`       |
| `file://h/C\|/x`                         | `file://h/C\|/x`                | `file://h/C:/x`                   |
| `sc://@h?q`                              | `sc://h/?q`                     | `sc://h?q`                        |
| Set host to `x:80` on `http://a:81/`     | `http://x:81/`                  | `http://x/`                       |
| Parse and serialize params `x=%ED%A0%80` | `x=%EF%BF%BD%EF%BF%BD%EF%BF%BD` | `x=%ED%A0%80`                     |

The accepted controls demonstrate why a blacklist of scalar differences would break valid inputs. A setter adapter also needs to preserve raw authority and file path details that Ada canonicalizes away. The standalone form comparison shows that WebIDL string and UTF-8 decoding adaptation covers the observed standalone form discrepancies with the old storage and iteration code.

## Performance

GCC 14.2.0 `-O2`, one ARM64 core pinned with `taskset -c 2`, median of seven samples of 100,000 operations after 5,000 warmup operations. Counts are C++ `operator new` calls per operation in a separate 5,000-operation pass. Direct `malloc` calls in dependencies are outside those counts. Each workload asserts equal hrefs or equal serialized form output before measurement. The raw Ada column diagnoses library cost on those inputs. The adapter column includes the retained C++ API, query parsing, and URL record conversion. These figures are performance evidence for the trial's matching inputs.

| Workload                         | Old ns/op | Adapter ns/op | Raw Ada ns/op | Old allocations/op | Adapter allocations/op | Raw Ada allocations/op |
| -------------------------------- | --------: | ------------: | ------------: | -----------------: | ---------------------: | ---------------------: |
| HTTPS gateway route              |   1,062.9 |         381.9 |          65.1 |                 12 |                      4 |                      1 |
| HTTPS route with query           |   2,058.5 |         683.0 |          88.1 |                 26 |                      9 |                      1 |
| IPv6 with port                   |   1,047.6 |         601.8 |         226.4 |                 10 |                      6 |                      2 |
| IDNA `bücher.example`            |   1,635.6 |       1,348.8 |         631.1 |                 16 |                     14 |                      5 |
| Relative URL with base and query |   2,232.6 |       1,161.4 |         285.5 |                 27 |                     14 |                      2 |
| Small form round trip            |     864.4 |         442.1 |         233.2 |                 10 |                      6 |                      2 |
| Redirect form with repeated keys |   2,912.2 |         980.3 |         554.7 |                 36 |                     11 |                      6 |

The URL inputs were `https://ai.ubq.fi/v1/responses`, the same URL with `?model=gpt-6-astra&stream=true`, `http://[2001:db8::1]:8080/v1/models`, `https://bücher.example/p?q#f`, and `../v1/responses?model=gpt-6-astra` against `https://ai.ubq.fi/path?q=1#hash`. The form inputs were `a=1&b=2&next=%2Fv1%2Fmodels` and `client_version=1.2.3&scope=read+write&next=%2Fv1%2Fresponses&name=B%C3%BCcher&scope=offline`.

## Integration findings

- The shared vendor registry hardcodes supported names and manifest hashes in `tools/vendor_sources.py` and `cmake/vendors.json`. A CMake-only registration would omit Ada from the SDK unit runner, portable CI driver and build input receipts. Those files are outside the task's write scope. [Vendor selection](../../tools/vendor_sources.py) [Registry](../../cmake/vendors.json)
- The disposable fuzz driver honors `extra_sources` and expects a corresponding `fuzz/corpus/<target>` directory. The production-host fuzz runner currently reads `common_sources` and the harness only. A future equivalence target needs a corpus directory and a legacy source closure in both runners to support both entry points. [Disposable driver](../../fuzz/ci.py) [Production-host runner](../../fuzz/run.py)
- Publication was blocked. `GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false git push --dry-run origin HEAD:refs/heads/cppgw-oss-ada-url-20261117` failed because the runner has no GitHub write credentials. The report commits are on the clean job branch for the C++ port integration.

## Commands

The commands below run from the repository root. `work` is external scratch. The Ada files were downloaded and verified as shown above in `$work/ada`. The runner already had Bun `1.4.0+34cbb9a40` and GCC `14.2.0`.

```bash
work=$(mktemp -d /tmp/cppgw-ada.XXXXXX)
bun_oracle=$(command -v bun)
mkdir -p "$work"/{ada,icu78,direct,asan}
curl -fsSL https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2 \
  -o "$work/boost_1_92_0.tar.bz2"
tar -xjf "$work/boost_1_92_0.tar.bz2" -C "$work"
(
  cd "$work/boost_1_92_0"
  ./bootstrap.sh --with-libraries=json,container
  ./b2 -j4 toolset=gcc-14 link=static variant=release --with-json --with-container stage
)
(
  cd "$work/icu78"
  curl -fsSLO https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz
  curl -fsSLO https://github.com/unicode-org/icu/releases/download/release-78.3/SHASUM512.txt
  rg 'icu4c-78.3-sources.tgz$' SHASUM512.txt | sha512sum -c -
  tar -xzf icu4c-78.3-sources.tgz
  cd icu/source
  ./configure --prefix="$work/icu78/install" --disable-tests --disable-samples --disable-extras \
    --disable-icuio --disable-layout --disable-dyload
  make -j3
  make install
)

strict=(-std=c++20 -O2 -Wall -Wextra -Wshadow -Wconversion -Werror
  -DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB
  -DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING -DUOS_GATEWAY_NATIVE_LIBRARIES=1
  -I cpp/gateway/include -I cpp/gateway -isystem "$work/boost_1_92_0"
  -isystem "$work/icu78/install/include")
libs=("$work/boost_1_92_0/stage/lib/libboost_json.a" -L"$work/icu78/install/lib" -licuuc -licudata)
sections=(-ffunction-sections -fdata-sections -Wl,--gc-sections)
san=(-O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)

isolated() (
  scratch=$(mktemp -d "$work/run.XXXXXX")
  trap 'rm -rf "$scratch"' EXIT
  mkdir "$scratch/h" "$scratch/t" "$scratch/w"
  cd "$scratch/w"
  sudo -n unshare --net setpriv --reuid="$(id -u)" --regid="$(id -g)" --clear-groups env -i \
    PATH=/usr/bin:/bin HOME="$scratch/h" TMPDIR="$scratch/t" LANG=C.UTF-8 TZ=UTC \
    LD_LIBRARY_PATH="$work/icu78/install/lib" \
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$@"
)

g++-14 "${strict[@]}" cpp/gateway/src/rt/url.cpp cpp/gateway/tests/rt/url_test.cpp \
  "${libs[@]}" -o "$work/direct/url_test"
g++-14 "${strict[@]}" "${sections[@]}" cpp/gateway/src/rt/{url,string}.cpp \
  cpp/gateway/tests/rt/url_idna_test.cpp "${libs[@]}" -o "$work/direct/url_idna_test"
g++-14 "${strict[@]}" "${san[@]}" "${sections[@]}" cpp/gateway/src/rt/url.cpp \
  cpp/gateway/tests/rt/url_test.cpp "${libs[@]}" -o "$work/asan/url_test"
g++-14 "${strict[@]}" "${san[@]}" "${sections[@]}" cpp/gateway/src/rt/{url,string}.cpp \
  cpp/gateway/tests/rt/url_idna_test.cpp "${libs[@]}" -o "$work/asan/url_idna_test"
isolated "$work/direct/url_test"
isolated "$work/direct/url_idna_test"
isolated "$work/asan/url_test"
isolated timeout 120 env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:malloc_context_size=1 \
  "$work/asan/url_idna_test"
clang++-18 "${strict[@]}" -O1 -gline-tables-only -fsanitize=address,undefined \
  -fno-omit-frame-pointer -fno-sanitize-recover=all "${sections[@]}" \
  cpp/gateway/src/rt/{url,string}.cpp cpp/gateway/tests/rt/url_idna_test.cpp \
  "${libs[@]}" -o "$work/asan/url_idna_clang18"
isolated timeout 90 "$work/asan/url_idna_clang18"
```

The source of the baseline `url.cpp` had SHA-256 `1ad477bba4c8b1a8e70eae45daddb9434af88027931b96a3cdc3af6242e41af5`. The IDNA fixture had SHA-256 `931ef0c33715ac686fffb08af389f19c738a0e3ecae8a69df151e07386f7046f`.

### Replay the scalar comparison

Save this as `$work/scalar.ts`.

```ts
const out = Bun.file(Bun.argv[2]).writer();
let rows: string[] = [];
for (let cp = 0; cp < 0x110000; ++cp) {
  const unit = String.fromCodePoint(cp);
  for (const context of [0, 1]) {
    const host = context === 0 ? `${unit}.example` : `a${unit}b.example`;
    const url = URL.parse(`http://${host}/p?q#f`);
    if (url) rows.push(`${cp}\t${context}\t${url.href}\n`);
  }
  if (rows.length >= 1024) {
    out.write(rows.join(""));
    rows = [];
  }
}
out.write(rows.join(""));
await out.end();
```

Save this as `$work/scalar.cpp`. The implementation include gives the offline probe access to the same WTF-8 and USVString conversions as the baseline.

```cpp
#include <ada.h>
#include "src/rt/url.cpp"
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const bool old = std::string_view(argv[1]) == "old";
  for (char32_t cp = 0; cp < 0x110000; ++cp) {
    std::string unit;
    uos::rt::appendUtf8(unit, cp);
    for (int context = 0; context < 2; ++context) {
      const std::string host = context == 0 ? unit + ".example" : "a" + unit + "b.example";
      const std::string input = "http://" + host + "/p?q#f";
      if (old) {
        if (auto url = uos::rt::URL::parse(input))
          std::cout << static_cast<std::uint32_t>(cp) << '\t' << context << '\t' << url->href() << '\n';
      } else if (auto url = ada::parse(uos::rt::usvString(input))) {
        std::cout << static_cast<std::uint32_t>(cp) << '\t' << context << '\t' << url->get_href() << '\n';
      }
    }
  }
}
```

```bash
g++-14 -std=c++20 -O2 -c "$work/ada/ada.cpp" -o "$work/ada.o"
g++-14 "${strict[@]}" -isystem "$work/ada" "$work/scalar.cpp" "$work/ada.o" "${libs[@]}" -o "$work/scalar"
isolated "$bun_oracle" --no-env-file --no-install "$work/scalar.ts" "$work/bun-scalar.tsv"
isolated "$work/scalar" old > "$work/old-scalar.tsv"
isolated "$work/scalar" ada > "$work/ada-scalar.tsv"
cmp "$work/bun-scalar.tsv" "$work/old-scalar.tsv"
wc -l "$work/"{bun,old,ada}-scalar.tsv
diff --old-line-format='old %L' --new-line-format='ada %L' --unchanged-line-format='' \
  "$work/bun-scalar.tsv" "$work/ada-scalar.tsv" | cut -d' ' -f1 | sort | uniq -c
```

For the recorded-vector comparison, the temporary probe read the `kUrlVectors`, `kIdnaLimitVectors`, `kSetterVectors`, and `kIdnaVectors` definitions verbatim and compared the corresponding Ada getters with the fixture strings. For the adapter trial, the temporary `url.cpp` copy used `ada::parse` to fill the existing URL record, then used `ada::url_search_params` for form parsing and serialization. Both trial binaries used the same first-party flags as the `-O2` baseline, with `-isystem "$work/ada"` and `$work/ada.o` added. The benchmark built each version with those same compiler settings and ran `isolated taskset -c 2 "$work/bench-old"`, `isolated taskset -c 2 "$work/bench-candidate"`, and `isolated taskset -c 2 "$work/bench-old" direct`, in that order.

The committed delta contains only this report. `bun --no-env-file run format:check cpp/gateway/docs/findings/cppgw-oss-ada-url-20261117.md` ran in a temporary copy with the committed formatter configuration and both frozen dependency installs. `git diff --check claude/cpp-gateway-port..HEAD` passed in the job checkout.
