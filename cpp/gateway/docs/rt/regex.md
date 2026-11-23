# ECMAScript regular expressions

`rt::Regex` uses QuickJS-ng libregexp 0.18.0 for compilation and matching. The C++ adapter keeps `lastIndex`,
capture and replacement behavior, and UTF-16 and byte offsets. Subjects, captures and replacements use WTF-8
in `std::string`. [API](../../include/uos/rt/regex.hpp) · [Text](text.md) ·
[Engine provenance](../../third_party/quickjs-libregexp/PROVENANCE.md)

## TypeScript to C++

| TypeScript                                                             | C++                                                                                                                               |
| ---------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| `const RE = /a(b)/i;` (module level or literal in a function)          | `static const rt::Regex RE("a(b)", "i");` (see "State" for `g`/`y`)                                                               |
| `new RegExp(source, flags)`                                            | `rt::Regex re(source, flags);` throws `rt::SyntaxError` on invalid patterns or flags                                              |
| `re.test(s)`                                                           | `re.test(s)`                                                                                                                      |
| `re.exec(s)` / `s.match(re)` without `g`                               | `re.exec(s)` / `rt::match(s, re)` -> `std::optional<rt::RegexMatch>` (nullopt = `null`)                                           |
| `m[0]`, `m[1]`, `m.index`, `m.groups?.name`, `m.input`                 | `m->str()`, `(*m)[1]` (`std::optional<std::string>`), `m->index`, `m->group("name")`, `m->input`                                  |
| `m.index + m[0].length`                                                | `m->endIndex()` (UTF-16); `m->byteEndIndex()` (bytes)                                                                             |
| `s.match(re)` with `g`                                                 | `rt::matchGlobal(s, re)` -> `std::optional<std::vector<std::string>>`                                                             |
| `[...s.matchAll(re)]`, `for (const m of s.matchAll(re))`               | `for (const rt::RegexMatch& m : rt::matchAll(s, re))`                                                                             |
| `s.replace(re, "x$1")`, `s.replaceAll(re, "x$1")`                      | `rt::replace(s, re, "x$1")`, `rt::replaceAll(s, re, "x$1")`                                                                       |
| `s.replace(re, (match, p1, offset) => ...)`                            | `rt::replace(s, re, [&](const rt::RegexMatch& m) { ... return std::string(...); })`                                               |
| `s.split(re)`, `s.split(re, n)`                                        | `rt::split(s, re[, n])` -> `std::vector<std::string>`; with capture groups `rt::splitWithCaptures` (undefined captures = nullopt) |
| `s.search(re)`                                                         | `rt::search(s, re)` -> `std::int64_t`, `-1` when none                                                                             |
| `re.lastIndex`, `re.source`, `re.flags`, `String(re)`, `re.global` ... | `re.lastIndex`, `re.source()`, `re.flags()`, `re.toString()`, `re.global()` ...                                                   |

```cpp
// const BEARER = /^Bearer\s+(\S[^\n\r\u2028\u2029]*)$/i;  const m = BEARER.exec(value);
static const rt::Regex BEARER("^Bearer\\s+(\\S[^\\n\\r\\u2028\\u2029]*)$", "i");
if (std::optional<rt::RegexMatch> m = BEARER.exec(value)) token = *(*m)[1];

// s.replace(/[^a-z0-9]+/g, (run) => run.length > 1 ? "-" : "_")
static const rt::Regex RUN("[^a-z0-9]+", "g");
std::string out = rt::replace(s, RUN, [](const rt::RegexMatch& m) { return std::string(m.endIndex() - m.index > 1 ? "-" : "_"); });

// const re = /\d+/g; let m; while ((m = re.exec(s)) !== null) { use(m.index); }
rt::Regex re("\\d+", "g");
while (std::optional<rt::RegexMatch> m = re.exec(s)) use(m->index);

// for (const line of raw.split(/\r\n|\r|\n/))
static const rt::Regex LINE_BREAK("\\r\\n|\\r|\\n", "");
for (const std::string& line : rt::split(raw, LINE_BREAK)) ...
```

