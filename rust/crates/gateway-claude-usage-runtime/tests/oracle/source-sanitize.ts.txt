export const sanitizeClaudeCodeErrorMessage = (raw: unknown, maxLength = 240): string | null => {
  if (typeof raw !== "string") return null;
  const collapsed = raw.replace(/\s+/g, " ").trim();
  if (!collapsed) return null;
  return collapsed.length > maxLength ? collapsed.slice(0, maxLength) : collapsed;
};
