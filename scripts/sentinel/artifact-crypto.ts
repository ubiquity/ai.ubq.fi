const TEXT_ENCODER = new TextEncoder();
const TEXT_DECODER = new TextDecoder();
const KEY_BYTES = 32;
const IV_BYTES = 12;
const AAD = TEXT_ENCODER.encode("uos-sentinel-artifact-v1");
const MAX_ARCHIVE_FILES = 10_000;
const MAX_ARCHIVE_BYTES = 256 * 1024 * 1024;

export type SentinelArtifactFile = Readonly<{
  path: string;
  bytes: Uint8Array<ArrayBuffer>;
}>;

type SerializedArtifactFile = Readonly<{
  path: string;
  sha256: string;
  data: string;
}>;

type SerializedArchive = Readonly<{
  version: 1;
  files: readonly SerializedArtifactFile[];
}>;

type EncryptedEnvelope = Readonly<{
  version: 1;
  algorithm: "AES-256-GCM";
  compression: "gzip";
  iv: string;
  ciphertext: string;
}>;

const asArrayBufferBytes = (value: Uint8Array): Uint8Array<ArrayBuffer> => new Uint8Array(value);

const compareArchivePaths = (left: string, right: string): number => left < right ? -1 : left > right ? 1 : 0;

const encodeBase64Url = (value: Uint8Array): string => {
  let binary = "";
  for (let offset = 0; offset < value.byteLength; offset += 0x8000) {
    binary += String.fromCharCode(...value.subarray(offset, offset + 0x8000));
  }
  return btoa(binary).replaceAll("+", "-").replaceAll("/", "_").replace(
    /=+$/u,
    "",
  );
};

const decodeBase64Url = (
  value: string,
  allowEmpty = false,
): Uint8Array<ArrayBuffer> => {
  if (value === "" && allowEmpty) return new Uint8Array();
  if (!/^[A-Za-z0-9_-]+$/u.test(value)) {
    throw new Error("Sentinel artifact base64url value is invalid");
  }
  const standard = value.replaceAll("-", "+").replaceAll("_", "/");
  const padded = standard + "=".repeat((4 - standard.length % 4) % 4);
  try {
    return Uint8Array.from(
      atob(padded),
      (character) => character.charCodeAt(0),
    );
  } catch {
    throw new Error("Sentinel artifact base64url value is invalid");
  }
};

const decodeStandardBase64 = (value: string): Uint8Array<ArrayBuffer> => {
  if (!/^[A-Za-z0-9+/]+={0,2}$/u.test(value) || value.length % 4 !== 0) {
    throw new Error("SENTINEL_ARTIFACT_KEY must be standard base64");
  }
  try {
    return Uint8Array.from(atob(value), (character) => character.charCodeAt(0));
  } catch {
    throw new Error("SENTINEL_ARTIFACT_KEY must be standard base64");
  }
};

export const decodeSentinelArtifactKey = (
  value: string,
): Uint8Array<ArrayBuffer> => {
  const decoded = decodeStandardBase64(value.trim());
  if (decoded.byteLength !== KEY_BYTES) {
    decoded.fill(0);
    throw new Error("SENTINEL_ARTIFACT_KEY must encode exactly 32 bytes");
  }
  return decoded;
};

const validArchivePath = (value: string): boolean => {
  if (
    value.length < 1 || value.length > 1_024 || value.startsWith("/") ||
    value.includes("\\")
  ) return false;
  const parts = value.split("/");
  return parts.every((part) => part.length > 0 && part !== "." && part !== ".." && !part.includes("\0"));
};

const sha256Hex = async (value: Uint8Array): Promise<string> =>
  Array.from(
    new Uint8Array(
      await crypto.subtle.digest("SHA-256", asArrayBufferBytes(value)),
    ),
  )
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");

const gzip = async (
  value: Uint8Array<ArrayBuffer>,
): Promise<Uint8Array<ArrayBuffer>> => {
  const compressed = new Blob([value]).stream().pipeThrough(
    new CompressionStream("gzip"),
  );
  return new Uint8Array(await new Response(compressed).arrayBuffer());
};

const gunzip = async (
  value: Uint8Array<ArrayBuffer>,
): Promise<Uint8Array<ArrayBuffer>> => {
  const decompressed = new Blob([value]).stream().pipeThrough(
    new DecompressionStream("gzip"),
  );
  return new Uint8Array(await new Response(decompressed).arrayBuffer());
};

const importKey = async (
  keyBytes: Uint8Array<ArrayBuffer>,
  usages: KeyUsage[],
): Promise<CryptoKey> => {
  if (keyBytes.byteLength !== KEY_BYTES) {
    throw new Error("Sentinel artifact key must be 32 bytes");
  }
  return await crypto.subtle.importKey(
    "raw",
    keyBytes,
    { name: "AES-GCM" },
    false,
    usages,
  );
};

const validateFiles = (
  files: readonly SentinelArtifactFile[],
): SentinelArtifactFile[] => {
  if (files.length > MAX_ARCHIVE_FILES) {
    throw new Error("Sentinel artifact contains too many files");
  }
  const sorted = [...files].sort((left, right) => compareArchivePaths(left.path, right.path));
  let totalBytes = 0;
  let previousPath: string | null = null;
  for (const file of sorted) {
    if (!validArchivePath(file.path)) {
      throw new Error("Sentinel artifact path is invalid");
    }
    if (file.path === previousPath) {
      throw new Error("Sentinel artifact paths must be unique");
    }
    previousPath = file.path;
    totalBytes += file.bytes.byteLength;
    if (!Number.isSafeInteger(totalBytes) || totalBytes > MAX_ARCHIVE_BYTES) {
      throw new Error("Sentinel artifact plaintext is too large");
    }
  }
  return sorted;
};

