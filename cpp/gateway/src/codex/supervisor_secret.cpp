#include "uos/codex/supervisor_secret.hpp"

#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace uos::codex::supervisor_secret {
  namespace {
    // A JS string as the scans read it: canonical WTF-8 for the regular
    // expressions, and its UTF-16 code units for `text[index]` in constant time.
    struct Text {
      std::string value;
      std::u16string units;

      explicit Text(std::string_view source) : value(source), units(rt::toUtf16(source)) {}
      explicit Text(std::u16string codeUnits) : value(rt::fromUtf16(codeUnits)), units(std::move(codeUnits)) {}
      // `text.length`
      std::int64_t length() const noexcept { return static_cast<std::int64_t>(units.size()); }
      // `text[index]`: the code unit, or -1 where JS reads undefined.
      std::int32_t operator[](std::int64_t index) const noexcept { return index >= 0 && index < length() ? units[static_cast<std::size_t>(index)] : -1; }
    };

    // The code units of `text.slice(start, end)`.
    std::u16string_view sliceUnits(const Text& text, std::int64_t start, std::int64_t end) {
      const std::int64_t length = text.length();
      const auto relative = [length](std::int64_t position) { return position < 0 ? std::max<std::int64_t>(length + position, 0) : std::min(position, length); };
      const std::int64_t from = relative(start);
      const std::int64_t to = relative(end);
      if (from >= to) return {};
      return std::u16string_view(text.units).substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
    }

    // `text.slice(start, end)`
    std::string slice(const Text& text, std::int64_t start, std::int64_t end) { return rt::fromUtf16(sliceUnits(text, start, end)); }

    // `text.startsWith(search, position)`
    bool startsWith(const Text& text, std::u16string_view search, std::int64_t position) {
      const std::int64_t start = std::clamp<std::int64_t>(position, 0, text.length());
      return std::u16string_view(text.units).substr(static_cast<std::size_t>(start)).starts_with(search);
    }

    // A one-code-unit JS string, `text[index]` for an index in range.
    std::string character(const Text& text, std::int64_t index) { return rt::fromCharCode(static_cast<std::uint16_t>(text[index])); }

    // `set.has(character)` / `set.includes(character)` for a set of single characters.
    bool has(std::u16string_view set, std::int32_t unit) noexcept { return unit >= 0 && set.find(static_cast<char16_t>(unit)) != std::u16string_view::npos; }

    struct RedactionSpan {
      std::int64_t start = 0;
      std::int64_t end = 0;
    };

    constexpr std::u16string_view MARKER = u"[redacted]";

    const rt::Regex PRIVATE_KEY_PATTERN(R"(-----BEGIN [A-Z ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z ]*PRIVATE KEY-----)", "g");

    /** Credential shapes recognized without a key. */
    const std::vector<rt::Regex> TOKEN_PATTERNS = {
      PRIVATE_KEY_PATTERN,
      rt::Regex(R"(\b[Bb]earer\s+[A-Za-z0-9._~+/=-]{12,})", "g"),
      rt::Regex(R"(\b(?:sk|ghp|gho|ghu|ghs|dsk)-[A-Za-z0-9_-]{12,})", "g"),
      rt::Regex(R"((?:ghp|gho|ghu|ghs)_[A-Za-z0-9]{12,})", "g"),
      rt::Regex(R"(github_pat_\w{12,})", "g"),
      rt::Regex(R"(\bu_[0-9a-fA-F]{32,}\b)", "g"),
    };

    /**
     * A credential key, up to the `:`, `=` or `=>` that assigns its value. The key
     * is a whole word holding a credential word, and it may run on through up to
     * sixteen `.` or `-` segments (`tokens-v2`, `secret.values`). It may close a
     * quote (`"api_keys"`, PHP `'password'`), which the second group captures.
     * The word is taken whole, without backtracking, and the segments are
     * bounded, so a long run of word characters, dots or dashes costs linear time.
     */
    const rt::Regex CREDENTIAL_KEY(R"re(\b(?=\w*(?:key|token|secret|password))(?=(\w*))\1(?:[.-]\w*){0,16}(["']?)\s*(?=[:=]))re", "gi");

    /** The shortest unquoted value the fallback rule treats as token-like. */
    constexpr std::int64_t MIN_BARE_VALUE = 8;

    // `/\s/.test(character)` for one code unit (-1 is undefined).
    bool WHITESPACE(std::int32_t unit) noexcept { return unit >= 0 && rt::isJsWhitespace(static_cast<std::uint32_t>(unit)); }
    // `/[\s,\]};]/.test(character)` for one code unit.
    bool QUOTED_BARE_END(std::int32_t unit) noexcept { return WHITESPACE(unit) || has(u",]};", unit); }

    /** End of a quoted value where a backslash escapes the next character. */
    std::int64_t quotedEnd(const Text& text, std::int64_t openQuote, bool closedOnly = false) {
      for (std::int64_t index = openQuote + 1; index < text.length(); index += 1) {
        if (text[index] == u'\\') index += 1;
        else if (text[index] == text[openQuote]) return index + 1;
      }
      // Only a per-record reading may redact an unclosed value through its tail.
      return closedOnly ? -1 : text.length();
    }

    /** End of the scalar read as YAML: `''` is an escaped quote and a backslash is content. */
    std::int64_t yamlScalarEnd(const Text& text, std::int64_t openQuote, bool closedOnly = false) {
      for (std::int64_t index = openQuote + 1; index < text.length(); index += 1) {
        if (text[index] != u'\'') continue;
        if (text[index + 1] == u'\'') {
          index += 1;
          continue;
        }
        return index + 1;
      }
      // Only a per-record reading may redact an unclosed scalar through its tail.
      return closedOnly ? -1 : text.length();
    }

    /** End of the scalar read with the pre-existing rule that `\x` escapes the next character. */
    std::int64_t escapedScalarEnd(const Text& text, std::int64_t openQuote, bool closedOnly = false) {
      for (std::int64_t index = openQuote + 1; index < text.length(); index += 1) {
        const std::int32_t unit = text[index];
        if (unit == u'\\') {
          index += 1;
          continue;
        }
        if (unit != u'\'') continue;
        if (text[index + 1] == u'\'') {
          index += 1;
          continue;
        }
        return index + 1;
      }
      return closedOnly ? -1 : text.length();
    }

    /**
     * How one reading treats a quote inside a bracketed value. A JSON, Python or
     * JavaScript string escapes with a backslash, and JavaScript also quotes with
     * a backtick. YAML reads a backslash in a single-quoted string as content,
     * and in a flow collection it opens a string only where a scalar starts, so
     * the apostrophe in a plain scalar such as `it's` is content. Every reading's
     * spans are kept, so a reading that misreads a quote can only widen a
     * redaction, never narrow it.
     */
    struct QuoteReading {
      bool yamlSingleQuotes = false;
      bool flowScalars = false;
      bool templateLiterals = false;
    };

    constexpr std::array<QuoteReading, 4> QUOTE_READINGS = {{
      // Keep the JSON/Python reading: a backtick is plain text, as before.
      {.yamlSingleQuotes = false, .flowScalars = false, .templateLiterals = false},
      // YAML quoting, with every quote opening a string.
      {.yamlSingleQuotes = true, .flowScalars = false, .templateLiterals = false},
      // YAML flow: a quote opens a string only where a scalar starts.
      {.yamlSingleQuotes = true, .flowScalars = true, .templateLiterals = false},
      // JavaScript adds template literals without narrowing any existing reading.
      {.yamlSingleQuotes = false, .flowScalars = false, .templateLiterals = true},
    }};

    /** A character after which a YAML flow scalar, and so a quoted string, can start. */
    constexpr std::u16string_view SCALAR_START = u"[{,:";

    const rt::Regex WORD_CHARACTER(R"([\p{L}\p{N}_])", "u");

    /** Python/C/SQL prefixes plus Perl, Ruby and Elixir forms such as `q'…'`, `%q'…'` and `~s'…'`. */
    const rt::Regex STRING_PREFIX(R"(^(?:[bfrtulnexqs]|br|rb|fr|rf|tr|rt|u8|qq)$)", "i");

    /**
     * Whether an apostrophe opens a string: always, unless it follows a word
     * character. There it never does in JSON, JavaScript or YAML, and in Python,
     * C, SQL, Perl, Ruby or Elixir only after a string prefix. Prose and comments write one inside a
     * word (`it's`, `don't`, `users'`), and reading it as a string would carry
     * the value on to the end of the text.
     */
    bool apostropheOpensString(const Text& text, std::int64_t index) {
      std::int64_t wordStart = index;
      while (wordStart > 0 && index - wordStart < 3 && WORD_CHARACTER.test(character(text, wordStart - 1))) wordStart -= 1;
      if (wordStart == index) return true;
      if (wordStart > 0 && WORD_CHARACTER.test(character(text, wordStart - 1))) return false;
      return STRING_PREFIX.test(slice(text, wordStart, index));
    }

    /** Whether the quote character at `index` opens a string in this reading. */
    bool opensString(const Text& text, std::int64_t index, const QuoteReading& reading, bool scalarStart) {
      const std::int32_t unit = text[index];
      if (unit == u'`') return reading.templateLiterals;
      if (unit != u'"' && unit != u'\'') return false;
      if (reading.flowScalars) return scalarStart;
      return unit == u'"' || apostropheOpensString(text, index);
    }

    /** Where a credential assignment starts and where its bracketed value opens. */
    struct ValueOpening {
      std::int64_t start = 0;
      std::int64_t bracket = 0;
    };

    /** One scan through a bracketed value: its depth, and the string it is inside, if any. */
    struct ValueScan {
      std::int64_t start = 0;
      std::int64_t depth = 0;
      // The quote character of the string the scan is inside; -1 is "" (none).
      std::int32_t quote = -1;
      bool escaped = false;
    };

    /** Advance a scan over the character at `index`, read one way. */
    void advanceScan(ValueScan& scan, const Text& text, std::int64_t index, const QuoteReading& reading, bool scalarStart) {
      const std::int32_t unit = text[index];
      if (scan.escaped) {
        scan.escaped = false;
      } else if (scan.quote == -1) {
        if (opensString(text, index, reading, scalarStart)) scan.quote = unit;
        else if (unit == u'[' || unit == u'{') scan.depth += 1;
        else if (unit == u']' || unit == u'}') scan.depth -= 1;
      } else if (unit == u'\\') {
        scan.escaped = scan.quote != u'\'' || !reading.yamlSingleQuotes;
      } else if (unit == scan.quote) {
        // A doubled quote inside a single-quoted string is an escaped quote.
        if (unit == u'\'' && text[index + 1] == u'\'') scan.escaped = true;
        else scan.quote = -1;
      }
    }

    /** The first of the `count` scans that is in the same string state as `scan`, if any. */
    ValueScan* twinScan(std::vector<ValueScan>& scans, std::size_t count, const ValueScan& scan) {
      for (std::size_t at = 0; at < count; at += 1) {
        if (scans[at].quote == scan.quote && scans[at].escaped == scan.escaped) return &scans[at];
      }
      return nullptr;
    }

    /**
     * Record each scan whose value closed just before `end`, and merge the rest in
     * place. Two scans in the same string state read every later character alike,
     * so the one with the greater depth ends no earlier; one scan with the earlier
     * start and the greater depth then covers exactly the union of their values.
     */
    void settleScans(std::vector<ValueScan>& scans, std::int64_t end, std::vector<RedactionSpan>& values) {
      std::size_t kept = 0;
      for (std::size_t position = 0; position < scans.size(); position += 1) {
        const ValueScan scan = scans[position];
        if (scan.depth == 0) {
          values.push_back({scan.start, end});
          continue;
        }
        ValueScan* twin = twinScan(scans, kept, scan);
        if (twin) {
          twin->start = std::min(twin->start, scan.start);
          twin->depth = std::max(twin->depth, scan.depth);
        } else {
          scans[kept] = scan;
          kept += 1;
        }
      }
      scans.resize(kept);
    }

    /**
     * Every bracketed value read one way: from each opening bracket through the
     * bracket that closes it, with any bracket inside a string skipped, or through
     * the end of the text when the value was clipped. Each opening gets its own
     * scan, so a value this reading ends inside a nested credential cannot hide
     * that credential. Merged scans leave at most one per string state, which keeps
     * the scan linear.
     */
    std::vector<RedactionSpan> bracketedValues(const Text& text, const std::vector<ValueOpening>& openings, const QuoteReading& reading) {
      std::vector<RedactionSpan> values;
      std::vector<ValueScan> scans;
      std::size_t next = 0;
      // The last non-space character read; -1 is "".
      std::int32_t previous = -1;
      std::int64_t index = 0;
      while (index < text.length()) {
        if (scans.empty()) {
          if (next == openings.size()) break;
          index = openings[next].bracket;
        }
        if (next < openings.size() && openings[next].bracket == index) {
          scans.push_back({.start = openings[next].start, .depth = 0, .quote = -1, .escaped = false});
          next += 1;
        }
        const bool scalarStart = has(SCALAR_START, previous);
        for (ValueScan& scan : scans) advanceScan(scan, text, index, reading, scalarStart);
        // YAML separates tokens with spaces, tabs and line breaks.
        if (text[index] > u' ') previous = text[index];
        index += 1;
        settleScans(scans, index, values);
      }
      for (const ValueScan& scan : scans) values.push_back({scan.start, text.length()});
      return values;
    }

    /** The non-whitespace run starting at `from`, remembered so that keys sharing one run read it once. */
    struct RunCache {
      std::int64_t start = 0;
      std::int64_t end = 0;
    };

    std::int64_t runEnd(const Text& text, std::int64_t from, RunCache& cache, bool (*stop)(std::int32_t) = WHITESPACE) {
      if (from >= cache.start && from < cache.end) return cache.end;
      std::int64_t end = from;
      while (end < text.length() && !stop(text[end])) end += 1;
      cache.start = from;
      cache.end = end;
      return end;
    }

    std::int64_t skipWhitespace(const Text& text, std::int64_t from) {
      std::int64_t index = from;
      while (index < text.length() && WHITESPACE(text[index])) index += 1;
      return index;
    }

    /** A credential key: where it starts, where its separator is, and whether it closed a quote. */
    struct CredentialKey {
      std::int64_t start = 0;
      std::int64_t separator = 0;
      bool quoted = false;
    };

    /** Candidate parameter commas inside parentheses; Perl hash entries can occupy the same positions. */
    std::unordered_set<std::int64_t> parameterCommas(const Text& text) {
      std::unordered_set<std::int64_t> commas;
      if (!rt::includes(text.value, "=>")) return commas;
      std::vector<char16_t> brackets;
      std::int64_t index = 0;
      while (index < text.length()) {
        const std::int32_t unit = text[index];
        if (has(u"\"'`", unit)) {
          index = quotedEnd(text, index);
          continue;
        }
        if (has(u"([{", unit)) brackets.push_back(static_cast<char16_t>(unit));
        else if (has(u")]}", unit)) {
          if (!brackets.empty()) brackets.pop_back();
        } else if (unit == u',' && !brackets.empty() && brackets.back() == u'(') commas.insert(index);
        index += 1;
      }
      return commas;
    }

    constexpr std::int64_t CALLBACK_BODY_LIMIT = 512;
    const rt::Regex CALLBACK_METHOD(R"(^([A-Za-z_$][\w$]*)((?:\.[A-Za-z_$][\w$]*(?:\(\))?)+)(?=\s*[,);]))");
    const rt::Regex CALLBACK_IDENTIFIER(R"(^[A-Za-z_$][\w$]*$)");
    const rt::Regex CALLBACK_DECLARATION(R"(^(?:const|let|var)\s+[A-Za-z_$][\w$]*\s*=\s*)");
    const rt::Regex CALLBACK_CALL(R"(^([^(]+)\(([^()]*)\)$)");

    /** Only positively recognized ordinary callbacks bypass credential redaction. */
    bool ordinaryCallbackBody(const Text& text, const CredentialKey& key) {
      static const rt::Regex SEPARATOR_AFTER_CLOSE(R"(^\s*[,);])");
      const std::string parameter = rt::trim(slice(text, key.start, key.separator));
      if (!CALLBACK_IDENTIFIER.test(parameter)) return false;
      const std::int64_t value = skipWhitespace(text, key.separator + 2);
      const std::string body = slice(text, value, value + CALLBACK_BODY_LIMIT);
      const std::optional<rt::RegexMatch> method = CALLBACK_METHOD.exec(body);
      if (method) return (*method)[1] == parameter && rt::includes(*(*method)[2], "()");
      if (!rt::startsWith(body, "{")) return false;
      const std::int64_t close = rt::indexOf(body, "}");
      if (close < 0 || !SEPARATOR_AFTER_CLOSE.test(rt::slice(body, close + 1))) return false;
      const std::string statements = rt::trim(rt::slice(body, 1, close));
      if (!rt::endsWith(statements, ";")) return false;
      bool usesParameter = false;
      for (const std::string& statement : rt::split(rt::slice(statements, 0, -1), ";")) {
        const std::string declared = rt::replace(rt::trim(statement), CALLBACK_DECLARATION, "");
        const std::optional<rt::RegexMatch> call = CALLBACK_CALL.exec(declared);
        if (!call) return false;
        for (const std::string& part : rt::split(*(*call)[1], ".")) {
          if (!CALLBACK_IDENTIFIER.test(rt::trim(part))) return false;
        }
        std::vector<std::string> arguments_;
        for (const std::string& argument : rt::split(*(*call)[2], ",")) arguments_.push_back(rt::trim(argument));
        for (const std::string& argument : arguments_) {
          if (!CALLBACK_IDENTIFIER.test(argument)) return false;
        }
        usesParameter = usesParameter || std::find(arguments_.begin(), arguments_.end(), parameter) != arguments_.end();
      }
      return usesParameter;
    }

    /** Parameter position alone also describes Perl hash/call entries; their values must stay covered. */
    bool isArrowParameter(const Text& text, const CredentialKey& key, const std::unordered_set<std::int64_t>& commas) {
      if (key.quoted || !startsWith(text, u"=>", key.separator)) return false;
      std::int64_t before = key.start - 1;
      while (before >= 0 && WHITESPACE(text[before])) before -= 1;
      return (text[before] == u'(' || commas.contains(before)) && ordinaryCallbackBody(text, key);
    }

    std::vector<CredentialKey> credentialKeys(const Text& text) {
      const std::unordered_set<std::int64_t> commas = parameterCommas(text);
      std::vector<CredentialKey> keys;
      for (const rt::RegexMatch& match : rt::matchAll(text.value, CREDENTIAL_KEY)) {
        const CredentialKey key{
          .start = static_cast<std::int64_t>(match.index),
          .separator = static_cast<std::int64_t>(match.endIndex()),
          .quoted = !match[2] || !match[2]->empty(),
        };
        if (!isArrowParameter(text, key, commas)) keys.push_back(key);
      }
      return keys;
    }

    /**
     * Where a key's value can start: past the separator and any whitespace. `=>`
     * (PHP, Perl, Ruby) also retains the old `=` reading for unquoted keys,
     * where a bare value started with `>`. Arrow parameters are excluded above.
     */
    std::vector<std::int64_t> valueStarts(const Text& text, const CredentialKey& key) {
      std::vector<std::int64_t> starts{skipWhitespace(text, key.separator + 1)};
      if (startsWith(text, u"=>", key.separator)) {
        if (key.quoted) return {skipWhitespace(text, key.separator + 2)};
        starts.push_back(skipWhitespace(text, key.separator + 2));
      }
      return starts;
    }

    std::vector<RedactionSpan> patternSpans(const Text& text, const rt::Regex& pattern) {
      std::vector<RedactionSpan> spans;
      for (const rt::RegexMatch& match : rt::matchAll(text.value, pattern)) spans.push_back({static_cast<std::int64_t>(match.index), static_cast<std::int64_t>(match.endIndex())});
      return spans;
    }

    /** Each key's double-quoted value or JavaScript template literal, through the end of the text when it never closes. */
    std::vector<RedactionSpan> quotedValueSpans(const Text& text, const std::vector<CredentialKey>& keys, bool closedOnly = false) {
      std::vector<RedactionSpan> spans;
      for (const CredentialKey& key : keys) {
        for (const std::int64_t value : valueStarts(text, key)) {
          if (text[value] == u'"' || text[value] == u'`') {
            const std::int64_t end = quotedEnd(text, value, closedOnly);
            if (end > value) spans.push_back({key.start, end});
          }
        }
      }
      return spans;
    }

    /** Each key's single-quoted scalar, read both ways, or token-like bare value. */
    std::vector<RedactionSpan> scalarValueSpans(const Text& text, const std::vector<CredentialKey>& keys, bool closedOnly = false) {
      RunCache cache{.start = 0, .end = 0};
      RunCache quotedCache{.start = 0, .end = 0};
      std::vector<RedactionSpan> spans;
      for (const CredentialKey& key : keys) {
        for (const std::int64_t value : valueStarts(text, key)) {
          if (text[value] == u'\'') {
            for (const std::int64_t end : {yamlScalarEnd(text, value, closedOnly), escapedScalarEnd(text, value, closedOnly)}) {
              if (end > value) spans.push_back({key.start, end});
            }
          }
          if (closedOnly || value == text.length()) continue;
          // Quoted keys may have bare YAML/JSON/PHP values, but their fallback
          // must not eat an adjacent field or reinterpret a quoted/flow value.
          if (key.quoted && has(u"\"'`[{", text[value])) continue;
          const std::int64_t end = key.quoted ? runEnd(text, value, quotedCache, QUOTED_BARE_END) : runEnd(text, value, cache);
          if (end - value >= MIN_BARE_VALUE) spans.push_back({key.start, end});
        }
      }
      return spans;
    }

    /** For each string-content position, the end of its string, or -1 if it is clipped. */
    std::vector<std::int32_t> stringContentEnds(const Text& text, char16_t quote, bool yamlSingleQuotes) {
      std::vector<std::int32_t> ends(static_cast<std::size_t>(text.length() + 2), -1);
      const auto at = [](std::int64_t index) { return static_cast<std::size_t>(index); };
      for (std::int64_t index = text.length() - 1; index >= 0; index -= 1) {
        if (text[index] == u'\\' && !(quote == u'\'' && yamlSingleQuotes)) ends[at(index)] = ends[at(index + 2)];
        else if (text[index] == quote) ends[at(index)] = quote == u'\'' && text[index + 1] == u'\'' ? ends[at(index + 2)] : static_cast<std::int32_t>(index + 1);
        else ends[at(index)] = ends[at(index + 1)];
      }
      return ends;
    }

    bool flowScalarStartsAt(const Text& text, std::int64_t index) {
      std::int64_t before = index - 1;
      while (before >= 0 && text[before] <= u' ') before -= 1;
      return has(SCALAR_START, text[before]);
    }

    std::int32_t followingClose(const std::vector<std::int32_t>& ends, std::int32_t end) { return end < 0 ? -1 : ends[static_cast<std::size_t>(end)]; }

    /**
     * Closed values only, in linear time. Resolve suffixes backwards so a clipped
     * outer value cannot absorb the scan for a closed nested credential. This is
     * the bounded reading used across records, never across prompt framing.
     */
    std::vector<std::int32_t> nextBracketCloses(const Text& text, const QuoteReading& reading) {
      const std::u16string_view quotes = reading.templateLiterals ? u"\"'`" : u"\"'";
      std::vector<std::vector<std::int32_t>> stringEnds;
      for (const char16_t quote : quotes) stringEnds.push_back(stringContentEnds(text, quote, reading.yamlSingleQuotes));
      std::vector<std::int32_t> nextClose(static_cast<std::size_t>(text.length() + 1), -1);
      const auto at = [](std::int64_t index) { return static_cast<std::size_t>(index); };
      for (std::int64_t index = text.length() - 1; index >= 0; index -= 1) {
        const std::int32_t unit = text[index];
        const std::size_t kind = quotes.find(static_cast<char16_t>(unit));
        const std::vector<std::int32_t>* quoted = kind == std::u16string_view::npos ? nullptr : &stringEnds[kind];
        if (quoted && opensString(text, index, reading, flowScalarStartsAt(text, index))) {
          nextClose[at(index)] = followingClose(nextClose, (*quoted)[at(index + 1)]);
        } else if (unit == u'[' || unit == u'{') {
          nextClose[at(index)] = followingClose(nextClose, nextClose[at(index + 1)]);
        } else {
          nextClose[at(index)] = unit == u']' || unit == u'}' ? static_cast<std::int32_t>(index + 1) : nextClose[at(index + 1)];
        }
      }
      return nextClose;
    }

    std::vector<RedactionSpan> closedBracketedValues(const Text& text, const std::vector<ValueOpening>& openings, const QuoteReading& reading) {
      const std::vector<std::int32_t> nextClose = nextBracketCloses(text, reading);
      std::vector<RedactionSpan> values;
      for (const ValueOpening& opening : openings) {
        const std::int32_t end = nextClose[static_cast<std::size_t>(opening.bracket + 1)];
        if (end >= 0) values.push_back({opening.start, end});
      }
      return values;
    }

    /** Each key's array or object value, read every quote way. */
    std::vector<RedactionSpan> bracketedValueSpans(const Text& text, const std::vector<CredentialKey>& keys, bool closedOnly = false) {
      std::vector<ValueOpening> openings;
      for (const CredentialKey& key : keys) {
        for (const std::int64_t value : valueStarts(text, key)) {
          if (text[value] == u'[' || text[value] == u'{') openings.push_back({.start = key.start, .bracket = value});
        }
      }
      if (openings.empty()) return {};
      // The scan walks the text once, so it takes the openings in bracket order.
      std::stable_sort(openings.begin(), openings.end(), [](const ValueOpening& left, const ValueOpening& right) { return left.bracket < right.bracket; });
      std::vector<RedactionSpan> spans;
      for (const QuoteReading& reading : QUOTE_READINGS) {
        const std::vector<RedactionSpan> found = closedOnly ? closedBracketedValues(text, openings, reading) : bracketedValues(text, openings, reading);
        spans.insert(spans.end(), found.begin(), found.end());
      }
      return spans;
    }

    /** Merge overlapping spans so one replacement can never cut through another secret. */
    std::vector<RedactionSpan> mergeSpans(const std::vector<RedactionSpan>& spans) {
      std::vector<RedactionSpan> ordered = spans;
      std::stable_sort(ordered.begin(), ordered.end(), [](const RedactionSpan& left, const RedactionSpan& right) {
        return left.start != right.start ? left.start < right.start : left.end < right.end;
      });
      std::vector<RedactionSpan> merged;
      for (const RedactionSpan& span : ordered) {
        if (!merged.empty() && span.start < merged.back().end) {
          merged.back().end = std::max(merged.back().end, span.end);
          continue;
        }
        merged.push_back(span);
      }
      return merged;
    }

    /** The text with each masked span replaced by the marker, and where each marker sits in it. */
    struct Marker {
      std::int64_t at = 0;
      RedactionSpan span;
    };
    struct Rendering {
      Text text;
      std::vector<Marker> markers;
    };

    Rendering render(const Text& text, const std::vector<RedactionSpan>& mask) {
      std::u16string rendered;
      std::int64_t cursor = 0;
      std::vector<Marker> markers;
      for (const RedactionSpan& span : mask) {
        rendered += sliceUnits(text, cursor, span.start);
        markers.push_back({.at = static_cast<std::int64_t>(rendered.size()), .span = span});
        rendered += MARKER;
        cursor = span.end;
      }
      rendered += sliceUnits(text, cursor, text.length());
      return {.text = Text(std::move(rendered)), .markers = std::move(markers)};
    }

    /** Where a rendered position falls in the original text: a marker stands for its whole span. */
    RedactionSpan sourceRange(const Rendering& rendering, std::int64_t position) {
      std::size_t low = 0;
      std::size_t high = rendering.markers.size();
      while (low < high) {
        const std::size_t middle = (low + high) >> 1;
        if (rendering.markers[middle].at <= position) low = middle + 1;
        else high = middle;
      }
      if (low == 0) return {position, position + 1};
      const Marker& marker = rendering.markers[low - 1];
      const std::int64_t offset = position - marker.at - static_cast<std::int64_t>(MARKER.size());
      if (offset < 0) return marker.span;
      return {marker.span.end + offset, marker.span.end + offset + 1};
    }

    using Rule = std::function<std::vector<RedactionSpan>(const Text&)>;

    const std::vector<Rule> TOKEN_RULES = [] {
      std::vector<Rule> rules;
      for (const rt::Regex& pattern : TOKEN_PATTERNS) rules.emplace_back([&pattern](const Text& text) { return patternSpans(text, pattern); });
      return rules;
    }();

    /**
     * The key-value rules in the shapes they had before a key could run on
     * through `.` or `-`, take `=>`, or hold a template literal. A value that a
     * broader rule replaced first could hide a key whose bare value runs on past
     * it, so every step that runs before another rule keeps these shapes; the
     * broader rules run on the original text and as the last step. Each key word
     * is taken whole, as above, which matches the same text without backtracking
     * through it.
     */
    const rt::Regex PREVIOUS_QUOTED_ASSIGNMENT(R"re(\b(?=\w*(?:key|token|secret|password))(?=(\w*))\1["']?\s*[:=]\s*"(?:\\.|[^"\\])*")re", "gi");
    const rt::Regex PREVIOUS_BARE_ASSIGNMENT(R"re(\b(?=\w*(?:key|token|secret|password))(?=(\w*))\1\s*[:=]\s*\S{8,})re", "gi");
    const rt::Regex PREVIOUS_SINGLE_QUOTED_OPENING(R"re(\b(?=\w*(?:key|token|secret|password))(?=(\w*))\1["']?\s*[:=]\s*')re", "gi");
    const rt::Regex PREVIOUS_BRACKETED_OPENING(R"re(\b(?=\w*(?:key|token|secret|password))(?=(\w*))\1["']?\s*[:=]\s*[[{])re", "gi");

    std::vector<RedactionSpan> previousQuotedRule(const Text& text) { return patternSpans(text, PREVIOUS_QUOTED_ASSIGNMENT); }

    // The legacy bare rule must also leave no-space arrow parameters alone.
    std::vector<RedactionSpan> previousBareRule(const Text& text) {
      const std::unordered_set<std::int64_t> commas = parameterCommas(text);
      std::vector<RedactionSpan> spans;
      for (const rt::RegexMatch& match : rt::matchAll(text.value, PREVIOUS_BARE_ASSIGNMENT)) {
        const std::int64_t start = static_cast<std::int64_t>(match.index);
        const CredentialKey key{.start = start, .separator = skipWhitespace(text, start + rt::length(*match[1])), .quoted = false};
        if (!isArrowParameter(text, key, commas)) spans.push_back({start, static_cast<std::int64_t>(match.endIndex())});
      }
      return spans;
    }

    /**
     * The first pass the bracketed rule made before: each array or object value
     * of a key in the previous shapes, merged with every token and previous-shape
     * scalar span that overlaps it, all found on the original text. A scalar
     * replaced on its own could remove a quote the bracket scan relies on, or
     * leave a scalar's tail after the value's marker.
     */
    std::vector<RedactionSpan> previousBracketedComponents(const Text& text) {
      std::vector<ValueOpening> openings;
      for (const RedactionSpan& span : patternSpans(text, PREVIOUS_BRACKETED_OPENING)) openings.push_back({.start = span.start, .bracket = span.end - 1});
      std::vector<RedactionSpan> readings;
      for (const QuoteReading& reading : QUOTE_READINGS) {
        const std::vector<RedactionSpan> found = bracketedValues(text, openings, reading);
        readings.insert(readings.end(), found.begin(), found.end());
      }
      const std::vector<RedactionSpan> values = mergeSpans(readings);
      std::vector<RedactionSpan> scalars;
      for (const Rule& rule : TOKEN_RULES) {
        const std::vector<RedactionSpan> found = rule(text);
        scalars.insert(scalars.end(), found.begin(), found.end());
      }
      const std::vector<RedactionSpan> quoted = previousQuotedRule(text);
      scalars.insert(scalars.end(), quoted.begin(), quoted.end());
      const std::vector<RedactionSpan> bare = previousBareRule(text);
      scalars.insert(scalars.end(), bare.begin(), bare.end());
      for (const RedactionSpan& opening : patternSpans(text, PREVIOUS_SINGLE_QUOTED_OPENING)) {
        const std::int64_t quote = opening.end - 1;
        scalars.push_back({opening.start, yamlScalarEnd(text, quote)});
        scalars.push_back({opening.start, escapedScalarEnd(text, quote)});
      }
      std::vector<RedactionSpan> everything = values;
      everything.insert(everything.end(), scalars.begin(), scalars.end());
      std::size_t next = 0;
      std::vector<RedactionSpan> components;
      for (const RedactionSpan& span : mergeSpans(everything)) {
        while (next < values.size() && values[next].end <= span.start) next += 1;
        if (next < values.size() && values[next].start < span.end) components.push_back(span);
      }
      return components;
    }

    std::vector<RedactionSpan> scalarValueRule(const Text& text) { return scalarValueSpans(text, credentialKeys(text)); }

    /** The order the token and scalar rules have always run in, each on the output of the rules before it. */
    const std::vector<Rule> PREVIOUS_ORDER = [] {
      std::vector<Rule> rules = TOKEN_RULES;
      rules.emplace_back(previousQuotedRule);
      rules.emplace_back(scalarValueRule);
      return rules;
    }();

    /** Everything one order of the rules removed, and how many separate spans its steps found. */
    struct Removal {
      std::vector<RedactionSpan> mask;
      std::int64_t count = 0;
    };

    /**
     * Apply each rule to the text redacted so far, starting from `mask`, and
     * return everything removed as spans on the original text. A marker stands in
     * for whatever it replaced, so a later rule can match through it: a bare value
     * runs on through a marker that replaced whitespace, and a scalar ends at a
     * later quote when a marker replaced the one it would have stopped at.
     */
    Removal sequentialMask(const Text& text, const std::vector<RedactionSpan>& mask, const std::vector<Rule>& rules) {
      std::vector<RedactionSpan> current = mask;
      std::int64_t count = 0;
      Rendering rendering = render(text, current);
      for (const Rule& rule : rules) {
        const std::vector<RedactionSpan> found = rule(rendering.text);
        if (found.empty()) continue;
        count += static_cast<std::int64_t>(mergeSpans(found).size());
        std::vector<RedactionSpan> spans = current;
        for (const RedactionSpan& span : found) spans.push_back({sourceRange(rendering, span.start).start, sourceRange(rendering, span.end - 1).end});
        current = mergeSpans(spans);
        rendering = render(text, current);
      }
      return {.mask = std::move(current), .count = count};
    }

    /** Union the full per-record rules with any projected cross-record spans. */
    RedactedText redactSecrets(const Text& text, const std::vector<RedactionSpan>& additionalSpans = {}) {
      const std::vector<CredentialKey> keys = credentialKeys(text);
      std::vector<RedactionSpan> everything;
      const auto append = [&everything](const std::vector<RedactionSpan>& spans) { everything.insert(everything.end(), spans.begin(), spans.end()); };
      for (const Rule& rule : TOKEN_RULES) append(rule(text));
      append(quotedValueSpans(text, keys));
      append(scalarValueSpans(text, keys));
      append(bracketedValueSpans(text, keys));
      append(sequentialMask(text, {}, PREVIOUS_ORDER).mask);
      const std::vector<RedactionSpan> components = previousBracketedComponents(text);
      const Removal afterBracketedValues = sequentialMask(text, components, PREVIOUS_ORDER);
      append(afterBracketedValues.mask);
      append(additionalSpans);
      const std::vector<RedactionSpan> mask = mergeSpans(everything);
      std::size_t next = 0;
      std::int64_t added = 0;
      for (const RedactionSpan& span : mask) {
        while (next < afterBracketedValues.mask.size() && afterBracketedValues.mask[next].end <= span.start) next += 1;
        if (next == afterBracketedValues.mask.size() || afterBracketedValues.mask[next].start >= span.end) added += 1;
      }
      return {.text = render(text, mask).text.value, .redactions = static_cast<std::int64_t>(components.size()) + afterBracketedValues.count + added};
    }
  }

  RedactedText redactSupervisorSecrets(std::string_view text) { return redactSecrets(Text(text)); }

  std::vector<RedactedText> redactSupervisorSecretParts(const std::vector<std::string>& parts) {
    std::vector<RedactedText> redacted;
    if (parts.size() < 2) {
      for (const std::string& part : parts) redacted.push_back(redactSupervisorSecrets(part));
      return redacted;
    }
    std::vector<Text> records;
    std::u16string joined;
    for (std::size_t index = 0; index < parts.size(); index += 1) {
      records.emplace_back(parts[index]);
      if (index > 0) joined += u'\n';
      joined += records.back().units;
    }
    const Text text(std::move(joined));
    const std::vector<CredentialKey> keys = credentialKeys(text);
    std::vector<RedactionSpan> closed = patternSpans(text, PRIVATE_KEY_PATTERN);
    const auto append = [&closed](const std::vector<RedactionSpan>& spans) { closed.insert(closed.end(), spans.begin(), spans.end()); };
    append(quotedValueSpans(text, keys, true));
    append(scalarValueSpans(text, keys, true));
    append(bracketedValueSpans(text, keys, true));
    const std::vector<RedactionSpan> mask = mergeSpans(closed);
    std::int64_t offset = 0;
    std::size_t next = 0;
    for (const Text& part : records) {
      const std::int64_t start = offset;
      offset += part.length() + 1;
      const std::int64_t end = start + part.length();
      while (next < mask.size() && mask[next].end <= start) next += 1;
      std::vector<RedactionSpan> spans;
      for (std::size_t at = next; at < mask.size() && mask[at].start < end; at += 1) {
        spans.push_back({std::max(start, mask[at].start) - start, std::min(end, mask[at].end) - start});
      }
      redacted.push_back(redactSecrets(part, spans));
    }
    return redacted;
  }
}
