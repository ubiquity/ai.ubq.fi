/**
 * Credential-shaped text in supervisor transcript excerpts.
 *
 * Transcript bytes leave the host for an external summarizer, so redaction is
 * deliberately over-broad: a false positive costs a little context, while a
 * false negative discloses a credential. The patterns stay simple on purpose;
 * this is a bounded sanitizer, not a JSON parser or a secret classifier.
 */
export const SUPERVISOR_SECRET_PATTERNS: readonly RegExp[] = [
  /-----BEGIN [A-Z ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z ]*PRIVATE KEY-----/g,
  /\b[Bb]earer\s+[A-Za-z0-9._~+/=-]{12,}/g,
  /\b(?:sk|ghp|gho|ghu|ghs|dsk)-[A-Za-z0-9_-]{12,}/g,
  /(?:ghp|gho|ghu|ghs)_[A-Za-z0-9]{12,}/g,
  /github_pat_\w{12,}/g,
  /\bu_[0-9a-fA-F]{32,}\b/g,
  /\b\w*(?:key|token|secret|password)\w*["']?\s*[:=]\s*"(?:\\.|[^"\\])*"/gi,
  /\b\w*(?:key|token|secret|password)\w*["']?\s*[:=]\s*'(?:\\.|[^'\\])*'/gi,
  /\b\w*(?:key|token|secret|password)\w*\s*[:=]\s*\S{8,}/gi,
];

/** Replace credential-shaped spans with a marker and report how many were removed. */
export const redactSupervisorSecrets = (text: string): { text: string; redactions: number } => {
  let redacted = text;
  let redactions = 0;
  for (const pattern of SUPERVISOR_SECRET_PATTERNS) {
    redacted = redacted.replace(pattern, () => {
      redactions += 1;
      return "[redacted]";
    });
  }
  return { text: redacted, redactions };
};