export const encryptSentinelArtifact = async (
  files: readonly SentinelArtifactFile[],
  keyBytes: Uint8Array<ArrayBuffer>,
  suppliedIv?: Uint8Array<ArrayBuffer>,
): Promise<Uint8Array<ArrayBuffer>> => {
  const sorted = validateFiles(files);
  const iv = suppliedIv ? asArrayBufferBytes(suppliedIv) : crypto.getRandomValues(new Uint8Array(IV_BYTES));
  if (iv.byteLength !== IV_BYTES) {
    throw new Error("Sentinel artifact IV must be 12 bytes");
  }

  const serializedFiles: SerializedArtifactFile[] = [];
  for (const file of sorted) {
    serializedFiles.push({
      path: file.path,
      sha256: await sha256Hex(file.bytes),
      data: encodeBase64Url(file.bytes),
    });
  }
  const plaintext = TEXT_ENCODER.encode(
    JSON.stringify(
      { version: 1, files: serializedFiles } satisfies SerializedArchive,
    ),
  );
  let compressed: Uint8Array<ArrayBuffer>;
  try {
    compressed = await gzip(plaintext);
  } finally {
    plaintext.fill(0);
  }
  let ciphertext: Uint8Array<ArrayBuffer>;
  try {
    ciphertext = new Uint8Array(
      await crypto.subtle.encrypt(
        { name: "AES-GCM", iv, additionalData: AAD },
        await importKey(keyBytes, ["encrypt"]),
        compressed,
      ),
    );
  } finally {
    compressed.fill(0);
  }
  try {
    const envelope: EncryptedEnvelope = {
      version: 1,
      algorithm: "AES-256-GCM",
      compression: "gzip",
      iv: encodeBase64Url(iv),
      ciphertext: encodeBase64Url(ciphertext),
    };
    return TEXT_ENCODER.encode(JSON.stringify(envelope));
  } finally {
    ciphertext.fill(0);
    iv.fill(0);
  }
};

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const parseEnvelope = (bytes: Uint8Array<ArrayBuffer>): EncryptedEnvelope => {
  let value: unknown;
  try {
    value = JSON.parse(TEXT_DECODER.decode(bytes));
  } catch {
    throw new Error("Sentinel artifact envelope is invalid JSON");
  }
  if (
    !isRecord(value) || value.version !== 1 ||
    value.algorithm !== "AES-256-GCM" ||
    value.compression !== "gzip" || typeof value.iv !== "string" ||
    typeof value.ciphertext !== "string"
  ) {
    throw new Error("Sentinel artifact envelope is invalid");
  }
  return value as EncryptedEnvelope;
};

export const decryptSentinelArtifact = async (
  encryptedBytes: Uint8Array<ArrayBuffer>,
  keyBytes: Uint8Array<ArrayBuffer>,
): Promise<SentinelArtifactFile[]> => {
  const envelope = parseEnvelope(encryptedBytes);
  const iv = decodeBase64Url(envelope.iv);
  if (iv.byteLength !== IV_BYTES) {
    throw new Error("Sentinel artifact IV is invalid");
  }
  const ciphertext = decodeBase64Url(envelope.ciphertext);
  let compressed: Uint8Array<ArrayBuffer>;
  try {
    compressed = new Uint8Array(
      await crypto.subtle.decrypt(
        { name: "AES-GCM", iv, additionalData: AAD },
        await importKey(keyBytes, ["decrypt"]),
        ciphertext,
      ),
    );
  } finally {
    ciphertext.fill(0);
    iv.fill(0);
  }
  let plaintext: Uint8Array<ArrayBuffer>;
  try {
    plaintext = await gunzip(compressed);
  } finally {
    compressed.fill(0);
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(TEXT_DECODER.decode(plaintext));
  } catch {
    throw new Error("Sentinel artifact plaintext is invalid JSON");
  } finally {
    plaintext.fill(0);
  }
  if (
    !isRecord(parsed) || parsed.version !== 1 || !Array.isArray(parsed.files)
  ) {
    throw new Error("Sentinel artifact plaintext is invalid");
  }
  if (parsed.files.length > MAX_ARCHIVE_FILES) {
    throw new Error("Sentinel artifact contains too many files");
  }

  const files: SentinelArtifactFile[] = [];
  let totalBytes = 0;
  let previousPath: string | null = null;
  for (const entry of parsed.files) {
    if (
      !isRecord(entry) || typeof entry.path !== "string" ||
      !validArchivePath(entry.path) ||
      typeof entry.sha256 !== "string" ||
      !/^[0-9a-f]{64}$/u.test(entry.sha256) || typeof entry.data !== "string"
    ) {
      throw new Error("Sentinel artifact file entry is invalid");
    }
    if (previousPath !== null && entry.path <= previousPath) {
      throw new Error("Sentinel artifact file paths are not strictly ordered");
    }
    previousPath = entry.path;
    const fileBytes = decodeBase64Url(entry.data, true);
    totalBytes += fileBytes.byteLength;
    if (!Number.isSafeInteger(totalBytes) || totalBytes > MAX_ARCHIVE_BYTES) {
      fileBytes.fill(0);
      throw new Error("Sentinel artifact plaintext is too large");
    }
    if (await sha256Hex(fileBytes) !== entry.sha256) {
      fileBytes.fill(0);
      throw new Error("Sentinel artifact file integrity check failed");
    }
    files.push({ path: entry.path, bytes: fileBytes });
  }
  return files;
};