Escape the pattern for a C++ string literal (double every backslash), or use a raw string
`R"(...)"`. The pattern text is the regex `source`, not a JS string literal: `/\u2028/` is
`"\\u2028"`, which the engine decodes, exactly like `new RegExp("\\u2028")`.

## State: `g`, `y` and `lastIndex`

`lastIndex` is a public member counted in UTF-16 units. Every function has a `Regex&` and a
`const Regex&` overload (`exec`/`test` are members):

- non-const: reads and writes `lastIndex` wherever JS does (`exec`/`test` on `g`/`y`, `replace`,
  `matchGlobal`). Use it for a regex that JS keeps in a variable and uses statefully: an exec loop,
  or a module-level `/x/g` whose `test` result depends on the previous call. Declare those
  `static rt::Regex` (not const) or as a local, as the TS does.
- const: starts from the current `lastIndex` (0 unless set) and never writes it, i.e. a fresh
  literal on every call. This is the right translation for every `static const` regex without
  `g`/`y`, and for `g` regexes that are only passed to `matchAll`, `replace`, `replaceAll`, `split`,
  `search` or `match` (those reset or ignore `lastIndex` in JS).

`matchAll` starts at `re.lastIndex` and leaves it alone, `search` and `split` ignore it, as in JS.
Copies of an `rt::Regex` share the compiled program (immutable, reference counted) and carry their
own `lastIndex`.

## Indices: UTF-16 or bytes

`RegexMatch::index`, `endIndex()`, `RegexSpan::start/end`, `lastIndex` and `search()` are UTF-16
code unit indices, as in JS. `byteIndex`, `byteEndIndex()`, `RegexSpan::byteStart/byteEnd` are
byte offsets into the subject.

- Use the UTF-16 values whenever they leave the regex code as numbers: arithmetic the TS does
  with `match.index`, values passed to `rt::slice`/`rt::substring`/`padStart` and friends, JSON
  output, comparisons with `.length`, and `lastIndex`.
- Use the byte offsets to cut the C++ subject directly (`s.substr(m.byteIndex, m.byteEndIndex() -
m.byteIndex)` equals `m.str()`); never mix them with UTF-16 numbers.
- Without `u` a match can begin or end between the two halves of a surrogate pair (`/\uDE00/`
  on "😀"). The UTF-16 indices are exact; the byte offset there rounds down to the start of the
  four-byte sequence, and the captured text is the lone surrogate as WTF-8 (`ED B8 80`).

`m.input` is a `std::string_view` of the subject: keep the subject alive and unmodified while the
match is used. Captures (`captures`, `group()`, `str()`) are owned copies.

## Replacement callbacks

The callback receives one `const rt::RegexMatch&`, which carries everything JS passes
positionally: `m.str()` = match, `m[1..]` = p1.., `m.index` = offset (UTF-16), `m.input` = string,
`m.group(name)` / `m.groups` = groups. It returns `std::string`. `rt::RegexReplacer` binds any
callable (lambda, function pointer, `std::function`) by reference for the duration of the call.
As in JS, a global replace finds all matches first and then calls the callback for each, in
order; the callback may use the same regex (it sees and moves `lastIndex` as JS would). Template
replacements support `$$ $& $` $' $n $nn $<name>` with JSC's rules (`$0`, `$00`, out-of-range
`$n` stay literal, `$10`with fewer groups is`$1`followed by`0`).

Output is canonical WTF-8: lone surrogates that end up adjacent (removing `x` from `"\uD83Dx\uDE00"`)
join into one four-byte sequence, exactly like the JS string.

## Syntax and errors

- Flags `d g i m s u v y` are supported. `u` and `v` are mutually exclusive. The `v` flag enables Unicode set operations
  and properties of strings. For example, `[a--b]` with `v` matches `a`. Stored regular expressions retain their source
  and flags through both typed-value codecs.
