#pragma once
/**
 * Credential-shaped text in supervisor transcript excerpts.
 *
 * Transcript bytes leave the host for an external summarizer, so redaction is
 * deliberately over-broad: a false positive costs a little context, while a
 * false negative discloses a credential. The patterns stay simple on purpose;
 * this is a bounded sanitizer, not a JSON parser or a secret classifier.
 */
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uos::codex::supervisor_secret {
  // `{ text: string; redactions: number }`, the result type of both exports.
  struct RedactedText {
    std::string text;
    std::int64_t redactions = 0;
    friend bool operator==(const RedactedText&, const RedactedText&) = default;
  };

  /**
   * Replace credential-shaped spans with a marker and report how many were
   * removed. A replacement can hide a key from a later rule, and it
   * can also let a later rule's value run on through its marker, so no single
   * order of the rules removes everything. The spans of three orders are merged
   * on the original text before anything is replaced:
   *
   * - every rule on the original text at once, so no match can hide the next
   *   key's own value;
   * - the previous order of the token and scalar rules;
   * - the same order after the first pass the bracketed rule made before.
   *
   * The result removes everything any one of them would. The count follows
   * the previous rules' steps, each step of the last order counting the
   * separate spans it found, plus one for each span only another order found.
   * Rare structured inputs can differ by one from the previous count even
   * when the redacted text is identical; this is not a count of merged spans.
   */
  RedactedText redactSupervisorSecrets(std::string_view text);

  /**
   * Redact record texts before any of them is clipped or loses a credential key.
   * Only closed PEM/quoted/flow values may cross records. Project their spans
   * back onto the original records and union with each record's full rules, so
   * neither a cross-record replacement nor an unclosed value can hide a match.
   */
  std::vector<RedactedText> redactSupervisorSecretParts(const std::vector<std::string>& parts);
}
