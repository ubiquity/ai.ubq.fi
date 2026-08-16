export const AUTH_RELAY_MESSAGE_TYPE = "uos_ai.admin_auth_relay";
export const AUTH_RELAY_ACTION_PASSKEY_LOGIN = "passkey-login";

const isLocalHost = (hostname) =>
  hostname === "localhost" || hostname === "127.0.0.1" || hostname === "::1" || hostname === "[::1]";

const DENO_PREVIEW_SUFFIX = "[a-z0-9]{12}";

const isAiGatewayDeployHost = (hostname) =>
  hostname === "ai.ubq.fi" ||
  hostname === "ai-ubq-fi.deno.dev" ||
  hostname === "ai-ubq-fi.ubiquity-dao.deno.net" ||
  new RegExp(`^ai-ubq-fi-${DENO_PREVIEW_SUFFIX}\\.deno\\.dev$`).test(hostname) ||
  new RegExp(`^ai-ubq-fi-${DENO_PREVIEW_SUFFIX}\\.ubiquity-dao\\.deno\\.net$`).test(hostname);

export const isAiGatewayPreviewOrigin = (value) => {
  try {
    const url = new URL(String(value ?? ""));
    return url.protocol === "https:" &&
      (new RegExp(`^ai-ubq-fi-${DENO_PREVIEW_SUFFIX}\\.deno\\.dev$`).test(url.hostname.toLowerCase()) ||
        new RegExp(`^ai-ubq-fi-${DENO_PREVIEW_SUFFIX}\\.ubiquity-dao\\.deno\\.net$`).test(url.hostname.toLowerCase()));
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
    if ((url.protocol !== "http:" && url.protocol !== "https:") || url.origin !== raw.replace(/\/+$/g, "")) {
      return "";
    }
    if (isLocalHost(hostname)) return url.origin;
    if (url.protocol === "https:" && isAiGatewayDeployHost(hostname)) return url.origin;
    return "";
  } catch {
    return "";
  }
};

export const parseAuthRelayAction = (value) =>
  value === AUTH_RELAY_ACTION_PASSKEY_LOGIN ? AUTH_RELAY_ACTION_PASSKEY_LOGIN : "";
