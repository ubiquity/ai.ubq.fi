#include "tests/helpers/supervisor_redaction_previous.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace uos::tests::helpers::supervisor_redaction_previous {
  namespace {
    namespace rt = uos::rt;
    using codex::supervisor_secret::RedactedText;

    // UTF-16 code unit positions, as in the TS.
    struct RedactionSpan {
      std::int64_t start;
      std::int64_t end;
    };

    /** Patterns whose match end is unambiguous, applied to the text in order. */
    const std::vector<rt::Regex>& supervisorSecretPatterns() {
      static const std::vector<rt::Regex> SUPERVISOR_SECRET_PATTERNS{
        rt::Regex(R"re(-----BEGIN [A-Z ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z ]*PRIVATE KEY-----)re", "g"),
        rt::Regex(R"re(\b[Bb]earer\s+[A-Za-z0-9._~+/=-]{12,})re", "g"),
        rt::Regex(R"re(\b(?:sk|ghp|gho|ghu|ghs|dsk)-[A-Za-z0-9_-]{12,})re", "g"),
        rt::Regex(R"re((?:ghp|gho|ghu|ghs)_[A-Za-z0-9]{12,})re", "g"),
        rt::Regex(R"re(github_pat_\w{12,})re", "g"),
        rt::Regex(R"re(\bu_[0-9a-fA-F]{32,}\b)re", "g"),
        rt::Regex(R"re(\b\w*(?:key|token|secret|password)\w*["']?\s*[:=]\s*"(?:\\.|[^"\\])*")re", "gi"),
      };
      return SUPERVISOR_SECRET_PATTERNS;
    }

    /** Last-resort fallback: a credential-key assignment with a token-like value. */
    const rt::Regex& keyedAssignment() {
      static const rt::Regex KEYED_ASSIGNMENT(R"re(\b\w*(?:key|token|secret|password)\w*\s*[:=]\s*\S{8,})re", "gi");
      return KEYED_ASSIGNMENT;
    }

    /** Opening of a credential assignment whose value is a single-quoted scalar. */
    const rt::Regex& singleQuotedAssignment() {
      static const rt::Regex SINGLE_QUOTED_ASSIGNMENT(R"re(\b\w*(?:key|token|secret|password)\w*["']?\s*[:=]\s*')re", "gi");
      return SINGLE_QUOTED_ASSIGNMENT;
    }

    bool unitIs(const std::u16string& text, std::int64_t index, char16_t unit) {
      return index >= 0 && index < static_cast<std::int64_t>(text.size()) && text[static_cast<std::size_t>(index)] == unit;
    }

    /** End of the scalar read as YAML: `''` is an escaped quote and a backslash is content. */
    std::int64_t yamlScalarEnd(const std::u16string& text, std::int64_t openQuote) {
      const std::int64_t length = static_cast<std::int64_t>(text.size());
      for (std::int64_t index = openQuote + 1; index < length; index += 1) {
        if (!unitIs(text, index, u'\'')) continue;
        if (unitIs(text, index + 1, u'\'')) {
          index += 1;
          continue;
        }
        return index + 1;
      }
      // A scalar with no closing quote was clipped, so redact its whole tail.
      return length;
    }

    /** End of the scalar read with the pre-existing rule that `\x` escapes the next character. */
    std::int64_t escapedScalarEnd(const std::u16string& text, std::int64_t openQuote) {
      const std::int64_t length = static_cast<std::int64_t>(text.size());
      for (std::int64_t index = openQuote + 1; index < length; index += 1) {
        if (unitIs(text, index, u'\\')) {
          index += 1;
          continue;
        }
        if (!unitIs(text, index, u'\'')) continue;
        if (unitIs(text, index + 1, u'\'')) {
          index += 1;
          continue;
        }
        return index + 1;
      }
      return length;
    }

    /** Merge overlapping spans so one replacement can never cut through another secret. */
    std::vector<RedactionSpan> mergeSpans(const std::vector<RedactionSpan>& spans) {
      std::vector<RedactionSpan> ordered = spans;
      rt::sort(ordered, [](const RedactionSpan& left, const RedactionSpan& right) -> double {
        const double byStart = static_cast<double>(left.start - right.start);
        return byStart != 0 ? byStart : static_cast<double>(left.end - right.end);
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

    /** Replace each span with the marker, measured on the text the spans came from. */
    std::string replaceSpans(std::string_view text, const std::vector<RedactionSpan>& spans) {
      std::string redacted;
      std::int64_t cursor = 0;
      for (const RedactionSpan& span : spans) {
        rt::append(redacted, rt::slice(text, cursor, span.start));
        rt::append(redacted, "[redacted]");
        cursor = span.end;
      }
      rt::append(redacted, rt::slice(text, cursor));
      return redacted;
    }

    /**
     * A single-quoted scalar has no single unambiguous end: YAML reads a backslash
     * as ordinary content and `''` as an escaped quote, while the pre-existing
     * support also accepted `\x` as an escape. Every reading's candidate end is
     * collected on the same text, the fallback assignment matches are added, and
     * overlapping spans are merged before replacement, because committing to one
     * longer reading alone can erase the next assignment's key and leave its value
     * behind.
     */
    RedactedText redactAmbiguousAssignments(const std::string& text) {
      const std::u16string units = rt::toUtf16(text);
      std::vector<RedactionSpan> spans;
      for (const rt::RegexMatch& match : rt::matchAll(text, keyedAssignment())) {
        spans.push_back(RedactionSpan{.start = static_cast<std::int64_t>(match.index), .end = static_cast<std::int64_t>(match.endIndex())});
      }
      for (const rt::RegexMatch& match : rt::matchAll(text, singleQuotedAssignment())) {
        const std::int64_t start = static_cast<std::int64_t>(match.index);
        const std::int64_t openQuote = static_cast<std::int64_t>(match.endIndex()) - 1;
        spans.push_back(RedactionSpan{.start = start, .end = yamlScalarEnd(units, openQuote)});
        spans.push_back(RedactionSpan{.start = start, .end = escapedScalarEnd(units, openQuote)});
      }
      const std::vector<RedactionSpan> merged = mergeSpans(spans);
      return RedactedText{.text = replaceSpans(text, merged), .redactions = static_cast<std::int64_t>(merged.size())};
    }
  }

  RedactedText redactWithPreviousRules(std::string_view text) {
    std::string redacted(text);
    std::int64_t redactions = 0;
    for (const rt::Regex& pattern : supervisorSecretPatterns()) {
      redacted = rt::replace(redacted, pattern, [&redactions](const rt::RegexMatch&) {
        redactions += 1;
        return std::string("[redacted]");
      });
    }
    const RedactedText ambiguous = redactAmbiguousAssignments(redacted);
    return RedactedText{.text = ambiguous.text, .redactions = redactions + ambiguous.redactions};
  }
}
