#pragma once
// Port of tests/helpers/supervisor-redaction-previous.ts.
/**
 * The supervisor redaction rules as they stood before arrays and objects were
 * redacted (`src/codex/supervisor-secret.ts` at 6e14d507^), kept verbatim as
 * the reference for a differential test: the current rules must remove
 * everything these removed.
 */
#include "uos/codex/supervisor_secret.hpp"

#include <string_view>

namespace uos::tests::helpers::supervisor_redaction_previous {
  /** Replace credential-shaped spans with a marker and report how many were removed, as the previous rules did. */
  // `{ text: string; redactions: number }` is the same shape as supervisor_secret::RedactedText.
  codex::supervisor_secret::RedactedText redactWithPreviousRules(std::string_view text);
}
