export const AUTH_RELAY_MESSAGE_TYPE = "uos_ai.admin_auth_relay";
export const AUTH_RELAY_ACTION_PASSKEY_LOGIN = "passkey-login";

const isLocalHost = (hostname) =>
  hostname === "localhost" || hostname === "127.0.0.1" || hostname === "::1" || hostname === "[::1]";

const DENO_PREVIEW_HOST = /^(?:p-)?ai-ubq-fi(?:-[a-z0-9]{12})?\.(?:deno\.dev|ubiquity-dao\.deno\.net)$/;

const isAiGatewayPreviewHost = (hostname) => DENO_PREVIEW_HOST.test(hostname);

export const isAiGatewayPreviewOrigin = (value) => {
  try {
    const url = new URL(String(value ?? "").trim());
    return url.protocol === "https:" && isAiGatewayPreviewHost(url.hostname.toLowerCase());
  } catch {
    return false;
  }
};

const isAiGatewayDeployHost = (hostname) =>
  hostname === "ai.ubq.fi" ||
  hostname === "ai-ubq-fi.deno.dev" ||
  hostname === "ai-ubq-fi.ubiquity-dao.deno.net" ||
  isAiGatewayPreviewHost(hostname);

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
