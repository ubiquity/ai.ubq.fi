export const AUTH_RELAY_MESSAGE_TYPE = "uos_ai.admin_auth_relay";
export const AUTH_RELAY_ACTION_PASSKEY_LOGIN = "passkey-login";

const DENO_PREVIEW_SUFFIX = "[a-z0-9]{12}";
const TRUSTED_DENO_ORGANIZATIONS = new Set(["ubiquity-dao", "0x4007", "ubiquity-os"]);

const isTrustedDenoOrganizationHost = (hostname) => {
  const labels = hostname.split(".");
  if (labels.length !== 4) return false;
  const [app, organization, platform, topLevelDomain] = labels;
  return /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/.test(app) &&
    TRUSTED_DENO_ORGANIZATIONS.has(organization) &&
    platform === "deno" &&
    topLevelDomain === "net";
};

const isAiGatewayDeployHost = (hostname) =>
  hostname === "ai.ubq.fi" ||
  hostname === "ai-ubq-fi.deno.dev" ||
  new RegExp(`^ai-ubq-fi-${DENO_PREVIEW_SUFFIX}\\.deno\\.dev$`).test(hostname) ||
  isTrustedDenoOrganizationHost(hostname);

export const isTrustedAuthRelayClientOrigin = (value) => {
  try {
    const url = new URL(String(value ?? ""));
    const hostname = url.hostname.toLowerCase();
    return url.protocol === "https:" && !url.port && url.origin === String(value ?? "").replace(/\/+$/g, "") &&
      (hostname === "ai-ubq-fi.deno.dev" ||
        new RegExp(`^ai-ubq-fi-${DENO_PREVIEW_SUFFIX}\\.deno\\.dev$`).test(hostname) ||
        isTrustedDenoOrganizationHost(hostname));
  } catch {
    return false;
  }
};

export const parseTrustedAuthRelayOrigin = (value) => {
  const raw = String(value ?? "").trim();
  if (!raw) return "";
  try {
    const url = new URL(raw);
    const hostname = url.hostname.toLowerCase();
    if (url.protocol !== "https:" || url.port || url.origin !== raw.replace(/\/+$/g, "")) {
      return "";
    }
    if (isAiGatewayDeployHost(hostname)) return url.origin;
    return "";
  } catch {
    return "";
  }
};

export const parseAuthRelayAction = (value) =>
  value === AUTH_RELAY_ACTION_PASSKEY_LOGIN ? AUTH_RELAY_ACTION_PASSKEY_LOGIN : "";