- Invalid patterns or flags raise `rt::SyntaxError`. Pattern diagnostics come from libregexp.
- `matchAll` and `replaceAll` raise `rt::TypeError` for a non-global regex. The C++ API raises `std::logic_error` when
  a caller passes a global regex to `rt::match`, passes a non-global regex to `rt::matchGlobal`, or calls `rt::split`
  on a pattern with capture groups. Use `rt::splitWithCaptures` for that result shape.
- Patterns above 4 KiB and parser stack exhaustion raise `rt::RangeError`. The 255th explicit capture group raises
  `rt::SyntaxError`. The parser stack guard allows at most 128 KiB below its entry frame and leaves 32 KiB above the
  thread stack base. The engine allocator caps individual requests at 72 MiB.
- Each matching operation has a subject-sized budget of interrupt callbacks. Backstop expiry and an exhausted
  work budget have distinct `rt::RangeError` messages. [Input limits](../SECURITY-LIMITS.md#regular-expressions) · [Observed syntax differences and limits](../DEVIATIONS-FROM-BUN.md#regular-expressions-with-quickjs-ng-libregexp-0180)

## Performance

Compile literals once in `static const` objects, or `static` where the caller needs mutable `lastIndex`. ASCII
subjects are matched in place. A non-ASCII subject is converted to UTF-16 once per operation. On a long non-ASCII
subject, `matchAll`, `replace`, `split` and `matchGlobal` can scan multiple matches with one conversion.

## Appendix: pattern inventory

Every regex literal in `src/`, `serve.ts` and `lib/jev_compaction/` (156 distinct patterns at
261 sites), with the methods that consume it. All are covered by the `inventory` vectors in
`tests/rt/regex_vectors.inc` (exec, test, search, match/matchAll, replace with template and
callback, replaceAll, split, split limits, lastIndex sweeps, recorded from Bun), as are the 570
distinct literals in `tests/` (`tests` vectors). Features used: `g` 30, `i` 26, `u` 10, word
boundaries 11, lookahead 7, backreferences 5 (all `(?=(\w*))\1`), `\p{..}` 1, lazy quantifier 1.

Dynamic `RegExp` constructions, also in the vectors with their concrete patterns:

- `src/auth/relay.ts:16,24`: `^${trustedApp}-[a-z0-9]{12}$` and `^ai-ubq-fi-[a-z0-9]{12}\.deno\.dev$` (test).
- `src/models/codex-models-fetch.ts:461,462`: `^resp_[A-Za-z0-9]{1,40}_(?:rs|msg|fc|ctc)(?:_\d+){1,2}$`
  and `^(?:rs|msg|fc|ctc)_resp_[A-Za-z0-9]{1,40}(?:_\d+){1,2}$` (test).
- `src/harmony/tools/backend.ts:147`: `patternToRegExp(glob)`, a `^...$` built from `[^/]*`,
  `.*`, `(?:.*/)?`, `[^/]` and escaped literals (test).
- `src/kv-conversion/typed-value.ts:133`, `src/kv-conversion/profile-json.ts:100`: stored RegExp
  values rebuilt from `source` and `flags`. The round trip includes `v` and preserves the typed graph bytes.

Literals (pattern, methods, sites):

- `/encrypt|ciphertext|opaque|blob|signature/i` test (lib/jev_compaction/codex_items.ts:135)
- `/^(call|result)_/` replace (lib/jev_compaction/jev-codex-smoke.ts:448)
- `/calls_dropped=3/` test (lib/jev_compaction/jev-compaction-smoke.ts:326)
- `/event: response.completed\n/g` match (lib/jev_compaction/jev-compaction-smoke.ts:414)
- `/[A-Za-z]+|\d+|[^\sA-Za-z\d]/g` matchAll (lib/jev_compaction/state.ts:18)
- `/\s+/g` replace (lib/jev_compaction/state.ts:104; src/codex/supervisor-brief.ts:599; src/kernel/attestation.ts:59; src/provider/claude-code-quota.ts:250; src/provider/lithos-rate-limits.ts:295; src/utils.ts:6,29)
- `/\s/` test (src/admin/api-keys.ts:105,154; src/auth/index.ts:451; src/codex/supervisor-secret.ts:39; src/metered-quota.ts:167)
- `/^\d+$/` test (src/admin/api-keys.ts:409; src/admin/sentinel-incident.ts:15; src/authority/credential-publication.ts:62; src/authority/native-api-key-audit.ts:157; src/authority/native-codex-429.ts:47; src/provider/lithos-rate-limits.ts:224,272)
- `/[\n,]/g` split (src/admin/chart-source-proxy.ts:61; src/config.ts:22)
- `/^[a-f0-9-]{36}$/` test (src/admin/claude-code.ts:103; src/provider/claude-code-accounts.ts:50)
- `/^[a-f0-9]{64}$/` test (src/admin/codex-reset.ts:8; src/authority/native-codex-contracts.ts:548; src/cache/kv-migration-validation.ts:206; src/codex/auth.ts:116; src/codex/manual-reset-state.ts:23,25; src/kv-conversion/legacy-codec.ts:18; src/provider/capacity-contract.ts:32; src/provider/claude-code-accounts.ts:56; src/provider/selection.ts:46)
- `/\r?\n/` split (src/admin/defaults.ts:44; src/provider/openrouter-streams.ts:31)
- `/^[A-Za-z0-9_-]+={0,2}$/` test (src/admin/sentinel-incident.ts:11; src/kv-conversion/cursors.ts:10; src/sentinel/incident-outbox.ts:254; src/sentinel/replay-admin.ts:18; src/sentinel/replay-model.ts:52)
- `/[\r\n]/` test (src/agent-messages.ts:49,60; src/authority/native-api-key-policy.ts:51)
- `/^[0-9a-fA-F]{64}$/` test (src/auth/index.ts:179)
- `/<title[^>]*>([^<]*)<\/title>/i` exec (src/auth/index.ts:498)
- `/[^a-z0-9._-]+/g` replace (src/auth/passkeys.ts:95)
- `/-{2,}/g` replace (src/auth/passkeys.ts:96)
- `/:$/` replace (src/auth/passkeys.ts:191)
- `/^uos_ai_passport_session_[A-Za-z0-9_-]{43}$/` test (src/auth/passport-sessions.ts:6)
- `/^uos_app_session_[A-Za-z0-9_-]{43}$/` test (src/auth/passport-sessions.ts:7)
- `/^[A-Za-z0-9_-]+$/` test (src/auth/passport.ts:189; src/sentinel/replay-model.ts:51; src/sentinel/replay-read.ts:43,108)
- `/^[A-Za-z0-9_-]{43}\.(?:admin|chat)$/` test (src/auth/passport.ts:301)
- `/^[A-Za-z0-9_-]{43}$/` test (src/auth/passport.ts:304; src/cache/kv-migration-paid-fallback.ts:26)
- `/^u_[0-9a-f]{64}$/` test (src/authority/native-api-key-policy.ts:44)
- `/^([A-Za-z]{3}), (\d{2}) ([A-Za-z]{3}) (\d{4}) (\d{2}:\d{2}:\d{2}) GMT$/` exec (src/authority/native-codex-429.ts:28)
- `/[\t\n\r ]/` test (src/authority/native-codex-429.ts:81)
- `/^(?:-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?)/` exec (src/authority/native-codex-429.ts:111)
- `/[^a-z0-9]/g` replace (src/authority/native-codex-capacity.ts:266)
- `/^u_[0-9a-f]{10}$/` test (src/cache/kv-migration-paid-fallback.ts:19)
- `/^\d{4}-\d{2}-\d{2}$/` test (src/cache/kv-migration-validation.ts:208; src/kernel/usage.ts:61)
- `/^[a-f0-9]{7,64}$/i` test (src/cache/prompt-analytics-core.ts:28; src/cache/telemetry-gate.ts:121)
- `/^(?:unknown|[a-f0-9]{64})$/` test (src/cache/prompt-analytics-queue.ts:60; src/cache/prompt-analytics-read.ts:71)
- `/-/g` replace (src/chat/completions.ts:57,535; src/chat/stream-translation.ts:233,246; src/codex/auth.ts:205; src/codex/external-auth.ts:11; src/embeddings/jobs.ts:453; src/kernel/attestation.ts:103; src/responses-failover-stream.ts:369,818; src/upstream-wire.ts:359; src/utils.ts:30)
- `/refresh token.{0,80}(already|previously) used|token.{0,40}reused/` test (src/codex/auth-refresh.ts:79)
- `/refresh token.{0,80}(expired|invalid|revoked)|authorization grant.{0,80}(expired|invalid|revoked)/` test (src/codex/auth-refresh.ts:83)
- `/_/g` replace (src/codex/auth.ts:205; src/codex/external-auth.ts:11; src/kernel/attestation.ts:103; src/utils.ts:30)
- `/^The '([^']+)' model is not supported when using Codex with a ChatGPT account\.$/` exec (src/codex/dispatch.ts:607)
- `/\.(\d{1,9})(?:Z|[+-]\d{2}:\d{2})$/` exec (src/codex/native-auth.ts:139)
- `/^```(?:json)?/i` replace (src/codex/supervisor-brief.ts:608)
- `/```$/` replace (src/codex/supervisor-brief.ts:609)
- `/^[a-z0-9][a-z0-9_-]{0,31}$/` test (src/codex/supervisor-config.ts:10)
- `/^[A-Za-z0-9._:-]{1,200}$/` test (src/codex/supervisor-config.ts:11)
- `/^state_(\d+)\.sqlite$/` exec (src/codex/supervisor-config.ts:23)
- `/approval/i` test (src/codex/supervisor-inventory.ts:429)
- `/user.?input|input.?request|needsinput/i` test (src/codex/supervisor-inventory.ts:430)
- `/-----BEGIN [A-Z ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z ]*PRIVATE KEY-----/g` matchAll (src/codex/supervisor-secret.ts:14)
- `/\b[Bb]earer\s+[A-Za-z0-9._~+/=-]{12,}/g` matchAll (src/codex/supervisor-secret.ts:19)
- `/\b(?:sk|ghp|gho|ghu|ghs|dsk)-[A-Za-z0-9_-]{12,}/g` matchAll (src/codex/supervisor-secret.ts:20)
- `/(?:ghp|gho|ghu|ghs)_[A-Za-z0-9]{12,}/g` matchAll (src/codex/supervisor-secret.ts:21)
- `/github_pat_\w{12,}/g` matchAll (src/codex/supervisor-secret.ts:22)
- `/\bu_[0-9a-fA-F]{32,}\b/g` matchAll (src/codex/supervisor-secret.ts:23)
- `/\b(?=\w*(?:key|token|secret|password))(?=(\w*))\1(?:[.-]\w*){0,16}(["']?)\s*(?=[:=])/gi` matchAll (src/codex/supervisor-secret.ts:34)
- `/[\s,\]};]/` test (src/codex/supervisor-secret.ts:40)
- `/[\p{L}\p{N}_]/u` test (src/codex/supervisor-secret.ts:109)
- `/^(?:[bfrtulnexqs]|br|rb|fr|rf|tr|rt|u8|qq)$/i` test (src/codex/supervisor-secret.ts:112)
- `/^([A-Za-z_$][\w$]*)((?:\.[A-Za-z_$][\w$]*(?:\(\))?)+)(?=\s*[,);])/` exec (src/codex/supervisor-secret.ts:271)
- `/^[A-Za-z_$][\w$]*$/` test (src/codex/supervisor-secret.ts:272)
- `/^(?:const|let|var)\s+[A-Za-z_$][\w$]*\s*=\s*/` replace (src/codex/supervisor-secret.ts:273)
- `/^([^(]+)\(([^()]*)\)$/` exec (src/codex/supervisor-secret.ts:274)
- `/^\s*[,);]/` test (src/codex/supervisor-secret.ts:286)
- `/\b(?=\w*(?:key|token|secret|password))(?=(\w*))\1["']?\s*[:=]\s*"(?:\\.|[^"\\])*"/gi` matchAll (src/codex/supervisor-secret.ts:493)
- `/\b(?=\w*(?:key|token|secret|password))(?=(\w*))\1\s*[:=]\s*\S{8,}/gi` matchAll (src/codex/supervisor-secret.ts:494)
- `/\b(?=\w*(?:key|token|secret|password))(?=(\w*))\1["']?\s*[:=]\s*'/gi` matchAll (src/codex/supervisor-secret.ts:495)
- `/\b(?=\w*(?:key|token|secret|password))(?=(\w*))\1["']?\s*[:=]\s*[[{]/gi` matchAll (src/codex/supervisor-secret.ts:496)
- `/^~?[^/\s]+\/[^/\s]+$/u` test (src/decisions/handlers.ts:38)
- `/^deepseek-v(\d+(?:\.\d+)?)-flash(?:-(\d{4}|\d{8}))?$/` exec (src/deepseek/flash-aliases.ts:48)
- `/(?:^|[-:])(?:fast|ultra|chat|vision|exp|web|e2ee|free|batch)(?:$|[-:])/` test (src/deepseek/flash-aliases.ts:49)
- `/(?:^|[/-])deepseek(?:-v[\d.]+)?-flash-latest$/` test (src/deepseek/flash-aliases.ts:50)
- `/-\d{4,8}$/` replace (src/deepseek/flash-aliases.ts:72)
- `/^~\w+\/deepseek-flash-latest$/i` test (src/deepseek/flash-aliases.ts:93,94)
- `/^deepseek-v\d+(?:\.\d+)?-flash(?:-\d{4}|\d{8})?$/` test (src/deepseek/flash-aliases.ts:172)
- `/^[A-Za-z0-9_.:-]{1,64}$/` test (src/deepseek/handlers.ts:187; src/provider/lithos-streams.ts:39; src/provider/openrouter-streams.ts:93)
- `/[^A-Za-z0-9]/g` replace (src/deepseek/handlers.ts:699; src/provider/anthropic-chat-responses.ts:22; src/provider/anthropic-responses.ts:26; src/provider/cerebras-responses.ts:372; src/provider/lithos-handlers.ts:915,948,966)
- `/\r\n\r\n|\n\n|\r\r/` exec (src/deepseek/index.ts:180; src/provider/anthropic-chat-stream.ts:37; src/provider/anthropic-responses.ts:452; src/provider/lithos.ts:823)
- `/\r\n|\r|\n/` split (src/deepseek/index.ts:181; src/provider/anthropic-chat-stream.ts:10; src/provider/anthropic-responses.ts:403; src/provider/lithos.ts:824; src/responses-stream.ts:51)
- `/^[\x21-\x7e]+$/` test (src/deepseek/index.ts:247; src/provider/cerebras.ts:101; src/provider/lithos.ts:194)
- `/^ /` replace (src/deepseek/stream.ts:44; src/provider/anthropic-chat-stream.ts:12; src/provider/anthropic-responses.ts:405; src/provider/lithos.ts:845; src/provider/openrouter-streams.ts:138; src/responses-stream.ts:54; src/sentinel/replay-observation.ts:410)
- `/^\/admin\/api-keys\/([^/]+)\/paid-fallbacks$/` exec (src/handler/index.ts:258)
- `/^\/admin\/providers\/codex\/(\d+)\/recheck$/` exec (src/handler/index.ts:280)
- `/^(true|false)$/i` test (src/harmony/classifier.ts:23)
- `/^\s*([^\s<]+)/` exec (src/harmony/parse.ts:32)
- `/(?:^|\s)to=([^\s<]+)/` exec (src/harmony/parse.ts:37)
- `/[.+^${}()|[\]\\]/g` replace (src/harmony/tools/backend.ts:142)
- `/^!/` replace (src/harmony/tools/fakes.ts:142)
- `/^Bearer\s+(\S[^\n\r\u2028\u2029]*)$/i` exec (src/http.ts:146)
- `/^data:/iu` test (src/images.ts:186)
- `/^https?:\/\//iu` test (src/images.ts:187)
- `/^data:([^;,]+);base64,([a-z0-9+/]*={0,2})$/iu` exec (src/images.ts:212)
- `/^(?:0|[1-9]\d*)$/u` test (src/images.ts:256,385; src/live/calls.ts:48)
- `/^-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?$/u` test (src/images.ts:262)
- `/Jev request failed \((\d{3})\)/` exec (src/jev_compaction/compaction.ts:179)
- `/dropped=(\d+), before=(\d+), after=(\d+)/` exec (src/jev_compaction/compaction.ts:188)
- `/summary is (\d+) chars/` exec (src/jev_compaction/compaction.ts:190)
- `/unpinned_kept=(\d+) pinned=(\d+) results_dropped=(\d+)/` exec (src/jev_compaction/compaction.ts:192)
- `/\b\d{4}-\d{2}-\d{2}(?:[T ]\d{2}:\d{2}(?::\d{2})?)?\b/g` matchAll (src/jev_compaction/hybrid.ts:61)
- `/\b[0-9a-f]{8,64}\b/g` matchAll (src/jev_compaction/hybrid.ts:62)
- `/[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}/g` matchAll (src/jev_compaction/hybrid.ts:63)
- `/\b[\w-]+\.(?:json|md|ts|sqlite3|sh|yml|yaml|service)\b/g` matchAll (src/jev_compaction/hybrid.ts:64)
- `/[A-Za-z0-9_.@-]/` test (src/jev_compaction/hybrid.ts:81)
- `/\s+/` split (src/jev_compaction/hybrid.ts:98)
- `/[0-9a-f]{8,}/` test (src/jev_compaction/hybrid.ts:100)
- `/[a-z-]/` test (src/jev_compaction/hybrid.ts:100)
- `/\n(?=\[)/` split (src/jev_compaction/hybrid.ts:160)
- `/\\n/g` replace (src/kernel/attestation.ts:15)
- `/^(?:[a-f0-9]{2})+$/` test (src/kv-conversion/cursors.ts:18)
- `/^(\d+)\|(.*)$/` exec (src/kv-conversion/cursors.ts:35)
- `/Error.cause/` test (src/kv-conversion/proof.ts:143)
- `/Negative NaN/` test (src/kv-conversion/proof.ts:144)
- `/boundary=/iu` test (src/live/calls.ts:91)
- `/^\/v1\/live\/([^/]+)$/` exec (src/live/routes.ts:16)
- `/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/iu` test (src/live/upstream.ts:98)
- `/^(\d+)\.(\d+)\.(\d+)$/` exec (src/models/codex-models.ts:505)
- `/[^a-z0-9]+/g` replace (src/models/openrouter-models.ts:160)
- `/^-/` replace (src/models/openrouter-models.ts:161)
- `/-$/` replace (src/models/openrouter-models.ts:162)
- `/[-:]latest$/` replace (src/models/openrouter-models.ts:174)
- `/[-:](?:free|batch)$/` replace (src/models/openrouter-models.ts:195)
- `/^(?:gateway|provider|upstream|server|network|timeout|deadline)[A-Z0-9_.:-]*$/i` test (src/openai-telemetry.ts:581)
- `/^(?:rs|fc|msg|ctc)_[A-Za-z0-9_-]{1,240}$/` test (src/provider/anthropic-replay.ts:53)
- `/^(?:rs|fc|msg|ctc)_resp_[A-Za-z0-9_-]+_\d+$/` test (src/provider/anthropic-replay.ts:54)
- `/^[a-z0-9][a-z0-9-]{0,31}$/` test (src/provider/capacity-analytics.ts:56)
- `/[\s,]/` test (src/provider/claude-code-access.ts:16)
- `/^sk-ant-o(at|rt)/i` test (src/provider/claude-code-credentials.ts:39)
- `/user:profile|(?:missing|required|insufficient).*scope|scope.*(?:missing|required|insufficient)/i` test (src/provider/claude-code-usage.ts:97)
- `/[\s,]+/` split (src/provider/claude-code.ts:96)
- `/^\/models\//` replace (src/provider/lithos.ts:440)
- `/^~?anthropic\/claude-/` test (src/provider/openrouter-handlers.ts:106; src/provider/openrouter-request-controls.ts:12)
- `/\r?\n\r?\n/` exec (src/provider/openrouter-streams.ts:30)
- `/^~?openai\//` test (src/provider/openrouter-web-tools.ts:13)
- `/^[^/\s]+\/[^/\s]+$/` test (src/provider/openrouter.ts:80)
- `/^[A-Za-z0-9_-]{1,64}$/` test (src/provider/surplus-claude-chat-request.ts:23; src/provider/surplus-claude-request.ts:31)
- `/^https?:\/\//` test (src/provider/surplus-claude-chat-request.ts:47; src/provider/surplus-claude-request.ts:61)
- `/^data:image\/(?:jpeg|png|gif|webp);base64,/` test (src/provider/surplus-claude-chat-request.ts:47)
- `/^(?:~?anthropic\/)?claude-/` test (src/provider/surplus-claude-request.ts:33)
- `/^data:(image\/(?:jpeg|png|gif|webp));base64,([A-Za-z0-9+/=\s]+)$/` exec (src/provider/surplus-claude-request.ts:59)
- `/[+,]/` split (src/provider/surplus.ts:148)
- `/^(?:0|[1-9]\d*)$/` test (src/request.ts:57; src/sentinel/replay-observation.ts:340)
- `/^text\/event-stream(?:\s*;|$)/i` test (src/responses-native-headers.ts:56; src/runtime/http.ts:114,178)
- `/^[0-9a-f]{64}$/` test (src/runtime/native-kv-bootstrap.ts:55; src/runtime/permissions-linux.ts:46; src/sentinel/incident-outbox.ts:67,189,307,339,577,578,579; src/sentinel/replay-model.ts:50; src/sentinel/replay-read.ts:221; src/sentinel/replay-retention-schema.ts:58; src/storage/native-addon.ts:53,66,82)
- `/^provider-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/` test (src/sentinel/incident-outbox.ts:8)
- `/^[0-9a-f]{40}$/` test (src/sentinel/incident-outbox.ts:9; src/sentinel/replay-store.ts:122,389)
- `/^capture:[A-Za-z0-9_-]{1,128}$/` test (src/sentinel/incident-outbox.ts:255)
- `/^[A-Za-z0-9_.:-]{1,128}$/` test (src/sentinel/incident-outbox.ts:312; src/sentinel/replay-observation.ts:41,200)
- `/^[A-Za-z0-9_.:-]{1,32}$/` test (src/sentinel/incident-outbox.ts:313)
- `/^[A-Za-z0-9_-]{43}=?$/` test (src/sentinel/replay-envelope.ts:40)
- `/^[A-Za-z0-9._:-]{1,128}$/` test (src/sentinel/replay-observation.ts:33)
- `/^[A-Za-z0-9_.:[\]-]{1,128}$/` test (src/sentinel/replay-observation.ts:203)
- `/^(?:response_incomplete:)?(?:gateway|provider|upstream|server|network|timeout|deadline)[a-z0-9_.:-]*$/i` test (src/sentinel/replay-observation.ts:679)
- `/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/` test (src/sentinel/upstream-capture.ts:110)
- `/^https?:\/\//i` test (src/storage/kv.ts:78)
- `/^Failed to perform '(sum|min|max)' mutation on a non-U64 value in the database$/` test (src/storage/native-kv.ts:24)
- `/^~?typesafe\/[a-z0-9][a-z0-9._-]{0,63}$/u` test (src/systemone/handlers.ts:39)
- `/\+/g` replace (src/utils.ts:23)
- `/\//g` replace (src/utils.ts:24)
- `/={1,2}$/` replace (src/utils.ts:25)
