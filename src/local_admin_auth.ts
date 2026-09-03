export type ServeRuntimeOptions = Readonly<{
  disableAdminAuth: boolean;
}>;

const DISABLE_ADMIN_AUTH_FLAG = "--disable-admin-auth";

const formatArgumentError = (argument: string): Error =>
  new Error(
    `[ai.ubq.fi] Unknown server argument '${argument}'. Supported arguments: ${DISABLE_ADMIN_AUTH_FLAG}`,
  );

export const parseServeRuntimeOptions = (
  args: readonly string[],
  options: Readonly<{ isDeploy: boolean }>,
): ServeRuntimeOptions => {
  let disableAdminAuth = false;

  for (const argument of args) {
    if (argument !== DISABLE_ADMIN_AUTH_FLAG) throw formatArgumentError(argument);
    if (disableAdminAuth) {
      throw new Error(`[ai.ubq.fi] Server argument '${DISABLE_ADMIN_AUTH_FLAG}' may only be specified once.`);
    }
    disableAdminAuth = true;
  }

  if (disableAdminAuth && options.isDeploy) {
    throw new Error(`[ai.ubq.fi] ${DISABLE_ADMIN_AUTH_FLAG} is unavailable in Deno Deploy.`);
  }

  return Object.freeze({ disableAdminAuth });
};

const normalizeHostname = (value: string): string => {
  const normalized = value.trim().toLowerCase();
  return normalized.startsWith("[") && normalized.endsWith("]") ? normalized.slice(1, -1) : normalized;
};

const isIpv4Loopback = (hostname: string): boolean => {
  const octets = hostname.split(".");
  if (octets.length !== 4) return false;
  const parsed = octets.map((octet) => Number(octet));
  return parsed.every((octet, index) =>
    Number.isInteger(octet) && octet >= 0 && octet <= 255 && String(octet) === octets[index]
  ) && parsed[0] === 127;
};

export const isLoopbackHostname = (value: string): boolean => {
  const hostname = normalizeHostname(value);
  return hostname === "localhost" || hostname === "::1" || hostname === "0:0:0:0:0:0:0:1" ||
    isIpv4Loopback(hostname);
};

const isNumericLoopbackHostname = (value: string): boolean => {
  const hostname = normalizeHostname(value);
  return hostname === "::1" || hostname === "0:0:0:0:0:0:0:1" || isIpv4Loopback(hostname);
};

const formatListenerAddress = (address: Deno.Addr): string => {
  if (address.transport === "tcp" || address.transport === "udp") {
    const hostname = address.hostname.includes(":") ? `[${address.hostname}]` : address.hostname;
    return `${address.transport}://${hostname}:${address.port}`;
  }
  if (address.transport === "unix") return `unix://${address.path}`;
  return address.transport;
};

export const shouldDisableAdminAuthForListener = (
  options: ServeRuntimeOptions,
  address: Deno.Addr,
): boolean => {
  if (!options.disableAdminAuth) return false;
  if (address.transport !== "tcp" || !isNumericLoopbackHostname(address.hostname)) {
    throw new Error(
      `[ai.ubq.fi] ${DISABLE_ADMIN_AUTH_FLAG} requires a loopback TCP listener; got ${formatListenerAddress(address)}.`,
    );
  }
  return true;
};

let adminAuthDisabled = false;
let adminAuthPeer: Deno.Addr | null = null;

export const configureAdminAuthForListener = (
  options: ServeRuntimeOptions,
  address: Deno.Addr,
): boolean => {
  const disabled = shouldDisableAdminAuthForListener(options, address);
  adminAuthDisabled = disabled;
  adminAuthPeer = null;
  return disabled;
};

/** Records the TCP peer of the request currently being handled by the server. */
export const configureAdminAuthPeerForRequest = (peer: Deno.Addr | null): void => {
  adminAuthPeer = peer;
};

const isLoopbackPeer = (peer: Deno.Addr): boolean =>
  peer.transport === "tcp" && isNumericLoopbackHostname(peer.hostname);

export const isAdminAuthDisabledForRequest = (request: Request): boolean => {
  if (!adminAuthDisabled) return false;
  // The bypass must be granted only to an actual loopback peer, never to a
  // forwarded, tunneled, or port-forwarded request whose URL hostname is
  // client-controlled. Fail closed when the peer is unknown (for example a
  // request constructed outside a Deno serve listener).
  const peer = adminAuthPeer;
  if (!peer || !isLoopbackPeer(peer)) return false;
  try {
    return isLoopbackHostname(new URL(request.url).hostname);
  } catch {
    return false;
  }
};
