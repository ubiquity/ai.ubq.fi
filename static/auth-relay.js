export const AUTH_RELAY_MESSAGE_TYPE = "uos_ai.admin_auth_relay";
export const AUTH_RELAY_ACTION_PASSKEY_LOGIN = "passkey-login";

const isLocalHost = (hostname) =>
  hostname === "localhost" || hostname === "127.0.0.1" || hostname === "::1" || hostname === "[::1]";

const isAiGatewayDeployHost = (hostname) =>
  hostname === "ai.ubq.fi" ||
  /^ai-ubq-fi(?:-[a-z0-9]+)*\.deno\.dev$/.test(hostname) ||
  /^ai-ubq-fi(?:-[a-z0-9]+)*\.ubiquity-dao\.deno\.net$/.test(hostname);

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
