export const isRecord = (value: unknown): value is Record<string, unknown> => typeof value === "object" && value !== null;

export const encodeHex = (bytes: Uint8Array): string => {
  let out = "";
  for (const b of bytes) out += b.toString(16).padStart(2, "0");
  return out;
};
export const sha256Hex = async (value: string): Promise<string> => {
  const bytes = new TextEncoder().encode(value);
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return encodeHex(new Uint8Array(digest));
};
