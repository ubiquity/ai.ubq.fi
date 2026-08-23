const DENO_PREVIEW_SUFFIX = "[a-z0-9]{12}";
const TRUSTED_DENO_APPLICATIONS = new Map<string, ReadonlySet<string>>([
  ["ubiquity-dao", new Set(["ai-ubq-fi", "p-ai-ubq-fi"])],
  ["0x4007", new Set(["telegram-daily-exporter"])],
  ["ubiquity-os", new Set(["agent-worker"])],
]);

const isTrustedDenoApplicationHost = (hostname: string): boolean => {
  const labels = hostname.split(".");
  if (labels.length !== 4) return false;
  const [app, organization, platform, topLevelDomain] = labels;
  if (platform !== "deno" || topLevelDomain !== "net") return false;
  const trustedApps = TRUSTED_DENO_APPLICATIONS.get(organization ?? "");
  if (!trustedApps || !app) return false;
  for (const trustedApp of trustedApps) {
    if (app === trustedApp || new RegExp(`^${trustedApp}-${DENO_PREVIEW_SUFFIX}$`).test(app)) return true;
  }
  return false;
};

const isAiGatewayDeployHost = (hostname: string): boolean =>
  hostname === "ai.ubq.fi" ||
  hostname === "ai-ubq-fi.deno.dev" ||
  new RegExp(`^ai-ubq-fi-${DENO_PREVIEW_SUFFIX}\\.deno\\.dev$`).test(hostname) ||
  isTrustedDenoApplicationHost(hostname);

/**
 * Parses the exact HTTPS origins allowed by the browser auth relay contract.
 * Keep this host allowlist aligned with static/auth-relay.js.
 */
export const parseTrustedAuthRelayOrigin = (value: unknown): string | null => {
  const raw = typeof value === "string" ? value.trim() : "";
  if (!raw) return null;
  try {
    const url = new URL(raw);
    const hostname = url.hostname.toLowerCase();
    if (url.protocol !== "https:" || url.port || url.origin !== raw.replace(/\/+$/g, "")) return null;
    return isAiGatewayDeployHost(hostname) ? url.origin : null;
  } catch {
    return null;
  }
};
