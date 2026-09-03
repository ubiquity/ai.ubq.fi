import "./network.js";
import {
  buildBackendUrl,
  formatAuthSessionLabel,
  hasAuthPasskeyCredential,
  hasStoredPasskeyCredentials,
  isLocalDevelopmentOrigin,
  LOCAL_DEVELOPMENT_ADMIN_TOKEN,
  registerPasskey,
  resolveBackendBase,
  signInWithPasskey,
  signOut,
  storage,
  STORAGE_KEYS as AUTH_STORAGE_KEYS,
} from "./auth.js?v=passkey-relay-20260823-v5";
import {
  AUTH_RELAY_MESSAGE_TYPE,
  isTrustedAuthRelayClientOrigin,
  parseAuthRelayAction,
  parseTrustedAuthRelayOrigin,
} from "./auth-relay.js?v=passkey-relay-20260823-v4";
import { createAdminSnapshotCache } from "./admin-cache.js?v=admin-indexeddb-cache-20260830-v7";
import { bindForegroundRefresh } from "./foreground-refresh.js";
import { setReasoningPlaceholder, updateReasoningSelectForModel } from "./reasoning-select.js";
import { toast } from "./toast.js?v=20260903-toast-v1";

const STORAGE_KEYS = {
  rememberToken: AUTH_STORAGE_KEYS.rememberToken,
  token: AUTH_STORAGE_KEYS.token,
  passkeyHandle: AUTH_STORAGE_KEYS.passkeyHandle,
  passkeyCredentialIds: AUTH_STORAGE_KEYS.passkeyCredentialIds,
  expiresPreset: "uos_ai.admin.expires_preset",
  base: AUTH_STORAGE_KEYS.base,
  view: "uos_ai.admin.view",
};

const AUTH_RELAY_TIMEOUT_MS = 120_000;
const API_KEY_REQUEST_LOGS_LIMIT = 20;
const API_KEY_REQUEST_LOGS_TTL_MS = 10_000;
const LEGACY_ADMIN_CACHE_STORAGE_KEYS = [
  "uos_ai.admin.defaults_snapshot",
  "uos_ai.admin.defaults_models",
];
const adminSnapshotCache = createAdminSnapshotCache();
let adminCacheScope = "";
let adminCacheEpoch = 0;

for (const key of LEGACY_ADMIN_CACHE_STORAGE_KEYS) storage.remove(key);

const fetchWithCredentials = (input, init = {}) => {
  const headers = new Headers(init.headers);
  return globalThis.fetch(input, { ...init, headers, credentials: "include" });
};

const requestMethod = (input, init = {}) =>
  String(init.method ?? (input instanceof Request ? input.method : "GET")).toUpperCase();

const adminCacheKeyForRequest = (input, init = {}) => {
  if (!adminCacheScope || requestMethod(input, init) !== "GET") return "";
  let url;
  try {
    url = new URL(input instanceof Request ? input.url : String(input), globalThis.location.origin);
  } catch {
    return "";
  }
  if (!url.pathname.startsWith("/admin/") && url.pathname !== "/health/providers") return "";
  url.hash = "";
  url.searchParams.sort();
  return `${url.pathname}${url.search}`;
};

const isAdminMutation = (input, init = {}) => {
  const method = requestMethod(input, init);
  if (method === "GET" || method === "HEAD" || method === "OPTIONS") return false;
  try {
    const url = new URL(input instanceof Request ? input.url : String(input), globalThis.location.origin);
    return url.pathname.startsWith("/admin/");
  } catch {
    return false;
  }
};

const invalidateAdminSnapshotCache = () => {
  const scope = adminCacheScope;
  adminCacheEpoch += 1;
  clearApiKeyRequestLogCaches();
  if (scope) void adminSnapshotCache.clear(scope);
};

const clearAdminSnapshotScope = () => {
  const scope = adminCacheScope;
  adminCacheScope = "";
  adminCacheEpoch += 1;
  clearApiKeyRequestLogCaches();
  if (scope) void adminSnapshotCache.clear(scope);
};

const cacheFreshAdminRead = (response, cacheKey, scope, epoch) => {
  if (!response.ok || !cacheKey || !scope || epoch !== adminCacheEpoch || scope !== adminCacheScope) return;
  if (!response.headers.get("content-type")?.includes("application/json")) return;
  void response.clone().json().then((payload) => {
    if (epoch !== adminCacheEpoch || scope !== adminCacheScope) return;
    void adminSnapshotCache.write(scope, cacheKey, payload);
  }).catch(() => {});
};

const fetch = async (input, init = {}) => {
  const headers = new Headers(init.headers);
  if (!getAdminToken() || (isRemoteRelayOrigin() && relaySessionActive)) headers.delete("Authorization");
  const request = { ...init, headers };
  const cacheKey = adminCacheKeyForRequest(input, request);
  const scope = adminCacheScope;
  const epoch = adminCacheEpoch;
  const response = await fetchWithCredentials(input, request);
  if (cacheKey) cacheFreshAdminRead(response, cacheKey, scope, epoch);
  else if (response.ok && isAdminMutation(input, request)) invalidateAdminSnapshotCache();
  return response;
};

const debounce = (fn, wait = 450) => {
  let timer = null;
  return (...args) => {
    if (timer) clearTimeout(timer);
    timer = setTimeout(() => {
      timer = null;
      fn(...args);
    }, wait);
  };
};

const mustGet = (id) => {
  const el = document.getElementById(id);
  if (!el) throw new Error(`Missing element: #${id}`);
  return el;
};

const tokenInput = mustGet("admin-token");
const rememberTokenInput = mustGet("remember-admin-token");
const showTokenInput = mustGet("show-admin-token");
const authBadge = mustGet("admin-auth-badge");
const passkeyHandleInput = mustGet("admin-passkey-handle");
const passkeyLoginBtn = mustGet("admin-passkey-login");
const passkeyRegisterBtn = mustGet("admin-passkey-register");
const signOutBtn = mustGet("admin-sign-out");
const passkeyStatus = mustGet("admin-passkey-status");
const baseSelect = mustGet("admin-base");
const basePreview = mustGet("base-preview");
const authWidget = mustGet("view-session");
const authWidgetPanel = mustGet("auth-widget-panel");
const authWidgetToggle = mustGet("auth-widget-toggle");
const authWidgetToggleLabel = mustGet("auth-widget-toggle-label");
const authWidgetClose = mustGet("auth-widget-close");
const loadingTitle = mustGet("admin-loading-title");
const loadingSummary = mustGet("admin-loading-summary");
const loadingGrid = mustGet("admin-loading-grid");
const authGate = mustGet("admin-auth-gate");
const authGateOpen = mustGet("admin-auth-gate-open");
const loadingAuthStatus = mustGet("loading-auth-status");
const loadingKeysStatus = mustGet("loading-keys-status");
const loadingUsersStatus = mustGet("loading-users-status");
const loadingKernelStatus = mustGet("loading-kernel-status");
const loadingQueueStatus = mustGet("loading-queue-status");
const loadingPubkeysStatus = mustGet("loading-pubkeys-status");
const loadingDefaultsStatus = mustGet("loading-defaults-status");
const loadingUpstreamStatus = mustGet("loading-upstream-status");
const loadingProvidersStatus = mustGet("loading-providers-status");

const keyNameInput = mustGet("key-name");
const keyUsageLimitInput = mustGet("key-usage-limit");
const keyUsageWindowInput = mustGet("key-usage-window");
const keyExpiresSelect = mustGet("key-expires");
const keyPaidFallbackEnabledInput = mustGet("key-paid-fallback-enabled");
const keyPaidFallbackLimitInput = mustGet("key-paid-fallback-limit");
const keyPaidFallbackSettings = mustGet("key-paid-fallback-settings");
const createKeyBtn = mustGet("create-key");
const createBadge = mustGet("create-badge");
const createResult = mustGet("create-result");
const keyWindowPreset1m = mustGet("key-window-1m");
const keyWindowPreset1h = mustGet("key-window-1h");
const keyWindowPreset1d = mustGet("key-window-1d");
const keyWindowPreset1w = mustGet("key-window-1w");

const keysBadge = mustGet("keys-badge");
const keysList = mustGet("keys-list");
const passkeyUsersBadge = mustGet("passkey-users-badge");
const passkeyUsersList = mustGet("passkey-users-list");

const keysTabAll = mustGet("keys-tab-all");
const keysTabActive = mustGet("keys-tab-active");
const keysTabRevoked = mustGet("keys-tab-revoked");

const viewTabKeys = mustGet("view-tab-keys");
const viewTabUsers = mustGet("view-tab-users");
const viewTabKernel = mustGet("view-tab-kernel");
const viewTabPubkeys = mustGet("view-tab-pubkeys");
const viewTabDefaults = mustGet("view-tab-defaults");
const viewTabProviders = mustGet("view-tab-providers");
const viewTabErrors = mustGet("view-tab-errors");

const viewLoading = mustGet("view-loading");
const viewKeys = mustGet("view-keys");
const viewUsers = mustGet("view-users");
const viewKernel = mustGet("view-kernel");
const viewPubkeys = mustGet("view-pubkeys");
const viewDefaults = mustGet("view-defaults");
const viewProviders = mustGet("view-providers");
const viewErrors = mustGet("view-errors");

const errorsBadge = mustGet("errors-badge");
const errorsUpdated = mustGet("errors-updated");
const errorsList = mustGet("errors-list");

const providerCapacityBadge = mustGet("provider-capacity-badge");
const providerCapacityUpdated = mustGet("provider-capacity-updated");
const providerCapacityChart = mustGet("provider-capacity-chart");
const providerCapacityList = mustGet("provider-capacity-list");

const quotaRunwayBadge = mustGet("quota-runway-badge");
const quotaRunwayUpdated = mustGet("quota-runway-updated");
const quotaRunwaySummary = mustGet("quota-runway-summary");
const quotaRunwayList = mustGet("quota-runway-list");
const quotaRunwayNote = mustGet("quota-runway-note");

let currentKeyView = "active";
let currentAdminView = "loading";
let pendingAdminView = null;
let adminAccessState = { checked: false, isAdmin: false, isSuperAdmin: false };
let relaySessionActive = false;
let localDevelopmentAutoAuth = false;
let adminPrefetchRunId = 0;
let adminPrefetchSignature = "";
let adminPrefetchPromise = null;
let authWidgetAutoOpened = false;
let allKeys = [];
let keysLoading = false;
let keysLoadedAt = 0;
let providersLoading = false;
let providersLoadId = 0;
let providersLoadedAt = 0;
let providerCapacityLoading = false;
let providerCapacityLoadedAt = 0;
let providerCapacityLoadedForOpen = false;
let quotaProjectionLoading = false;
let quotaProjectionLoadedAt = 0;
let quotaProjectionLoadedForOpen = false;
let quotaProjectionLoadId = 0;
let latestProviderCapacityChartState = null;
let capacityChartResizeFrame = 0;
let capacityChartScrollState = null;
let latestProviderHealth = null;
let errorsLoading = false;
let errorsLoadId = 0;
let errorsLoadedAt = 0;
const apiKeyRequestLogCache = new Map();
const apiKeyRequestLogPromises = new Map();
const API_KEY_REQUEST_LOG_STATUS_OK = "OK";
const API_KEY_REQUEST_LOG_STATUS_ERROR = "Error";
const API_KEY_REQUEST_LOG_STATUS_UNAVAILABLE = "Unavailable";
const getApiKeyRequestLogCacheKey = (keyId) => {
  if (typeof keyId !== "string") return "";
  const normalized = keyId.trim();
  return normalized || "";
};
const clearApiKeyRequestLogCaches = () => {
  apiKeyRequestLogCache.clear();
  apiKeyRequestLogPromises.clear();
};
const isCurrentApiKeyRequestLogScope = (scope, epoch) =>
  Boolean(scope) && scope === adminCacheScope && epoch === adminCacheEpoch;
const isCurrentApiKeyRequestLogCacheEntry = (entry, scope, epoch) => entry?.scope === scope && entry?.epoch === epoch;
let passkeyUsers = [];
let passkeyUsersLoading = false;
let passkeyUsersLoadedAt = 0;

const accessApiKeys = mustGet("access-api-keys");
const accessGithubRepos = mustGet("access-github-repos");
const accessGithubQueue = mustGet("access-github-queue");
const accessKernelPubkeys = mustGet("access-kernel-pubkeys");
const accessUpstreamSource = mustGet("access-upstream-source");
const accessUpstreamExpiry = mustGet("access-upstream-expiry");

const defaultsModelSelect = mustGet("defaults-model");
const defaultsReasoningSelect = mustGet("defaults-reasoning");
const defaultsKernelLimitInput = mustGet("defaults-kernel-limit");
const defaultsKernelWindowInput = mustGet("defaults-kernel-window");
const defaultsKernelWindowPreset1m = mustGet("defaults-kernel-window-1m");
const defaultsKernelWindowPreset1h = mustGet("defaults-kernel-window-1h");
const defaultsKernelWindowPreset1d = mustGet("defaults-kernel-window-1d");
const defaultsKernelWindowPreset1w = mustGet("defaults-kernel-window-1w");
const defaultsBadge = mustGet("defaults-badge");
const meteredQuotaBadge = mustGet("metered-quota-badge");
const meteredQuotaRemaining = mustGet("metered-quota-remaining");
const meteredQuotaProgress = mustGet("metered-quota-progress");
const meteredQuotaBalance = mustGet("metered-quota-balance");
const meteredQuotaGranted = mustGet("metered-quota-granted");
const meteredQuotaTokenUsage = mustGet("metered-quota-token-usage");
const meteredQuotaBaseline = mustGet("metered-quota-baseline");
const meteredQuotaLatestRefill = mustGet("metered-quota-latest-refill");
const meteredQuotaInferredCredit = mustGet("metered-quota-inferred-credit");
const meteredQuotaCache = mustGet("metered-quota-cache");
const meteredQuotaConfidence = mustGet("metered-quota-confidence");
const meteredQuotaObserved = mustGet("metered-quota-observed");
const meteredQuotaCycleStarted = mustGet("metered-quota-cycle-started");
let defaultsLoaded = false;
let defaultsSaving = false;
let defaultsModelMap = new Map();
let defaultsTouched = false;
let defaultsLoadId = 0;
let defaultsLoadedAt = 0;

const kernelListBadge = mustGet("kernel-list-badge");
const kernelAttention = mustGet("kernel-attention");
const kernelList = mustGet("kernel-list");
const kernelQueueBadge = mustGet("kernel-queue-badge");
const kernelQueueList = mustGet("kernel-queue-list");
const kernelFilterInput = mustGet("kernel-filter");
const kernelShowSelect = mustGet("kernel-show");
const kernelSortSelect = mustGet("kernel-sort");
const kernelNewToggle = mustGet("kernel-new-toggle");
const kernelNewPanel = mustGet("kernel-new-panel");
const kernelNewOwnerInput = mustGet("kernel-new-owner");
const _kernelNewRepoField = mustGet("kernel-new-repo-field");
const kernelNewRepoInput = mustGet("kernel-new-repo");
const kernelNewLimitInput = mustGet("kernel-new-limit");
const kernelNewWindowInput = mustGet("kernel-new-window");
const kernelNewExpiresInput = mustGet("kernel-new-expires");
const kernelNewNeverInput = mustGet("kernel-new-never");
const kernelNewPreset1m = mustGet("kernel-new-window-1m");
const kernelNewPreset1h = mustGet("kernel-new-window-1h");
const kernelNewPreset1d = mustGet("kernel-new-window-1d");
const kernelNewPreset1w = mustGet("kernel-new-window-1w");
const kernelNewSaveBtn = mustGet("kernel-new-save");
const kernelNewBadge = mustGet("kernel-new-badge");
const kernelPubKeysBadge = mustGet("kernel-pubkeys-badge");
const kernelPubKeysList = mustGet("kernel-pubkeys-list");
const kernelPubKeyAppIdInput = mustGet("kernel-pubkey-app-id");
const kernelPubKeyOwnerInput = mustGet("kernel-pubkey-owner");
const kernelPubKeyPemInput = mustGet("kernel-pubkey-pem");
const kernelPubKeyCreateBtn = mustGet("kernel-pubkey-create");
const kernelPubKeyCreateBadge = mustGet("kernel-pubkey-create-badge");
let kernelListLoadId = 0;
let kernelListLoadedAt = 0;
let kernelNewSaving = false;
let kernelListRecords = { org: [], repo: [] };
let kernelQueueItems = [];
let kernelQueueLoading = false;
let kernelQueueLoadedAt = 0;
let kernelPubKeys = [];
let kernelPubKeysLoading = false;
let kernelPubKeysLoadedAt = 0;
let kernelPubKeysSaving = false;
const kernelOrgRepoAccordionState = new Map();
let kernelPolicyState = {
  available: false,
  message: "",
  orgAvailable: false,
  repoAvailable: false,
  orgLimits: new Map(),
  repoLimits: new Map(),
};

const setBadge = (badge, state, text) => {
  badge.dataset.state = state;
  badge.textContent = text;
};

const loadingStatusElements = {
  auth: loadingAuthStatus,
  keys: loadingKeysStatus,
  users: loadingUsersStatus,
  kernel: loadingKernelStatus,
  queue: loadingQueueStatus,
  pubkeys: loadingPubkeysStatus,
  defaults: loadingDefaultsStatus,
  upstream: loadingUpstreamStatus,
  providers: loadingProvidersStatus,
};

const setLoadingStatus = (key, state, text) => {
  const el = loadingStatusElements[key];
  if (!el) return;
  setBadge(el, state, text);
};

const setLoadingSummary = (text) => {
  loadingSummary.textContent = text;
};

const updateLoadingAuthStatus = () => {
  if (!adminAccessState.checked) {
    setLoadingStatus("auth", "unknown", "Checking");
    return;
  }
  if (adminAccessState.isSuperAdmin) {
    setLoadingStatus("auth", "ok", "Super admin");
    return;
  }
  if (adminAccessState.isAdmin) {
    setLoadingStatus("auth", "ok", "Admin");
    return;
  }
  setLoadingStatus("auth", "bad", "Sign in");
};

const syncLoadingGate = () => {
  const preparing = adminAccessState.isAdmin || (!adminAccessState.checked && hasAdminCredential());
  authGate.hidden = preparing;
  loadingGrid.hidden = !preparing;
  loadingTitle.textContent = preparing ? "Preparing admin console" : "Sign in to manage the gateway";
};

const resetLoadingPrefetchStatuses = (text = "Waiting") => {
  ["keys", "users", "kernel", "queue", "pubkeys", "defaults", "upstream", "providers"].forEach((key) => {
    setLoadingStatus(key, "unknown", text);
  });
};

const setAuthBadge = (state, text) => setBadge(authBadge, state, text);
const setPasskeyStatus = (state, text) => setBadge(passkeyStatus, state, text);

const setAuthWidgetOpen = (open, options = {}) => {
  const expanded = Boolean(open);
  authWidgetAutoOpened = expanded && options.auto === true;
  document.body.dataset.authWidget = expanded ? "expanded" : "collapsed";
  authWidget.setAttribute("aria-expanded", expanded ? "true" : "false");
  authWidgetPanel.hidden = !expanded;
  authWidgetToggle.setAttribute("aria-expanded", expanded ? "true" : "false");
  authWidgetToggle.setAttribute("aria-label", expanded ? "Collapse authentication" : "Open authentication");
  authWidgetToggle.dataset.tooltip = expanded ? "Collapse authentication" : "Open authentication";
  authWidgetToggleLabel.textContent = expanded ? "Close" : "Session";
  if (expanded && options.focus) {
    globalThis.requestAnimationFrame(() => authWidgetPanel.focus({ preventScroll: true }));
  }
};

const setSignedInState = (signedIn, options = {}) => {
  const deviceRegistered = options.deviceRegistered ?? hasStoredPasskeyCredentials();
  const canRegisterPasskey = options.canRegisterPasskey ?? false;
  passkeyLoginBtn.hidden = signedIn;
  passkeyRegisterBtn.hidden = isAuthRelayMode || isRemoteRelayOrigin() || deviceRegistered ||
    (signedIn && !canRegisterPasskey);
  signOutBtn.hidden = !signedIn;
  if (signedIn) setPasskeyStatus("ok", options.statusText ?? "Token active");
  else setPasskeyStatus("unknown", "Passkey idle");
};
const setCreateBadge = (state, text) => setBadge(createBadge, state, text);
const setKeysBadge = (state, text) => setBadge(keysBadge, state, text);
const setPasskeyUsersBadge = (state, text) => setBadge(passkeyUsersBadge, state, text);
const setDefaultsBadge = (state, text) => setBadge(defaultsBadge, state, text);
const setMeteredQuotaBadge = (state, text) => setBadge(meteredQuotaBadge, state, text);
const setKernelListBadge = (state, text) => setBadge(kernelListBadge, state, text);
const setKernelNewBadge = (state, text) => setBadge(kernelNewBadge, state, text);
const setKernelQueueBadge = (state, text) => setBadge(kernelQueueBadge, state, text);
const setKernelPubKeysBadge = (state, text) => setBadge(kernelPubKeysBadge, state, text);
const setKernelPubKeyCreateBadge = (state, text) => setBadge(kernelPubKeyCreateBadge, state, text);
const setKernelAttention = (text) => {
  if (!text) {
    kernelAttention.textContent = "";
    kernelAttention.hidden = true;
    return;
  }
  kernelAttention.textContent = text;
  kernelAttention.hidden = false;
};

const resetKernelPolicyState = () => {
  kernelPolicyState = {
    available: false,
    message: "",
    orgAvailable: false,
    repoAvailable: false,
    orgLimits: new Map(),
    repoLimits: new Map(),
  };
};

const getBaseChoice = () => (baseSelect.value === "ai" ? "ai" : "local");

const resolveBaseUrl = () => resolveBackendBase(getBaseChoice());

const apiUrl = (path) => {
  const endpoint = new URL(buildBackendUrl(path, resolveBaseUrl()));
  if (isRemoteRelayOrigin() && endpoint.origin === PASSKEY_CANONICAL_ORIGIN) {
    endpoint.searchParams.set("cors_origin", globalThis.location.origin);
  }
  return endpoint.toString();
};

const updateBasePreview = () => {
  basePreview.textContent = resolveBaseUrl();
};

const pageUrl = new URL(globalThis.location.href);
const authRelayOrigin = parseTrustedAuthRelayOrigin(pageUrl.searchParams.get("auth_relay_origin"));
const authRelayAction = parseAuthRelayAction(pageUrl.searchParams.get("auth_relay_action"));
const isAuthRelayMode = Boolean(authRelayOrigin && authRelayAction && globalThis.opener);

const getPasskeyBaseUrl = () => isAuthRelayMode ? globalThis.location.origin : resolveBaseUrl();

const PASSKEY_CANONICAL_ORIGIN = "https://ai.ubq.fi";
const isRemoteRelayOrigin = () => isTrustedAuthRelayClientOrigin(globalThis.location.origin);

const getAdminToken = () => tokenInput.value.trim();
const hasAdminCredential = () => Boolean(getAdminToken()) || relaySessionActive || adminAccessState.isAdmin;

const canUseLocalDevelopmentAuth = () => isLocalDevelopmentOrigin() && getBaseChoice() === "local";

const setAdminSnapshotScopeFromAuth = (auth) => {
  const method = typeof auth?.method?.kind === "string" ? auth.method.kind : "";
  const tokenFingerprint = typeof auth?.token?.sha256_12 === "string" ? auth.token.sha256_12 : "";
  const passkeyUserId = typeof auth?.method?.user?.id === "string" ? auth.method.user.id : "";
  const localDevelopmentPrincipal = method === "disabled" && canUseLocalDevelopmentAuth() ? "local-development" : "";
  const principal = tokenFingerprint || (passkeyUserId ? `${method}:${passkeyUserId}` : localDevelopmentPrincipal);
  if (!method || !principal) {
    clearAdminSnapshotScope();
    return false;
  }
  const target = new URL(resolveBaseUrl()).origin;
  const nextScope = `v1:${target}:${method}:${principal}`;
  if (adminCacheScope === nextScope) return true;
  clearAdminSnapshotScope();
  clearApiKeyRequestLogCaches();
  adminCacheScope = nextScope;
  return true;
};

const readAdminSnapshot = (path) => {
  const scope = adminCacheScope;
  if (!scope) return Promise.resolve(null);
  const cacheKey = adminCacheKeyForRequest(apiUrl(path));
  return cacheKey ? adminSnapshotCache.read(scope, cacheKey) : Promise.resolve(null);
};

const applyLocalDevelopmentAuth = () => {
  if (!canUseLocalDevelopmentAuth() || getAdminToken()) return false;
  tokenInput.value = LOCAL_DEVELOPMENT_ADMIN_TOKEN;
  localDevelopmentAutoAuth = true;
  return true;
};

const clearLocalDevelopmentAuth = () => {
  if (!localDevelopmentAutoAuth) return false;
  tokenInput.value = "";
  localDevelopmentAutoAuth = false;
  return true;
};

const isAdminAuthCheckPending = () => Boolean(getAdminToken()) && !adminAccessState.checked;

const openAuthWidgetForAuth = (options = {}) => setAuthWidgetOpen(true, { ...options, auto: true });

const closeAutoOpenedAuthWidget = () => {
  if (authWidgetAutoOpened) setAuthWidgetOpen(false);
};

const getPasskeyHandle = () => passkeyHandleInput.value.trim();

const logPasskeyUsername = (handle) => {
  const username = (handle ?? "").trim();
  if (username) console.info("[ai.ubq.fi] passkey username:", username);
};

const persistTokenIfEnabled = () => {
  if (!rememberTokenInput.checked) return;
  const token = getAdminToken();
  if (token) storage.set(STORAGE_KEYS.token, token);
  else storage.remove(STORAGE_KEYS.token);
};

const persistPasskeyHandle = () => {
  const handle = getPasskeyHandle();
  if (handle) storage.set(STORAGE_KEYS.passkeyHandle, handle);
  else storage.remove(STORAGE_KEYS.passkeyHandle);
};

const schedulePasskeyHandlePersist = debounce(() => {
  persistPasskeyHandle();
}, 500);

const setPasskeyHandleValue = (handle) => {
  passkeyHandleInput.value = handle ?? "";
  persistPasskeyHandle();
  logPasskeyUsername(handle);
};

const applySignedInToken = (token, options = {}) => {
  tokenInput.value = token;
  rememberTokenInput.checked = true;
  storage.set(STORAGE_KEYS.rememberToken, "1");
  storage.set(STORAGE_KEYS.token, token);
  setSignedInState(true, options);
  tokenInput.dispatchEvent(new Event("input", { bubbles: true }));
};

const postAuthRelayResult = (result) => {
  if (!isAuthRelayMode || result?.relay_session !== true) return false;
  globalThis.opener.postMessage({
    type: AUTH_RELAY_MESSAGE_TYPE,
    authenticated: true,
    handle: result.handle ?? getPasskeyHandle(),
    expires_at_ms: result.expires_at_ms ?? null,
  }, authRelayOrigin);
  setPasskeyStatus("ok", "Signed in. Returning to the requesting admin...");
  setTimeout(() => globalThis.close(), 300);
  return true;
};

const formatPasskeyLoginError = (error) => {
  const message = error?.message ?? "Passkey sign-in failed";
  if (
    !isAuthRelayMode && isLocalDevelopmentOrigin() &&
    /invalid passkey assertion|unknown passkey|passkey account not found|no passkeys registered/i.test(message)
  ) {
    return `${message}. This origin uses localhost passkeys; choose ai.ubq.fi or add a local passkey with the fallback token.`;
  }
  return message;
};

let authRelayRequest = null;
const requestRemotePasskeySession = () => {
  if (!isRemoteRelayOrigin()) {
    return Promise.reject(new Error("Remote passkey relay is available only from an approved Deno Deploy origin."));
  }
  if (authRelayRequest) return authRelayRequest;
  const targetOrigin = PASSKEY_CANONICAL_ORIGIN;
  const relayUrl = new URL("/admin", targetOrigin);
  relayUrl.searchParams.set("auth_relay_origin", globalThis.location.origin);
  relayUrl.searchParams.set("auth_relay_action", "passkey-login");

  const popup = globalThis.open(relayUrl.toString(), "uos_ai_admin_auth_relay", "popup,width=520,height=720");
  if (!popup) {
    return Promise.reject(new Error("Allow the ai.ubq.fi sign-in popup, then try again."));
  }

  authRelayRequest = new Promise((resolve, reject) => {
    let finished = false;
    let closedTimer = 0;
    let timeout = 0;
    const cleanup = () => {
      globalThis.removeEventListener("message", onMessage);
      if (closedTimer) globalThis.clearInterval(closedTimer);
      if (timeout) globalThis.clearTimeout(timeout);
    };
    const finish = (fn, value) => {
      if (finished) return;
      finished = true;
      cleanup();
      fn(value);
    };
    const onMessage = (event) => {
      if (event.origin !== targetOrigin) return;
      const data = event.data;
      if (!data || data.type !== AUTH_RELAY_MESSAGE_TYPE || data.authenticated !== true) return;
      try {
        popup.close();
      } catch {
        // ignore
      }
      finish(resolve, data);
    };
    globalThis.addEventListener("message", onMessage);
    closedTimer = globalThis.setInterval(() => {
      if (!popup.closed) return;
      finish(reject, new Error("The ai.ubq.fi sign-in window was closed."));
    }, 1000);
    timeout = globalThis.setTimeout(() => {
      finish(reject, new Error("Timed out waiting for ai.ubq.fi sign-in."));
    }, AUTH_RELAY_TIMEOUT_MS);
  }).finally(() => {
    authRelayRequest = null;
  });

  return authRelayRequest;
};

const signInAdminWithPasskey = async () => {
  if (!isAuthRelayMode && isRemoteRelayOrigin()) {
    const relay = await requestRemotePasskeySession();
    if (relay.handle) setPasskeyHandleValue(relay.handle);
    relaySessionActive = false;
    const authenticated = await testAdminToken({ allowBearerFallback: false });
    if (!authenticated || !relaySessionActive) {
      relaySessionActive = false;
      throw new Error("The ai.ubq.fi sign-in session was not established.");
    }
    setPasskeyStatus("ok", "Passkey signed in");
    return relay;
  }

  const result = await signInWithPasskey({
    baseUrl: getPasskeyBaseUrl(),
    handle: getPasskeyHandle(),
    useHandle: Boolean(getPasskeyHandle()),
    audienceOrigin: isAuthRelayMode ? authRelayOrigin : "",
  });
  if (result.handle) setPasskeyHandleValue(result.handle);
  if (isAuthRelayMode) {
    setPasskeyStatus("ok", "Passkey signed in");
    postAuthRelayResult(result);
    return result;
  }
  applySignedInToken(result.token, { deviceRegistered: true });
  setPasskeyStatus("ok", "Passkey signed in");
  return result;
};

const runPasskeyLogin = async ({ automatic = false } = {}) => {
  setPasskeyStatus("unknown", automatic ? "Starting passkey sign-in..." : "Signing in...");
  passkeyLoginBtn.disabled = true;
  passkeyRegisterBtn.disabled = true;
  try {
    return await signInAdminWithPasskey();
  } catch (error) {
    setSignedInState(false);
    const message = formatPasskeyLoginError(error);
    setPasskeyStatus(
      automatic ? "unknown" : "bad",
      automatic ? `${message} Click the sign-in button to continue.` : message,
    );
    return null;
  } finally {
    passkeyLoginBtn.disabled = false;
    passkeyRegisterBtn.disabled = false;
  }
};

const getRegistrationAdminToken = () => {
  const token = getAdminToken();
  if (!isRemoteRelayOrigin()) return token;
  if (relaySessionActive) return "";
  if (token) return token;
  throw new Error("Register a passkey from the ai.ubq.fi admin page.");
};

const restoreSettings = () => {
  const remember = storage.get(STORAGE_KEYS.rememberToken) === "1";
  rememberTokenInput.checked = remember;
  if (remember) tokenInput.value = storage.get(STORAGE_KEYS.token) ?? "";
  passkeyHandleInput.value = storage.get(STORAGE_KEYS.passkeyHandle) ?? "";
  keyExpiresSelect.value = storage.get(STORAGE_KEYS.expiresPreset) ?? "quarter";
  baseSelect.value = isRemoteRelayOrigin() ? "ai" : storage.get(STORAGE_KEYS.base) ?? "local";
  if (isRemoteRelayOrigin()) storage.set(STORAGE_KEYS.base, "ai");
  applyLocalDevelopmentAuth();
  updateBasePreview();
};

const clearCreateResult = () => {
  createResult.textContent = "";
  createResult.hidden = true;
};

const syncCreatePaidFallbackControls = () => {
  const enabled = keyPaidFallbackEnabledInput.checked;
  keyPaidFallbackSettings.hidden = !enabled;
  keyPaidFallbackLimitInput.disabled = !enabled;
  keyPaidFallbackLimitInput.required = enabled;
};

const clearKeysListLoading = () => {
  delete keysList.dataset.loading;
  keysList.removeAttribute("aria-busy");
};

const setKeysListLoading = () => {
  keysList.textContent = "";
  keysList.dataset.loading = "true";
  keysList.setAttribute("aria-busy", "true");
  for (let i = 0; i < 3; i++) {
    const row = document.createElement("article");
    row.dataset.keySkeleton = "row";
    row.setAttribute("aria-hidden", "true");

    const title = document.createElement("span");
    title.dataset.keySkeletonLine = "title";
    const meta = document.createElement("span");
    meta.dataset.keySkeletonLine = "meta";

    row.appendChild(title);
    row.appendChild(meta);
    keysList.appendChild(row);
  }
};

const setKeyListMessage = (text) => {
  clearKeysListLoading();
  keysList.textContent = "";
  const message = document.createElement("p");
  message.dataset.empty = "keys";
  message.textContent = text;
  keysList.appendChild(message);
};

const setPasskeyUsersMessage = (text) => {
  passkeyUsersList.textContent = "";
  const message = document.createElement("p");
  message.dataset.empty = "passkey-users";
  message.textContent = text;
  passkeyUsersList.appendChild(message);
};

const dateFormatter = new Intl.DateTimeFormat(undefined, {
  year: "numeric",
  month: "short",
  day: "2-digit",
  hour: "2-digit",
  minute: "2-digit",
});

const formatDate = (ms) => {
  if (typeof ms !== "number" || !Number.isFinite(ms)) return "unknown";
  return dateFormatter.format(new Date(ms));
};

const formatExpires = (ms) => (ms === -1 ? "Never" : formatDate(ms));

const isExpiredAt = (ms) => typeof ms === "number" && Number.isFinite(ms) && ms !== -1 && ms <= Date.now();

const numberFormatter = new Intl.NumberFormat();
const compactNumberFormatter = new Intl.NumberFormat(undefined, { notation: "compact", maximumFractionDigits: 1 });
const creditFormatter = new Intl.NumberFormat(undefined, {
  minimumFractionDigits: 0,
  maximumFractionDigits: 6,
});
const decimalFormatter = new Intl.NumberFormat(undefined, {
  minimumFractionDigits: 0,
  maximumFractionDigits: 6,
});
const quotaPercentFormatter = new Intl.NumberFormat(undefined, {
  minimumFractionDigits: 0,
  maximumFractionDigits: 2,
});
const MICROCREDITS_PER_CREDIT = 1_000_000;

const toNumber = (value) => {
  if (typeof value !== "number" || !Number.isFinite(value)) return 0;
  return Math.trunc(value);
};

const formatNumber = (value) => numberFormatter.format(toNumber(value));
const formatCompactNumber = (value) => compactNumberFormatter.format(toNumber(value));
const formatDecimal = (value) => {
  if (typeof value !== "number" || !Number.isFinite(value)) return "unknown";
  return decimalFormatter.format(value);
};
const formatCredits = (value) => {
  if (typeof value !== "number" || !Number.isFinite(value)) return "unknown";
  return `${creditFormatter.format(value)} credits`;
};
const formatPaidFallbackLimit = (value) => value === -1 ? "Unlimited" : formatCredits(value);
const formatMicrocreditsAsCredits = (value) => {
  if (typeof value !== "number" || !Number.isFinite(value)) return "unknown";
  return formatCredits(value / MICROCREDITS_PER_CREDIT);
};
const formatOptionalText = (value) => {
  if (typeof value !== "string") return "unknown";
  const trimmed = value.trim();
  return trimmed ? trimmed : "unknown";
};

const providerBadgeState = (state) => state === "healthy" ? "ok" : state === "unknown" ? "unknown" : "bad";

const providerStateLabel = (health) => {
  const state = formatOptionalText(health?.state);
  return health?.stale === true ? `${state} · stale` : state;
};

const appendProviderFact = (list, label, value) => {
  const item = document.createElement("div");
  const term = document.createElement("dt");
  const description = document.createElement("dd");
  term.textContent = label;
  description.textContent = value;
  item.append(term, description);
  list.appendChild(item);
};

const capacityBadgeState = (state) => state === "available" ? "ok" : state === "stale" ? "unknown" : "bad";

const capacityStateLabel = (state) => {
  if (state === "available") return "Live";
  if (state === "stale") return "Quota stale";
  return "Quota unavailable";
};

const formatCapacityTimestamp = (value, unavailable = "Not reported") =>
  typeof value === "number" && Number.isFinite(value) ? formatDate(value) : unavailable;

const formatCapacityPercent = (value) =>
  typeof value === "number" && Number.isFinite(value) ? `${quotaPercentFormatter.format(value)}%` : "Not reported";

const capacityRemainingPercent = (usedPercent) =>
  typeof usedPercent === "number" && Number.isFinite(usedPercent)
    ? Math.max(0, Math.min(100, 100 - usedPercent))
    : null;

const capacityProgressTone = (remainingPercent) => {
  if (remainingPercent === null) return "unavailable";
  if (remainingPercent <= 10) return "critical";
  if (remainingPercent <= 25) return "warning";
  return "healthy";
};

const renderCapacityWindow = (container, label, window) => {
  const card = document.createElement("section");
  card.dataset.capacityWindow = "";
  const title = document.createElement("h4");
  title.textContent = label;
  const remainingPercent = capacityRemainingPercent(window?.used_percent);
  const usage = document.createElement("div");
  usage.dataset.capacityUsage = "";
  const remaining = document.createElement("div");
  remaining.dataset.capacityRemaining = "";
  const remainingValue = document.createElement("strong");
  remainingValue.textContent = remainingPercent === null
    ? "Not reported"
    : `${formatCapacityPercent(remainingPercent)} remaining`;
  remaining.appendChild(remainingValue);
  usage.appendChild(remaining);
  const progress = document.createElement("div");
  progress.dataset.capacityProgress = "";
  progress.dataset.tone = capacityProgressTone(remainingPercent);
  progress.setAttribute("role", "progressbar");
  progress.setAttribute("aria-label", `${label} remaining capacity`);
  progress.setAttribute("aria-valuemin", "0");
  progress.setAttribute("aria-valuemax", "100");
  if (remainingPercent === null) {
    progress.dataset.state = "unavailable";
    progress.setAttribute("aria-valuetext", "Remaining capacity not reported");
  } else {
    progress.style.setProperty("--capacity-progress", String(remainingPercent / 100));
    progress.setAttribute("aria-valuenow", String(remainingPercent));
    progress.setAttribute("aria-valuetext", `${formatCapacityPercent(remainingPercent)} remaining`);
  }
  usage.appendChild(progress);
  const facts = document.createElement("dl");
  facts.dataset.capacityFacts = "";
  appendProviderFact(facts, "Used", formatCapacityPercent(window?.used_percent));
  appendProviderFact(
    facts,
    "Window",
    typeof window?.limit_window_seconds === "number"
      ? formatWindowShort(window.limit_window_seconds * 1000)
      : "Not reported",
  );
  appendProviderFact(facts, "Reset", formatCapacityTimestamp(window?.reset_at_ms));
  card.append(title, usage, facts);
  container.appendChild(card);
};

const capacityAdditionalLimitLabel = (limit) => {
  const name = typeof limit?.limit_name === "string" ? limit.limit_name.trim() : "";
  if (name) return name;
  const feature = typeof limit?.metered_feature === "string" ? limit.metered_feature.trim() : "";
  return feature ? `Additional limit · ${feature}` : "Additional limit";
};

const renderCapacityAdditionalLimit = (container, limit) => {
  const label = capacityAdditionalLimitLabel(limit);
  const primary = limit?.windows?.primary;
  const secondary = limit?.windows?.secondary;
  if (primary) renderCapacityWindow(container, `${label} · primary window`, primary);
  if (secondary) renderCapacityWindow(container, `${label} · secondary window`, secondary);
};

const capacityProviderStatus = (source, provider) => {
  const health = provider?.health ?? provider ?? null;
  const state = provider?.configured === false ? "unconfigured" : health?.state;
  const hasState = typeof state === "string" && state.trim().length > 0;
  let badgeState = capacityBadgeState(source.state);
  if (hasState) {
    badgeState = providerBadgeState(state);
    if (source.state !== "available" && badgeState === "ok") badgeState = "unknown";
  }
  const healthLabel = hasState ? providerStateLabel({ ...health, state }) : "";
  return {
    badgeState,
    label: healthLabel && healthLabel !== "unknown"
      ? `${healthLabel} · ${capacityStateLabel(source.state)}`
      : capacityStateLabel(source.state),
    health,
  };
};

const appendCapacitySourceMeta = (row, source, provider = null) => {
  const facts = document.createElement("dl");
  facts.dataset.capacityMeta = "";
  appendProviderFact(facts, "Observed", formatCapacityTimestamp(source.source_observed_at_ms));
  appendProviderFact(facts, "Snapshot", formatCapacityTimestamp(source.snapshot_at_ms));
  if (source.source === "codex") {
    const health = provider?.health ?? {};
    appendProviderFact(facts, "Last response", formatDate(health.last_observed_at_ms));
    appendProviderFact(
      facts,
      "HTTP status",
      typeof health.last_status === "number" ? String(health.last_status) : "Not observed",
    );
    appendProviderFact(facts, "Token expires", formatDate(provider?.access_token_exp_ms));
    appendProviderFact(
      facts,
      "Refresh",
      health.last_refresh_succeeded === true
        ? `Succeeded · ${formatDate(health.last_refresh_at_ms)}`
        : health.last_refresh_succeeded === false
        ? `Failed · ${formatDate(health.last_refresh_at_ms)}`
        : "Not observed",
    );
  } else if (source.source === "metered") {
    const status = capacityProviderStatus(source, provider);
    appendProviderFact(facts, "Inference", status.health ? providerStateLabel(status.health) : "Not observed");
    appendProviderFact(facts, "Last response", formatDate(status.health?.last_observed_at_ms));
    appendProviderFact(
      facts,
      "Cache",
      source.wallet?.cache_state ?? provider?.quota?.cache_state ?? "Unavailable",
    );
  }
  row.appendChild(facts);
};

const renderCodexCapacitySource = (source, provider = null) => {
  const row = document.createElement("article");
  row.dataset.capacitySource = "codex";
  row.dataset.state = source.state;
  row.setAttribute("role", "listitem");

  const header = document.createElement("header");
  const title = document.createElement("h3");
  title.textContent = typeof source.label === "string" && source.label.trim()
    ? source.label.trim()
    : `Codex account ${source.slot}`;
  const badge = document.createElement("span");
  badge.dataset.badge = "";
  const status = capacityProviderStatus(source, provider);
  setBadge(badge, status.badgeState, status.label);
  header.append(title, badge);

  const windows = document.createElement("div");
  windows.dataset.capacityWindows = "";
  renderCapacityWindow(windows, "Primary window", source.windows?.primary);
  if (source.windows?.secondary) renderCapacityWindow(windows, "Secondary window", source.windows.secondary);
  const additionalLimits = Array.isArray(source.additional_rate_limits) ? source.additional_rate_limits : [];
  for (const limit of additionalLimits) renderCapacityAdditionalLimit(windows, limit);
  row.append(header, windows);
  appendCapacitySourceMeta(row, source, provider);
  return row;
};

const renderMeteredCapacitySource = (source, provider = null) => {
  const row = document.createElement("article");
  row.dataset.capacitySource = "metered";
  row.dataset.state = source.state;
  row.setAttribute("role", "listitem");

  const header = document.createElement("header");
  const title = document.createElement("h3");
  title.textContent = "Metered 2";
  const badge = document.createElement("span");
  badge.dataset.badge = "";
  const status = capacityProviderStatus(source, provider);
  setBadge(badge, status.badgeState, status.label);
  header.append(title, badge);

  const facts = document.createElement("dl");
  facts.dataset.capacityFacts = "";
  const wallet = source.wallet ?? {};
  appendProviderFact(
    facts,
    "Available tokens",
    typeof wallet.total_available === "number" && Number.isFinite(wallet.total_available)
      ? numberFormatter.format(wallet.total_available)
      : "Not reported",
  );
  appendProviderFact(
    facts,
    "Granted tokens",
    typeof wallet.total_granted === "number" && Number.isFinite(wallet.total_granted)
      ? numberFormatter.format(wallet.total_granted)
      : "Not reported",
  );
  appendProviderFact(
    facts,
    "Refill remaining",
    wallet.unlimited_quota === true ? "Not applicable" : formatCapacityPercent(wallet.refill_cycle_remaining_percent),
  );
  appendProviderFact(
    facts,
    "Refill baseline",
    wallet.unlimited_quota === true
      ? "Not applicable"
      : wallet.baseline_credits === null
      ? "Not available"
      : formatCredits(wallet.baseline_credits),
  );
  appendProviderFact(
    facts,
    "Used tokens",
    typeof wallet.total_used === "number" && Number.isFinite(wallet.total_used)
      ? numberFormatter.format(wallet.total_used)
      : "Not reported",
  );
  appendProviderFact(facts, "Confidence", wallet.confidence ?? "Not available");
  appendProviderFact(facts, "Cycle started", formatCapacityTimestamp(wallet.cycle_started_at_ms));
  appendProviderFact(facts, "Reset", "Not provided for refill cycle");
  row.append(header, facts);
  appendCapacitySourceMeta(row, source, provider);
  return row;
};

const renderSurplusProviderHealthSource = (provider = null) => {
  const row = document.createElement("article");
  row.dataset.capacitySource = "surplus";
  row.dataset.state = provider?.configured === false ? "unavailable" : provider?.health?.state ?? "unknown";
  row.setAttribute("role", "listitem");

  const header = document.createElement("header");
  const title = document.createElement("h3");
  title.textContent = "Metered 1";
  const badge = document.createElement("span");
  badge.dataset.badge = "";
  const configured = provider?.configured === true;
  const state = configured ? provider?.health?.state ?? "unknown" : "unconfigured";
  setBadge(
    badge,
    configured ? providerBadgeState(state) : "bad",
    configured ? providerStateLabel(provider?.health) : "Not configured",
  );
  header.append(title, badge);

  const facts = document.createElement("dl");
  facts.dataset.capacityFacts = "";
  appendProviderFact(facts, "Configured", configured ? "Yes" : "No");
  appendProviderFact(facts, "Inference", configured ? providerStateLabel(provider?.health) : "Unavailable");
  appendProviderFact(facts, "Last response", formatDate(provider?.health?.last_observed_at_ms));
  appendProviderFact(facts, "Quota", provider?.quota?.available === true ? "Reported" : "Not reported");
  appendProviderFact(facts, "Usage", "Shown per API key");
  appendProviderFact(facts, "Settlement", "Response usage");
  row.append(header, facts);
  return row;
};

const providerForCodexSlot = (slot) => {
  const accounts = Array.isArray(latestProviderHealth?.codex?.accounts) ? latestProviderHealth.codex.accounts : [];
  return accounts.find((account) => String(account?.slot) === String(slot)) ?? null;
};

const renderProviderCapacityList = (sources) => {
  providerCapacityList.replaceChildren();
  for (const source of sources) {
    providerCapacityList.appendChild(
      source.source === "metered"
        ? renderMeteredCapacitySource(source, latestProviderHealth?.metered)
        : renderCodexCapacitySource(source, providerForCodexSlot(source.slot)),
    );
  }
  if (latestProviderHealth?.surplus) {
    providerCapacityList.appendChild(renderSurplusProviderHealthSource(latestProviderHealth.surplus));
  }
};

const unavailableCapacitySource = (source, slot = null) =>
  source === "metered"
    ? {
      source: "metered",
      state: "unavailable",
      source_observed_at_ms: null,
      snapshot_at_ms: null,
      wallet: {
        balance_credits: null,
        baseline_credits: null,
        refill_cycle_remaining_percent: null,
        refill_cycle_used_percent: null,
        unlimited_quota: null,
        total_available: null,
        total_granted: null,
        total_used: null,
        cycle_started_at_ms: null,
        last_credit_at_ms: null,
        confidence: null,
        cache_state: null,
      },
    }
    : {
      source: "codex",
      slot,
      state: "unavailable",
      source_observed_at_ms: null,
      snapshot_at_ms: null,
      windows: { primary: null, secondary: null },
    };

const CAPACITY_CHART_SVG_NS = "http://www.w3.org/2000/svg";
const CAPACITY_CHART_MIN_DAYS = 7;
const CAPACITY_CHART_MAX_DAYS = 14;
const CAPACITY_CHART_MIN_WIDTH_PX = 980;
const CAPACITY_CHART_PIXELS_PER_DAY = 150;
const CAPACITY_CHART_DAY_MS = 24 * 60 * 60 * 1_000;
const CAPACITY_CHART_HOUR_MS = 60 * 60 * 1_000;
const CAPACITY_CHART_MINUTE_MS = 60 * 1_000;
const CAPACITY_CHART_PLOT_HEIGHT = 100;
const CAPACITY_CHART_PLOT_LEFT = 48;
const CAPACITY_CHART_PLOT_TOP = 24;
const CAPACITY_CHART_PLOT_RIGHT = 12;
const CAPACITY_CHART_PLOT_BOTTOM = 56;
const CAPACITY_CHART_BUCKET_MS = 15 * CAPACITY_CHART_MINUTE_MS;
const CAPACITY_CHART_MAX_PIXELS_PER_PERCENT = 4;
const CAPACITY_CHART_MEDIUM_PIXELS_PER_PERCENT = 2;
const CAPACITY_CHART_MIN_PIXELS_PER_PERCENT = 1;
const CAPACITY_CHART_VIEWPORT_GAP_PX = 16;
const CAPACITY_CHART_FIGURE_OVERHEAD_PX = 48;
const CAPACITY_CHART_RESET_BAND_WIDTH_PX = 18;
const CAPACITY_CHART_RESET_MIN_GAIN_PERCENTAGE_POINTS = 25;
const CAPACITY_CHART_OPTIMAL_WEEK_MS = 7 * CAPACITY_CHART_DAY_MS;
const CAPACITY_CHART_DAY_FORMATTER = new Intl.DateTimeFormat(undefined, {
  weekday: "short",
  month: "short",
  day: "numeric",
});
const CAPACITY_CHART_DATE_TIME_FORMATTER = new Intl.DateTimeFormat(undefined, {
  month: "short",
  day: "numeric",
  hour: "numeric",
  minute: "2-digit",
});
const CAPACITY_CHART_SERIES = [
  { key: "available-capacity", label: "Codex capacity", source: "aggregate" },
  { key: "metered-refill", label: "Metered 2 refill", source: "metered", valueKey: "refill_cycle_remaining_percent" },
];

const capacityChartFailureIsDowntime = (source) =>
  source?.source === "codex" &&
  source?.state === "unavailable" &&
  (source.failure_kind === "upstream_error" || source.failure_kind === "unreachable") &&
  (source.failure_kind === "unreachable" ||
    (typeof source.failure_status === "number" && Number.isFinite(source.failure_status) &&
      source.failure_status >= 500 && source.failure_status <= 599));

const capacityChartSampleIsDowntime = (sample) => {
  if (!Array.isArray(sample?.sources)) return false;
  const codexSources = sample.sources.filter((source) => source?.source === "codex");
  const configuredCodexSources = codexSources.filter((source) => source?.failure_kind !== "not_configured");
  // A single failed account is still provider-side degradation. Do not let a
  // healthy sibling account hide the outage: the aggregate white path should
  // break for this sample and the red bridge should connect the surrounding
  // observed values.
  return configuredCodexSources.length > 0 && configuredCodexSources.some(capacityChartFailureIsDowntime);
};

const capacityChartDowntimeEventIsDowntime = (event) => {
  if (event?.provider !== "openai") return false;
  if (event.failure_kind === "unreachable") return true;
  return event.failure_kind === "upstream_error" &&
    typeof event.status === "number" && Number.isFinite(event.status) &&
    event.status >= 500 && event.status <= 599;
};

const capacityChartDowntimeEventTimes = (events, displayInterval) =>
  (Array.isArray(events) ? events : [])
    .filter((event) =>
      capacityChartDowntimeEventIsDowntime(event) &&
      typeof event.observed_at_ms === "number" && Number.isFinite(event.observed_at_ms) &&
      (!displayInterval ||
        (event.observed_at_ms >= displayInterval.startAtMs && event.observed_at_ms <= displayInterval.resetAtMs))
    )
    .map((event) => event.observed_at_ms)
    .sort((left, right) => left - right);

const capacityChartDowntimeEventBetween = (eventTimes, startAtMs, endAtMs) =>
  eventTimes.some((eventAtMs) => eventAtMs > startAtMs && eventAtMs <= endAtMs);

const capacityChartSampleBucketStart = (sample) => {
  if (typeof sample?.bucket_start_at_ms === "number" && Number.isFinite(sample.bucket_start_at_ms)) {
    return sample.bucket_start_at_ms;
  }
  const sampledAtMs = typeof sample?.sampled_at_ms === "number" ? sample.sampled_at_ms : sample?.sampledAtMs;
  if (typeof sampledAtMs === "number" && Number.isFinite(sampledAtMs)) {
    return Math.floor(sampledAtMs / CAPACITY_CHART_BUCKET_MS) * CAPACITY_CHART_BUCKET_MS;
  }
  return null;
};

const capacityChartSampleGapBetween = (left, right) => {
  const leftBucket = capacityChartSampleBucketStart(left);
  const rightBucket = capacityChartSampleBucketStart(right);
  return typeof leftBucket === "number" && typeof rightBucket === "number" &&
    rightBucket - leftBucket > CAPACITY_CHART_BUCKET_MS;
};

const capacityChartSvgElement = (name, attributes = {}) => {
  const element = document.createElementNS(CAPACITY_CHART_SVG_NS, name);
  for (const [attribute, value] of Object.entries(attributes)) element.setAttribute(attribute, String(value));
  return element;
};

const clampCapacityChartPercent = (value) => Math.max(0, Math.min(100, value));

const capacityChartTickConfig = (chartWindow, plotWidth = Number.POSITIVE_INFINITY) => {
  const maxTickCount = Number.isFinite(plotWidth) && plotWidth > 0
    ? Math.max(1, Math.floor(plotWidth / 96))
    : CAPACITY_CHART_MAX_DAYS;
  const durationDays = chartWindow.durationMs / CAPACITY_CHART_DAY_MS;
  if (durationDays < 1) {
    return {
      count: Math.max(
        1,
        Math.min(8, maxTickCount, Math.ceil(chartWindow.durationMs / (3 * CAPACITY_CHART_HOUR_MS))),
      ),
      formatter: CAPACITY_CHART_DATE_TIME_FORMATTER,
    };
  }
  return {
    count: Math.max(1, Math.min(CAPACITY_CHART_MAX_DAYS, maxTickCount, Math.ceil(durationDays))),
    formatter: CAPACITY_CHART_DAY_FORMATTER,
  };
};

const capacityChartIntervalLabel = (durationMs) => {
  const minutes = Math.max(1, Math.ceil(durationMs / CAPACITY_CHART_MINUTE_MS));
  if (minutes < 60) return `${minutes} minute${minutes === 1 ? "" : "s"}`;
  const hours = Math.max(1, Math.ceil(minutes / 60));
  if (hours < 24) return `${hours} hour${hours === 1 ? "" : "s"}`;
  const days = Math.max(1, Math.ceil(hours / 24));
  return `${days} day${days === 1 ? "" : "s"}`;
};

const capacityChartWindow = (window) => {
  const durationSeconds = window?.limit_window_seconds;
  const resetAtMs = window?.reset_at_ms;
  if (
    typeof durationSeconds !== "number" ||
    !Number.isFinite(durationSeconds) ||
    durationSeconds <= 0 ||
    typeof resetAtMs !== "number" ||
    !Number.isFinite(resetAtMs)
  ) return null;
  const durationMs = durationSeconds * 1_000;
  const startAtMs = resetAtMs - durationMs;
  if (!Number.isFinite(durationMs) || !Number.isFinite(startAtMs)) return null;
  return { startAtMs, resetAtMs, durationMs };
};

const capacityChartViewportWidth = () => {
  const chartStyles = getComputedStyle(providerCapacityChart);
  const horizontalPadding = (Number.parseFloat(chartStyles.paddingLeft) || 0) +
    (Number.parseFloat(chartStyles.paddingRight) || 0);
  const width = providerCapacityChart.getBoundingClientRect().width - horizontalPadding;
  return Number.isFinite(width) && width > 0
    ? Math.max(CAPACITY_CHART_PLOT_LEFT + CAPACITY_CHART_PLOT_RIGHT + 1, width)
    : 740;
};

const capacityChartIntrinsicWidth = (displayWindow) =>
  Math.max(
    capacityChartViewportWidth(),
    CAPACITY_CHART_MIN_WIDTH_PX,
    Math.ceil(
      ((displayWindow?.durationMs ?? CAPACITY_CHART_MIN_DAYS * CAPACITY_CHART_DAY_MS) /
        CAPACITY_CHART_DAY_MS) * CAPACITY_CHART_PIXELS_PER_DAY,
    ),
  );

const capacityChartPlotHeight = () => {
  const viewportHeight = Number.isFinite(globalThis.innerHeight) ? globalThis.innerHeight : 0;
  const availableHeight = Math.max(
    0,
    viewportHeight - CAPACITY_CHART_VIEWPORT_GAP_PX - CAPACITY_CHART_FIGURE_OVERHEAD_PX,
  );
  const plotChromeHeight = CAPACITY_CHART_PLOT_TOP + CAPACITY_CHART_PLOT_BOTTOM;
  const pixelsPerPercent = availableHeight >= plotChromeHeight + CAPACITY_CHART_MAX_PIXELS_PER_PERCENT * 100
    ? CAPACITY_CHART_MAX_PIXELS_PER_PERCENT
    : availableHeight >= plotChromeHeight + CAPACITY_CHART_MEDIUM_PIXELS_PER_PERCENT * 100
    ? CAPACITY_CHART_MEDIUM_PIXELS_PER_PERCENT
    : CAPACITY_CHART_MIN_PIXELS_PER_PERCENT;
  return CAPACITY_CHART_PLOT_HEIGHT * pixelsPerPercent;
};

// Keep the Codex pool and Metered wallet on separate series. Their percentages
// use different quota systems and must not be averaged into one value.
const capacityChartCodexAggregateRemainingPercent = (sample) => {
  const sources = Array.isArray(sample?.sources) ? sample.sources : [];
  const remaining = [];
  for (const source of sources) {
    if (source?.source !== "codex" || source?.state === "unavailable") continue;
    const usedPercent = source.windows?.primary?.used_percent;
    if (typeof usedPercent === "number" && Number.isFinite(usedPercent)) {
      remaining.push(capacityRemainingPercent(usedPercent));
    }
  }
  const reported = remaining.filter((value) => typeof value === "number" && Number.isFinite(value));
  return reported.length ? reported.reduce((total, value) => total + value, 0) / reported.length : null;
};

const capacityChartSpendPacing = (chartWindow, sources, nowMs) => {
  if (
    !chartWindow ||
    typeof chartWindow.startAtMs !== "number" ||
    !Number.isFinite(chartWindow.startAtMs) ||
    typeof chartWindow.durationMs !== "number" ||
    !Number.isFinite(chartWindow.durationMs) ||
    chartWindow.durationMs <= 0 ||
    typeof nowMs !== "number" ||
    !Number.isFinite(nowMs)
  ) {
    return {
      elapsedPercent: null,
      targetSpendPercent: null,
      targetRemainingPercent: null,
      currentSpendPercent: null,
      currentRemainingPercent: null,
      spendVariancePercent: null,
    };
  }

  const elapsedPercent = clampCapacityChartPercent(
    ((nowMs - chartWindow.startAtMs) / chartWindow.durationMs) * 100,
  );
  const targetRemainingPercent = clampCapacityChartPercent(100 - elapsedPercent);
  const currentRemainingPercent = capacityChartCodexAggregateRemainingPercent({ sources });
  const currentSpendPercent = currentRemainingPercent === null
    ? null
    : clampCapacityChartPercent(100 - currentRemainingPercent);
  return {
    elapsedPercent,
    targetSpendPercent: elapsedPercent,
    targetRemainingPercent,
    currentSpendPercent,
    currentRemainingPercent,
    spendVariancePercent: currentSpendPercent === null ? null : currentSpendPercent - elapsedPercent,
  };
};

const formatCapacitySpendDelta = (value) => {
  if (typeof value !== "number" || !Number.isFinite(value)) return "Not reported";
  if (Math.abs(value) < 0.01) return "On target";
  const direction = value > 0 ? "over target" : "under target";
  const sign = value > 0 ? "+" : "-";
  return `${sign}${quotaPercentFormatter.format(Math.abs(value))} pp ${direction}`;
};

const renderCapacitySpendSummary = (pacing, activeCycleWindow) => {
  const summary = document.createElement("aside");
  summary.dataset.capacitySpendSummary = "";
  summary.setAttribute("aria-label", "Optimal token spend pacing");

  const title = document.createElement("h4");
  title.textContent = "Optimal token spend";
  const description = document.createElement("p");
  description.textContent = "Linear pacing across the active usage period.";

  const metrics = document.createElement("dl");
  metrics.dataset.capacitySpendMetrics = "";
  const appendMetric = (label, value, detail) => {
    const item = document.createElement("div");
    const term = document.createElement("dt");
    term.textContent = label;
    const definition = document.createElement("dd");
    const primary = document.createElement("strong");
    primary.textContent = value;
    const secondary = document.createElement("small");
    secondary.textContent = detail;
    definition.append(primary, secondary);
    item.append(term, definition);
    metrics.appendChild(item);
  };

  appendMetric(
    "Target spend now",
    formatCapacityPercent(pacing.targetSpendPercent),
    `${formatCapacityPercent(pacing.targetRemainingPercent)} remaining on plan`,
  );
  appendMetric(
    "Current spend",
    formatCapacityPercent(pacing.currentSpendPercent),
    `${formatCapacityPercent(pacing.currentRemainingPercent)} remaining across Codex`,
  );
  appendMetric("Pacing", formatCapacitySpendDelta(pacing.spendVariancePercent), "Actual spend minus target");

  const note = document.createElement("small");
  note.dataset.capacitySpendNote = "";
  note.textContent = `100% → 0% remaining · ${capacityChartIntervalLabel(activeCycleWindow?.durationMs)}`;
  summary.append(title, description, metrics, note);
  return summary;
};

const capacityChartPoint = (
  sample,
  series,
  activeInterval = null,
  chartWindow = null,
) => {
  if (series.source === "aggregate") {
    const displayInterval = chartWindow ?? activeInterval;
    const sampledAtMs = sample?.sampled_at_ms;
    if (capacityChartSampleIsDowntime(sample)) return null;
    const remainingPercent = capacityChartCodexAggregateRemainingPercent(sample);
    if (
      !displayInterval ||
      typeof sampledAtMs !== "number" || !Number.isFinite(sampledAtMs) ||
      sampledAtMs < displayInterval.startAtMs || sampledAtMs > displayInterval.resetAtMs ||
      typeof remainingPercent !== "number" || !Number.isFinite(remainingPercent)
    ) return null;
    return {
      elapsedPercent: clampCapacityChartPercent(
        ((sampledAtMs - displayInterval.startAtMs) / displayInterval.durationMs) * 100,
      ),
      remainingPercent: clampCapacityChartPercent(remainingPercent),
    };
  }
  const source = Array.isArray(sample?.sources)
    ? sample.sources.find((candidate) =>
      candidate?.source === series.source && (series.source === "metered" || candidate.slot === series.slot)
    )
    : null;
  const window = series.source === "codex" ? source?.windows?.[series.windowKey] : null;
  const interval = series.source === "metered" ? chartWindow : activeInterval ?? capacityChartWindow(window);
  const displayInterval = chartWindow ?? interval;
  const reportedPercent = series.source === "metered" ? source?.wallet?.[series.valueKey] : window?.used_percent;
  const remainingPercent = series.source === "metered" ? reportedPercent : capacityRemainingPercent(reportedPercent);
  const sampledAtMs = sample?.sampled_at_ms;
  if (
    !source ||
    source?.state === "unavailable" ||
    !interval ||
    !displayInterval ||
    typeof reportedPercent !== "number" ||
    !Number.isFinite(reportedPercent) ||
    typeof remainingPercent !== "number" ||
    !Number.isFinite(remainingPercent) ||
    typeof sampledAtMs !== "number" ||
    !Number.isFinite(sampledAtMs) ||
    sampledAtMs < displayInterval.startAtMs ||
    sampledAtMs > displayInterval.resetAtMs
  ) return null;
  return {
    elapsedPercent: clampCapacityChartPercent(
      ((sampledAtMs - displayInterval.startAtMs) / displayInterval.durationMs) * 100,
    ),
    remainingPercent: clampCapacityChartPercent(remainingPercent),
  };
};

const capacityChartResetPoint = (event, series, activeInterval = null, chartWindow = null) => {
  if (series.source !== "aggregate") return null;
  const interval = activeInterval ?? chartWindow;
  const displayInterval = chartWindow ?? interval;
  const sampledAtMs = event?.observed_at_ms;
  if (
    !interval || !displayInterval ||
    typeof sampledAtMs !== "number" || !Number.isFinite(sampledAtMs) ||
    sampledAtMs < displayInterval.startAtMs || sampledAtMs > displayInterval.resetAtMs
  ) return null;
  return {
    elapsedPercent: clampCapacityChartPercent(
      ((sampledAtMs - displayInterval.startAtMs) / displayInterval.durationMs) * 100,
    ),
    remainingPercent: 0,
  };
};

const capacityChartSeriesPoints = (
  history,
  series,
  activeInterval,
  chartWindow,
  currentPoint,
  nowMs,
  resetEvents,
  downtimeEvents,
) => {
  const eventTimes = series.source === "aggregate"
    ? capacityChartDowntimeEventTimes(downtimeEvents, chartWindow ?? activeInterval)
    : [];
  const runs = [];
  let run = [];
  let previousSample = null;
  const pushRun = () => {
    if (run.length) runs.push(run);
    run = [];
  };
  for (const sample of [...history].sort((left, right) => (left?.sampled_at_ms ?? 0) - (right?.sampled_at_ms ?? 0))) {
    const sampledAtMs = sample?.sampled_at_ms;
    if (
      previousSample && typeof sampledAtMs === "number" &&
      (capacityChartSampleGapBetween(previousSample, sample) ||
        capacityChartDowntimeEventBetween(eventTimes, previousSample.sampled_at_ms, sampledAtMs))
    ) pushRun();
    const point = capacityChartPoint(sample, series, activeInterval, chartWindow);
    if (!point) {
      pushRun();
      previousSample = typeof sampledAtMs === "number" ? sample : previousSample;
      continue;
    }
    run.push({ sampledAtMs, point, synthetic: false });
    previousSample = typeof sampledAtMs === "number" ? sample : previousSample;
  }
  if (
    currentPoint && previousSample &&
    (capacityChartSampleGapBetween(previousSample, { sampled_at_ms: nowMs }) ||
      capacityChartDowntimeEventBetween(eventTimes, previousSample.sampled_at_ms, nowMs))
  ) pushRun();
  if (currentPoint) run.push({ sampledAtMs: nowMs, point: currentPoint, synthetic: false });
  pushRun();

  for (const event of Array.isArray(resetEvents) ? resetEvents : []) {
    const point = capacityChartResetPoint(event, series, activeInterval, chartWindow);
    if (!point) continue;
    const sampledAtMs = event.observed_at_ms;
    let target = runs.find((candidate) =>
      sampledAtMs >= candidate[0].sampledAtMs && sampledAtMs <= candidate[candidate.length - 1].sampledAtMs
    );
    if (!target && runs.length) {
      target = sampledAtMs >= runs[runs.length - 1][runs[runs.length - 1].length - 1].sampledAtMs
        ? runs[runs.length - 1]
        : runs[0];
    }
    if (!target) {
      target = [];
      runs.push(target);
    }
    target.push({ sampledAtMs, point, synthetic: true });
  }

  for (const candidate of runs) {
    candidate.sort((left, right) =>
      left.sampledAtMs - right.sampledAtMs || Number(left.synthetic) - Number(right.synthetic)
    );
  }
  runs.sort((left, right) => left[0].sampledAtMs - right[0].sampledAtMs);
  return runs.flatMap((candidate, index) => [
    ...(index === 0 ? [] : [null]),
    ...candidate.map(({ point }) => point),
  ]);
};

const capacityChartDowntimeBridges = (
  history,
  series,
  activeInterval,
  chartWindow,
  currentPoint,
  nowMs,
  downtimeEvents,
) => {
  if (series.source !== "aggregate") return [];
  const displayInterval = chartWindow ?? activeInterval;
  const samples = [...history].sort((left, right) => (left?.sampled_at_ms ?? 0) - (right?.sampled_at_ms ?? 0)).map((
    sample,
  ) => ({
    sampledAtMs: sample?.sampled_at_ms,
    downtime: capacityChartSampleIsDowntime(sample),
    point: capacityChartPoint(sample, series, activeInterval, chartWindow),
  }));
  if (currentPoint) samples.push({ sampledAtMs: nowMs, downtime: false, point: currentPoint });

  const validSamples = samples.filter((sample) =>
    typeof sample.sampledAtMs === "number" && sample.point &&
    (!displayInterval ||
      (sample.sampledAtMs >= displayInterval.startAtMs && sample.sampledAtMs <= displayInterval.resetAtMs))
  );
  const markerTimes = [
    ...samples
      .filter((sample) =>
        sample.downtime && typeof sample.sampledAtMs === "number" &&
        (!displayInterval ||
          (sample.sampledAtMs >= displayInterval.startAtMs && sample.sampledAtMs <= displayInterval.resetAtMs))
      )
      .map((sample) => sample.sampledAtMs),
    ...capacityChartDowntimeEventTimes(downtimeEvents, displayInterval),
  ].sort((left, right) => left - right);

  const bridges = [];
  const seen = new Set();
  const addBridge = (left, right) => {
    if (!left?.point || !right?.point) return;
    const key = `${left.sampledAtMs}:${right.sampledAtMs}`;
    if (seen.has(key)) return;
    seen.add(key);
    bridges.push([left.point, right.point]);
  };

  // A missing 15-minute bucket is an unobserved interval. Keep it out of the
  // white path and show the connecting segment in the outage colour, even if
  // the older sample predates explicit downtime event recording.
  for (let index = 1; index < validSamples.length; index += 1) {
    const left = validSamples[index - 1];
    const right = validSamples[index];
    if (capacityChartSampleGapBetween(left, right)) addBridge(left, right);
  }

  for (const markerAtMs of markerTimes) {
    const left = [...validSamples].reverse().find((sample) => sample.sampledAtMs < markerAtMs);
    const right = validSamples.find((sample) => sample.sampledAtMs > markerAtMs);
    addBridge(left, right);
  }
  return bridges;
};

const capacityChartBridgePath = (bridges, plot) =>
  bridges.map(([from, to]) => {
    const fromX = plot.left + (from.elapsedPercent / 100) * plot.width;
    const fromY = plot.top + ((100 - from.remainingPercent) / 100) * plot.height;
    const toX = plot.left + (to.elapsedPercent / 100) * plot.width;
    const toY = plot.top + ((100 - to.remainingPercent) / 100) * plot.height;
    return `M${fromX.toFixed(2)} ${fromY.toFixed(2)} L${toX.toFixed(2)} ${toY.toFixed(2)}`;
  }).join(" ");

const capacityChartDowntimeBandCoordinates = (bridge, plot) => {
  const [from, to] = bridge ?? [];
  if (!from || !to) return null;
  const fromX = plot.left + (from.elapsedPercent / 100) * plot.width;
  const toX = plot.left + (to.elapsedPercent / 100) * plot.width;
  const left = Math.max(plot.left, Math.min(fromX, toX));
  const right = Math.min(plot.left + plot.width, Math.max(fromX, toX));
  if (!Number.isFinite(left) || !Number.isFinite(right) || right <= left) return null;
  return { x: left, width: right - left };
};

const capacityChartPath = (points, plot, options = {}) => {
  const anchorStart = options.anchorStart !== false;
  const anchorEnd = options.anchorEnd !== false;
  const runs = [];
  let run = [];
  for (const point of points) {
    if (!point) {
      if (run.length) runs.push(run);
      run = [];
      continue;
    }
    run.push(point);
  }
  if (run.length) runs.push(run);
  if (!runs.length) return "";

  let path = "";
  runs.forEach((runPoints, runIndex) => {
    const anchored = [];
    if (runIndex === 0 && anchorStart) anchored.push({ elapsedPercent: 0, remainingPercent: 100 });
    anchored.push(...runPoints);
    if (runIndex === runs.length - 1 && anchorEnd) {
      const last = runPoints[runPoints.length - 1];
      anchored.push({ elapsedPercent: 100, remainingPercent: last.remainingPercent });
    }

    let cursorX = Number.NEGATIVE_INFINITY;
    let connected = false;
    for (const point of anchored) {
      const x = plot.left + (point.elapsedPercent / 100) * plot.width;
      const y = plot.top + ((100 - point.remainingPercent) / 100) * plot.height;
      if (x < cursorX) continue;
      if (!connected) path += `M${x.toFixed(2)} ${y.toFixed(2)}`;
      path += ` H${x.toFixed(2)} V${y.toFixed(2)}`;
      cursorX = x;
      connected = true;
    }
  });
  return path;
};

const capacityChartActiveUsageWindow = (sources, nowMs) => {
  const codexWindows = sources
    .filter((source) => source?.source === "codex" && source.state !== "unavailable")
    .flatMap((source) => [source.windows?.primary, source.windows?.secondary])
    .map((window) => capacityChartWindow(window))
    .filter((interval) => interval !== null);
  const primaryWindows = sources
    .filter((source) => source?.source === "codex" && source.state !== "unavailable")
    .map((source) => capacityChartWindow(source.windows?.primary))
    .filter((interval) => interval !== null);
  // Primary is the account usage period. Secondary windows remain separate
  // series, but must not extend the shared axis into a later reset date.
  const usageWindows = primaryWindows.length > 0 ? primaryWindows : codexWindows;
  if (usageWindows.length > 0) {
    const startAtMs = Math.min(...usageWindows.map((interval) => interval.startAtMs));
    const latestResetAtMs = Math.max(...usageWindows.map((interval) => interval.resetAtMs));
    const durationMs = latestResetAtMs - startAtMs;
    return { startAtMs, resetAtMs: startAtMs + durationMs, durationMs };
  }
  return {
    startAtMs: nowMs - CAPACITY_CHART_MIN_DAYS * CAPACITY_CHART_DAY_MS,
    resetAtMs: nowMs,
    durationMs: CAPACITY_CHART_MIN_DAYS * CAPACITY_CHART_DAY_MS,
  };
};

const capacityChartHistoryWindow = (nowMs) => ({
  startAtMs: nowMs - CAPACITY_CHART_MIN_DAYS * CAPACITY_CHART_DAY_MS,
  resetAtMs: nowMs,
  durationMs: CAPACITY_CHART_MIN_DAYS * CAPACITY_CHART_DAY_MS,
});

const capacityChartInferredRateLimitResetMarkers = (history, displayWindow, downtimeEvents = []) => {
  if (!displayWindow) return [];
  const downtimeTimes = capacityChartDowntimeEventTimes(downtimeEvents, displayWindow);
  const samples = (Array.isArray(history) ? history : [])
    .filter((sample) => typeof sample?.sampled_at_ms === "number" && Number.isFinite(sample.sampled_at_ms))
    .sort((left, right) => left.sampled_at_ms - right.sampled_at_ms);
  const markers = [];
  for (let index = 1; index < samples.length; index += 1) {
    const previous = samples[index - 1];
    const current = samples[index];
    if (
      current.sampled_at_ms <= previous.sampled_at_ms || capacityChartSampleGapBetween(previous, current) ||
      capacityChartDowntimeEventBetween(downtimeTimes, previous.sampled_at_ms, current.sampled_at_ms)
    ) continue;
    for (const slot of [1, 2]) {
      const previousSource = previous.sources?.find((source) => source?.source === "codex" && source.slot === slot);
      const currentSource = current.sources?.find((source) => source?.source === "codex" && source.slot === slot);
      if (
        !previousSource || !currentSource || previousSource.state === "unavailable" ||
        currentSource.state === "unavailable" ||
        !/^[a-f0-9]{64}$/.test(previousSource.account_cohort_id ?? "") ||
        previousSource.account_cohort_id !== currentSource.account_cohort_id
      ) continue;
      for (const window of ["primary", "secondary"]) {
        const previousWindow = previousSource.windows?.[window];
        const currentWindow = currentSource.windows?.[window];
        const previousUsedPercent = previousWindow?.used_percent;
        const currentUsedPercent = currentWindow?.used_percent;
        const previousResetAtMs = previousWindow?.reset_at_ms;
        const resetAtMs = currentWindow?.reset_at_ms;
        if (
          typeof previousUsedPercent !== "number" || !Number.isFinite(previousUsedPercent) ||
          typeof currentUsedPercent !== "number" || !Number.isFinite(currentUsedPercent) ||
          typeof previousResetAtMs !== "number" || !Number.isFinite(previousResetAtMs) ||
          typeof resetAtMs !== "number" || !Number.isFinite(resetAtMs) || resetAtMs <= previousResetAtMs
        ) continue;
        const capacityGain = previousUsedPercent - currentUsedPercent;
        if (
          capacityGain < CAPACITY_CHART_RESET_MIN_GAIN_PERCENTAGE_POINTS ||
          current.sampled_at_ms < displayWindow.startAtMs || current.sampled_at_ms > displayWindow.resetAtMs
        ) continue;
        markers.push({
          v: 1,
          event_id: `history-openai-${slot}-${window}-${previous.sampled_at_ms}-${current.sampled_at_ms}`,
          provider: "openai",
          slot,
          window,
          observed_at_ms: current.sampled_at_ms,
          previous_sampled_at_ms: previous.sampled_at_ms,
          previous_reset_at_ms: previousResetAtMs,
          reset_at_ms: resetAtMs,
          previous_used_percent: previousUsedPercent,
          current_used_percent: currentUsedPercent,
          capacity_gain_percentage_points: capacityGain,
          inferred_from_history: true,
        });
      }
    }
  }
  return markers;
};

const capacityChartRateLimitResetMarkers = (events, history, displayWindow, downtimeEvents = []) => {
  if (!displayWindow) return [];
  const inferredEvents = capacityChartInferredRateLimitResetMarkers(history, displayWindow, downtimeEvents);
  const recordedEvents = (Array.isArray(events) ? events : []).filter((event) => {
    const observedAtMs = event?.observed_at_ms;
    const previousSampledAtMs = event?.previous_sampled_at_ms;
    const previousResetAtMs = event?.previous_reset_at_ms;
    const resetAtMs = event?.reset_at_ms;
    const previousUsedPercent = event?.previous_used_percent;
    const currentUsedPercent = event?.current_used_percent;
    const capacityGain = event?.capacity_gain_percentage_points;
    return event?.provider === "openai" &&
      (event.slot === 1 || event.slot === 2) &&
      (event.window === "primary" || event.window === "secondary") &&
      typeof observedAtMs === "number" && Number.isFinite(observedAtMs) &&
      typeof previousSampledAtMs === "number" && Number.isFinite(previousSampledAtMs) &&
      previousSampledAtMs < observedAtMs &&
      typeof previousResetAtMs === "number" && Number.isFinite(previousResetAtMs) &&
      typeof resetAtMs === "number" && Number.isFinite(resetAtMs) &&
      resetAtMs > previousResetAtMs &&
      typeof previousUsedPercent === "number" && Number.isFinite(previousUsedPercent) &&
      previousUsedPercent >= 0 && previousUsedPercent <= 100 &&
      typeof currentUsedPercent === "number" && Number.isFinite(currentUsedPercent) &&
      currentUsedPercent >= 0 && currentUsedPercent <= 100 &&
      typeof capacityGain === "number" && Number.isFinite(capacityGain) &&
      Math.abs(previousUsedPercent - currentUsedPercent - capacityGain) <= 0.001 &&
      capacityGain >= CAPACITY_CHART_RESET_MIN_GAIN_PERCENTAGE_POINTS &&
      observedAtMs >= displayWindow.startAtMs && observedAtMs <= displayWindow.resetAtMs;
  });
  const markers = new Map();
  const candidates = [
    ...inferredEvents.filter((inferred) =>
      !recordedEvents.some((recorded) =>
        recorded?.slot === inferred.slot && recorded?.window === inferred.window &&
        typeof recorded?.observed_at_ms === "number" &&
        recorded.observed_at_ms > inferred.previous_sampled_at_ms &&
        recorded.observed_at_ms <= inferred.observed_at_ms
      )
    ),
    ...recordedEvents,
  ];
  for (const event of candidates) {
    const observedAtMs = event?.observed_at_ms;
    if (
      event?.provider !== "openai" ||
      (event.slot !== 1 && event.slot !== 2) ||
      (event.window !== "primary" && event.window !== "secondary") ||
      typeof observedAtMs !== "number" || !Number.isFinite(observedAtMs) ||
      typeof event.capacity_gain_percentage_points !== "number" ||
      !Number.isFinite(event.capacity_gain_percentage_points) ||
      event.capacity_gain_percentage_points < CAPACITY_CHART_RESET_MIN_GAIN_PERCENTAGE_POINTS ||
      observedAtMs < displayWindow.startAtMs || observedAtMs > displayWindow.resetAtMs
    ) continue;
    const key = `${event.slot}:${event.window}:${observedAtMs}`;
    markers.set(key, event);
  }
  return [...markers.values()].sort((left, right) =>
    left.observed_at_ms - right.observed_at_ms ||
    String(left.event_id ?? "").localeCompare(String(right.event_id ?? ""))
  );
};

const capacityChartMarkerX = (observedAtMs, chartWindow, plot) =>
  plot.left + ((observedAtMs - chartWindow.startAtMs) / chartWindow.durationMs) * plot.width;

const capacityChartPromptCacheBuckets = (snapshot, chartWindow) => {
  const bucketMs = snapshot?.prompt_cache?.bucket_ms;
  if (
    snapshot?.prompt_cache?.status !== "ready" ||
    bucketMs !== CAPACITY_CHART_BUCKET_MS ||
    !Array.isArray(snapshot?.prompt_cache?.buckets)
  ) return [];
  const buckets = [];
  for (const bucket of snapshot.prompt_cache.buckets) {
    const bucketStartAtMs = bucket?.bucket_start_at_ms;
    const inputTokens = bucket?.input_tokens;
    const cachedInputTokens = bucket?.cached_input_tokens;
    const cacheWriteFieldsMissing = bucket?.cache_write_input_tokens === undefined &&
      bucket?.cache_write_reported_sample_count === undefined;
    const cacheWriteInputTokens = cacheWriteFieldsMissing ? null : bucket?.cache_write_input_tokens;
    const cacheWriteReportedSampleCount = cacheWriteFieldsMissing ? 0 : bucket?.cache_write_reported_sample_count;
    const sampleCount = bucket?.sample_count;
    if (
      typeof bucketStartAtMs !== "number" || !Number.isFinite(bucketStartAtMs) ||
      bucketStartAtMs < chartWindow.startAtMs || bucketStartAtMs > chartWindow.resetAtMs ||
      typeof inputTokens !== "number" || !Number.isSafeInteger(inputTokens) || inputTokens <= 0 ||
      typeof cachedInputTokens !== "number" || !Number.isSafeInteger(cachedInputTokens) ||
      cachedInputTokens < 0 || cachedInputTokens > inputTokens ||
      (cacheWriteInputTokens !== null &&
        (typeof cacheWriteInputTokens !== "number" || !Number.isSafeInteger(cacheWriteInputTokens) ||
          cacheWriteInputTokens < 0)) ||
      typeof cacheWriteReportedSampleCount !== "number" || !Number.isSafeInteger(cacheWriteReportedSampleCount) ||
      cacheWriteReportedSampleCount < 0 || cacheWriteReportedSampleCount > sampleCount ||
      (cacheWriteInputTokens === null) !== (cacheWriteReportedSampleCount === 0) ||
      typeof sampleCount !== "number" || !Number.isSafeInteger(sampleCount) || sampleCount <= 0
    ) continue;
    buckets.push({
      bucketStartAtMs,
      bucketEndAtMs: bucketStartAtMs + bucketMs,
      inputTokens,
      cachedInputTokens,
      cacheWriteInputTokens,
      cacheWriteReportedSampleCount,
      sampleCount,
      cachedPercent: clampCapacityChartPercent((cachedInputTokens / inputTokens) * 100),
    });
  }
  return buckets.sort((left, right) => left.bucketStartAtMs - right.bucketStartAtMs);
};

const capacityChartScrollBehavior = () =>
  globalThis.matchMedia?.("(prefers-reduced-motion: reduce)")?.matches ? "auto" : "smooth";

const capacityChartScrollAmount = (scroll) => Math.max(80, Math.round(scroll.clientWidth * 0.8));

const capacityChartScrollMaximum = (scroll) =>
  Math.max(0, scroll.scrollWidth - Math.max(scroll.clientWidth, scroll.offsetWidth || 0));

const updateCapacityChartScrollControls = (scroll, olderButton, newerButton) => {
  const maximum = capacityChartScrollMaximum(scroll);
  olderButton.disabled = maximum <= 1 || scroll.scrollLeft <= 1;
  newerButton.disabled = maximum <= 1 || maximum - scroll.scrollLeft <= 1;
};

const rememberCapacityChartScroll = () => {
  const current = providerCapacityChart.querySelector("[data-capacity-chart-scroll]");
  const svg = current?.querySelector("[data-capacity-chart-svg]");
  if (!current || !svg) return;
  const maximum = capacityChartScrollMaximum(current);
  const scrollLeft = Number.isFinite(current.scrollLeft) ? current.scrollLeft : 0;
  const clientWidth = Number.isFinite(current.clientWidth) ? current.clientWidth : 0;
  const startAtMs = Number(svg.dataset.capacityChartStartAtMs);
  const durationMs = Number(svg.dataset.capacityChartDurationMs);
  const plotLeft = Number(svg.dataset.capacityChartPlotLeft);
  const plotWidth = Number(svg.dataset.capacityChartPlotWidth);
  const atEnd = maximum <= 1 || maximum - scrollLeft <= 2;
  capacityChartScrollState = {
    atEnd,
    anchorAtMs: !atEnd && clientWidth > 0 && Number.isFinite(startAtMs) && Number.isFinite(durationMs) &&
        durationMs > 0 && Number.isFinite(plotLeft) && Number.isFinite(plotWidth) && plotWidth > 0
      ? startAtMs + ((scrollLeft + clientWidth / 2 - plotLeft) / plotWidth) * durationMs
      : null,
  };
};

const restoreCapacityChartScroll = (scroll, displayWindow, plot) => {
  const maximum = capacityChartScrollMaximum(scroll);
  const state = capacityChartScrollState;
  let nextScrollLeft = maximum;
  if (!state?.atEnd && typeof state?.anchorAtMs === "number" && Number.isFinite(state.anchorAtMs)) {
    const markerX = capacityChartMarkerX(state.anchorAtMs, displayWindow, plot);
    nextScrollLeft = markerX - scroll.clientWidth / 2;
  }
  scroll.scrollLeft = Math.max(0, Math.min(maximum, nextScrollLeft));
  rememberCapacityChartScroll();
};

const capacityChartOptimalSpendCoordinates = (activeWindow, resetMarkers, displayWindow, plot, nowMs) => {
  if (!displayWindow) return [];
  const segments = [];
  const seenStarts = new Set();
  const resetStarts = [
    ...new Set(
      (Array.isArray(resetMarkers) ? resetMarkers : [])
        .map((event) => event?.observed_at_ms)
        .filter((timestamp) => typeof timestamp === "number" && Number.isFinite(timestamp)),
    ),
  ].sort((left, right) => left - right);
  const addWeeklySegment = (startAtMs, nextResetAtMs = Number.POSITIVE_INFINITY) => {
    if (!Number.isFinite(startAtMs) || seenStarts.has(startAtMs)) return;
    const endAtMs = Math.min(startAtMs + CAPACITY_CHART_OPTIMAL_WEEK_MS, nextResetAtMs);
    const visibleStartAtMs = Math.max(startAtMs, displayWindow.startAtMs);
    const visibleEndAtMs = Math.min(endAtMs, displayWindow.resetAtMs, nowMs);
    if (!Number.isFinite(visibleStartAtMs) || !Number.isFinite(visibleEndAtMs) || visibleEndAtMs <= visibleStartAtMs) {
      return;
    }
    const point = (timestamp) => ({
      x: capacityChartMarkerX(timestamp, displayWindow, plot),
      y: plot.top + clampCapacityChartPercent(
            ((timestamp - startAtMs) / CAPACITY_CHART_OPTIMAL_WEEK_MS) * 100,
          ) / 100 * plot.height,
    });
    seenStarts.add(startAtMs);
    segments.push({ start: point(visibleStartAtMs), end: point(visibleEndAtMs) });
  };

  for (const [index, startAtMs] of resetStarts.entries()) {
    addWeeklySegment(startAtMs, resetStarts[index + 1]);
  }
  if (!resetStarts.length) addWeeklySegment(activeWindow?.startAtMs);
  return segments;
};

const renderProviderCapacityChart = (snapshot, sources, fiveXxBuckets = []) => {
  rememberCapacityChartScroll();
  const history = Array.isArray(snapshot?.history) ? snapshot.history : [];
  const nowMs = Date.now();
  const chartWindow = capacityChartHistoryWindow(nowMs);
  const activeUsageWindow = capacityChartActiveUsageWindow(sources, nowMs);
  const width = capacityChartIntrinsicWidth(chartWindow);
  const plot = {
    left: CAPACITY_CHART_PLOT_LEFT,
    top: CAPACITY_CHART_PLOT_TOP,
    width: Math.max(1, width - CAPACITY_CHART_PLOT_LEFT - CAPACITY_CHART_PLOT_RIGHT),
    height: capacityChartPlotHeight(),
  };
  const height = plot.top + plot.height + CAPACITY_CHART_PLOT_BOTTOM;
  const chartTicks = capacityChartTickConfig(chartWindow, plot.width);
  const chartSectionMs = chartWindow.durationMs / chartTicks.count;
  const promptCacheBuckets = capacityChartPromptCacheBuckets(snapshot, chartWindow);
  const figure = document.createElement("figure");
  figure.dataset.capacityChartFigure = "";
  figure.style.setProperty("--capacity-chart-height-px", `${height}px`);
  const chartHeader = document.createElement("div");
  chartHeader.dataset.capacityChartHeader = "";
  const title = document.createElement("h3");
  title.textContent = "Capacity and prompt cache history";
  const range = document.createElement("span");
  range.dataset.capacityChartRange = "";
  range.textContent = "Trailing 7 days · 15-minute buckets";
  chartHeader.append(title, range);
  figure.appendChild(chartHeader);

  const legend = document.createElement("div");
  legend.dataset.capacityChartLegend = "";
  legend.setAttribute("role", "list");
  for (
    const series of [
      { key: "cached-input", label: "Cached input share" },
      ...CAPACITY_CHART_SERIES,
      { key: "rate-limit-reset", label: "OpenAI rate-limit reset" },
      { key: "openai-downtime", label: "OpenAI downtime" },
      { key: "inference-error", label: "Failed inference responses (HTTP 5xx)" },
      { key: "optimal-spend", label: "Optimal token spend" },
    ]
  ) {
    const item = document.createElement("span");
    item.dataset.capacityLegendItem = series.key;
    item.setAttribute("role", "listitem");
    const swatch = document.createElement("span");
    swatch.dataset.capacityLegendSwatch = "";
    swatch.setAttribute("aria-hidden", "true");
    const label = document.createElement("span");
    label.textContent = series.label;
    item.append(swatch, label);
    legend.appendChild(item);
  }
  figure.appendChild(legend);
  const legendNote = document.createElement("p");
  legendNote.dataset.capacityChartLegendNote = "";
  legendNote.textContent = "Error markers identify 15-minute buckets; use the event list for counts and timestamps.";
  figure.appendChild(legendNote);

  const svg = capacityChartSvgElement("svg", {
    viewBox: `0 0 ${width} ${height}`,
    width,
    height,
    preserveAspectRatio: "xMinYMin meet",
    role: "img",
    "aria-label":
      "Codex and Metered 2 provider capacity lines and cached input percentage bars over the trailing seven days, including rate-limit resets, provider downtime, and failed inference response markers",
    focusable: "false",
  });
  svg.dataset.capacityChartSvg = "";
  svg.dataset.capacityChartStartAtMs = String(chartWindow.startAtMs);
  svg.dataset.capacityChartDurationMs = String(chartWindow.durationMs);
  svg.dataset.capacityChartPlotLeft = String(plot.left);
  svg.dataset.capacityChartPlotWidth = String(plot.width);
  svg.style.width = `${width}px`;

  const currentSample = { sampled_at_ms: nowMs, sources };
  const aggregateDowntimeBridges = capacityChartDowntimeBridges(
    history,
    CAPACITY_CHART_SERIES[0],
    chartWindow,
    chartWindow,
    capacityChartPoint(currentSample, CAPACITY_CHART_SERIES[0], chartWindow, chartWindow),
    nowMs,
    snapshot?.downtime_events,
  );
  const defs = capacityChartSvgElement("defs");
  const downtimePattern = capacityChartSvgElement("pattern", {
    id: "capacity-chart-downtime-stripes",
    width: 12,
    height: 12,
    patternUnits: "userSpaceOnUse",
  });
  const downtimeStripe = capacityChartSvgElement("path", {
    d: "M-3 -3L15 15 M-3 9L3 15 M9 -3L15 3",
    fill: "none",
    stroke: "#ff5f56",
    "stroke-opacity": 0.3,
    "stroke-width": 1.25,
  });
  downtimePattern.appendChild(downtimeStripe);
  const resetPattern = capacityChartSvgElement("pattern", {
    id: "capacity-chart-rate-limit-reset-stripes",
    width: 10,
    height: 10,
    patternUnits: "userSpaceOnUse",
  });
  const resetStripe = capacityChartSvgElement("path", {
    d: "M-2 10L10 -2 M3 12L12 3",
    fill: "none",
    stroke: "#55d98a",
    "stroke-opacity": 0.72,
    "stroke-width": 1.5,
  });
  resetPattern.appendChild(resetStripe);
  defs.append(downtimePattern, resetPattern);
  svg.appendChild(defs);

  for (const bucket of promptCacheBuckets) {
    const visibleStartAtMs = Math.max(bucket.bucketStartAtMs, chartWindow.startAtMs);
    const visibleEndAtMs = Math.min(bucket.bucketEndAtMs, chartWindow.resetAtMs);
    if (visibleEndAtMs <= visibleStartAtMs) continue;
    const startX = capacityChartMarkerX(visibleStartAtMs, chartWindow, plot);
    const endX = capacityChartMarkerX(visibleEndAtMs, chartWindow, plot);
    const barWidth = Math.max(0.75, endX - startX - 0.45);
    const barHeight = (bucket.cachedPercent / 100) * plot.height;
    const group = capacityChartSvgElement("g");
    group.dataset.capacityCacheBucket = String(bucket.bucketStartAtMs);
    const traffic = capacityChartSvgElement("rect", {
      x: startX,
      y: plot.top,
      width: barWidth,
      height: plot.height,
    });
    traffic.dataset.capacityCacheTraffic = "";
    const cached = capacityChartSvgElement("rect", {
      x: startX,
      y: plot.top + plot.height - barHeight,
      width: barWidth,
      height: barHeight,
    });
    cached.dataset.capacityCacheFill = "";
    const tooltip = capacityChartSvgElement("title");
    const cacheWriteText = bucket.cacheWriteInputTokens === null
      ? "cache writes unavailable"
      : `${formatNumber(bucket.cacheWriteInputTokens)} written to cache across ${
        formatNumber(bucket.cacheWriteReportedSampleCount)
      } response${bucket.cacheWriteReportedSampleCount === 1 ? "" : "s"}`;
    tooltip.textContent = `${formatCapacityTimestamp(bucket.bucketStartAtMs)} · ${
      quotaPercentFormatter.format(bucket.cachedPercent)
    }% cached · ${formatNumber(bucket.cachedInputTokens)} of ${
      formatNumber(bucket.inputTokens)
    } input tokens · ${cacheWriteText} · ${formatNumber(bucket.sampleCount)} response${
      bucket.sampleCount === 1 ? "" : "s"
    }`;
    group.setAttribute("aria-label", tooltip.textContent);
    group.append(tooltip, traffic, cached);
    svg.appendChild(group);
  }

  for (const bridge of aggregateDowntimeBridges) {
    const band = capacityChartDowntimeBandCoordinates(bridge, plot);
    if (!band) continue;
    const background = capacityChartSvgElement("rect", {
      x: band.x,
      y: plot.top,
      width: band.width,
      height: plot.height,
      fill: "#ff5f56",
      "fill-opacity": 0.055,
    });
    background.dataset.capacityDowntimeBand = "openai";
    background.setAttribute("aria-hidden", "true");
    const stripes = capacityChartSvgElement("rect", {
      x: band.x,
      y: plot.top,
      width: band.width,
      height: plot.height,
      fill: "url(#capacity-chart-downtime-stripes)",
    });
    stripes.dataset.capacityDowntimeBand = "openai";
    stripes.setAttribute("aria-hidden", "true");
    svg.append(background, stripes);
  }

  const rateLimitResetMarkers = capacityChartRateLimitResetMarkers(
    snapshot?.rate_limit_reset_events,
    history,
    chartWindow,
    snapshot?.downtime_events,
  );
  for (const event of rateLimitResetMarkers) {
    const markerX = capacityChartMarkerX(event.observed_at_ms, chartWindow, plot);
    const markerWidth = CAPACITY_CHART_RESET_BAND_WIDTH_PX;
    const markerLeft = Math.max(plot.left, Math.min(plot.left + plot.width - markerWidth, markerX - markerWidth / 2));
    const marker = capacityChartSvgElement("g");
    const background = capacityChartSvgElement("rect", {
      x: markerLeft,
      y: plot.top,
      width: markerWidth,
      height: plot.height,
      fill: "#55d98a",
      "fill-opacity": 0.16,
    });
    const stripes = capacityChartSvgElement("rect", {
      x: markerLeft,
      y: plot.top,
      width: markerWidth,
      height: plot.height,
      fill: "url(#capacity-chart-rate-limit-reset-stripes)",
    });
    const centerLine = capacityChartSvgElement("line", {
      x1: markerX,
      y1: plot.top,
      x2: markerX,
      y2: plot.top + plot.height,
      stroke: "#55d98a",
      "stroke-opacity": 0.96,
      "stroke-width": 2,
    });
    marker.dataset.capacityRateLimitReset = event.event_id || `${event.slot}-${event.window}-${event.observed_at_ms}`;
    marker.setAttribute(
      "aria-label",
      `OpenAI ${event.window} rate-limit reset for account ${event.slot}: ${
        quotaPercentFormatter.format(event.capacity_gain_percentage_points)
      } percentage points of capacity gained`,
    );
    marker.append(background, stripes, centerLine);
    svg.appendChild(marker);
  }

  const visibleFiveXxBuckets = (Array.isArray(fiveXxBuckets) ? fiveXxBuckets : []).filter((bucket) =>
    Number.isFinite(bucket?.bucket_start_at_ms) && Number.isSafeInteger(bucket?.count) && bucket.count > 0 &&
    bucket.bucket_start_at_ms >= chartWindow.startAtMs && bucket.bucket_start_at_ms <= chartWindow.resetAtMs
  );
  const fiveXxMarkerElements = new Map();
  for (const bucket of visibleFiveXxBuckets) {
    const markerX = capacityChartMarkerX(bucket.bucket_start_at_ms, chartWindow, plot);
    const marker = capacityChartSvgElement("g");
    marker.dataset.capacityInferenceError = String(bucket.bucket_start_at_ms);
    marker.setAttribute("role", "img");
    marker.setAttribute("tabindex", "0");
    marker.setAttribute(
      "aria-label",
      `${bucket.count} failed inference response${
        bucket.count === 1 ? "" : "s"
      } (HTTP 5xx) during the 15-minute bucket starting ${formatCapacityTimestamp(bucket.bucket_start_at_ms)}`,
    );
    const line = capacityChartSvgElement("line", {
      x1: markerX,
      y1: plot.top - 4,
      x2: markerX,
      y2: plot.top + plot.height,
      stroke: "#ff625f",
      "stroke-opacity": 0.72,
      "stroke-dasharray": "2 5",
      "stroke-width": 1.5,
    });
    const markerY = plot.top - 10;
    const diamond = capacityChartSvgElement("path", {
      d: `M ${markerX} ${markerY - 5} L ${markerX + 5} ${markerY} L ${markerX} ${markerY + 5} L ${
        markerX - 5
      } ${markerY} Z`,
      fill: "#ff625f",
      stroke: "#fff4f3",
      "stroke-width": 1,
    });
    const tooltip = capacityChartSvgElement("title");
    tooltip.textContent = marker.getAttribute("aria-label");
    marker.append(tooltip, line, diamond);
    svg.appendChild(marker);
    fiveXxMarkerElements.set(String(bucket.bucket_start_at_ms), marker);
  }

  for (const remaining of [100, 75, 50, 25, 0]) {
    const y = plot.top + ((100 - remaining) / 100) * plot.height;
    const grid = capacityChartSvgElement("line", {
      x1: plot.left,
      y1: y,
      x2: plot.left + plot.width,
      y2: y,
    });
    grid.dataset.capacityChartGrid = "";
    svg.appendChild(grid);
    const label = capacityChartSvgElement("text", { x: plot.left - 8, y: y + 4, "text-anchor": "end" });
    label.textContent = `${remaining}%`;
    label.dataset.capacityChartAxisLabel = "y";
    svg.appendChild(label);
  }
  for (let tickIndex = 0; tickIndex <= chartTicks.count; tickIndex += 1) {
    const x = plot.left + (tickIndex / chartTicks.count) * plot.width;
    const tick = capacityChartSvgElement("line", { x1: x, y1: plot.top, x2: x, y2: plot.top + plot.height });
    tick.dataset.capacityChartGrid = "";
    svg.appendChild(tick);
    const label = capacityChartSvgElement("text", {
      x,
      y: plot.top + plot.height + 22,
      "text-anchor": tickIndex === 0 ? "start" : tickIndex === chartTicks.count ? "end" : "middle",
    });
    label.textContent = chartTicks.formatter.format(new Date(chartWindow.startAtMs + tickIndex * chartSectionMs));
    label.dataset.capacityChartAxisLabel = "x";
    svg.appendChild(label);
  }

  const xAxisTitle = capacityChartSvgElement("text", {
    x: plot.left + plot.width / 2,
    y: height - 8,
    "text-anchor": "middle",
  });
  xAxisTitle.textContent = "Trailing 7-day provider analytics";
  xAxisTitle.dataset.capacityChartAxisTitle = "x";
  svg.appendChild(xAxisTitle);
  const yAxisTitle = capacityChartSvgElement("text", {
    x: 12,
    y: plot.top + plot.height / 2,
    transform: `rotate(-90 12 ${plot.top + plot.height / 2})`,
    "text-anchor": "middle",
  });
  yAxisTitle.textContent = "Percent";
  yAxisTitle.dataset.capacityChartAxisTitle = "y";
  svg.appendChild(yAxisTitle);

  const optimalSpendCoordinates = capacityChartOptimalSpendCoordinates(
    activeUsageWindow,
    rateLimitResetMarkers,
    chartWindow,
    plot,
    nowMs,
  );
  for (const [index, coordinates] of optimalSpendCoordinates.entries()) {
    const optimalSpendTrend = capacityChartSvgElement("line", {
      x1: coordinates.start.x,
      y1: coordinates.start.y,
      x2: coordinates.end.x,
      y2: coordinates.end.y,
    });
    optimalSpendTrend.dataset.capacityTrend = "optimal-spend";
    optimalSpendTrend.setAttribute("aria-label", `Optimal token spend for weekly reset ${index + 1}`);
    svg.appendChild(optimalSpendTrend);
  }

  const pacing = capacityChartSpendPacing(activeUsageWindow, sources, nowMs);
  const currentX = capacityChartMarkerX(nowMs, chartWindow, plot);
  const reticule = capacityChartSvgElement("line", {
    x1: currentX,
    y1: plot.top,
    x2: currentX,
    y2: plot.top + plot.height,
  });
  reticule.dataset.capacityReticule = "current-time";
  reticule.setAttribute("aria-label", "Current time in usage period");
  svg.appendChild(reticule);

  for (const series of CAPACITY_CHART_SERIES) {
    const activeInterval = series.source === "aggregate" || series.source === "metered"
      ? chartWindow
      : activeUsageWindow;
    const shouldRender = true;
    const currentPoint = shouldRender ? capacityChartPoint(currentSample, series, activeInterval, chartWindow) : null;
    const chartPoints = shouldRender
      ? capacityChartSeriesPoints(
        history,
        series,
        activeInterval,
        chartWindow,
        currentPoint,
        nowMs,
        snapshot?.reset_events,
        snapshot?.downtime_events,
      )
      : [];
    const path = capacityChartSvgElement("path", {
      d: capacityChartPath(chartPoints, plot, {
        anchorStart: false,
        anchorEnd: false,
      }),
      fill: "none",
    });
    path.style.fill = "none";
    path.dataset.capacitySeries = series.key;
    path.setAttribute("aria-label", series.label);
    svg.appendChild(path);

    const downtimeBridges = series.source === "aggregate" ? aggregateDowntimeBridges : capacityChartDowntimeBridges(
      history,
      series,
      activeInterval,
      chartWindow,
      currentPoint,
      nowMs,
      snapshot?.downtime_events,
    );
    if (downtimeBridges.length) {
      const downtimePath = capacityChartSvgElement("path", {
        d: capacityChartBridgePath(downtimeBridges, plot),
        fill: "none",
      });
      downtimePath.style.fill = "none";
      downtimePath.dataset.capacityDowntime = "openai";
      downtimePath.setAttribute("aria-label", "OpenAI downtime between observed capacity samples");
      svg.appendChild(downtimePath);
    }
  }

  const chartBody = document.createElement("div");
  chartBody.dataset.capacityChartBody = "";
  const chartScroll = document.createElement("div");
  chartScroll.dataset.capacityChartScroll = "";
  chartScroll.tabIndex = 0;
  chartScroll.setAttribute("role", "region");
  chartScroll.setAttribute("aria-label", "Scrollable seven-day provider analytics history");
  chartScroll.appendChild(svg);

  const chartScrollControls = document.createElement("div");
  chartScrollControls.dataset.capacityChartScrollControls = "";
  chartScrollControls.setAttribute("aria-label", "History navigation");
  const olderButton = document.createElement("button");
  olderButton.type = "button";
  olderButton.textContent = "← Older";
  olderButton.setAttribute("aria-label", "Scroll to older provider analytics");
  const newerButton = document.createElement("button");
  newerButton.type = "button";
  newerButton.textContent = "Newer →";
  newerButton.setAttribute("aria-label", "Scroll to newer provider analytics");
  chartScrollControls.append(olderButton, newerButton);

  const syncCapacityChartScroll = () => {
    rememberCapacityChartScroll();
    updateCapacityChartScrollControls(chartScroll, olderButton, newerButton);
  };
  chartScroll.addEventListener("scroll", syncCapacityChartScroll, { passive: true });
  olderButton.addEventListener("click", () => {
    chartScroll.scrollBy({ left: -capacityChartScrollAmount(chartScroll), behavior: capacityChartScrollBehavior() });
  });
  newerButton.addEventListener("click", () => {
    chartScroll.scrollBy({ left: capacityChartScrollAmount(chartScroll), behavior: capacityChartScrollBehavior() });
  });
  chartScroll.addEventListener("keydown", (event) => {
    const amount = capacityChartScrollAmount(chartScroll);
    if (event.key === "ArrowLeft" || event.key === "PageUp") {
      event.preventDefault();
      chartScroll.scrollBy({ left: -amount, behavior: capacityChartScrollBehavior() });
    } else if (event.key === "ArrowRight" || event.key === "PageDown") {
      event.preventDefault();
      chartScroll.scrollBy({ left: amount, behavior: capacityChartScrollBehavior() });
    } else if (event.key === "Home") {
      event.preventDefault();
      chartScroll.scrollTo({ left: 0, behavior: capacityChartScrollBehavior() });
    } else if (event.key === "End") {
      event.preventDefault();
      chartScroll.scrollTo({ left: chartScroll.scrollWidth, behavior: capacityChartScrollBehavior() });
    }
  });
  const chartPane = document.createElement("div");
  chartPane.dataset.capacityChartPane = "";
  chartPane.append(chartScroll, chartScrollControls);
  chartBody.append(chartPane, renderCapacitySpendSummary(pacing, activeUsageWindow));
  figure.appendChild(chartBody);
  if (visibleFiveXxBuckets.length) {
    const errorSummary = document.createElement("div");
    errorSummary.dataset.capacityErrorSummary = "";
    const errorSummaryHeader = document.createElement("div");
    errorSummaryHeader.dataset.capacityEventHeader = "";
    const errorSummaryTitle = document.createElement("strong");
    errorSummaryTitle.textContent = "Failed inference buckets";
    const errorCount = visibleFiveXxBuckets.reduce((total, bucket) => total + bucket.count, 0);
    const errorSummaryMeta = document.createElement("span");
    errorSummaryMeta.textContent = `${errorCount} failed response${
      errorCount === 1 ? "" : "s"
    } in ${visibleFiveXxBuckets.length} bucket${visibleFiveXxBuckets.length === 1 ? "" : "s"}`;
    errorSummaryHeader.append(errorSummaryTitle, errorSummaryMeta);
    const errorNavigation = document.createElement("div");
    errorNavigation.dataset.capacityErrorNavigation = "";
    errorNavigation.setAttribute("role", "group");
    errorNavigation.setAttribute("aria-label", "Recent failed inference response buckets");
    const recentBuckets = [...visibleFiveXxBuckets]
      .sort((left, right) => right.bucket_start_at_ms - left.bucket_start_at_ms)
      .slice(0, 12);
    for (const bucket of recentBuckets) {
      const button = document.createElement("button");
      button.type = "button";
      button.dataset.capacityErrorBucket = String(bucket.bucket_start_at_ms);
      button.textContent = `${formatCapacityTimestamp(bucket.bucket_start_at_ms)} · ${bucket.count} failed`;
      button.setAttribute(
        "aria-label",
        `${bucket.count} failed inference response${bucket.count === 1 ? "" : "s"}, 15-minute bucket starting ${
          formatCapacityTimestamp(bucket.bucket_start_at_ms)
        }. Move the chart to this marker.`,
      );
      button.addEventListener("click", () => {
        const markerX = capacityChartMarkerX(bucket.bucket_start_at_ms, chartWindow, plot);
        chartScroll.scrollTo({
          left: Math.max(0, markerX - chartScroll.clientWidth / 2),
          behavior: capacityChartScrollBehavior(),
        });
        fiveXxMarkerElements.get(String(bucket.bucket_start_at_ms))?.focus({ preventScroll: true });
      });
      errorNavigation.appendChild(button);
    }
    errorSummary.append(errorSummaryHeader, errorNavigation);
    figure.appendChild(errorSummary);
  }
  if (rateLimitResetMarkers.length) {
    const navigation = document.createElement("nav");
    navigation.dataset.capacityResetNavigation = "";
    navigation.setAttribute("aria-label", "Rate-limit reset markers");
    for (const [index, event] of rateLimitResetMarkers.entries()) {
      const button = document.createElement("button");
      button.type = "button";
      button.dataset.capacityReset = event.event_id || `${event.slot}-${event.window}-${event.observed_at_ms}`;
      button.textContent = `${formatCapacityTimestamp(event.observed_at_ms)} · +${
        quotaPercentFormatter.format(event.capacity_gain_percentage_points)
      } pp`;
      button.setAttribute(
        "aria-label",
        `Go to rate-limit reset ${
          index + 1
        } of ${rateLimitResetMarkers.length}, account ${event.slot} ${event.window} window, ${
          quotaPercentFormatter.format(event.capacity_gain_percentage_points)
        } percentage points of capacity gained and timer reset`,
      );
      button.addEventListener("click", () => {
        const markerX = capacityChartMarkerX(event.observed_at_ms, chartWindow, plot);
        chartScroll.scrollTo({
          left: Math.max(0, markerX - chartScroll.clientWidth / 2),
          behavior: capacityChartScrollBehavior(),
        });
      });
      navigation.appendChild(button);
    }
    figure.appendChild(navigation);
  }
  const caption = document.createElement("figcaption");
  caption.dataset.capacityChartMeta = "";
  const samples = history.filter((sample) => typeof sample?.sampled_at_ms === "number");
  const staleNotes = [
    sources.some((source) => source?.source === "codex" && source?.state === "stale") ? "Codex samples stale" : null,
    sources.some((source) => source?.source === "metered" && source?.state === "stale") ? "Metered sample stale" : null,
  ].filter((note) => note !== null);
  const staleSuffix = staleNotes.length ? ` · ${staleNotes.join(" · ")}` : "";
  const resetEvents = Array.isArray(snapshot?.reset_events) ? snapshot.reset_events : [];
  const resetSuffix = resetEvents.length
    ? ` · ${resetEvents.length} verified reset${resetEvents.length === 1 ? "" : "s"}`
    : "";
  const rateLimitResetSuffix = rateLimitResetMarkers.length
    ? ` · ${rateLimitResetMarkers.length} observed rate-limit reset${rateLimitResetMarkers.length === 1 ? "" : "s"}`
    : "";
  const downtimeBridgeCount = capacityChartDowntimeBridges(
    history,
    CAPACITY_CHART_SERIES[0],
    chartWindow,
    chartWindow,
    capacityChartPoint(currentSample, CAPACITY_CHART_SERIES[0], chartWindow, chartWindow),
    nowMs,
    snapshot?.downtime_events,
  ).length;
  const downtimeSuffix = downtimeBridgeCount
    ? ` · ${downtimeBridgeCount} OpenAI downtime bridge${downtimeBridgeCount === 1 ? "" : "s"}`
    : "";
  const fiveXxCount = visibleFiveXxBuckets.reduce((total, bucket) => total + bucket.count, 0);
  const fiveXxSuffix = fiveXxCount
    ? ` · ${fiveXxCount} failed inference response${fiveXxCount === 1 ? "" : "s"} (HTTP 5xx)`
    : "";
  const latestCacheBucket = promptCacheBuckets.at(-1);
  const latestCacheWrite = latestCacheBucket?.cacheWriteInputTokens === null
    ? "cache writes unavailable"
    : latestCacheBucket
    ? `${formatNumber(latestCacheBucket.cacheWriteInputTokens)} written to cache`
    : null;
  const cacheSuffix = snapshot?.prompt_cache?.status !== "ready"
    ? " · cache analytics unavailable"
    : latestCacheBucket
    ? ` · ${promptCacheBuckets.length} cache bucket${promptCacheBuckets.length === 1 ? "" : "s"} · latest ${
      quotaPercentFormatter.format(latestCacheBucket.cachedPercent)
    }% cached · ${latestCacheWrite} at ${formatCapacityTimestamp(latestCacheBucket.bucketStartAtMs)}`
    : " · no cache-token history yet";
  caption.textContent = samples.length
    ? `15-minute buckets · ${formatCapacityTimestamp(samples[0].sampled_at_ms)} → ${
      formatCapacityTimestamp(samples[samples.length - 1].sampled_at_ms)
    } · ${samples.length} sample${
      samples.length === 1 ? "" : "s"
    }${resetSuffix}${rateLimitResetSuffix}${downtimeSuffix}${fiveXxSuffix}${cacheSuffix}${staleSuffix}`
    : `No retained capacity samples yet · trailing seven-day window${resetSuffix}${rateLimitResetSuffix}${downtimeSuffix}${fiveXxSuffix}${cacheSuffix}${staleSuffix}`;
  figure.appendChild(caption);
  providerCapacityChart.replaceChildren(figure);
  restoreCapacityChartScroll(chartScroll, chartWindow, plot);
  updateCapacityChartScrollControls(chartScroll, olderButton, newerButton);
};

const renderProviderCapacity = (snapshot, fiveXxBuckets = []) => {
  const rawSources = Array.isArray(snapshot?.sources) ? snapshot.sources : [];
  const sourceForSlot = (slot) =>
    rawSources.find((source) => source?.source === "codex" && source.slot === slot) ??
      unavailableCapacitySource("codex", slot);
  const sources = [
    sourceForSlot(1),
    sourceForSlot(2),
    rawSources.find((source) => source?.source === "metered") ?? unavailableCapacitySource("metered"),
  ];
  latestProviderCapacityChartState = { snapshot, sources, fiveXxBuckets };
  renderProviderCapacityChart(snapshot, sources, fiveXxBuckets);
  renderProviderCapacityList(sources);

  const unavailableCount = sources.filter((source) => source.state === "unavailable").length;
  const staleCount = sources.filter((source) => source.state === "stale").length;
  const cacheAnalyticsUnavailable = snapshot?.prompt_cache?.status !== "ready";
  if (unavailableCount === sources.length) {
    setBadge(providerCapacityBadge, "unknown", "Quota unavailable");
  } else if (unavailableCount > 0) {
    setBadge(providerCapacityBadge, "unknown", `Partial quota · ${unavailableCount} unavailable`);
  } else if (staleCount > 0) {
    setBadge(providerCapacityBadge, "unknown", `Quota stale · ${staleCount} source${staleCount === 1 ? "" : "s"}`);
  } else if (cacheAnalyticsUnavailable) {
    setBadge(providerCapacityBadge, "unknown", "Cache analytics unavailable");
  } else {
    setBadge(providerCapacityBadge, "ok", "Snapshot ready");
  }
  const snapshotAt = typeof snapshot?.snapshot_at_ms === "number" ? snapshot.snapshot_at_ms : null;
  const cacheState = typeof snapshot?.cache_state === "string" ? snapshot.cache_state : "unavailable";
  providerCapacityUpdated.textContent = `Snapshot ${formatCapacityTimestamp(snapshotAt)} · ${cacheState}`;
};

const loadProviderCapacity = async () => {
  if (providerCapacityLoading) return false;
  const token = getAdminToken();
  if (!adminAccessState.isAdmin || !hasAdminCredential()) {
    setBadge(providerCapacityBadge, "bad", "Sign in required");
    return false;
  }
  providerCapacityLoading = true;
  setBadge(providerCapacityBadge, "unknown", providerCapacityLoadedAt ? "Cached · refreshing" : "Loading capacity");
  try {
    const headers = { Authorization: `Bearer ${token}` };
    const [response, errorsResponse] = await Promise.all([
      fetch(apiUrl("/admin/providers/capacity"), { cache: "no-store", headers }),
      fetch(apiUrl("/admin/errors?limit=1"), { cache: "no-store", headers }),
    ]);
    const [payload, errorsPayload] = await Promise.all([
      response.json().catch(() => null),
      errorsResponse.json().catch(() => null),
    ]);
    if (!response.ok || !payload) {
      if (providerCapacityLoadedAt) {
        setBadge(providerCapacityBadge, "unknown", "Cached · refresh unavailable");
        providerCapacityUpdated.textContent = "Cached · refresh unavailable";
        return false;
      }
      setBadge(providerCapacityBadge, "bad", payload?.error?.message ?? "Capacity unavailable");
      providerCapacityUpdated.textContent = "Snapshot unavailable";
      return false;
    }
    renderProviderCapacity(
      payload,
      errorsResponse.ok && Array.isArray(errorsPayload?.five_xx_buckets) ? errorsPayload.five_xx_buckets : [],
    );
    providerCapacityLoadedAt = Date.now();
    return true;
  } catch {
    if (providerCapacityLoadedAt) {
      setBadge(providerCapacityBadge, "unknown", "Cached · offline");
      providerCapacityUpdated.textContent = "Cached · offline";
      return false;
    }
    setBadge(providerCapacityBadge, "bad", "Offline");
    providerCapacityUpdated.textContent = "Snapshot unavailable";
    return false;
  } finally {
    providerCapacityLoading = false;
  }
};

const quotaProjectionProviderLabel = (provider) =>
  provider === "surplus" ? "Surplus" : provider === "metered" ? "Metered" : String(provider ?? "unknown");

const quotaProjectionDuration = (ms) => {
  if (typeof ms !== "number" || !Number.isFinite(ms) || ms < 0) return "unknown";
  const minutes = Math.max(0, Math.trunc(ms / 60_000));
  if (minutes < 60) return `${formatNumber(minutes)}m`;
  const hours = Math.trunc(minutes / 60);
  const days = Math.trunc(hours / 24);
  if (days >= 365) return `${Math.round(days / 365 * 10) / 10}y`;
  if (days >= 1) return `${formatNumber(days)}d ${hours % 24}h`;
  return `${formatNumber(hours)}h ${minutes % 60}m`;
};

const quotaProjectionEstimate = (entry, windowDays = 30) => {
  const estimate = (entry?.estimates ?? []).find((candidate) => candidate?.window_days === windowDays);
  const usage = (entry?.usage ?? []).find((candidate) => candidate?.window_days === windowDays);
  return { estimate, usage };
};

const renderQuotaProjectionRows = (payload, historyUnavailable) => {
  const models = Array.isArray(payload?.models) ? payload.models : [];
  quotaRunwayList.replaceChildren();
  if (!models.length) {
    const empty = document.createElement("p");
    empty.dataset.empty = "quota-runway";
    empty.textContent = historyUnavailable
      ? "Paid-fallback usage history could not be read — check KV availability."
      : "No settled paid-fallback usage in the retained window yet. Rows appear after requests settle.";
    quotaRunwayList.appendChild(empty);
    return;
  }
  for (const entry of models) {
    const { estimate, usage } = quotaProjectionEstimate(entry, 30);
    const row = document.createElement("div");
    row.dataset.quotaRunwayRow = "";
    const heading = document.createElement("div");
    heading.dataset.quotaRunwayHeading = "";
    const model = document.createElement("strong");
    model.textContent = String(entry.model ?? "unknown");
    const provider = document.createElement("span");
    provider.dataset.muted = "";
    provider.textContent = quotaProjectionProviderLabel(entry.provider);
    heading.append(model, provider);
    const details = document.createElement("div");
    details.dataset.quotaRunwayDetails = "";
    const history = document.createElement("p");
    history.dataset.muted = "";
    history.textContent = usage?.request_count
      ? `30d: ${formatNumber(usage.request_count)} requests · ${
        formatDecimal(usage.avg_quota_per_request)
      } quota avg/request`
      : "No settled usage in the trailing 30 days";
    const projection = document.createElement("p");
    if (!estimate) {
      projection.textContent = "No exhaustion estimate — quota is not monitored for this provider";
    } else if (estimate?.unlimited === true) {
      projection.textContent = "Unlimited quota — no exhaustion estimate";
    } else if (estimate?.requests_remaining === null || estimate?.requests_remaining === undefined) {
      projection.textContent = "Exhaustion estimate unknown (no quota balance or usage rate)";
    } else {
      const parts = [`~${formatNumber(estimate.requests_remaining)} requests left`];
      if (typeof estimate.time_remaining_ms === "number") {
        parts.push(`~${quotaProjectionDuration(estimate.time_remaining_ms)} run-time left`);
        if (typeof estimate.exhausted_at_ms === "number") {
          parts.push(`exhausts ${formatDate(estimate.exhausted_at_ms)}`);
        }
      }
      const knocked = estimate.percent_per_request_vs_balance;
      if (typeof knocked === "number") {
        parts.push(`${quotaPercentFormatter.format(knocked)}% of balance per request`);
      }
      if (estimate.stale_balance === true) parts.push("stale balance snapshot");
      projection.textContent = parts.join(" · ");
    }
    details.append(history, projection);
    row.append(heading, details);
    quotaRunwayList.appendChild(row);
  }
};

const renderQuotaProjection = (payload) => {
  const quota = payload?.quota ?? {};
  const balanceHistory = Array.isArray(payload?.balance_history) ? payload.balance_history : [];
  quotaRunwaySummary.replaceChildren();
  quotaRunwayNote.textContent = "";
  if (!quota.available) {
    setBadge(quotaRunwayBadge, "bad", "Quota not monitored");
    quotaRunwayUpdated.textContent = "Consumption history only";
    const summary = document.createElement("p");
    summary.dataset.muted = "";
    summary.textContent =
      "Metered quota monitoring is not configured or has no snapshot. Per-model consumption history is still reported below.";
    quotaRunwaySummary.appendChild(summary);
  } else if (quota.unlimited_quota === true) {
    setBadge(quotaRunwayBadge, "ok", "Unlimited quota");
    quotaRunwayUpdated.textContent = "No exhaustion estimate";
    const summary = document.createElement("p");
    summary.dataset.muted = "";
    summary.textContent =
      "The Metered report is unlimited or only publishes totals; balance-based run-time estimates are unavailable.";
    quotaRunwaySummary.appendChild(summary);
  } else {
    const balanceCredits = quota.balance_credits;
    const baselineCredits = quota.baseline_credits;
    const remainingPercent = quota.remaining_percent;
    const balanceText = typeof balanceCredits === "number"
      ? `Balance ${formatNumber(balanceCredits)} credits`
      : typeof quota.total_available === "number"
      ? `Available ${formatNumber(quota.total_available)} tokens`
      : "Balance unavailable";
    const healthyBalance = typeof balanceCredits === "number"
      ? balanceCredits > 0
      : typeof quota.total_available === "number"
      ? quota.total_available > 0
      : false;
    setBadge(
      quotaRunwayBadge,
      healthyBalance ? "ok" : "bad",
      balanceText,
    );
    const updatedParts = [];
    if (typeof remainingPercent === "number") {
      updatedParts.push(`${quotaPercentFormatter.format(remainingPercent)} of baseline left`);
    }
    if (typeof baselineCredits === "number" && baselineCredits !== 0) {
      updatedParts.push(`baseline ${formatNumber(baselineCredits)} credits`);
    }
    if (typeof quota.observed_at_ms === "number") updatedParts.push(`observed ${formatDate(quota.observed_at_ms)}`);
    quotaRunwayUpdated.textContent = updatedParts.join(" · ") || "Waiting for projection";
    const refill = [];
    if (typeof quota.latest_refill_amount_credits === "number") {
      refill.push(`last refill ${formatNumber(quota.latest_refill_amount_credits)} credits`);
    }
    if (typeof quota.last_credit_at_ms === "number") refill.push(`at ${formatDate(quota.last_credit_at_ms)}`);
    if (refill.length) {
      const summary = document.createElement("p");
      summary.dataset.muted = "";
      summary.textContent = `${refill.join(" ")}. Estimates assume no further refill;${
        typeof quota.cycle_started_at_ms === "number"
          ? ` current cycle began ${formatDate(quota.cycle_started_at_ms)}.`
          : ""
      }`;
      quotaRunwaySummary.appendChild(summary);
    }
  }
  const bucketCount = balanceHistory.length;
  const windowDays = typeof payload?.window_days === "number" ? payload.window_days : 30;
  const windowLabel = `${windowDays}-day`;
  const balanceWindowDays = typeof payload?.balance_window_days === "number" ? payload.balance_window_days : null;
  const balanceBucketMs = payload?.retention?.balance_history_bucket_ms;
  const balanceBucketLabel = balanceBucketMs === 24 * 60 * 60_000
    ? "daily"
    : balanceBucketMs === 60 * 60_000
    ? "hourly"
    : typeof balanceBucketMs === "number"
    ? `${quotaProjectionDuration(balanceBucketMs)}-bucket`
    : "";
  const balanceWindowLabel = balanceWindowDays === null
    ? "the returned window"
    : `the trailing ${balanceWindowDays} days`;
  const historyUnavailable = payload?.rollup_scan !== "ok";
  const balanceUnavailable = payload?.balance_history_scan !== "ok";
  if (historyUnavailable) {
    quotaRunwayNote.textContent =
      "Paid-fallback usage history could not be read — totals and exhaustion estimates are unavailable until KV reads recover.";
  } else if (balanceUnavailable) {
    quotaRunwayNote.textContent =
      "Balance history could not be read — the run-down curve is unavailable until KV reads recover.";
  } else if (bucketCount) {
    quotaRunwayNote.textContent = `${formatNumber(bucketCount)} ${
      balanceBucketLabel ? `${balanceBucketLabel} ` : ""
    }balance samples in ${balanceWindowLabel} · estimates use the ${windowLabel} consumption window · raw request rows retain one year; hourly model rollups are retained indefinitely.`;
  } else {
    quotaRunwayNote.textContent =
      `Estimates use the ${windowLabel} consumption window · raw request rows retain one year; hourly model rollups are retained indefinitely.`;
  }
  renderQuotaProjectionRows(payload, historyUnavailable);
};

const loadQuotaProjection = async () => {
  if (quotaProjectionLoading) return false;
  const token = getAdminToken();
  if (!adminAccessState.isAdmin || !hasAdminCredential()) {
    setBadge(quotaRunwayBadge, "bad", "Sign in required");
    return false;
  }
  // Fence the render against token or API-base target changes while this
  // request is in flight, so a switched session can never display another
  // gateway's balance and usage.
  const loadId = ++quotaProjectionLoadId;
  const signature = `${token}\u0000${resolveBaseUrl()}`;
  quotaProjectionLoading = true;
  setBadge(quotaRunwayBadge, "unknown", quotaProjectionLoadedAt ? "Cached · refreshing" : "Loading projection");
  try {
    const response = await fetch(apiUrl("/admin/providers/quota-projection?window_days=30&balance_window_days=365"), {
      cache: "no-store",
      headers: { Authorization: `Bearer ${token}` },
    });
    const payload = await response.json().catch(() => null);
    if (
      loadId !== quotaProjectionLoadId ||
      signature !== `${getAdminToken()}\u0000${resolveBaseUrl()}`
    ) return false;
    if (!response.ok || !payload) {
      if (quotaProjectionLoadedAt) {
        setBadge(quotaRunwayBadge, "unknown", "Cached · refresh unavailable");
        quotaRunwayUpdated.textContent = "Cached · refresh unavailable";
        return false;
      }
      setBadge(quotaRunwayBadge, "bad", payload?.error?.message ?? "Projection unavailable");
      quotaRunwayUpdated.textContent = "Projection unavailable";
      return false;
    }
    renderQuotaProjection(payload);
    quotaProjectionLoadedAt = Date.now();
    return true;
  } catch {
    if (loadId !== quotaProjectionLoadId) return false;
    if (quotaProjectionLoadedAt) {
      setBadge(quotaRunwayBadge, "unknown", "Cached · offline");
      quotaRunwayUpdated.textContent = "Cached · offline";
      return false;
    }
    setBadge(quotaRunwayBadge, "bad", "Offline");
    quotaRunwayUpdated.textContent = "Projection unavailable";
    return false;
  } finally {
    if (loadId === quotaProjectionLoadId) quotaProjectionLoading = false;
  }
};

const scheduleProviderCapacityChartResize = () => {
  if (capacityChartResizeFrame) return;
  capacityChartResizeFrame = globalThis.requestAnimationFrame(() => {
    capacityChartResizeFrame = 0;
    if (currentAdminView !== "providers" || !latestProviderCapacityChartState) return;
    renderProviderCapacityChart(
      latestProviderCapacityChartState.snapshot,
      latestProviderCapacityChartState.sources,
      latestProviderCapacityChartState.fiveXxBuckets,
    );
  });
};

globalThis.addEventListener("resize", scheduleProviderCapacityChartResize);

const loadProviders = async () => {
  if (providersLoading) return;
  const token = getAdminToken();
  if (!adminAccessState.isAdmin || !hasAdminCredential()) {
    return;
  }
  const loadId = ++providersLoadId;
  providersLoading = true;
  try {
    const response = await fetch(apiUrl("/admin/providers"), {
      cache: "no-store",
      headers: { Authorization: `Bearer ${token}` },
    });
    const payload = await response.json().catch(() => null);
    if (loadId !== providersLoadId) return;
    if (!response.ok || !payload) {
      if (providersLoadedAt) {
        if (latestProviderCapacityChartState?.sources) {
          renderProviderCapacityList(latestProviderCapacityChartState.sources);
        }
        return;
      }
      latestProviderHealth = null;
      if (latestProviderCapacityChartState?.sources) {
        renderProviderCapacityList(latestProviderCapacityChartState.sources);
      }
      return;
    }
    latestProviderHealth = payload;
    if (latestProviderCapacityChartState?.sources) renderProviderCapacityList(latestProviderCapacityChartState.sources);
    providersLoadedAt = Date.now();
  } catch {
    if (loadId !== providersLoadId) return;
    if (providersLoadedAt) {
      if (latestProviderCapacityChartState?.sources) {
        renderProviderCapacityList(latestProviderCapacityChartState.sources);
      }
      return;
    }
    latestProviderHealth = null;
    if (latestProviderCapacityChartState?.sources) renderProviderCapacityList(latestProviderCapacityChartState.sources);
  } finally {
    if (loadId === providersLoadId) providersLoading = false;
  }
};

const formatPemPreview = (value) => {
  if (typeof value !== "string") return "";
  const trimmed = value.trim();
  if (!trimmed) return "";
  const flattened = trimmed.replace(/\s+/g, "");
  if (flattened.length <= 20) return flattened;
  return `${flattened.slice(0, 10)}...${flattened.slice(-6)}`;
};

const formatLimitValue = (value) => {
  if (value === -1) return "unlimited";
  if (typeof value !== "number" || !Number.isFinite(value)) return "unknown";
  return formatNumber(value);
};

const formatWindowMs = (value) => {
  if (typeof value !== "number" || !Number.isFinite(value)) return "unknown";
  const ms = Math.trunc(value);
  const presets = {
    60000: "1m",
    3600000: "1h",
    86400000: "1d",
    604800000: "1w",
  };
  const label = presets[ms];
  if (label) return `${label} (${formatNumber(ms)} ms)`;
  return `${formatNumber(ms)} ms`;
};

const formatWindowShort = (value) => {
  if (typeof value !== "number" || !Number.isFinite(value)) return "unknown";
  const ms = Math.trunc(value);
  const presets = {
    60000: "1m",
    3600000: "1h",
    86400000: "1d",
    604800000: "1w",
  };
  return presets[ms] ?? `${formatNumber(ms)} ms`;
};

const formatPlural = (count, singular, plural = `${singular}s`) =>
  `${formatNumber(count)} ${count === 1 ? singular : plural}`;

const formatAuthMethodLabel = (method) => {
  if (!method) return "unknown";
  const lookup = {
    deno_deploy_token: "Deno Deploy token",
    admin_allowlist: "Admin allowlist",
    auth_tokens_allowlist: "Allowlist token",
    kv_api_key: "API key",
    github_token: "GitHub token",
    passkey_session: "Passkey",
    disabled: "Auth disabled",
  };
  return lookup[method] ?? method.replace(/_/g, " ");
};

const pad2 = (value) => String(value).padStart(2, "0");

const toDateTimeLocalValue = (ms) => {
  if (typeof ms !== "number" || !Number.isFinite(ms) || ms <= 0) return "";
  const date = new Date(ms);
  return `${date.getFullYear()}-${pad2(date.getMonth() + 1)}-${pad2(date.getDate())}T${pad2(date.getHours())}:${
    pad2(date.getMinutes())
  }`;
};

const parseDateTimeLocalValue = (value) => {
  if (typeof value !== "string") return null;
  const trimmed = value.trim();
  if (!trimmed) return null;
  const date = new Date(trimmed);
  const ms = date.getTime();
  if (!Number.isFinite(ms)) return null;
  return Math.trunc(ms);
};

const ICON_PATHS = {
  edit: `
    <path d="M3 17.25V21h3.75L18.37 9.38l-3.75-3.75L3 17.25z"></path>
    <path d="M14.62 5.63l3.75 3.75"></path>
  `,
  close: `
    <line x1="6" y1="6" x2="18" y2="18"></line>
    <line x1="6" y1="18" x2="18" y2="6"></line>
  `,
  trash: `
    <path d="M4 7h16"></path>
    <path d="M9 7V5h6v2"></path>
    <rect x="7" y="7" width="10" height="12" rx="1"></rect>
  `,
  revoke: `
    <circle cx="12" cy="12" r="8"></circle>
    <line x1="8" y1="16" x2="16" y2="8"></line>
  `,
  restore: `
    <path d="M9 14l-4-4 4-4"></path>
    <path d="M5 10h7a6 6 0 1 1 0 12h-1"></path>
  `,
  save: `
    <path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"></path>
    <polyline points="17 21 17 13 7 13 7 21"></polyline>
    <polyline points="7 3 7 8 15 8"></polyline>
  `,
  plus: `
    <line x1="12" y1="5" x2="12" y2="19"></line>
    <line x1="5" y1="12" x2="19" y2="12"></line>
  `,
};

const buildIconSvg = (name) => {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("aria-hidden", "true");
  svg.setAttribute("focusable", "false");
  svg.innerHTML = ICON_PATHS[name] ?? "";
  return svg;
};

const applyIconButton = (button, name, label) => {
  button.dataset.iconButton = "true";
  button.title = label;
  button.setAttribute("aria-label", label);
  button.textContent = "";
  button.appendChild(buildIconSvg(name));
};

const applyActionButton = (button, name, label) => {
  delete button.dataset.iconButton;
  button.removeAttribute("title");
  button.setAttribute("aria-label", label);
  button.textContent = "";
  const text = document.createElement("span");
  text.textContent = label;
  button.append(buildIconSvg(name), text);
};

const KERNEL_ACTIVITY_WINDOW_MS = 30 * 24 * 60 * 60 * 1000;
const KERNEL_NEAR_LIMIT_RATIO = 0.8;
const DEFAULT_API_KEY_WINDOW_MS = 604800000;
const DEFAULT_KERNEL_POLICY_LIMIT = -1;
const DEFAULT_KERNEL_POLICY_WINDOW_MS = 7 * 24 * 60 * 60 * 1000;

applyActionButton(kernelNewSaveBtn, "save", "Save rate limit");
applyActionButton(kernelPubKeyCreateBtn, "plus", "Add attestation key");

const setKernelListMessage = (text) => {
  kernelList.textContent = "";
  const message = document.createElement("p");
  message.dataset.empty = "kernel";
  message.textContent = text;
  kernelList.appendChild(message);
};

const setKernelQueueMessage = (text) => {
  kernelQueueList.textContent = "";
  const message = document.createElement("p");
  message.dataset.empty = "kernel-queue";
  message.textContent = text;
  kernelQueueList.appendChild(message);
};

const setAccessValue = (el, value, fallback = "—") => {
  if (!el) return;
  el.textContent = value && String(value).trim() ? String(value) : fallback;
};

const updateAccessApiKeysSummary = () => {
  if (!hasAdminCredential()) {
    setAccessValue(accessApiKeys, "Missing token");
    return;
  }
  if (!keysLoadedAt) {
    setAccessValue(accessApiKeys, "Not loaded");
    return;
  }
  const total = Array.isArray(allKeys) ? allKeys.length : 0;
  if (total === 0) {
    setAccessValue(accessApiKeys, "None");
    return;
  }
  const active = allKeys.filter((key) => !(typeof key?.revoked_at_ms === "number" && key.revoked_at_ms > 0)).length;
  setAccessValue(accessApiKeys, `${formatNumber(active)} active · ${formatPlural(total, "key")}`);
};

const updateAccessGithubSummary = () => {
  if (!hasAdminCredential()) {
    setAccessValue(accessGithubRepos, "Missing token");
    setAccessValue(accessGithubQueue, "Missing token");
    return;
  }
  if (kernelListLoadId === 0) {
    setAccessValue(accessGithubRepos, "Not loaded");
  } else {
    const repoRecords = Array.isArray(kernelListRecords?.repo) ? kernelListRecords.repo : [];
    if (!repoRecords.length) {
      setAccessValue(accessGithubRepos, "No analytics yet");
    } else {
      const active = repoRecords.filter((record) => getKernelTotalRequests(record) > 0).length;
      setAccessValue(accessGithubRepos, `${formatNumber(active)} active · ${formatPlural(repoRecords.length, "repo")}`);
    }
  }
  if (!kernelQueueLoadedAt) {
    setAccessValue(accessGithubQueue, "Not loaded");
  } else if (kernelQueueItems.length === 0) {
    setAccessValue(accessGithubQueue, "None");
  } else {
    setAccessValue(accessGithubQueue, formatPlural(kernelQueueItems.length, "request"));
  }
};

const updateAccessPubkeysSummary = () => {
  if (!hasAdminCredential()) {
    setAccessValue(accessKernelPubkeys, "Missing token");
    return;
  }
  if (!kernelPubKeysLoadedAt) {
    setAccessValue(accessKernelPubkeys, "Not loaded");
    return;
  }
  if (kernelPubKeys.length === 0) {
    setAccessValue(accessKernelPubkeys, "None");
    return;
  }
  setAccessValue(accessKernelPubkeys, formatPlural(kernelPubKeys.length, "key"));
};

let accessUpstreamLoading = false;
let accessUpstreamLoadedAt = 0;
const refreshAccessUpstreamSummary = async () => {
  if (accessUpstreamLoading) return;
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setAccessValue(accessUpstreamSource, "Missing token");
    setAccessValue(accessUpstreamExpiry, "Missing token");
    return;
  }
  accessUpstreamLoading = true;
  if (!accessUpstreamLoadedAt) {
    setAccessValue(accessUpstreamSource, "Loading...");
    setAccessValue(accessUpstreamExpiry, "Loading...");
  }
  try {
    const res = await fetch(apiUrl("/health/providers"), {
      cache: "no-store",
      headers: { Authorization: `Bearer ${token}` },
    });
    const data = await res.json().catch(() => null);
    const source = data?.codex?.source ?? "unknown";
    const expirations = Array.isArray(data?.codex?.accounts)
      ? data.codex.accounts
        .map((account) => account?.access_token_exp_ms)
        .filter((value) => typeof value === "number")
      : [];
    const expMs = expirations.length ? Math.min(...expirations) : null;
    if (!res.ok) {
      if (accessUpstreamLoadedAt) return;
      setAccessValue(accessUpstreamSource, source === "none" ? "None" : source);
      setAccessValue(accessUpstreamExpiry, data?.error ?? "Unavailable");
      return;
    }
    setAccessValue(accessUpstreamSource, source === "none" ? "None" : source);
    setAccessValue(accessUpstreamExpiry, typeof expMs === "number" ? formatDate(expMs) : "Unknown");
    accessUpstreamLoadedAt = Date.now();
  } catch {
    if (accessUpstreamLoadedAt) return;
    setAccessValue(accessUpstreamSource, "Offline");
    setAccessValue(accessUpstreamExpiry, "Offline");
  } finally {
    accessUpstreamLoading = false;
  }
};

const refreshAccessOverview = async () => {
  updateAccessApiKeysSummary();
  updateAccessGithubSummary();
  updateAccessPubkeysSummary();
  await Promise.all([
    refreshAccessUpstreamSummary(),
    keysLoadedAt ? Promise.resolve() : refreshKeys(),
    kernelPubKeysLoadedAt ? Promise.resolve() : refreshKernelPubKeys(),
  ]);
  updateAccessApiKeysSummary();
  updateAccessGithubSummary();
  updateAccessPubkeysSummary();
};

const getKernelListMissingTokenMessage = () => "Paste an admin token to load GitHub access analytics and rate limits.";

const getKernelListTargetChangedMessage = () => "Target changed. Loading GitHub access analytics and rate limits...";

const getKernelQueueMissingTokenMessage = () => "Paste an admin token to load the rate limit queue.";

const getKernelQueueTargetChangedMessage = () => "Target changed. Loading the rate limit queue...";

const normalizeKernelFilterText = (value) => value.trim().toLowerCase();

const getKernelRecordOwner = (record) => (typeof record?.owner === "string" ? record.owner : "");
const getKernelRecordRepo = (record) => (typeof record?.repo === "string" ? record.repo : "");

const getKernelRecordLabel = (record) => {
  const owner = getKernelRecordOwner(record);
  const repo = getKernelRecordRepo(record);
  if (!owner) return repo ? `unknown/${repo}` : "unknown";
  return repo ? `${owner}/${repo}` : owner;
};

const getKernelRecordUsage = (record) => {
  if (!record || typeof record !== "object") return null;
  if (Object.prototype.hasOwnProperty.call(record, "usage")) return record.usage;
  if (Object.prototype.hasOwnProperty.call(record, "total_requests")) return record;
  return null;
};

const getKernelUsageLastSeen = (usage) => {
  if (!usage || typeof usage !== "object") return null;
  if (typeof usage.last_seen_at_ms !== "number" || !Number.isFinite(usage.last_seen_at_ms)) return null;
  return Math.trunc(usage.last_seen_at_ms);
};

const getKernelWindowRequests = (record) => {
  if (typeof record?.usage_requests === "number" && Number.isFinite(record.usage_requests)) {
    return Math.trunc(record.usage_requests);
  }
  return 0;
};

const getKernelTotalRequests = (record) => {
  const usage = getKernelRecordUsage(record);
  if (!usage || typeof usage !== "object") return 0;
  return toNumber(usage.total_requests);
};

const isKernelUsageEmpty = (usage) => {
  if (usage === null || usage === undefined) return true;
  if (typeof usage !== "object") return false;
  return toNumber(usage.total_requests) <= 0;
};

const getKernelPolicyLimitValue = (record) => {
  if (!record || typeof record !== "object") return null;
  const limit = record.usage_limit_requests;
  if (typeof limit !== "number" || !Number.isFinite(limit)) return null;
  return Math.trunc(limit);
};

const openKernelPolicyEditor = (owner, repo) => {
  if (!owner || owner === "unknown") return;
  setKernelNewPanelOpen(true);
  kernelNewOwnerInput.value = owner;
  kernelNewRepoInput.value = repo ?? "";
  setKernelNewBadge("unknown", "Editing...");
  kernelNewLimitInput.focus();
  kernelNewPanel.scrollIntoView({ behavior: "smooth", block: "start" });
};

const hasKernelPolicyCap = (owner, repo, policyState) => {
  if (!policyState) return false;
  const orgAvailable = policyState.orgAvailable === true;
  const repoAvailable = policyState.repoAvailable === true;
  const orgRecord = orgAvailable && owner ? policyState.orgLimits.get(owner) ?? null : null;
  const repoRecord = repoAvailable && owner && repo ? policyState.repoLimits.get(`${owner}/${repo}`) ?? null : null;
  const orgLimit = orgAvailable ? getKernelPolicyLimitValue(orgRecord) : null;
  const repoLimit = repoAvailable ? getKernelPolicyLimitValue(repoRecord) : null;
  return (orgLimit !== null && orgLimit !== -1) || (repoLimit !== null && repoLimit !== -1);
};

const buildKernelAnalyticsSummaryRow = (usage) => {
  const infoRow = document.createElement("div");
  infoRow.dataset.keyInfo = "info";

  const hasUsage = usage && typeof usage === "object";
  const errorCount = hasUsage ? toNumber(usage.error_requests) : 0;
  appendKeyInfo(infoRow, "Requests", hasUsage ? formatNumber(usage.total_requests) : "—");
  appendKeyInfo(infoRow, "Tokens", hasUsage ? formatNumber(usage.total_tokens) : "—");
  appendKeyInfo(infoRow, "Errors", hasUsage ? formatNumber(errorCount) : "—", { state: errorCount > 0 ? "bad" : "" });
  appendKeyInfo(infoRow, "Last seen", hasUsage ? formatDate(usage.last_seen_at_ms) : "—");
  return infoRow;
};

const buildKernelOrgSummaryRow = (group) => {
  const infoRow = document.createElement("div");
  infoRow.dataset.keyInfo = "info";

  const repos = Array.isArray(group?.repos) ? group.repos : [];
  const repoTotal = repos.length;
  let activeRepos = 0;
  let lastSeen = 0;

  repos.forEach((record) => {
    const usage = getKernelRecordUsage(record);
    if (usage && typeof usage === "object") {
      if (toNumber(usage.total_requests) > 0) activeRepos += 1;
      const seen = getKernelUsageLastSeen(usage);
      if (seen !== null) lastSeen = Math.max(lastSeen, seen);
    }
  });

  if (repoTotal === 0) {
    const orgUsage = group?.org ? getKernelRecordUsage(group.org) : null;
    const orgSeen = getKernelUsageLastSeen(orgUsage);
    if (orgSeen !== null) lastSeen = orgSeen;
  }

  appendKeyInfo(infoRow, "Repos", formatNumber(repoTotal));
  appendKeyInfo(infoRow, "Active repos", formatNumber(activeRepos));
  appendKeyInfo(infoRow, "Last seen", lastSeen ? formatDate(lastSeen) : "—");
  return infoRow;
};

const summarizeKernelPolicyCoverage = (records, policyState) => {
  let total = 0;
  let unbounded = 0;

  records.forEach((record) => {
    if (getKernelTotalRequests(record) <= 0) return;
    total += 1;
    if (policyState?.available) {
      const owner = getKernelRecordOwner(record);
      const repo = getKernelRecordRepo(record);
      if (!hasKernelPolicyCap(owner, repo, policyState)) unbounded += 1;
    }
  });

  return { total, unbounded };
};

const updateKernelAttention = (summary, policyState) => {
  const hasUsage = summary?.total > 0;
  if (!policyState?.available) {
    if (!hasUsage) {
      setKernelAttention("");
      return;
    }
    const message = policyState?.message
      ? `Rate limit data unavailable: ${policyState.message}`
      : "Rate limit data unavailable.";
    setKernelAttention(message);
    return;
  }
  if (!summary || summary.total === 0 || summary.unbounded === 0) {
    setKernelAttention("");
    return;
  }
  const label = "repos";
  const countText = `${summary.unbounded} of ${summary.total}`;
  setKernelAttention(`Attention: ${countText} ${label} with analytics have no caps (rate limits unset or unlimited).`);
};

const normalizeKernelListRecords = (records) => ({
  org: Array.isArray(records?.org) ? records.org : [],
  repo: Array.isArray(records?.repo) ? records.repo : [],
});

const recordMatchesKernelFilters = (record, filterText, filterMode, nowMs) => {
  const label = getKernelRecordLabel(record).toLowerCase();
  if (filterText && !label.includes(filterText)) return false;

  const usage = getKernelRecordUsage(record);
  if (filterMode === "active") {
    const lastSeen = getKernelUsageLastSeen(usage);
    return lastSeen !== null && lastSeen >= nowMs - KERNEL_ACTIVITY_WINDOW_MS;
  }
  if (filterMode === "near-limit") {
    const limit = getKernelPolicyLimitValue(record);
    if (limit === null || limit <= 0 || limit === -1) return false;
    const current = getKernelWindowRequests(record);
    return current / limit >= KERNEL_NEAR_LIMIT_RATIO;
  }
  if (filterMode === "no-usage") {
    return isKernelUsageEmpty(usage);
  }
  return true;
};

const applyKernelFilters = (records) => {
  const { org, repo } = normalizeKernelListRecords(records);
  const filterText = normalizeKernelFilterText(kernelFilterInput.value);
  const filterMode = kernelShowSelect.value;
  const nowMs = Date.now();

  return {
    org: org.filter((record) => recordMatchesKernelFilters(record, filterText, filterMode, nowMs)),
    repo: repo.filter((record) => recordMatchesKernelFilters(record, filterText, filterMode, nowMs)),
  };
};

const getKernelRecordLastSeen = (record) => getKernelUsageLastSeen(getKernelRecordUsage(record)) ?? 0;

const buildKernelGroups = (records) => {
  const { org, repo } = normalizeKernelListRecords(records);
  const groups = new Map();
  const ensureGroup = (owner) => {
    const key = owner || "unknown";
    const existing = groups.get(key);
    if (existing) return existing;
    const next = { owner: key, org: null, repos: [] };
    groups.set(key, next);
    return next;
  };

  org.forEach((record) => {
    const owner = getKernelRecordOwner(record);
    const group = ensureGroup(owner);
    group.org = record;
  });

  repo.forEach((record) => {
    const owner = getKernelRecordOwner(record);
    const group = ensureGroup(owner);
    group.repos.push(record);
  });

  return [...groups.values()];
};

const getKernelGroupMetrics = (group) => {
  let lastSeen = 0;
  let usage = 0;
  const records = [];
  if (group.org) records.push(group.org);
  if (Array.isArray(group.repos)) records.push(...group.repos);
  records.forEach((record) => {
    lastSeen = Math.max(lastSeen, getKernelRecordLastSeen(record));
    usage = Math.max(usage, getKernelTotalRequests(record));
  });
  return { lastSeen, usage };
};

const sortKernelGroups = (groups) => {
  const mode = kernelSortSelect.value;
  const sorted = [...groups];
  sorted.sort((a, b) => {
    const aMetrics = getKernelGroupMetrics(a);
    const bMetrics = getKernelGroupMetrics(b);
    if (mode === "recent") {
      const aSeen = aMetrics.lastSeen;
      const bSeen = bMetrics.lastSeen;
      if (aSeen !== bSeen) return bSeen - aSeen;
    } else if (mode === "usage") {
      const aUsage = aMetrics.usage;
      const bUsage = bMetrics.usage;
      if (aUsage !== bUsage) return bUsage - aUsage;
    }
    return a.owner.localeCompare(b.owner);
  });
  return sorted;
};

const sortKernelRepoRecords = (records) => {
  const mode = kernelSortSelect.value;
  const sorted = [...records];
  sorted.sort((a, b) => {
    if (mode === "recent") {
      const aSeen = getKernelRecordLastSeen(a);
      const bSeen = getKernelRecordLastSeen(b);
      if (aSeen !== bSeen) return bSeen - aSeen;
    } else if (mode === "usage") {
      const aUsage = getKernelTotalRequests(a);
      const bUsage = getKernelTotalRequests(b);
      if (aUsage !== bUsage) return bUsage - aUsage;
    }
    return getKernelRecordRepo(a).localeCompare(getKernelRecordRepo(b));
  });
  return sorted;
};

const formatKernelListBadge = (counts) => {
  const orgTotal = counts?.orgTotal ?? 0;
  const repoTotal = counts?.repoTotal ?? 0;
  const orgVisible = counts?.orgVisible ?? 0;
  const repoVisible = counts?.repoVisible ?? 0;
  const total = orgTotal + repoTotal;
  if (total === 0) return "No analytics or rate limits";

  const buildCountText = (visible, totalCount, singular, plural) => {
    if (totalCount === 0) return "";
    if (visible === totalCount) return `${totalCount} ${totalCount === 1 ? singular : plural}`;
    return `${visible}/${totalCount} ${plural}`;
  };

  const orgText = buildCountText(orgVisible, orgTotal, "org", "orgs");
  const repoText = buildCountText(repoVisible, repoTotal, "repo", "repos");
  return [orgText, repoText].filter(Boolean).join(" · ");
};

const refreshKernelList = () => {
  if (!kernelListRecords || typeof kernelListRecords !== "object") {
    setKernelListMessage("No analytics or rate limits yet.");
    setKernelListBadge("ok", formatKernelListBadge({ orgTotal: 0, repoTotal: 0, orgVisible: 0, repoVisible: 0 }));
    return;
  }
  const result = renderKernelList(kernelListRecords, kernelPolicyState);
  setKernelListBadge("ok", formatKernelListBadge(result));
};

const refreshKernelListDebounced = debounce(refreshKernelList, 250);

const setKernelPubKeysMessage = (text) => {
  kernelPubKeysList.textContent = "";
  const message = document.createElement("p");
  message.dataset.empty = "kernel-pubkeys";
  message.textContent = text;
  kernelPubKeysList.appendChild(message);
};

const renderKernelPolicyQueue = (records) => {
  kernelQueueList.textContent = "";
  if (!Array.isArray(records) || records.length === 0) {
    setKernelQueueMessage("No rate limit gaps yet.");
    return;
  }

  let rendered = 0;
  records.forEach((record, index) => {
    if (!record || typeof record !== "object") return;
    const owner = typeof record.owner === "string" ? record.owner : "";
    const repo = typeof record.repo === "string" ? record.repo : "";
    if (!owner || !repo) return;
    const lastRoute = typeof record.last_route === "string" ? record.last_route : "";

    const row = document.createElement("article");
    row.dataset.key = "kernel-queue";
    row.dataset.state = "warning";
    row.style.setProperty("--i", index);
    row.setAttribute("role", "listitem");

    const main = document.createElement("div");
    main.dataset.keyMain = "main";

    const header = document.createElement("div");
    header.dataset.keyHeader = "header";

    const title = document.createElement("div");
    title.dataset.keyTitle = "title";
    title.textContent = `${owner}/${repo}`;

    const controls = document.createElement("div");
    controls.dataset.keyControls = "controls";

    const status = document.createElement("span");
    status.dataset.badge = "status";
    status.dataset.state = "bad";
    status.textContent = "Needs rate limit";

    const actionRow = document.createElement("div");
    actionRow.dataset.actionRow = "actions";
    const addBtn = document.createElement("button");
    addBtn.type = "button";
    addBtn.dataset.variant = "primary";
    applyIconButton(addBtn, "plus", `Add rate limit for ${owner}/${repo}`);
    actionRow.appendChild(addBtn);

    controls.appendChild(status);
    controls.appendChild(actionRow);
    header.appendChild(title);
    header.appendChild(controls);

    const infoRow = document.createElement("div");
    infoRow.dataset.keyInfo = "info";
    appendKeyInfo(infoRow, "Requests", formatNumber(record.request_count));
    appendKeyInfo(infoRow, "First seen", formatDate(record.first_seen_at_ms));
    appendKeyInfo(infoRow, "Last seen", formatDate(record.last_seen_at_ms));
    appendKeyInfo(infoRow, "Route", lastRoute ? lastRoute : "unknown", { mono: true });

    main.appendChild(header);
    main.appendChild(infoRow);
    row.appendChild(main);
    kernelQueueList.appendChild(row);
    rendered += 1;

    addBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      openKernelPolicyEditor(owner, repo);
    });
  });

  if (rendered === 0) {
    setKernelQueueMessage("No rate limit gaps yet.");
  }

  updateAccessGithubSummary();
};

const refreshKernelPolicyQueue = async () => {
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setKernelQueueBadge("bad", "Missing token");
    setKernelQueueMessage(getKernelQueueMissingTokenMessage());
    updateAccessGithubSummary();
    return;
  }

  if (kernelQueueLoading) return;
  kernelQueueLoading = true;
  setKernelQueueBadge("unknown", kernelQueueLoadedAt ? "Cached · refreshing" : "Loading...");

  try {
    const res = await fetch(apiUrl("/admin/kernel-policy-queue"), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      if (kernelQueueLoadedAt) {
        setKernelQueueBadge("unknown", "Cached · refresh unavailable");
        return;
      }
      setKernelQueueBadge("bad", data?.error?.message ?? "Error");
      setKernelQueueMessage("Failed to load the rate limit queue.");
      return;
    }

    const records = Array.isArray(data?.data) ? data.data : [];
    kernelQueueItems = records;
    kernelQueueLoadedAt = Date.now();
    renderKernelPolicyQueue(records);
    setKernelQueueBadge(
      "ok",
      records.length === 0 ? "No requests" : `${records.length} request${records.length === 1 ? "" : "s"}`,
    );
  } catch {
    if (kernelQueueLoadedAt) {
      setKernelQueueBadge("unknown", "Cached · offline");
      return;
    }
    kernelQueueItems = [];
    setKernelQueueBadge("bad", "Offline");
    setKernelQueueMessage("Failed to load the rate limit queue.");
  } finally {
    kernelQueueLoading = false;
    updateAccessGithubSummary();
  }
};

const ensureKernelPolicyQueueLoaded = async () => {
  if (currentAdminView !== "kernel") return;
  if (kernelQueueLoading) return;
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setKernelQueueBadge("bad", "Missing token");
    setKernelQueueMessage(getKernelQueueMissingTokenMessage());
    updateAccessGithubSummary();
    return;
  }
  if (kernelQueueLoadedAt && Date.now() - kernelQueueLoadedAt < 10_000) {
    const count = kernelQueueItems.length;
    setKernelQueueBadge("ok", count === 0 ? "No requests" : `${count} request${count === 1 ? "" : "s"}`);
    updateAccessGithubSummary();
    return;
  }
  await refreshKernelPolicyQueue();
};

const setKernelNewPanelOpen = (open) => {
  kernelNewPanel.hidden = !open;
  applyActionButton(kernelNewToggle, open ? "close" : "plus", open ? "Close form" : "New rate limit");
  if (!open) {
    resetKernelNewForm();
  }
};

const resetKernelNewForm = () => {
  kernelNewOwnerInput.value = "";
  kernelNewRepoInput.value = "";
  kernelNewLimitInput.value = "-1";
  kernelNewWindowInput.value = "";
  kernelNewExpiresInput.value = "";
  kernelNewNeverInput.checked = true;
  kernelNewExpiresInput.disabled = true;
  setKernelNewBadge("unknown", "Idle");
};

const resetKernelPubKeyForm = () => {
  kernelPubKeyAppIdInput.value = "";
  kernelPubKeyOwnerInput.value = "";
  kernelPubKeyPemInput.value = "";
  setKernelPubKeyCreateBadge("unknown", "Idle");
};

const setKernelNewWindowPreset = (ms) => {
  kernelNewWindowInput.value = String(ms);
};

const parseKernelLimitValue = (raw, setBadgeFn) => {
  const trimmed = raw.trim();
  if (!trimmed) {
    setBadgeFn("bad", "Rate limit required");
    return null;
  }
  if (trimmed === "unlimited" || trimmed === "-1") return -1;
  const parsed = Number(trimmed);
  if (!Number.isFinite(parsed) || parsed < 0) {
    setBadgeFn("bad", "Invalid rate limit");
    return null;
  }
  return Math.trunc(parsed);
};

const parseKernelWindowValue = (raw, setBadgeFn) => {
  const trimmed = raw.trim();
  if (!trimmed) return { ok: true, value: null };
  const parsed = Number(trimmed);
  if (!Number.isFinite(parsed) || parsed <= 0) {
    setBadgeFn("bad", "Invalid window");
    return { ok: false, value: null };
  }
  return { ok: true, value: Math.trunc(parsed) };
};

const parseKernelExpiresValue = (raw, never, setBadgeFn) => {
  if (never) return { ok: true, value: -1 };
  const parsed = parseDateTimeLocalValue(raw);
  if (parsed === null) {
    setBadgeFn("bad", "Expiration required");
    return { ok: false, value: null };
  }
  if (parsed <= Date.now()) {
    setBadgeFn("bad", "Expiration must be in the future");
    return { ok: false, value: null };
  }
  return { ok: true, value: parsed };
};

const saveNewKernelLimit = async () => {
  if (kernelNewSaving) return;
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setKernelNewBadge("bad", "Missing token");
    setAuthBadge("bad", "Missing token");
    tokenInput.focus();
    return;
  }

  const owner = kernelNewOwnerInput.value.trim();
  if (!owner) {
    setKernelNewBadge("bad", "Owner required");
    kernelNewOwnerInput.focus();
    return;
  }

  const repo = kernelNewRepoInput.value.trim();
  const scope = repo ? "repo" : "org";

  const limitValue = parseKernelLimitValue(kernelNewLimitInput.value, setKernelNewBadge);
  if (limitValue === null) return;
  const windowResult = parseKernelWindowValue(kernelNewWindowInput.value, setKernelNewBadge);
  if (!windowResult.ok) return;
  const expiresResult = parseKernelExpiresValue(
    kernelNewExpiresInput.value,
    kernelNewNeverInput.checked,
    setKernelNewBadge,
  );
  if (!expiresResult.ok) return;

  kernelNewSaving = true;
  kernelNewSaveBtn.disabled = true;
  setKernelNewBadge("unknown", "Saving...");

  try {
    const payload = {
      owner,
      scope,
      usage_limit_requests: limitValue,
      expires_at_ms: expiresResult.value,
    };
    if (scope === "repo") payload.repo = repo;
    if (windowResult.value !== null) payload.window_ms = windowResult.value;

    const res = await fetch(apiUrl("/admin/kernel-usage"), {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${token}`,
      },
      body: JSON.stringify(payload),
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      setKernelNewBadge("bad", data?.error?.message ?? "Error");
      return;
    }

    setKernelNewBadge("ok", "Saved");
    setKernelNewPanelOpen(false);
    await loadKernelList();
    kernelQueueLoadedAt = 0;
    await ensureKernelPolicyQueueLoaded();
  } catch {
    setKernelNewBadge("bad", "Offline");
  } finally {
    kernelNewSaving = false;
    kernelNewSaveBtn.disabled = false;
  }
};

const fetchKernelPolicyList = async (token, scope) => {
  try {
    const url = new URL(apiUrl("/admin/kernel-usage"));
    url.searchParams.set("scope", scope);
    url.searchParams.set("list", "1");

    const res = await fetch(url.toString(), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      return { ok: false, message: data?.error?.message ?? "Error", records: [] };
    }
    return { ok: true, message: "", records: Array.isArray(data?.limits) ? data.limits : [] };
  } catch {
    return { ok: false, message: "Offline", records: [] };
  }
};

const fetchKernelUsageInventory = async (token, scope) => {
  try {
    const url = new URL(apiUrl("/admin/kernel-usage"));
    url.searchParams.set("scope", scope);
    url.searchParams.set("list", "1");
    url.searchParams.set("inventory", "1");

    const res = await fetch(url.toString(), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      return { ok: false, message: data?.error?.message ?? "Error", records: [] };
    }
    return { ok: true, message: "", records: Array.isArray(data?.usage) ? data.usage : [] };
  } catch {
    return { ok: false, message: "Offline", records: [] };
  }
};

const buildKernelPolicyStateFromLists = (orgResult, repoResult) => {
  if (!orgResult.ok && !repoResult.ok) {
    const message = [orgResult.message, repoResult.message].filter(Boolean).join(" · ") || "Unavailable";
    return {
      available: false,
      message,
      orgAvailable: false,
      repoAvailable: false,
      orgLimits: new Map(),
      repoLimits: new Map(),
    };
  }

  const orgLimits = new Map();
  if (orgResult.ok) {
    orgResult.records.forEach((record) => {
      const owner = typeof record?.owner === "string" ? record.owner : "";
      if (owner) orgLimits.set(owner, record);
    });
  }

  const repoLimits = new Map();
  if (repoResult.ok) {
    repoResult.records.forEach((record) => {
      const owner = typeof record?.owner === "string" ? record.owner : "";
      const repo = typeof record?.repo === "string" ? record.repo : "";
      if (owner && repo) repoLimits.set(`${owner}/${repo}`, record);
    });
  }

  const messageParts = [];
  if (!orgResult.ok) messageParts.push(`Org rate limits unavailable: ${orgResult.message}`);
  if (!repoResult.ok) messageParts.push(`Repo rate limits unavailable: ${repoResult.message}`);

  return {
    available: orgResult.ok && repoResult.ok,
    message: messageParts.join(" · "),
    orgAvailable: orgResult.ok,
    repoAvailable: repoResult.ok,
    orgLimits,
    repoLimits,
  };
};

const mergeKernelRecords = (usageRecords, policyRecords, scope) => {
  const usageMap = new Map();
  usageRecords.forEach((record) => {
    if (!record || typeof record !== "object") return;
    const owner = getKernelRecordOwner(record);
    const repo = scope === "repo" ? getKernelRecordRepo(record) : "";
    if (!owner || (scope === "repo" && !repo)) return;
    const key = scope === "repo" ? `${owner}/${repo}` : owner;
    usageMap.set(key, record);
  });

  const policyMap = new Map();
  policyRecords.forEach((record) => {
    if (!record || typeof record !== "object") return;
    const owner = getKernelRecordOwner(record);
    const repo = scope === "repo" ? getKernelRecordRepo(record) : "";
    if (!owner || (scope === "repo" && !repo)) return;
    const key = scope === "repo" ? `${owner}/${repo}` : owner;
    policyMap.set(key, record);
  });

  const keys = new Set([...usageMap.keys(), ...policyMap.keys()]);
  const merged = [];
  keys.forEach((key) => {
    const usage = usageMap.get(key) ?? null;
    const policy = policyMap.get(key) ?? null;
    const owner = policy?.owner ?? usage?.owner ?? (scope === "repo" ? key.split("/")[0] : key);
    const repo = scope === "repo" ? (policy?.repo ?? usage?.repo ?? key.split("/")[1]) : "";
    const record = policy ? policy : { owner, repo };
    record.owner = owner;
    record.repo = scope === "repo" ? repo : "";
    record.usage = usage ?? null;
    merged.push(record);
  });

  return merged;
};

const loadKernelList = async () => {
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setKernelListBadge("bad", "Missing token");
    setAuthBadge("bad", "Missing token");
    setKernelListMessage(getKernelListMissingTokenMessage());
    resetKernelPolicyState();
    setKernelAttention("");
    updateAccessGithubSummary();
    return;
  }

  const loadId = ++kernelListLoadId;
  if (kernelListLoadedAt) {
    setKernelListBadge("unknown", "Cached · refreshing");
  } else {
    setKernelListBadge("unknown", "Loading...");
    resetKernelPolicyState();
    setKernelAttention("");
  }

  try {
    const [orgUsageResult, repoUsageResult, orgPolicyResult, repoPolicyResult] = await Promise.all([
      fetchKernelUsageInventory(token, "org"),
      fetchKernelUsageInventory(token, "repo"),
      fetchKernelPolicyList(token, "org"),
      fetchKernelPolicyList(token, "repo"),
    ]);
    if (loadId !== kernelListLoadId) return;
    if (
      !orgUsageResult.ok &&
      !repoUsageResult.ok &&
      !orgPolicyResult.ok &&
      !repoPolicyResult.ok
    ) {
      const message = [
        orgUsageResult.message,
        repoUsageResult.message,
        orgPolicyResult.message,
        repoPolicyResult.message,
      ]
        .filter(Boolean)
        .join(" · ") || "Error";
      if (kernelListLoadedAt) {
        setKernelListBadge("unknown", "Cached · refresh unavailable");
        updateAccessGithubSummary();
        return;
      }
      setKernelListBadge("bad", message);
      setKernelListMessage(message);
      updateAccessGithubSummary();
      return;
    }

    kernelPolicyState = buildKernelPolicyStateFromLists(orgPolicyResult, repoPolicyResult);
    kernelListRecords = {
      org: mergeKernelRecords(
        orgUsageResult.ok ? orgUsageResult.records : [],
        orgPolicyResult.ok ? orgPolicyResult.records : [],
        "org",
      ),
      repo: mergeKernelRecords(
        repoUsageResult.ok ? repoUsageResult.records : [],
        repoPolicyResult.ok ? repoPolicyResult.records : [],
        "repo",
      ),
    };
    const result = renderKernelList(kernelListRecords, kernelPolicyState);
    kernelListLoadedAt = Date.now();
    const badgeText = formatKernelListBadge(result);
    const warnings = [];
    if (!orgUsageResult.ok) warnings.push(`Org analytics ${orgUsageResult.message}`);
    if (!repoUsageResult.ok) warnings.push(`Repo analytics ${repoUsageResult.message}`);
    if (!orgPolicyResult.ok) warnings.push(`Org rate limits ${orgPolicyResult.message}`);
    if (!repoPolicyResult.ok) warnings.push(`Repo rate limits ${repoPolicyResult.message}`);
    if (warnings.length) {
      setKernelListBadge("warning", `${badgeText} · ${warnings.join(" · ")}`);
    } else {
      setKernelListBadge("ok", badgeText);
    }
    updateAccessGithubSummary();
  } catch {
    if (loadId !== kernelListLoadId) return;
    if (kernelListLoadedAt) {
      setKernelListBadge("unknown", "Cached · offline");
      updateAccessGithubSummary();
      return;
    }
    setKernelListBadge("bad", "Offline");
    setKernelListMessage("Request failed.");
    updateAccessGithubSummary();
  }
};

const KERNEL_ANALYTICS_DETAILS = {
  label: "Analytics",
  unavailable: "Analytics unavailable.",
  empty: "No analytics yet.",
};

const buildKernelPolicyPlaceholder = (record, options = {}) => {
  const owner = options.owner ?? getKernelRecordOwner(record);
  const repo = options.repo ?? getKernelRecordRepo(record);
  const scope = options.scope ?? (repo ? "repo" : "org");
  const titleText = options.titleText || (repo ? repo : owner || "unknown");
  const policyAvailable = options.policyAvailable !== false;
  const showDetails = options.showDetails ?? options.isSubtile === true;
  const canAdd = policyAvailable && owner && owner !== "unknown" && (scope === "org" || repo);

  const row = document.createElement("article");
  row.dataset.key = "kernel-policy";
  row.dataset.state = "warning";
  if (options.isSubtile) row.dataset.subtile = "true";
  if (typeof options.index === "number") row.style.setProperty("--i", options.index);
  row.setAttribute("role", "listitem");

  const main = document.createElement("div");
  main.dataset.keyMain = "main";

  const header = document.createElement("div");
  header.dataset.keyHeader = "header";

  const title = document.createElement("div");
  title.dataset.keyTitle = "title";
  title.textContent = titleText;

  header.appendChild(title);
  main.appendChild(header);

  const usage = getKernelRecordUsage(record);
  const summaryRow = options.summaryRow ?? buildKernelAnalyticsSummaryRow(usage);
  if (summaryRow) main.appendChild(summaryRow);

  const infoRow = document.createElement("div");
  infoRow.dataset.keyInfo = "info";
  const policyLabel = scope === "repo" ? "Repo rate limit" : "Org rate limit";
  const policyValue = policyAvailable ? "Not set" : "Unavailable";
  const policyItem = appendKeyInfo(infoRow, policyLabel, policyValue, { state: "warning" });
  if (canAdd) {
    const addBtn = document.createElement("button");
    addBtn.type = "button";
    addBtn.dataset.variant = "primary";
    applyIconButton(addBtn, "plus", scope === "repo" ? "Add repo rate limit" : "Add org rate limit");
    const valueText = document.createElement("span");
    valueText.textContent = policyValue;
    policyItem.valueEl.textContent = "";
    policyItem.valueEl.dataset.inlineActions = "true";
    policyItem.valueEl.appendChild(valueText);
    policyItem.valueEl.appendChild(addBtn);
    addBtn.addEventListener("click", (event) => {
      event.stopPropagation();
      openKernelPolicyEditor(owner, scope === "repo" ? repo : "");
    });
  }
  appendKeyInfo(infoRow, "Window", "—");
  appendKeyInfo(infoRow, "Window requests", "—");
  appendKeyInfo(infoRow, "Reset at", "—");
  appendKeyInfo(infoRow, "Expires", "—");
  appendKeyInfo(infoRow, "Updated", "—");
  main.appendChild(infoRow);

  if (showDetails) {
    main.appendChild(buildUsageDetails(usage, KERNEL_ANALYTICS_DETAILS));
  }

  row.appendChild(main);
  return row;
};

const buildKernelPolicyTile = (record, options = {}) => {
  if (!record || typeof record !== "object") return null;
  const owner = options.owner ?? getKernelRecordOwner(record);
  const repo = options.repo ?? getKernelRecordRepo(record);
  const scope = options.scope ?? (repo ? "repo" : "org");
  const titleText = options.titleText || (repo ? repo : owner || "unknown");
  const confirmLabel = repo ? `${owner}/${repo}` : owner || titleText;

  const row = document.createElement("article");
  row.dataset.key = "kernel-policy";
  if (options.isSubtile) row.dataset.subtile = "true";
  if (typeof options.index === "number") row.style.setProperty("--i", options.index);
  const expired = isExpiredAt(record.expires_at_ms);
  row.dataset.state = expired || record.usage_limit_requests === 0 ? "revoked" : "active";
  row.setAttribute("role", "listitem");

  const main = document.createElement("div");
  main.dataset.keyMain = "main";

  const header = document.createElement("div");
  header.dataset.keyHeader = "header";

  const title = document.createElement("div");
  title.dataset.keyTitle = "title";
  title.textContent = titleText;

  const controls = document.createElement("div");
  controls.dataset.keyControls = "controls";

  const actionRow = document.createElement("div");
  actionRow.dataset.actionRow = "actions";

  const editBtn = document.createElement("button");
  editBtn.type = "button";
  applyIconButton(editBtn, "edit", "Edit rate limit");

  const deleteBtn = document.createElement("button");
  deleteBtn.type = "button";
  deleteBtn.dataset.variant = "danger";
  applyIconButton(deleteBtn, "trash", "Delete rate limit");

  actionRow.appendChild(editBtn);
  actionRow.appendChild(deleteBtn);
  controls.appendChild(actionRow);
  header.appendChild(title);
  header.appendChild(controls);

  const usage = getKernelRecordUsage(record);
  const summaryRow = options.summaryRow ?? buildKernelAnalyticsSummaryRow(usage);

  const infoRow = document.createElement("div");
  infoRow.dataset.keyInfo = "info";

  const limitState = record.usage_limit_requests === 0 ? "bad" : "";
  const limitInfo = appendKeyInfo(
    infoRow,
    "Rate limit",
    formatLimitValue(record.usage_limit_requests),
    limitState ? { state: limitState } : {},
  );
  const windowInfo = appendKeyInfo(infoRow, "Window", formatWindowMs(record.window_ms));
  const usageInfo = appendKeyInfo(infoRow, "Window requests", formatNumber(record.usage_requests));
  const resetInfo = appendKeyInfo(infoRow, "Reset at", formatDate(record.usage_reset_at_ms));
  const expiresInfo = appendKeyInfo(infoRow, "Expires", formatExpires(record.expires_at_ms), {
    state: expired ? "bad" : "",
  });
  const updatedInfo = appendKeyInfo(infoRow, "Updated", formatDate(record.updated_at_ms));

  main.appendChild(header);
  if (summaryRow) main.appendChild(summaryRow);
  main.appendChild(infoRow);

  const editPanel = document.createElement("div");
  editPanel.dataset.editPanel = "panel";
  editPanel.hidden = true;

  const editFields = document.createElement("div");
  editFields.dataset.editFields = "fields";

  const limitField = document.createElement("label");
  limitField.dataset.field = "true";
  const limitLabel = document.createElement("span");
  limitLabel.dataset.label = "label";
  limitLabel.textContent = "Rate limit";
  const limitInput = document.createElement("input");
  limitInput.type = "number";
  limitInput.inputMode = "numeric";
  limitInput.value = typeof record.usage_limit_requests === "number" ? String(record.usage_limit_requests) : "-1";
  limitField.appendChild(limitLabel);
  limitField.appendChild(limitInput);

  const windowField = document.createElement("label");
  windowField.dataset.field = "true";
  const windowLabel = document.createElement("span");
  windowLabel.dataset.label = "label";
  windowLabel.textContent = "Window (ms)";
  const windowInput = document.createElement("input");
  windowInput.type = "number";
  windowInput.inputMode = "numeric";
  windowInput.value = typeof record.window_ms === "number" ? String(Math.trunc(record.window_ms)) : "";
  windowField.appendChild(windowLabel);
  windowField.appendChild(windowInput);

  const expiresField = document.createElement("label");
  expiresField.dataset.field = "true";
  const expiresLabel = document.createElement("span");
  expiresLabel.dataset.label = "label";
  expiresLabel.textContent = "Expires";
  const expiresInput = document.createElement("input");
  expiresInput.type = "datetime-local";
  expiresInput.value = record.expires_at_ms === -1 ? "" : toDateTimeLocalValue(record.expires_at_ms);
  expiresInput.disabled = record.expires_at_ms === -1;
  expiresField.appendChild(expiresLabel);
  expiresField.appendChild(expiresInput);

  const neverLabel = document.createElement("label");
  neverLabel.dataset.check = "true";
  const neverInput = document.createElement("input");
  neverInput.type = "checkbox";
  neverInput.checked = record.expires_at_ms === -1;
  const neverText = document.createElement("span");
  neverText.textContent = "Never expires";
  neverLabel.appendChild(neverInput);
  neverLabel.appendChild(neverText);

  editFields.appendChild(limitField);
  editFields.appendChild(windowField);
  editFields.appendChild(expiresField);

  const presetRow = document.createElement("div");
  presetRow.dataset.layout = "row";
  const preset1m = document.createElement("button");
  preset1m.type = "button";
  preset1m.textContent = "1m";
  const preset1h = document.createElement("button");
  preset1h.type = "button";
  preset1h.textContent = "1h";
  const preset1d = document.createElement("button");
  preset1d.type = "button";
  preset1d.textContent = "1d";
  const preset1w = document.createElement("button");
  preset1w.type = "button";
  preset1w.textContent = "1w";
  presetRow.appendChild(preset1m);
  presetRow.appendChild(preset1h);
  presetRow.appendChild(preset1d);
  presetRow.appendChild(preset1w);

  const editActions = document.createElement("div");
  editActions.dataset.actionRow = "actions";
  const saveBtn = document.createElement("button");
  saveBtn.type = "button";
  saveBtn.dataset.variant = "primary";
  applyIconButton(saveBtn, "save", "Save changes");
  const cancelBtn = document.createElement("button");
  cancelBtn.type = "button";
  cancelBtn.textContent = "Cancel";
  editActions.appendChild(saveBtn);
  editActions.appendChild(cancelBtn);

  const editBadge = document.createElement("span");
  editBadge.dataset.badge = "status";
  editBadge.dataset.state = "unknown";
  editBadge.textContent = "Idle";

  editPanel.appendChild(editFields);
  editPanel.appendChild(neverLabel);
  editPanel.appendChild(presetRow);
  editPanel.appendChild(editActions);
  editPanel.appendChild(editBadge);

  main.appendChild(editPanel);
  const hasUsageField = Object.prototype.hasOwnProperty.call(record, "usage");
  const detailUsage = hasUsageField ? record.usage : undefined;
  if (hasUsageField && options.showDetails !== false) {
    main.appendChild(buildUsageDetails(detailUsage, KERNEL_ANALYTICS_DETAILS));
  }
  row.appendChild(main);

  const setEditBadge = (state, text) => setBadge(editBadge, state, text);

  const resetEditInputs = () => {
    limitInput.value = typeof record.usage_limit_requests === "number" ? String(record.usage_limit_requests) : "-1";
    windowInput.value = typeof record.window_ms === "number" ? String(Math.trunc(record.window_ms)) : "";
    neverInput.checked = record.expires_at_ms === -1;
    expiresInput.disabled = neverInput.checked;
    expiresInput.value = neverInput.checked ? "" : toDateTimeLocalValue(record.expires_at_ms);
    setEditBadge("unknown", "Idle");
  };

  const updateInfo = (updated) => {
    const next = updated ?? record;
    const nextExpired = isExpiredAt(next.expires_at_ms);
    limitInfo.valueEl.textContent = formatLimitValue(next.usage_limit_requests);
    if (nextExpired || next.usage_limit_requests === 0) {
      limitInfo.item.dataset.state = "bad";
      row.dataset.state = "revoked";
    } else {
      delete limitInfo.item.dataset.state;
      row.dataset.state = "active";
    }
    windowInfo.valueEl.textContent = formatWindowMs(next.window_ms);
    usageInfo.valueEl.textContent = formatNumber(next.usage_requests);
    resetInfo.valueEl.textContent = formatDate(next.usage_reset_at_ms);
    expiresInfo.valueEl.textContent = formatExpires(next.expires_at_ms);
    if (nextExpired) {
      expiresInfo.item.dataset.state = "bad";
    } else {
      delete expiresInfo.item.dataset.state;
    }
    updatedInfo.valueEl.textContent = formatDate(next.updated_at_ms);
  };

  const saveEdits = async () => {
    const limitValue = parseKernelLimitValue(limitInput.value, setEditBadge);
    if (limitValue === null) return;
    const windowResult = parseKernelWindowValue(windowInput.value, setEditBadge);
    if (!windowResult.ok) return;
    const expiresResult = parseKernelExpiresValue(expiresInput.value, neverInput.checked, setEditBadge);
    if (!expiresResult.ok) return;

    const token = getAdminToken();
    if (!token && !hasAdminCredential()) {
      setEditBadge("bad", "Missing token");
      setAuthBadge("bad", "Missing token");
      tokenInput.focus();
      return;
    }

    saveBtn.disabled = true;
    setEditBadge("unknown", "Saving...");

    try {
      const payload = {
        owner,
        scope,
        usage_limit_requests: limitValue,
        expires_at_ms: expiresResult.value,
      };
      if (scope === "repo") payload.repo = repo;
      if (windowResult.value !== null) payload.window_ms = windowResult.value;

      const res = await fetch(apiUrl("/admin/kernel-usage"), {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${token}`,
        },
        body: JSON.stringify(payload),
      });
      const data = await res.json().catch(() => null);
      if (!res.ok) {
        setEditBadge("bad", data?.error?.message ?? "Error");
        toast.error("Save failed", { description: data?.error?.message ?? "Error" });
        return;
      }

      if (data?.limit && typeof data.limit === "object") {
        Object.assign(record, data.limit);
      } else {
        record.usage_limit_requests = limitValue;
        if (windowResult.value !== null) record.window_ms = windowResult.value;
        record.expires_at_ms = expiresResult.value;
      }
      updateInfo(record);
      resetEditInputs();
      setEditBadge("ok", "Saved");
      toast.success("Rate limit saved");
    } catch {
      setEditBadge("bad", "Offline");
      toast.error("Save failed", { description: "Offline" });
    } finally {
      saveBtn.disabled = false;
    }
  };

  const deleteLimit = async () => {
    const token = getAdminToken();
    if (!token && !hasAdminCredential()) {
      setKernelListBadge("bad", "Missing token");
      setAuthBadge("bad", "Missing token");
      tokenInput.focus();
      return;
    }
    if (!confirm(`Delete rate limit for ${confirmLabel}?`)) return;

    setKernelListBadge("unknown", "Deleting...");
    deleteBtn.disabled = true;

    try {
      const res = await fetch(apiUrl("/admin/kernel-usage"), {
        method: "DELETE",
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${token}`,
        },
        body: JSON.stringify({ owner, repo: scope === "repo" ? repo : undefined, scope }),
      });
      const data = await res.json().catch(() => null);
      if (!res.ok) {
        setKernelListBadge("bad", data?.error?.message ?? "Error");
        toast.error("Delete failed", { description: data?.error?.message ?? "Error" });
        return;
      }
      await loadKernelList();
      toast.success("Rate limit deleted");
    } catch {
      setKernelListBadge("bad", "Offline");
      toast.error("Delete failed", { description: "Offline" });
    } finally {
      deleteBtn.disabled = false;
    }
  };

  editBtn.addEventListener("click", () => {
    if (!editPanel.hidden) {
      limitInput.focus();
      return;
    }
    editPanel.hidden = false;
    editBtn.disabled = true;
    limitInput.focus();
  });

  cancelBtn.addEventListener("click", () => {
    editPanel.hidden = true;
    editBtn.disabled = false;
    resetEditInputs();
  });

  saveBtn.addEventListener("click", () => {
    void saveEdits();
  });

  preset1m.addEventListener("click", () => {
    windowInput.value = "60000";
    setEditBadge("unknown", "Editing...");
  });
  preset1h.addEventListener("click", () => {
    windowInput.value = "3600000";
    setEditBadge("unknown", "Editing...");
  });
  preset1d.addEventListener("click", () => {
    windowInput.value = "86400000";
    setEditBadge("unknown", "Editing...");
  });
  preset1w.addEventListener("click", () => {
    windowInput.value = "604800000";
    setEditBadge("unknown", "Editing...");
  });

  deleteBtn.addEventListener("click", () => {
    void deleteLimit();
  });

  limitInput.addEventListener("input", () => setEditBadge("unknown", "Editing..."));
  windowInput.addEventListener("input", () => setEditBadge("unknown", "Editing..."));
  expiresInput.addEventListener("input", () => setEditBadge("unknown", "Editing..."));
  neverInput.addEventListener("change", () => {
    expiresInput.disabled = neverInput.checked;
    if (neverInput.checked) expiresInput.value = "";
    setEditBadge("unknown", "Editing...");
  });

  return row;
};

const renderKernelList = (records, policyState = kernelPolicyState) => {
  kernelList.textContent = "";
  const { org: _org, repo } = normalizeKernelListRecords(records);
  const allGroups = buildKernelGroups(records);
  const totalOrgCount = allGroups.length;
  const totalRepoCount = repo.length;

  if (totalOrgCount === 0 && totalRepoCount === 0) {
    setKernelListMessage("No analytics or rate limits yet.");
    updateKernelAttention({ total: 0, unbounded: 0 }, policyState);
    return { orgTotal: 0, repoTotal: 0, orgVisible: 0, repoVisible: 0 };
  }

  const filtered = applyKernelFilters(records);
  const visibleGroups = buildKernelGroups(filtered);
  if (visibleGroups.length === 0 && filtered.repo.length === 0) {
    setKernelListMessage("No matches.");
    updateKernelAttention({ total: 0, unbounded: 0 }, policyState);
    return {
      orgTotal: totalOrgCount,
      repoTotal: totalRepoCount,
      orgVisible: 0,
      repoVisible: 0,
    };
  }

  const sortedGroups = sortKernelGroups(visibleGroups);
  sortedGroups.forEach((group, index) => {
    const policyAvailableOrg = policyState?.orgAvailable !== false;
    const orgRecord = group.org ?? { owner: group.owner, repo: "", usage: null };
    const orgHasPolicy = policyAvailableOrg && typeof orgRecord?.usage_limit_requests === "number";
    const orgSummaryRow = buildKernelOrgSummaryRow(group);
    const groupTile = orgHasPolicy
      ? buildKernelPolicyTile(orgRecord, {
        owner: group.owner,
        repo: "",
        scope: "org",
        titleText: group.owner || "unknown",
        index,
        summaryRow: orgSummaryRow,
        showDetails: false,
      })
      : buildKernelPolicyPlaceholder(orgRecord, {
        owner: group.owner,
        repo: "",
        scope: "org",
        titleText: group.owner || "unknown",
        index,
        summaryRow: orgSummaryRow,
        showDetails: false,
        policyAvailable: policyAvailableOrg,
      });

    if (!groupTile) return;
    groupTile.dataset.group = "org";

    const repos = sortKernelRepoRecords(group.repos);
    if (repos.length) {
      const policyAvailableRepo = policyState?.repoAvailable !== false;
      const sublist = document.createElement("div");
      sublist.dataset.sublist = "repos";
      sublist.setAttribute("role", "list");
      repos.forEach((record, repoIndex) => {
        const repoName = getKernelRecordRepo(record) || "unknown";
        const repoHasPolicy = policyAvailableRepo && typeof record?.usage_limit_requests === "number";
        const tile = repoHasPolicy
          ? buildKernelPolicyTile(record, {
            owner: group.owner,
            repo: repoName,
            scope: "repo",
            titleText: repoName,
            index: repoIndex,
            isSubtile: true,
            showDetails: true,
          })
          : buildKernelPolicyPlaceholder(record, {
            owner: group.owner,
            repo: repoName,
            scope: "repo",
            titleText: repoName,
            index: repoIndex,
            isSubtile: true,
            showDetails: true,
            policyAvailable: policyAvailableRepo,
          });
        if (tile) sublist.appendChild(tile);
      });

      const accordion = document.createElement("details");
      accordion.dataset.kernelRepos = group.owner || "unknown";
      const existing = kernelOrgRepoAccordionState.get(group.owner);
      accordion.open = existing === undefined ? true : existing === true;

      const summary = document.createElement("summary");
      summary.dataset.kernelReposTitle = "title";
      const label = document.createElement("span");
      label.dataset.kernelReposLabel = "label";
      label.textContent = "Repos";
      const meta = document.createElement("span");
      meta.dataset.kernelReposMeta = "meta";
      meta.textContent = formatPlural(repos.length, "repo");
      summary.appendChild(label);
      summary.appendChild(meta);
      accordion.appendChild(summary);
      accordion.appendChild(sublist);

      accordion.addEventListener("toggle", () => {
        kernelOrgRepoAccordionState.set(group.owner, accordion.open);
      });

      groupTile.appendChild(accordion);
    }
    kernelList.appendChild(groupTile);
  });

  const summary = summarizeKernelPolicyCoverage(filtered.repo, policyState);
  updateKernelAttention(summary, policyState);

  return {
    orgTotal: totalOrgCount,
    repoTotal: totalRepoCount,
    orgVisible: visibleGroups.length,
    repoVisible: filtered.repo.length,
  };
};

const appendMetaItem = (container, label, value, options = {}) => {
  const item = document.createElement("div");
  item.dataset.metaItem = "usage";
  if (options.state) item.dataset.state = options.state;

  const labelEl = document.createElement("span");
  labelEl.dataset.metaLabel = "label";
  labelEl.textContent = label;

  const valueEl = document.createElement("span");
  valueEl.dataset.metaValue = "value";
  if (options.mono) valueEl.dataset.mono = "true";
  const displayValue = options.display ?? value;
  valueEl.textContent = displayValue;
  if (options.title) valueEl.title = options.title;
  else if (displayValue.length > 24) valueEl.title = value;

  item.appendChild(labelEl);
  item.appendChild(valueEl);
  container.appendChild(item);
};

const appendKeyInfo = (container, label, value, options = {}) => {
  const item = document.createElement("div");
  item.dataset.keyInfoItem = "item";
  if (options.state) item.dataset.state = options.state;

  const labelEl = document.createElement("span");
  labelEl.dataset.keyInfoLabel = "label";
  labelEl.textContent = label;

  const valueEl = document.createElement("span");
  valueEl.dataset.keyInfoValue = "value";
  if (options.mono) valueEl.dataset.mono = "true";
  valueEl.textContent = value;
  if (options.title) valueEl.title = options.title;

  item.appendChild(labelEl);
  item.appendChild(valueEl);
  container.appendChild(item);
  return { item, valueEl };
};

const normalizeKernelPubKeyAppId = (value) => {
  if (typeof value !== "string") return null;
  const trimmed = value.trim();
  if (!trimmed) return null;
  const parsed = Number(trimmed);
  if (!Number.isFinite(parsed) || !Number.isInteger(parsed) || parsed <= 0) return null;
  return parsed;
};

const normalizeKernelPubKeyOwner = (value) => {
  if (typeof value !== "string") return null;
  const trimmed = value.trim();
  return trimmed ? trimmed : null;
};

const normalizeKernelPubKeyPem = (value) => {
  if (typeof value !== "string") return null;
  const trimmed = value.trim();
  if (!trimmed) return null;
  if (!trimmed.startsWith("-----BEGIN PUBLIC KEY-----") || !trimmed.endsWith("-----END PUBLIC KEY-----")) return null;
  return trimmed;
};

const renderKernelPubKeys = (records) => {
  kernelPubKeysList.textContent = "";
  if (!Array.isArray(records) || records.length === 0) {
    setKernelPubKeysMessage("No kernel attestation keys yet.");
    return;
  }

  const sorted = [...records].sort((a, b) => {
    const aId = typeof a?.app_id === "number" ? a.app_id : 0;
    const bId = typeof b?.app_id === "number" ? b.app_id : 0;
    return aId - bId;
  });

  sorted.forEach((record, index) => {
    if (!record || typeof record !== "object") return;
    const appId = typeof record.app_id === "number" ? Math.trunc(record.app_id) : null;
    const owner = typeof record.owner === "string" ? record.owner : "";
    const pem = typeof record.pem === "string" ? record.pem : "";
    const addedAt = typeof record.added_at_ms === "number" ? record.added_at_ms : null;

    const row = document.createElement("article");
    row.dataset.key = "kernel-pubkey";
    row.dataset.state = "active";
    row.style.setProperty("--i", index);

    const main = document.createElement("div");
    main.dataset.keyMain = "main";

    const header = document.createElement("div");
    header.dataset.keyHeader = "header";

    const title = document.createElement("div");
    title.dataset.keyTitle = "title";
    title.textContent = appId ? `App ${appId}` : "App ID unknown";

    const controls = document.createElement("div");
    controls.dataset.keyControls = "controls";

    const actionRow = document.createElement("div");
    actionRow.dataset.actionRow = "actions";

    const deleteBtn = document.createElement("button");
    deleteBtn.type = "button";
    deleteBtn.dataset.variant = "danger";
    applyIconButton(deleteBtn, "trash", appId ? `Delete App ${appId}` : "Delete kernel attestation key");

    actionRow.appendChild(deleteBtn);
    controls.appendChild(actionRow);
    header.appendChild(title);
    header.appendChild(controls);

    const infoRow = document.createElement("div");
    infoRow.dataset.keyInfo = "info";
    appendKeyInfo(infoRow, "App ID", appId ? String(appId) : "unknown", { mono: true });
    appendKeyInfo(infoRow, "Owner", formatOptionalText(owner));
    appendKeyInfo(infoRow, "Key preview", formatPemPreview(pem) || "—", { mono: true });
    appendKeyInfo(infoRow, "Added", formatDate(addedAt));

    main.appendChild(header);
    main.appendChild(infoRow);

    if (pem) {
      const details = document.createElement("details");
      details.dataset.usage = "details";

      const summary = document.createElement("summary");
      summary.dataset.usageTitle = "title";

      const label = document.createElement("span");
      label.dataset.usageLabel = "label";
      label.textContent = "Attestation key";

      const summaryMeta = document.createElement("span");
      summaryMeta.dataset.usageSummary = "summary";
      summaryMeta.textContent = formatPemPreview(pem) || "View PEM";

      summary.appendChild(label);
      summary.appendChild(summaryMeta);
      details.appendChild(summary);

      const pre = document.createElement("pre");
      pre.dataset.code = "pem";
      pre.textContent = pem;
      details.appendChild(pre);

      main.appendChild(details);
    }

    row.appendChild(main);
    kernelPubKeysList.appendChild(row);

    if (appId) {
      deleteBtn.addEventListener("click", () => {
        void deleteKernelPubKey(appId, deleteBtn);
      });
    } else {
      deleteBtn.disabled = true;
    }
  });

  updateAccessPubkeysSummary();
};

const refreshKernelPubKeys = async () => {
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setKernelPubKeysBadge("bad", "Missing token");
    setKernelPubKeysMessage("Paste an admin token to load kernel attestation keys.");
    updateAccessPubkeysSummary();
    return;
  }

  if (kernelPubKeysLoading) return;
  kernelPubKeysLoading = true;
  setKernelPubKeysBadge("unknown", kernelPubKeysLoadedAt ? "Cached · refreshing" : "Loading...");

  try {
    const res = await fetch(apiUrl("/admin/kernel-pubkeys"), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      if (kernelPubKeysLoadedAt) {
        setKernelPubKeysBadge("unknown", "Cached · refresh unavailable");
        return;
      }
      setKernelPubKeysBadge("bad", data?.error?.message ?? "Error");
      setKernelPubKeysMessage("Failed to load kernel attestation keys.");
      return;
    }

    const records = Array.isArray(data?.data) ? data.data : [];
    kernelPubKeys = records;
    kernelPubKeysLoadedAt = Date.now();
    renderKernelPubKeys(records);
    setKernelPubKeysBadge("ok", `${records.length} key${records.length === 1 ? "" : "s"}`);
  } catch {
    if (kernelPubKeysLoadedAt) {
      setKernelPubKeysBadge("unknown", "Cached · offline");
      return;
    }
    kernelPubKeys = [];
    setKernelPubKeysBadge("bad", "Offline");
    setKernelPubKeysMessage("Failed to load kernel attestation keys.");
  } finally {
    kernelPubKeysLoading = false;
    updateAccessPubkeysSummary();
  }
};

const ensureKernelPubKeysLoaded = async () => {
  if (currentAdminView !== "pubkeys") return;
  if (kernelPubKeysLoading) return;
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setKernelPubKeysBadge("bad", "Missing token");
    setKernelPubKeysMessage("Paste an admin token to load kernel attestation keys.");
    updateAccessPubkeysSummary();
    return;
  }
  if (kernelPubKeysLoadedAt && Date.now() - kernelPubKeysLoadedAt < 10_000) {
    setKernelPubKeysBadge("ok", `${kernelPubKeys.length} key${kernelPubKeys.length === 1 ? "" : "s"}`);
    updateAccessPubkeysSummary();
    return;
  }
  await refreshKernelPubKeys();
};

const createKernelPubKey = async () => {
  if (kernelPubKeysSaving) return;
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setKernelPubKeyCreateBadge("bad", "Missing token");
    setAuthBadge("bad", "Missing token");
    tokenInput.focus();
    return;
  }

  const appIdRaw = kernelPubKeyAppIdInput.value;
  const appId = normalizeKernelPubKeyAppId(appIdRaw);
  if (!appId) {
    setKernelPubKeyCreateBadge("bad", appIdRaw.trim() ? "App ID must be a number" : "App ID required");
    kernelPubKeyAppIdInput.focus();
    return;
  }

  const pemRaw = kernelPubKeyPemInput.value;
  const pem = normalizeKernelPubKeyPem(pemRaw);
  if (!pem) {
    setKernelPubKeyCreateBadge("bad", pemRaw.trim() ? "Invalid PEM" : "PEM required");
    kernelPubKeyPemInput.focus();
    return;
  }

  const owner = normalizeKernelPubKeyOwner(kernelPubKeyOwnerInput.value);

  kernelPubKeysSaving = true;
  kernelPubKeyCreateBtn.disabled = true;
  setKernelPubKeyCreateBadge("unknown", "Saving...");

  try {
    const payload = { app_id: appId, pem };
    if (owner) payload.owner = owner;

    const res = await fetch(apiUrl("/admin/kernel-pubkeys"), {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${token}`,
      },
      body: JSON.stringify(payload),
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      setKernelPubKeyCreateBadge("bad", data?.error?.message ?? "Error");
      return;
    }

    resetKernelPubKeyForm();
    setKernelPubKeyCreateBadge("ok", "Saved");
    await refreshKernelPubKeys();
  } catch {
    setKernelPubKeyCreateBadge("bad", "Offline");
  } finally {
    kernelPubKeysSaving = false;
    kernelPubKeyCreateBtn.disabled = false;
  }
};

const deleteKernelPubKey = async (appId, button) => {
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setKernelPubKeysBadge("bad", "Missing token");
    setAuthBadge("bad", "Missing token");
    tokenInput.focus();
    return;
  }
  if (!confirm(`Delete kernel attestation key for App ${appId}?`)) return;

  setKernelPubKeysBadge("unknown", "Deleting...");
  if (button) button.disabled = true;

  try {
    const url = new URL(apiUrl("/admin/kernel-pubkeys"));
    url.searchParams.set("app_id", String(appId));
    const res = await fetch(url.toString(), {
      method: "DELETE",
      headers: {
        Authorization: `Bearer ${token}`,
      },
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      setKernelPubKeysBadge("bad", data?.error?.message ?? "Error");
      return;
    }
    await refreshKernelPubKeys();
  } catch {
    setKernelPubKeysBadge("bad", "Offline");
  } finally {
    if (button) button.disabled = false;
  }
};

const appendUsagePill = (container, label, value = "", options = {}) => {
  const pill = document.createElement("span");
  pill.dataset.usagePill = "pill";
  if (options.state) pill.dataset.state = options.state;
  pill.textContent = value ? `${label} ${value}` : label;
  if (options.title) pill.title = options.title;
  container.appendChild(pill);
};

const SPARKLINE_MIN_WIDTH = 240;
const SPARKLINE_HEIGHT = 96;
const SPARKLINE_PAD_Y = 6;
let sparklineCounter = 0;
const buildUsageSparkline = (usage, options = {}) => {
  const dailyKey = options.dailyKey ?? "daily_requests";
  const daily = Array.isArray(usage?.[dailyKey]) ? usage[dailyKey] : null;
  if (!daily || daily.length === 0) return null;

  const values = daily.map((value) =>
    typeof value === "number" && Number.isFinite(value) && value > 0 ? Math.trunc(value) : 0
  );
  const count = values.length;
  if (!count) return null;

  const dayWidth = count >= 90 ? 8 : count >= 60 ? 10 : count >= 30 ? 12 : count >= 14 ? 16 : 22;
  const width = Math.max(SPARKLINE_MIN_WIDTH, count * dayWidth);
  const height = SPARKLINE_HEIGHT;
  const padX = Math.min(10, Math.max(4, Math.round(width * 0.02)));
  const plotWidth = Math.max(1, width - padX * 2);
  const maxValue = Math.max(...values);
  const sorted = [...values].sort((a, b) => a - b);
  const p90 = sorted[Math.floor((sorted.length - 1) * 0.9)] ?? maxValue;
  const p50 = sorted[Math.floor((sorted.length - 1) * 0.5)] ?? p90;
  const spread = Math.max(1, p90 - p50);
  let scaleMax = maxValue;
  if (maxValue > p90 + spread * 1.5) {
    scaleMax = p90 + spread;
  }
  scaleMax = Math.max(1, scaleMax);
  const padTop = SPARKLINE_PAD_Y;
  const padBottom = SPARKLINE_PAD_Y;
  const drawHeight = height - padTop - padBottom;
  const baseline = height - padBottom;
  const stepX = count === 1 ? 0 : plotWidth / (count - 1);

  const points = values.map((value, index) => {
    const clampedValue = Math.min(value, scaleMax);
    const ratio = scaleMax === 0 ? 0 : clampedValue / scaleMax;
    const y = baseline - ratio * drawHeight;
    const x = count === 1 ? width / 2 : padX + index * stepX;
    return { x, y, value, clamped: value > scaleMax };
  });

  const linePath = points.map((point, index) => `${index === 0 ? "M" : "L"}${point.x},${point.y}`).join(" ");
  const areaPath = `${linePath} L ${points[points.length - 1].x},${baseline} L ${points[0].x},${baseline} Z`;

  const avgWindow = Math.min(7, Math.max(3, Math.round(count / 12)));
  const avgValues = values.map((_, index) => {
    const start = Math.max(0, index - avgWindow + 1);
    const slice = values.slice(start, index + 1);
    return slice.reduce((sum, value) => sum + value, 0) / slice.length;
  });
  const avgPoints = avgValues.map((value, index) => {
    const clampedValue = Math.min(value, scaleMax);
    const ratio = scaleMax === 0 ? 0 : clampedValue / scaleMax;
    const y = baseline - ratio * drawHeight;
    const x = count === 1 ? width / 2 : padX + index * stepX;
    return { x, y };
  });
  const avgPath = avgPoints.map((point, index) => `${index === 0 ? "M" : "L"}${point.x},${point.y}`).join(" ");

  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  const gradientId = `sparkline-${++sparklineCounter}`;
  const areaGradientId = `${gradientId}-area`;
  svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
  svg.setAttribute("width", String(width));
  svg.setAttribute("height", String(height));
  svg.setAttribute("preserveAspectRatio", "none");
  svg.setAttribute("role", "img");
  svg.setAttribute("aria-label", `Last ${count} days ${options.ariaLabel ?? "requests"}`);

  const defs = document.createElementNS("http://www.w3.org/2000/svg", "defs");
  const gradient = document.createElementNS("http://www.w3.org/2000/svg", "linearGradient");
  gradient.setAttribute("id", gradientId);
  gradient.setAttribute("x1", "0");
  gradient.setAttribute("y1", "0");
  gradient.setAttribute("x2", "1");
  gradient.setAttribute("y2", "0");

  const stopStart = document.createElementNS("http://www.w3.org/2000/svg", "stop");
  stopStart.setAttribute("offset", "0%");
  stopStart.setAttribute("stop-color", "#ffffff");
  stopStart.setAttribute("stop-opacity", "0");

  const stopEnd = document.createElementNS("http://www.w3.org/2000/svg", "stop");
  stopEnd.setAttribute("offset", "100%");
  stopEnd.setAttribute("stop-color", "#ffffff");
  stopEnd.setAttribute("stop-opacity", "0.9");

  gradient.appendChild(stopStart);
  gradient.appendChild(stopEnd);

  const areaGradient = document.createElementNS("http://www.w3.org/2000/svg", "linearGradient");
  areaGradient.setAttribute("id", areaGradientId);
  areaGradient.setAttribute("x1", "0");
  areaGradient.setAttribute("y1", "0");
  areaGradient.setAttribute("x2", "0");
  areaGradient.setAttribute("y2", "1");

  const areaStart = document.createElementNS("http://www.w3.org/2000/svg", "stop");
  areaStart.setAttribute("offset", "0%");
  areaStart.setAttribute("stop-color", "#ffffff");
  areaStart.setAttribute("stop-opacity", "0.24");

  const areaEnd = document.createElementNS("http://www.w3.org/2000/svg", "stop");
  areaEnd.setAttribute("offset", "100%");
  areaEnd.setAttribute("stop-color", "#ffffff");
  areaEnd.setAttribute("stop-opacity", "0");

  areaGradient.appendChild(areaStart);
  areaGradient.appendChild(areaEnd);
  defs.appendChild(gradient);
  defs.appendChild(areaGradient);
  svg.appendChild(defs);

  [0.25, 0.5, 0.75].forEach((offset) => {
    const y = baseline - drawHeight * offset;
    const line = document.createElementNS("http://www.w3.org/2000/svg", "line");
    line.setAttribute("x1", "0");
    line.setAttribute("x2", String(width));
    line.setAttribute("y1", String(y));
    line.setAttribute("y2", String(y));
    line.dataset.usageSparkGrid = "line";
    svg.appendChild(line);
  });

  if (count >= 14) {
    for (let i = 6; i < count; i += 7) {
      const x = padX + i * stepX;
      const line = document.createElementNS("http://www.w3.org/2000/svg", "line");
      line.setAttribute("x1", String(x));
      line.setAttribute("x2", String(x));
      line.setAttribute("y1", String(padTop));
      line.setAttribute("y2", String(baseline));
      line.dataset.usageSparkGrid = "vertical";
      svg.appendChild(line);
    }
  }

  const area = document.createElementNS("http://www.w3.org/2000/svg", "path");
  area.setAttribute("d", areaPath);
  area.setAttribute("fill", `url(#${areaGradientId})`);
  area.dataset.usageSparkArea = "area";
  svg.appendChild(area);

  const avgPathEl = document.createElementNS("http://www.w3.org/2000/svg", "path");
  avgPathEl.setAttribute("d", avgPath);
  avgPathEl.setAttribute("fill", "none");
  avgPathEl.setAttribute("stroke", "rgba(255, 255, 255, 0.4)");
  avgPathEl.setAttribute("stroke-width", "1.6");
  avgPathEl.setAttribute("stroke-linecap", "round");
  avgPathEl.setAttribute("stroke-linejoin", "round");
  avgPathEl.dataset.usageSparkTrend = "trend";
  svg.appendChild(avgPathEl);

  const linePathEl = document.createElementNS("http://www.w3.org/2000/svg", "path");
  linePathEl.setAttribute("d", linePath);
  linePathEl.setAttribute("fill", "none");
  linePathEl.setAttribute("stroke", `url(#${gradientId})`);
  linePathEl.setAttribute("stroke-width", "2.2");
  linePathEl.setAttribute("stroke-linecap", "round");
  linePathEl.setAttribute("stroke-linejoin", "round");
  linePathEl.dataset.usageSparkLine = "line";
  svg.appendChild(linePathEl);

  const scaleMaxLabel = Math.max(1, Math.round(scaleMax));
  const formattedScaleMax = typeof options.formatScaleMax === "function"
    ? options.formatScaleMax(scaleMaxLabel)
    : formatCompactNumber(scaleMaxLabel);
  const maxLabel = maxValue > scaleMax ? `Max ${formattedScaleMax}+` : `Max ${formattedScaleMax}`;

  const container = document.createElement("div");
  container.dataset.usageSpark = "spark";
  container.style.setProperty("--spark-days", String(count));
  container.style.setProperty("--spark-height", `${height}px`);

  const scaleRow = document.createElement("div");
  scaleRow.dataset.usageSparkScale = "scale";
  scaleRow.textContent = maxLabel;
  container.appendChild(scaleRow);

  const chart = document.createElement("div");
  chart.dataset.usageSparkChart = "chart";
  chart.appendChild(svg);
  container.appendChild(chart);

  const axis = document.createElement("div");
  axis.dataset.usageSparkAxis = "axis";
  if (count > 1) {
    const startLabel = document.createElement("span");
    startLabel.textContent = `Last ${count}d`;
    axis.appendChild(startLabel);
  } else {
    axis.dataset.single = "true";
  }
  const endLabel = document.createElement("span");
  endLabel.textContent = "Today";
  axis.appendChild(endLabel);
  container.appendChild(axis);
  return container;
};

const appendUsageSeries = (container, label, sparkline) => {
  if (!sparkline) return;
  const section = document.createElement("section");
  section.dataset.usageSeries = "series";

  const title = document.createElement("h4");
  title.dataset.usageSeriesTitle = "title";
  title.textContent = label;

  section.appendChild(title);
  section.appendChild(sparkline);
  container.appendChild(section);
};

const buildUsageSummary = (usage) => {
  const summary = document.createElement("div");
  summary.dataset.usageSummary = "summary";

  if (usage === undefined) {
    appendUsagePill(summary, "Unavailable");
    return summary;
  }
  if (usage === null) {
    appendUsagePill(summary, "No data");
    return summary;
  }
  if (typeof usage !== "object") {
    appendUsagePill(summary, "Unavailable");
    return summary;
  }

  if (Object.keys(usage).length === 0) {
    appendUsagePill(summary, "No data");
    return summary;
  }

  if (Object.prototype.hasOwnProperty.call(usage, "request_count")) {
    appendUsagePill(summary, "Requests", formatCompactNumber(usage.request_count), {
      title: formatNumber(usage.request_count),
    });
    appendUsagePill(
      summary,
      "Limit",
      toNumber(usage.limit) === -1 ? "Unlimited" : formatCompactNumber(usage.limit),
    );
    appendUsagePill(summary, "Resets", formatDate(usage.reset_at_ms));
    return summary;
  }

  appendUsagePill(summary, "Requests", formatCompactNumber(usage.total_requests), {
    title: formatNumber(usage.total_requests),
  });
  appendUsagePill(summary, "Tokens", formatCompactNumber(usage.total_tokens), {
    title: formatNumber(usage.total_tokens),
  });
  appendUsagePill(summary, "Last", formatDate(usage.last_seen_at_ms));

  if (Object.prototype.hasOwnProperty.call(usage, "metered_fallback_requests")) {
    appendUsagePill(summary, "Metered", formatCompactNumber(usage.metered_fallback_requests), {
      title: `${formatNumber(usage.metered_fallback_requests)} fallback requests`,
    });
  }
  if (Object.prototype.hasOwnProperty.call(usage, "metered_spend_microcredits")) {
    appendUsagePill(summary, "Spend", formatMicrocreditsAsCredits(usage.metered_spend_microcredits), {
      title: `${formatNumber(usage.metered_spend_microcredits)} microcredits`,
    });
  }

  const errorCount = toNumber(usage.error_requests);
  if (errorCount > 0) {
    appendUsagePill(summary, "Errors", formatCompactNumber(errorCount), {
      state: "bad",
      title: formatNumber(errorCount),
    });
  }

  return summary;
};

const buildUsageDetails = (usage, options = {}) => {
  const labelText = options.label ?? "Analytics";
  const unavailableText = options.unavailable ?? "Analytics unavailable.";
  const emptyText = options.empty ?? "No analytics yet.";

  const usageSection = document.createElement("details");
  usageSection.dataset.usage = "details";

  const usageTitle = document.createElement("summary");
  usageTitle.dataset.usageTitle = "title";

  const usageLabel = document.createElement("span");
  usageLabel.dataset.usageLabel = "label";
  usageLabel.textContent = labelText;

  usageTitle.appendChild(usageLabel);
  usageTitle.appendChild(buildUsageSummary(usage));
  usageSection.appendChild(usageTitle);

  if (usage && typeof usage === "object" && Object.prototype.hasOwnProperty.call(usage, "request_count")) {
    const usageList = document.createElement("div");
    usageList.dataset.usageList = "list";
    appendMetaItem(usageList, "Requests", formatNumber(usage.request_count));
    appendMetaItem(
      usageList,
      "Limit",
      toNumber(usage.limit) === -1 ? "Unlimited" : formatNumber(usage.limit),
    );
    appendMetaItem(usageList, "Resets", formatDate(usage.reset_at_ms));
    usageSection.appendChild(usageList);
    return usageSection;
  }

  const sparkline = buildUsageSparkline(usage);
  if (sparkline) usageSection.appendChild(sparkline);
  appendUsageSeries(
    usageSection,
    "Daily Metered fallbacks",
    buildUsageSparkline(usage, {
      dailyKey: "daily_metered_fallback_requests",
      ariaLabel: "Metered fallback requests",
    }),
  );
  appendUsageSeries(
    usageSection,
    "Daily Metered spend",
    buildUsageSparkline(usage, {
      dailyKey: "daily_metered_spend_microcredits",
      ariaLabel: "Metered spend",
      formatScaleMax: formatMicrocreditsAsCredits,
    }),
  );

  if (usage === undefined) {
    const empty = document.createElement("div");
    empty.dataset.usageEmpty = "empty";
    empty.textContent = unavailableText;
    usageSection.appendChild(empty);
  } else if (usage === null) {
    const empty = document.createElement("div");
    empty.dataset.usageEmpty = "empty";
    empty.textContent = emptyText;
    usageSection.appendChild(empty);
  } else if (typeof usage !== "object") {
    const empty = document.createElement("div");
    empty.dataset.usageEmpty = "empty";
    empty.textContent = unavailableText;
    usageSection.appendChild(empty);
  } else if (Object.keys(usage).length === 0) {
    const empty = document.createElement("div");
    empty.dataset.usageEmpty = "empty";
    empty.textContent = emptyText;
    usageSection.appendChild(empty);
  } else {
    const usageList = document.createElement("div");
    usageList.dataset.usageList = "list";
    appendMetaItem(usageList, "Requests", formatNumber(usage.total_requests));
    appendMetaItem(usageList, "Completed", formatNumber(usage.completed_requests));
    const errorCount = toNumber(usage.error_requests);
    appendMetaItem(usageList, "Errors", formatNumber(errorCount), { state: errorCount > 0 ? "bad" : "" });
    appendMetaItem(usageList, "Stream", formatNumber(usage.stream_requests));
    appendMetaItem(usageList, "Tokens in", formatNumber(usage.input_tokens));
    appendMetaItem(usageList, "Tokens out", formatNumber(usage.output_tokens));
    appendMetaItem(usageList, "Tokens total", formatNumber(usage.total_tokens));
    appendMetaItem(usageList, "First seen", formatDate(usage.first_seen_at_ms));
    appendMetaItem(usageList, "Last seen", formatDate(usage.last_seen_at_ms));
    appendMetaItem(usageList, "Last model", formatOptionalText(usage.last_model), { mono: true });
    appendMetaItem(usageList, "Last reasoning", formatOptionalText(usage.last_reasoning), { mono: true });
    appendMetaItem(usageList, "Last route", formatOptionalText(usage.last_route));
    if (Object.prototype.hasOwnProperty.call(usage, "metered_fallback_requests")) {
      appendMetaItem(usageList, "Metered fallbacks", formatNumber(usage.metered_fallback_requests));
    }
    if (Object.prototype.hasOwnProperty.call(usage, "metered_input_tokens")) {
      appendMetaItem(usageList, "Metered tokens in", formatNumber(usage.metered_input_tokens));
    }
    if (Object.prototype.hasOwnProperty.call(usage, "metered_output_tokens")) {
      appendMetaItem(usageList, "Metered tokens out", formatNumber(usage.metered_output_tokens));
    }
    if (Object.prototype.hasOwnProperty.call(usage, "metered_total_tokens")) {
      appendMetaItem(usageList, "Metered tokens total", formatNumber(usage.metered_total_tokens));
    }
    if (Object.prototype.hasOwnProperty.call(usage, "metered_spend_microcredits")) {
      appendMetaItem(
        usageList,
        "Metered spend",
        formatMicrocreditsAsCredits(usage.metered_spend_microcredits),
        { title: `${formatNumber(usage.metered_spend_microcredits)} microcredits` },
      );
    }
    usageSection.appendChild(usageList);
  }

  return usageSection;
};

const normalizeFiniteNumber = (value) => {
  if (typeof value !== "number" || !Number.isFinite(value)) return null;
  return value;
};

const normalizeOptionalString = (value) => {
  if (typeof value !== "string") return null;
  const normalized = value.trim();
  return normalized || null;
};

const normalizeApiKeyRequestLogRecord = (record) => {
  if (!record || typeof record !== "object") return null;

  const startedAtMs = normalizeFiniteNumber(record.started_at_ms);
  const createdAtMs = normalizeFiniteNumber(record.created_at_ms) ?? startedAtMs;
  const completedAtMs = normalizeFiniteNumber(record.completed_at_ms);
  const dispatchedAtMs = normalizeFiniteNumber(record.dispatched_at_ms);
  const terminalAtMs = normalizeFiniteNumber(record.terminal_at_ms);
  const settledAtMs = normalizeFiniteNumber(record.settled_at_ms);
  const updatedAtMs = normalizeFiniteNumber(record.updated_at_ms);
  const lastReconciliationAtMs = normalizeFiniteNumber(record.last_reconciliation_at_ms);
  const statusCode = normalizeFiniteNumber(record.status_code);
  const method = typeof record.method === "string" ? record.method.trim().toUpperCase() : "";
  const route = typeof record.route === "string" ? record.route.trim() : "";
  const path = typeof record.path === "string" ? record.path.trim() : "";
  const stream = record.stream === true;
  const model = typeof record.model === "string" ? record.model.trim() : "";
  const reasoning = typeof record.reasoning === "string" ? record.reasoning.trim() : "";

  return {
    id: typeof record.id === "string" && record.id.trim()
      ? record.id.trim()
      : `${route || "request"}-${createdAtMs || Date.now()}`,
    created_at_ms: createdAtMs === null ? null : Math.trunc(createdAtMs),
    started_at_ms: startedAtMs === null ? null : Math.trunc(startedAtMs),
    completed_at_ms: completedAtMs === null ? null : Math.trunc(completedAtMs),
    dispatched_at_ms: dispatchedAtMs === null ? null : Math.trunc(dispatchedAtMs),
    terminal_at_ms: terminalAtMs === null ? null : Math.trunc(terminalAtMs),
    settled_at_ms: settledAtMs === null ? null : Math.trunc(settledAtMs),
    updated_at_ms: updatedAtMs === null ? null : Math.trunc(updatedAtMs),
    last_reconciliation_at_ms: lastReconciliationAtMs === null ? null : Math.trunc(lastReconciliationAtMs),
    latency_ms: normalizeFiniteNumber(record.latency_ms),
    status_code: statusCode === null ? null : Math.trunc(statusCode),
    method: method || "GET",
    route: route || "unknown",
    path: path || null,
    stream,
    model: model || null,
    reasoning: reasoning || null,
    provider: normalizeOptionalString(record.provider),
    fallback_reason: normalizeOptionalString(record.fallback_reason),
    provider_request_id: normalizeOptionalString(record.provider_request_id),
    input_tokens: normalizeFiniteNumber(record.input_tokens),
    output_tokens: normalizeFiniteNumber(record.output_tokens),
    provider_quota: normalizeFiniteNumber(record.provider_quota),
    quota_per_credit: normalizeFiniteNumber(record.quota_per_credit),
    reserved_microcredits: normalizeFiniteNumber(record.reserved_microcredits),
    spend_microcredits: normalizeFiniteNumber(record.spend_microcredits),
    policy_version: normalizeOptionalString(record.policy_version),
    dispatch_state: normalizeOptionalString(record.dispatch_state),
    terminal_state: normalizeOptionalString(record.terminal_state),
    billing_state: normalizeOptionalString(record.billing_state),
    billing_status: normalizeOptionalString(record.billing_status ?? record.billing_state),
    reconciliation_attempts: normalizeFiniteNumber(record.reconciliation_attempts),
    window_reset_at_ms: normalizeFiniteNumber(record.window_reset_at_ms),
  };
};

const buildApiKeyRequestLogsPanel = (keyId) => {
  const panel = document.createElement("details");
  panel.dataset.usage = "details";
  panel.dataset.apiKeyRequestLogs = "panel";
  panel.dataset.keyId = keyId;

  const title = document.createElement("summary");
  title.dataset.usageTitle = "title";

  const label = document.createElement("span");
  label.dataset.usageLabel = "label";
  label.textContent = "Paid fallbacks";

  const summary = document.createElement("span");
  summary.dataset.usageSummary = "summary";
  summary.textContent = "Not loaded";

  const list = document.createElement("div");
  list.dataset.apiKeyRequestList = "list";

  title.appendChild(label);
  title.appendChild(summary);
  panel.appendChild(title);
  panel.appendChild(list);

  return { panel, summary, list };
};

const setRequestLogsPanelMessage = (list, summary, text, status = API_KEY_REQUEST_LOG_STATUS_OK) => {
  list.textContent = "";
  const message = document.createElement("div");
  message.dataset.usageEmpty = "empty";
  message.textContent = text;
  list.appendChild(message);
  summary.textContent = text;
  if (status === API_KEY_REQUEST_LOG_STATUS_ERROR) {
    summary.dataset.state = "bad";
  } else {
    delete summary.dataset.state;
  }
};

const createRequestLogRow = (record) => {
  const row = document.createElement("div");
  row.dataset.apiKeyRequestLog = "row";

  const statusCode = normalizeFiniteNumber(record.status_code);
  const statusState = statusCode !== null && statusCode >= 400 ? "bad" : "";
  const provider = formatOptionalText(record.provider);
  const billingStatus = formatOptionalText(record.billing_state ?? record.billing_status);
  const normalizedBillingStatus = billingStatus.toLowerCase();
  const billingState = normalizedBillingStatus === "pending"
    ? "warning"
    : (/failed|error|unresolved/.test(normalizedBillingStatus) ? "bad" : "");
  row.dataset.provider = provider.toLowerCase();
  if (record.billing_status) row.dataset.billingStatus = normalizedBillingStatus;

  const routeText = record.path
    ? `${record.method} ${record.route} (${record.path})`
    : `${record.method} ${record.route}`;

  appendMetaItem(row, "Request", routeText, { mono: true });
  appendMetaItem(row, "Started", formatDate(record.started_at_ms ?? record.created_at_ms));
  appendMetaItem(
    row,
    "Status",
    `${statusCode === null ? "unknown" : formatNumber(statusCode)}${record.stream ? " · stream" : " · single"}`,
    {
      state: statusState,
    },
  );
  appendMetaItem(row, "Provider", provider);
  appendMetaItem(row, "Model", formatOptionalText(record.model));
  appendMetaItem(row, "Reasoning", formatOptionalText(record.reasoning));
  if (record.policy_version) {
    appendMetaItem(row, "Policy", record.policy_version, { mono: true });
  }
  if (record.dispatch_state) {
    appendMetaItem(row, "Dispatch", record.dispatch_state);
  }
  if (record.terminal_state) {
    const terminalState = record.terminal_state === "completed"
      ? ""
      : (record.terminal_state === "pending" ? "warning" : "bad");
    appendMetaItem(row, "Terminal", record.terminal_state, {
      state: terminalState,
    });
  }
  if (record.dispatched_at_ms !== null) {
    appendMetaItem(row, "Dispatched", formatDate(record.dispatched_at_ms));
  }
  if (record.terminal_at_ms !== null) {
    appendMetaItem(row, "Terminal at", formatDate(record.terminal_at_ms));
  }
  if (record.completed_at_ms !== null) {
    appendMetaItem(row, "Completed", formatDate(record.completed_at_ms));
  }
  if (record.latency_ms !== null) {
    appendMetaItem(row, "Latency", formatLatency(record.latency_ms));
  }
  if (record.fallback_reason) {
    appendMetaItem(row, "Fallback reason", record.fallback_reason);
  }
  if (record.provider_request_id) {
    appendMetaItem(row, "Provider request", record.provider_request_id, { mono: true });
  }
  if (record.input_tokens !== null) {
    appendMetaItem(row, "Tokens in", formatNumber(record.input_tokens));
  }
  if (record.output_tokens !== null) {
    appendMetaItem(row, "Tokens out", formatNumber(record.output_tokens));
  }
  if (record.provider_quota !== null) {
    appendMetaItem(row, "Provider quota", formatDecimal(record.provider_quota));
  }
  if (record.quota_per_credit !== null) {
    appendMetaItem(row, "Quota per credit", formatDecimal(record.quota_per_credit));
  }
  if (record.reserved_microcredits !== null) {
    appendMetaItem(row, "Reservation", formatMicrocreditsAsCredits(record.reserved_microcredits), {
      title: `${formatNumber(record.reserved_microcredits)} microcredits`,
    });
  }
  if (record.spend_microcredits !== null) {
    appendMetaItem(row, "Exact spend", formatMicrocreditsAsCredits(record.spend_microcredits), {
      title: `${formatNumber(record.spend_microcredits)} microcredits`,
    });
  }
  if (record.billing_status) {
    appendMetaItem(row, "Billing", billingStatus, {
      state: billingState,
    });
  }
  if (record.reconciliation_attempts !== null) {
    appendMetaItem(row, "Reconciliation attempts", formatNumber(record.reconciliation_attempts));
  }
  if (record.last_reconciliation_at_ms !== null) {
    appendMetaItem(row, "Last reconciliation", formatDate(record.last_reconciliation_at_ms));
  }
  if (record.settled_at_ms !== null) {
    appendMetaItem(row, "Settled", formatDate(record.settled_at_ms));
  }
  if (record.window_reset_at_ms !== null) {
    appendMetaItem(row, "Window resets", formatDate(record.window_reset_at_ms));
  }
  if (record.updated_at_ms !== null) {
    appendMetaItem(row, "Last updated", formatDate(record.updated_at_ms));
  }

  return row;
};

const getApiKeyRequestLogsPath = (keyId) =>
  `/admin/api-keys/${encodeURIComponent(keyId)}/paid-fallbacks?limit=${API_KEY_REQUEST_LOGS_LIMIT}`;

const normalizeApiKeyRequestLogRecords = (payload) => {
  const rawRecords = Array.isArray(payload?.data) ? payload.data : [];
  return rawRecords
    .map((record) => normalizeApiKeyRequestLogRecord(record))
    .filter((record) => record && record.created_at_ms !== null);
};

const readCachedApiKeyRequestLogs = async (keyId) => {
  const cacheKey = getApiKeyRequestLogCacheKey(keyId);
  if (!cacheKey) return null;
  const snapshot = await readAdminSnapshot(getApiKeyRequestLogsPath(cacheKey));
  if (!Array.isArray(snapshot?.payload?.data)) return null;
  return {
    records: normalizeApiKeyRequestLogRecords(snapshot.payload),
    savedAt: snapshot.savedAt,
  };
};

const loadApiKeyRequestLogs = (keyId) => {
  const cacheKey = getApiKeyRequestLogCacheKey(keyId);
  if (!cacheKey) return { ok: false, records: [], error: "Missing key id" };

  const token = getAdminToken();
  if (!token && !hasAdminCredential()) return { ok: false, records: [], error: "Missing token" };

  const scope = adminCacheScope;
  const epoch = adminCacheEpoch;
  const now = Date.now();
  const cached = apiKeyRequestLogCache.get(cacheKey);
  if (
    isCurrentApiKeyRequestLogCacheEntry(cached, scope, epoch) &&
    now - cached.fetchedAt < API_KEY_REQUEST_LOGS_TTL_MS
  ) {
    return { ok: true, records: cached.records, fromCache: true };
  }

  const inFlight = apiKeyRequestLogPromises.get(cacheKey);
  if (isCurrentApiKeyRequestLogCacheEntry(inFlight, scope, epoch)) return inFlight.request;

  const inFlightEntry = { scope, epoch, request: null };

  const request = (async () => {
    try {
      const res = await fetch(apiUrl(getApiKeyRequestLogsPath(cacheKey)), {
        headers: {
          Authorization: `Bearer ${token}`,
        },
        cache: "no-store",
      });
      const data = await res.json().catch(() => null);
      if (!res.ok) {
        return {
          ok: false,
          records: [],
          error: data?.error?.message || API_KEY_REQUEST_LOG_STATUS_UNAVAILABLE,
        };
      }

      const records = normalizeApiKeyRequestLogRecords(data);

      if (isCurrentApiKeyRequestLogScope(scope, epoch)) {
        apiKeyRequestLogCache.set(cacheKey, {
          records,
          fetchedAt: Date.now(),
          scope,
          epoch,
        });
      }

      return { ok: true, records };
    } catch {
      return { ok: false, records: [], error: API_KEY_REQUEST_LOG_STATUS_UNAVAILABLE };
    } finally {
      if (apiKeyRequestLogPromises.get(cacheKey) === inFlightEntry) {
        apiKeyRequestLogPromises.delete(cacheKey);
      }
    }
  })();

  inFlightEntry.request = request;
  apiKeyRequestLogPromises.set(cacheKey, inFlightEntry);
  return request;
};

const renderApiKeyRequestLogs = (panel, list, summary, records, cacheStatus = "") => {
  panel.dataset.requestLogsState = "ready";
  if (!records.length) {
    const text = cacheStatus ? `${cacheStatus} · No paid fallbacks recorded` : "No paid fallbacks recorded";
    setRequestLogsPanelMessage(list, summary, text);
    return;
  }

  const countText = records.length === 1 ? "1 paid fallback" : `${records.length} paid fallbacks`;
  summary.textContent = `${countText} (showing last ${API_KEY_REQUEST_LOGS_LIMIT})${
    cacheStatus ? ` · ${cacheStatus}` : ""
  }`;
  delete summary.dataset.state;
  list.textContent = "";
  records.forEach((record) => {
    list.appendChild(createRequestLogRow(record));
  });
};

const hydrateApiKeyRequestLogs = async (panel, keyId) => {
  if (!panel?.dataset) return;
  const currentKeyId = panel.dataset.keyId;
  if (!currentKeyId || currentKeyId !== keyId) return;

  const scope = adminCacheScope;
  const epoch = adminCacheEpoch;
  const isCurrentPanel = () =>
    panel.isConnected &&
    panel.dataset.keyId === currentKeyId &&
    isCurrentApiKeyRequestLogScope(scope, epoch);
  const summary = panel.querySelector("[data-usage-summary]");
  const list = panel.querySelector("[data-api-key-request-list]");
  if (!summary || !list) return;
  if (panel.dataset.requestLogsState === "ready" && panel.dataset.requestLogsLoading !== "1") {
    const cacheKey = getApiKeyRequestLogCacheKey(currentKeyId);
    if (cacheKey) {
      const cached = apiKeyRequestLogCache.get(cacheKey);
      if (
        isCurrentApiKeyRequestLogCacheEntry(cached, scope, epoch) &&
        Date.now() - cached.fetchedAt < API_KEY_REQUEST_LOGS_TTL_MS
      ) return;
    }
  }

  if (panel.dataset.requestLogsLoading === "1") return;
  panel.dataset.requestLogsLoading = "1";
  setRequestLogsPanelMessage(list, summary, "Loading...", API_KEY_REQUEST_LOG_STATUS_OK);
  const refresh = loadApiKeyRequestLogs(currentKeyId);
  const cached = await readCachedApiKeyRequestLogs(currentKeyId);

  if (!isCurrentPanel()) {
    panel.dataset.requestLogsLoading = "0";
    return;
  }

  if (cached) {
    renderApiKeyRequestLogs(panel, list, summary, cached.records, "Cached · refreshing");
  }

  const response = await refresh;

  if (!isCurrentPanel()) {
    panel.dataset.requestLogsLoading = "0";
    return;
  }

  if (!response.ok) {
    if (cached) {
      renderApiKeyRequestLogs(panel, list, summary, cached.records, "Cached · refresh unavailable");
      panel.dataset.requestLogsLoading = "0";
      return;
    }
    panel.dataset.requestLogsState = "error";
    setRequestLogsPanelMessage(
      list,
      summary,
      response.error || API_KEY_REQUEST_LOG_STATUS_ERROR,
      API_KEY_REQUEST_LOG_STATUS_ERROR,
    );
    panel.dataset.requestLogsLoading = "0";
    return;
  }

  renderApiKeyRequestLogs(panel, list, summary, response.records || []);
  panel.dataset.requestLogsLoading = "0";
};

const renderKeys = (keys, view = "all") => {
  clearKeysListLoading();
  keysList.textContent = "";
  let filteredKeys = keys;
  if (view === "active") {
    filteredKeys = keys.filter((k) => !k.revoked_at_ms);
  } else if (view === "revoked") {
    filteredKeys = keys.filter((k) => k.revoked_at_ms);
  }
  if (!filteredKeys.length) {
    setKeyListMessage(view === "all" ? "No API keys yet." : `No ${view} API keys.`);
    setKeysBadge("ok", view === "all" ? "No API keys" : `No ${view} API keys`);
    updateAccessApiKeysSummary();
    return;
  }

  filteredKeys.forEach((key, index) => {
    const row = document.createElement("article");
    row.dataset.key = "row";
    row.dataset.state = key.revoked_at_ms ? "revoked" : "active";
    row.style.setProperty("--i", index);
    row.setAttribute("role", "listitem");

    const main = document.createElement("div");
    main.dataset.keyMain = "main";

    const header = document.createElement("header");
    header.dataset.keyHeader = "header";

    const title = document.createElement("h3");
    title.dataset.keyTitle = "title";
    title.textContent = key.name || "Untitled";

    const controls = document.createElement("div");
    controls.dataset.keyControls = "controls";

    const infoRow = document.createElement("div");
    infoRow.dataset.keyInfo = "info";
    appendKeyInfo(infoRow, "Created", formatDate(key.created_at_ms));
    const expiresInfo = appendKeyInfo(infoRow, "Expires", formatExpires(key.expires_at_ms));
    if (key.revoked_at_ms) {
      appendKeyInfo(infoRow, "Revoked", formatDate(key.revoked_at_ms), { state: "bad" });
    }

    const resolveKeyWindowMs = () => {
      if (typeof key.window_ms === "number" && Number.isFinite(key.window_ms)) return Math.trunc(key.window_ms);
      return DEFAULT_API_KEY_WINDOW_MS;
    };

    const getUsageInfoData = () => {
      const limitValue = typeof key.usage_limit_requests === "number" ? key.usage_limit_requests : -1;
      const current = typeof key.usage_requests === "number" ? key.usage_requests : 0;
      const windowMs = resolveKeyWindowMs();
      const limitText = limitValue === -1 ? "Unlimited" : formatNumber(limitValue);
      const usageText = limitValue === -1 ? `${formatNumber(current)}` : `${formatNumber(current)}/${limitText}`;
      const hasLimit = limitValue !== -1;
      const isNearLimit = hasLimit && limitValue > 0 && current / limitValue >= 0.8;
      const isAtLimit = hasLimit && current >= limitValue;
      const title = `${formatNumber(current)} requests${
        hasLimit ? ` of ${formatNumber(limitValue)} rate limit` : ""
      } (window ${formatWindowShort(windowMs)}, resets ${formatDate(key.usage_reset_at_ms)})`;
      const state = isAtLimit ? "bad" : (isNearLimit ? "warning" : "");
      return { usageText, title, state, limitValue, windowMs };
    };

    const usageData = getUsageInfoData();
    const limitState = usageData.limitValue === 0 ? "bad" : "";
    const limitInfo = appendKeyInfo(
      infoRow,
      "Rate limit",
      formatLimitValue(usageData.limitValue),
      limitState ? { state: limitState } : {},
    );
    const windowInfo = appendKeyInfo(infoRow, "Window", formatWindowShort(usageData.windowMs), {
      title: formatWindowMs(usageData.windowMs),
    });
    const usageInfo = appendKeyInfo(infoRow, "Window requests", usageData.usageText, {
      state: usageData.state,
      title: usageData.title,
    });
    const resetInfo = appendKeyInfo(infoRow, "Reset at", formatDate(key.usage_reset_at_ms));

    const paidFallbackSummary = document.createElement("section");
    paidFallbackSummary.dataset.paidFallbackSummary = "summary";

    const paidFallbackHeader = document.createElement("header");
    paidFallbackHeader.dataset.paidFallbackHeader = "header";
    const paidFallbackTitle = document.createElement("span");
    paidFallbackTitle.dataset.paidFallbackTitle = "title";
    paidFallbackTitle.textContent = "Metered paid overflow";
    const paidFallbackStatus = document.createElement("span");
    paidFallbackStatus.dataset.badge = "status";
    paidFallbackHeader.appendChild(paidFallbackTitle);
    paidFallbackHeader.appendChild(paidFallbackStatus);

    const paidFallbackInfo = document.createElement("div");
    paidFallbackInfo.dataset.keyInfo = "info";
    paidFallbackInfo.dataset.paidFallbackInfo = "info";
    const paidLimitInfo = appendKeyInfo(paidFallbackInfo, "Window limit", "unknown");
    const paidSpentInfo = appendKeyInfo(paidFallbackInfo, "Window spent", "unknown");
    const paidReservedInfo = appendKeyInfo(paidFallbackInfo, "Window reserved", "unknown");
    const paidResetInfo = appendKeyInfo(paidFallbackInfo, "Window resets", "unknown");
    const lifetimeSpendInfo = appendKeyInfo(paidFallbackInfo, "Lifetime spend", "unknown");
    const fallbackCountInfo = appendKeyInfo(paidFallbackInfo, "Fallbacks", "unknown");
    const meteredWindowCountInfo = appendKeyInfo(paidFallbackInfo, "Metered 2 window requests", "unknown");
    const surplusWindowCountInfo = appendKeyInfo(paidFallbackInfo, "Metered 1 window requests", "unknown");
    const meteredWindowSpendInfo = appendKeyInfo(paidFallbackInfo, "Metered 2 window spend", "unknown");
    const surplusWindowSpendInfo = appendKeyInfo(paidFallbackInfo, "Metered 1 window spend", "unknown");

    paidFallbackSummary.appendChild(paidFallbackHeader);
    paidFallbackSummary.appendChild(paidFallbackInfo);

    const updatePaidFallbackInfo = () => {
      const enabled = key.paid_fallback_enabled === true;
      const limit = normalizeFiniteNumber(key.paid_fallback_limit_credits) ?? 0;
      const spent = normalizeFiniteNumber(key.paid_fallback_spent_credits) ?? 0;
      const reserved = normalizeFiniteNumber(key.paid_fallback_reserved_credits) ?? 0;
      const usage = key.usage && typeof key.usage === "object" ? key.usage : null;
      const providerUsage = key.paid_fallback_provider_usage && typeof key.paid_fallback_provider_usage === "object"
        ? key.paid_fallback_provider_usage
        : null;
      const meteredUsage = providerUsage?.metered && typeof providerUsage.metered === "object"
        ? providerUsage.metered
        : null;
      const surplusUsage = providerUsage?.surplus && typeof providerUsage.surplus === "object"
        ? providerUsage.surplus
        : null;

      paidFallbackSummary.hidden = !enabled;
      paidFallbackStatus.dataset.state = enabled ? "ok" : "unknown";
      paidFallbackStatus.textContent = enabled ? "Enabled" : "Disabled";
      paidFallbackSummary.dataset.state = enabled ? "enabled" : "disabled";
      paidLimitInfo.valueEl.textContent = formatPaidFallbackLimit(limit);
      paidSpentInfo.valueEl.textContent = formatCredits(spent);
      paidReservedInfo.valueEl.textContent = formatCredits(reserved);
      paidResetInfo.valueEl.textContent = formatDate(key.usage_reset_at_ms);
      lifetimeSpendInfo.valueEl.textContent = usage &&
          Object.prototype.hasOwnProperty.call(usage, "metered_spend_microcredits")
        ? formatMicrocreditsAsCredits(usage.metered_spend_microcredits)
        : "unknown";
      fallbackCountInfo.valueEl.textContent = usage &&
          Object.prototype.hasOwnProperty.call(usage, "metered_fallback_requests")
        ? formatNumber(usage.metered_fallback_requests)
        : "unknown";
      meteredWindowCountInfo.valueEl.textContent = meteredUsage ? formatNumber(meteredUsage.request_count) : "unknown";
      surplusWindowCountInfo.valueEl.textContent = surplusUsage ? formatNumber(surplusUsage.request_count) : "unknown";
      meteredWindowSpendInfo.valueEl.textContent = meteredUsage
        ? formatMicrocreditsAsCredits(meteredUsage.spend_microcredits)
        : "unknown";
      surplusWindowSpendInfo.valueEl.textContent = surplusUsage
        ? formatMicrocreditsAsCredits(surplusUsage.spend_microcredits)
        : "unknown";

      if (enabled && limit !== -1 && limit <= 0) {
        paidLimitInfo.item.dataset.state = "bad";
      } else {
        delete paidLimitInfo.item.dataset.state;
      }
      if (reserved > 0) {
        paidReservedInfo.item.dataset.state = "warning";
      } else {
        delete paidReservedInfo.item.dataset.state;
      }
    };
    updatePaidFallbackInfo();

    const status = document.createElement("span");
    status.dataset.badge = "status";
    status.dataset.state = key.revoked_at_ms ? "bad" : "ok";
    status.textContent = key.revoked_at_ms ? "Revoked" : "Active";
    const actionRow = document.createElement("div");
    actionRow.dataset.keyActions = "actions";
    actionRow.dataset.actionRow = "actions";

    const editBtn = document.createElement("button");
    editBtn.type = "button";
    applyIconButton(editBtn, "edit", "Edit API key");
    actionRow.appendChild(editBtn);

    if (!key.revoked_at_ms) {
      const revokeBtn = document.createElement("button");
      revokeBtn.type = "button";
      revokeBtn.dataset.variant = "danger";
      applyIconButton(revokeBtn, "revoke", "Revoke API key");
      revokeBtn.addEventListener("click", () => {
        void revokeKey(key.id, key.name || "this API key", revokeBtn);
      });
      actionRow.appendChild(revokeBtn);
    } else {
      const restoreBtn = document.createElement("button");
      restoreBtn.type = "button";
      applyIconButton(restoreBtn, "restore", "Unrevoke API key");
      restoreBtn.addEventListener("click", () => {
        void unrevokeKey(key.id, key.name || "this API key", restoreBtn);
      });
      actionRow.appendChild(restoreBtn);

      const deleteBtn = document.createElement("button");
      deleteBtn.type = "button";
      deleteBtn.dataset.variant = "danger";
      applyIconButton(deleteBtn, "trash", "Delete API key");
      deleteBtn.addEventListener("click", () => {
        void deleteKey(key.id, key.name || "this API key", deleteBtn);
      });
      actionRow.appendChild(deleteBtn);
    }

    controls.appendChild(status);
    controls.appendChild(actionRow);

    header.appendChild(title);
    header.appendChild(controls);

    main.appendChild(header);
    main.appendChild(infoRow);
    main.appendChild(paidFallbackSummary);

    const editPanel = document.createElement("div");
    editPanel.dataset.keyEdit = "panel";
    editPanel.dataset.editPanel = "panel";
    editPanel.hidden = true;

    const editFields = document.createElement("div");
    editFields.dataset.keyEditFields = "fields";
    editFields.dataset.editFields = "fields";

    const nameField = document.createElement("label");
    nameField.dataset.field = "true";
    const nameLabel = document.createElement("span");
    nameLabel.dataset.label = "label";
    nameLabel.textContent = "Name";
    const nameInput = document.createElement("input");
    nameInput.type = "text";
    nameInput.inputMode = "text";
    nameInput.autocomplete = "off";
    nameInput.value = key.name || "";
    nameField.appendChild(nameLabel);
    nameField.appendChild(nameInput);

    const limitField = document.createElement("label");
    limitField.dataset.field = "true";
    const limitLabel = document.createElement("span");
    limitLabel.dataset.label = "label";
    limitLabel.textContent = "Rate limit";
    const limitInput = document.createElement("input");
    limitInput.type = "number";
    limitInput.inputMode = "numeric";
    limitInput.value = typeof key.usage_limit_requests === "number" ? String(key.usage_limit_requests) : "-1";
    limitField.appendChild(limitLabel);
    limitField.appendChild(limitInput);

    const windowField = document.createElement("label");
    windowField.dataset.field = "true";
    const windowLabel = document.createElement("span");
    windowLabel.dataset.label = "label";
    windowLabel.textContent = "Window (ms)";
    const windowInput = document.createElement("input");
    windowInput.type = "number";
    windowInput.inputMode = "numeric";
    windowInput.value = String(resolveKeyWindowMs());
    windowField.appendChild(windowLabel);
    windowField.appendChild(windowInput);

    const expiresField = document.createElement("label");
    expiresField.dataset.field = "true";
    const expiresLabel = document.createElement("span");
    expiresLabel.dataset.label = "label";
    expiresLabel.textContent = "Expires At";
    const expiresInput = document.createElement("input");
    expiresInput.type = "datetime-local";
    expiresInput.value = key.expires_at_ms === -1 ? "" : toDateTimeLocalValue(key.expires_at_ms);
    expiresInput.disabled = key.expires_at_ms === -1;
    expiresField.appendChild(expiresLabel);
    expiresField.appendChild(expiresInput);

    const neverLabel = document.createElement("label");
    neverLabel.dataset.check = "true";
    const neverInput = document.createElement("input");
    neverInput.type = "checkbox";
    neverInput.checked = key.expires_at_ms === -1;
    const neverText = document.createElement("span");
    neverText.textContent = "Never expires";
    neverLabel.appendChild(neverInput);
    neverLabel.appendChild(neverText);

    const paidFallbackEditor = document.createElement("section");
    paidFallbackEditor.dataset.paidFallbackEditor = "editor";

    const paidFallbackToggle = document.createElement("label");
    paidFallbackToggle.dataset.check = "true";
    paidFallbackToggle.dataset.paidFallbackToggle = "toggle";
    const paidFallbackToggleCopy = document.createElement("span");
    const paidFallbackToggleTitle = document.createElement("strong");
    paidFallbackToggleTitle.textContent = "Metered paid overflow";
    const paidFallbackToggleHint = document.createElement("small");
    paidFallbackToggleHint.textContent = "Fallback after the final Codex 429.";
    paidFallbackToggleCopy.appendChild(paidFallbackToggleTitle);
    paidFallbackToggleCopy.appendChild(paidFallbackToggleHint);
    const paidFallbackInput = document.createElement("input");
    paidFallbackInput.type = "checkbox";
    paidFallbackInput.setAttribute("role", "switch");
    paidFallbackInput.checked = key.paid_fallback_enabled === true;
    paidFallbackToggle.appendChild(paidFallbackToggleCopy);
    paidFallbackToggle.appendChild(paidFallbackInput);

    const paidFallbackSettings = document.createElement("div");
    paidFallbackSettings.dataset.paidFallbackSettings = "settings";
    const paidFallbackLimitField = document.createElement("label");
    paidFallbackLimitField.dataset.field = "true";
    const paidFallbackLimitLabel = document.createElement("span");
    paidFallbackLimitLabel.dataset.label = "label";
    paidFallbackLimitLabel.textContent = "Window cap (credits; -1 = unlimited)";
    const paidFallbackLimitInput = document.createElement("input");
    paidFallbackLimitInput.type = "number";
    paidFallbackLimitInput.inputMode = "decimal";
    paidFallbackLimitInput.min = "-1";
    paidFallbackLimitInput.step = "0.000001";
    paidFallbackLimitInput.placeholder = "-1 or 1";
    paidFallbackLimitInput.value = typeof key.paid_fallback_limit_credits === "number"
      ? String(key.paid_fallback_limit_credits)
      : "0";
    paidFallbackLimitField.appendChild(paidFallbackLimitLabel);
    paidFallbackLimitField.appendChild(paidFallbackLimitInput);

    const paidFallbackWarning = document.createElement("p");
    paidFallbackWarning.dataset.paidFallbackWarning = "warning";
    paidFallbackWarning.textContent =
      "Enabling fallback can send prompts, code, tools, and attachments to Metered. Pricing is checked only when this key is enabled; re-enabling checks it again. Ordinary requests never recheck it.";

    paidFallbackSettings.appendChild(paidFallbackLimitField);
    paidFallbackSettings.appendChild(paidFallbackWarning);
    paidFallbackEditor.appendChild(paidFallbackToggle);
    paidFallbackEditor.appendChild(paidFallbackSettings);

    const syncPaidFallbackEditorVisibility = () => {
      const enabled = paidFallbackInput.checked;
      paidFallbackSettings.hidden = !enabled;
      paidFallbackLimitInput.disabled = !enabled;
      paidFallbackLimitInput.required = enabled;
    };
    syncPaidFallbackEditorVisibility();

    editFields.appendChild(nameField);
    editFields.appendChild(limitField);
    editFields.appendChild(windowField);
    editFields.appendChild(expiresField);
    editPanel.appendChild(editFields);

    const presetRow = document.createElement("div");
    presetRow.dataset.layout = "row";
    const preset1m = document.createElement("button");
    preset1m.type = "button";
    preset1m.textContent = "1m";
    const preset1h = document.createElement("button");
    preset1h.type = "button";
    preset1h.textContent = "1h";
    const preset1d = document.createElement("button");
    preset1d.type = "button";
    preset1d.textContent = "1d";
    const preset1w = document.createElement("button");
    preset1w.type = "button";
    preset1w.textContent = "1w";
    presetRow.appendChild(preset1m);
    presetRow.appendChild(preset1h);
    presetRow.appendChild(preset1d);
    presetRow.appendChild(preset1w);

    editPanel.appendChild(presetRow);
    editPanel.appendChild(neverLabel);
    editPanel.appendChild(paidFallbackEditor);

    const editBadge = document.createElement("span");
    editBadge.dataset.badge = "status";
    editBadge.dataset.state = "unknown";
    editBadge.textContent = "Idle";

    editPanel.appendChild(editBadge);

    const setEditBadge = (state, text) => setBadge(editBadge, state, text);

    let editSaving = false;
    let editQueued = false;
    let editDirty = false;
    let editSnapshot = {
      name: key.name || "",
      usage_limit_requests: typeof key.usage_limit_requests === "number" ? key.usage_limit_requests : -1,
      window_ms: resolveKeyWindowMs(),
      expires_at_ms: typeof key.expires_at_ms === "number" ? key.expires_at_ms : -1,
      paid_fallback_enabled: key.paid_fallback_enabled === true,
      paid_fallback_limit_credits: normalizeFiniteNumber(key.paid_fallback_limit_credits) ?? 0,
    };

    const getEditInputState = () => ({
      name: nameInput.value,
      limit: limitInput.value,
      window: windowInput.value,
      expires: expiresInput.value,
      never: neverInput.checked,
      paidFallbackEnabled: paidFallbackInput.checked,
      paidFallbackLimit: paidFallbackLimitInput.value,
    });

    const isSameEditInputState = (left, right) =>
      left.name === right.name &&
      left.limit === right.limit &&
      left.window === right.window &&
      left.expires === right.expires &&
      left.never === right.never &&
      left.paidFallbackEnabled === right.paidFallbackEnabled &&
      left.paidFallbackLimit === right.paidFallbackLimit;

    const syncEditInputsFromKey = () => {
      nameInput.value = key.name || "";
      limitInput.value = String(key.usage_limit_requests);
      windowInput.value = String(resolveKeyWindowMs());
      neverInput.checked = key.expires_at_ms === -1;
      expiresInput.disabled = neverInput.checked;
      expiresInput.value = neverInput.checked ? "" : toDateTimeLocalValue(key.expires_at_ms);
      paidFallbackInput.checked = key.paid_fallback_enabled === true;
      paidFallbackLimitInput.value = String(normalizeFiniteNumber(key.paid_fallback_limit_credits) ?? 0);
      syncPaidFallbackEditorVisibility();
    };

    const updateUsageInfo = () => {
      const usageData = getUsageInfoData();
      limitInfo.valueEl.textContent = formatLimitValue(usageData.limitValue);
      if (usageData.limitValue === 0) {
        limitInfo.item.dataset.state = "bad";
      } else {
        delete limitInfo.item.dataset.state;
      }
      windowInfo.valueEl.textContent = formatWindowShort(usageData.windowMs);
      windowInfo.valueEl.title = formatWindowMs(usageData.windowMs);
      usageInfo.valueEl.textContent = usageData.usageText;
      if (usageData.state) {
        usageInfo.item.dataset.state = usageData.state;
      } else {
        delete usageInfo.item.dataset.state;
      }
      usageInfo.valueEl.title = usageData.title;
      resetInfo.valueEl.textContent = formatDate(key.usage_reset_at_ms);
      updatePaidFallbackInfo();
    };

    const buildEditPayload = () => {
      const payload = { id: key.id };
      let changed = false;

      const nextName = nameInput.value.trim();
      if (!nextName) {
        setEditBadge("bad", "Name required");
        return null;
      }
      if (nextName !== editSnapshot.name) {
        payload.name = nextName;
        changed = true;
      }

      const limitRaw = limitInput.value.trim();
      if (!limitRaw) {
        setEditBadge("bad", "Rate limit required");
        return null;
      }
      let nextLimit;
      if (limitRaw === "unlimited" || limitRaw === "-1") {
        nextLimit = -1;
      } else {
        const parsed = Number(limitRaw);
        if (!Number.isFinite(parsed) || parsed < 0) {
          setEditBadge("bad", "Invalid rate limit");
          return null;
        }
        nextLimit = Math.trunc(parsed);
      }
      if (nextLimit !== editSnapshot.usage_limit_requests) {
        payload.usage_limit_requests = nextLimit;
        changed = true;
      }

      const windowResult = parseKernelWindowValue(windowInput.value, setEditBadge);
      if (!windowResult.ok) return null;
      let nextWindowMs = editSnapshot.window_ms;
      if (windowResult.value !== null) {
        nextWindowMs = windowResult.value;
      } else {
        windowInput.value = String(editSnapshot.window_ms);
      }
      if (nextWindowMs !== editSnapshot.window_ms) {
        payload.window_ms = nextWindowMs;
        changed = true;
      }

      let nextExpiresAtMs = editSnapshot.expires_at_ms;
      if (neverInput.checked) {
        nextExpiresAtMs = -1;
      } else {
        const parsed = parseDateTimeLocalValue(expiresInput.value);
        if (parsed === null) {
          setEditBadge("bad", "Expiration required");
          return null;
        }
        nextExpiresAtMs = parsed;
      }
      if (nextExpiresAtMs !== editSnapshot.expires_at_ms) {
        payload.expires_at_ms = nextExpiresAtMs;
        changed = true;
      }

      const nextPaidFallbackEnabled = paidFallbackInput.checked;
      const paidFallbackLimitRaw = paidFallbackLimitInput.value.trim();
      let nextPaidFallbackLimit = editSnapshot.paid_fallback_limit_credits;
      if (nextPaidFallbackEnabled && !paidFallbackLimitRaw) {
        setEditBadge("bad", "Fallback cap or -1 required");
        return null;
      }
      if (paidFallbackLimitRaw) {
        const parsed = Number(paidFallbackLimitRaw);
        const scaled = parsed * MICROCREDITS_PER_CREDIT;
        if (!Number.isFinite(parsed) || (parsed !== -1 && parsed < 0)) {
          setEditBadge("bad", "Invalid fallback cap");
          return null;
        }
        if (parsed !== -1 && Math.abs(scaled - Math.round(scaled)) > 0.000001) {
          setEditBadge("bad", "Fallback cap supports 6 decimals");
          return null;
        }
        nextPaidFallbackLimit = parsed === -1 ? -1 : Math.round(scaled) / MICROCREDITS_PER_CREDIT;
      }
      if (nextPaidFallbackEnabled && nextPaidFallbackLimit !== -1 && nextPaidFallbackLimit <= 0) {
        setEditBadge("bad", "Fallback cap must be positive or -1");
        return null;
      }
      if (nextPaidFallbackEnabled !== editSnapshot.paid_fallback_enabled) {
        payload.paid_fallback_enabled = nextPaidFallbackEnabled;
        changed = true;
      }
      if (nextPaidFallbackLimit !== editSnapshot.paid_fallback_limit_credits) {
        payload.paid_fallback_limit_credits = nextPaidFallbackLimit;
        changed = true;
      }

      if (!changed) {
        editDirty = false;
        setEditBadge("ok", "Saved");
        return null;
      }

      return {
        payload,
        nextSnapshot: {
          name: nextName,
          usage_limit_requests: nextLimit,
          window_ms: nextWindowMs,
          expires_at_ms: nextExpiresAtMs,
          paid_fallback_enabled: nextPaidFallbackEnabled,
          paid_fallback_limit_credits: nextPaidFallbackLimit,
        },
      };
    };

    const saveEdits = async () => {
      if (!editDirty || editSaving) return;
      const result = buildEditPayload();
      if (!result) return;
      const token = getAdminToken();
      if (!token && !hasAdminCredential()) {
        setEditBadge("bad", "Missing token");
        setAuthBadge("bad", "Missing token");
        tokenInput.focus();
        return;
      }
      const { payload, nextSnapshot } = result;
      const hasExpiresField = Object.prototype.hasOwnProperty.call(payload, "expires_at_ms");
      const requestInputs = getEditInputState();
      const initializingPaidFallback = payload.paid_fallback_enabled === true &&
        editSnapshot.paid_fallback_enabled === false;
      editSaving = true;
      setEditBadge("unknown", initializingPaidFallback ? "Initializing Metered..." : "Saving...");
      try {
        const res = await fetch(apiUrl("/admin/api-keys"), {
          method: "PATCH",
          headers: {
            "Content-Type": "application/json",
            Authorization: `Bearer ${token}`,
          },
          body: JSON.stringify(payload),
        });
        const data = await res.json().catch(() => null);
        if (!res.ok) {
          setEditBadge("bad", data?.error?.message ?? "Error");
          return;
        }

        const updatedName = typeof data?.name === "string" ? data.name : nextSnapshot.name;
        const updatedLimit = typeof data?.usage_limit_requests === "number"
          ? data.usage_limit_requests
          : nextSnapshot.usage_limit_requests;
        const updatedWindowMs = typeof data?.window_ms === "number" ? data.window_ms : nextSnapshot.window_ms;
        const updatedExpires = typeof data?.expires_at_ms === "number"
          ? data.expires_at_ms
          : nextSnapshot.expires_at_ms;
        const updatedPaidFallbackEnabled = typeof data?.paid_fallback_enabled === "boolean"
          ? data.paid_fallback_enabled
          : nextSnapshot.paid_fallback_enabled;
        const updatedPaidFallbackLimit = typeof data?.paid_fallback_limit_credits === "number"
          ? data.paid_fallback_limit_credits
          : nextSnapshot.paid_fallback_limit_credits;
        const expiresMismatch = hasExpiresField &&
          typeof data?.expires_at_ms === "number" &&
          data.expires_at_ms !== nextSnapshot.expires_at_ms;
        const resolvedExpires = expiresMismatch ? nextSnapshot.expires_at_ms : updatedExpires;

        key.name = updatedName;
        key.usage_limit_requests = updatedLimit;
        key.window_ms = updatedWindowMs;
        key.expires_at_ms = resolvedExpires;
        key.paid_fallback_enabled = updatedPaidFallbackEnabled;
        key.paid_fallback_limit_credits = updatedPaidFallbackLimit;
        if (typeof data?.usage_requests === "number") key.usage_requests = data.usage_requests;
        if (typeof data?.usage_reset_at_ms === "number") key.usage_reset_at_ms = data.usage_reset_at_ms;
        if (typeof data?.paid_fallback_spent_credits === "number") {
          key.paid_fallback_spent_credits = data.paid_fallback_spent_credits;
        }
        if (typeof data?.paid_fallback_reserved_credits === "number") {
          key.paid_fallback_reserved_credits = data.paid_fallback_reserved_credits;
        }

        title.textContent = key.name || "Untitled";
        expiresInfo.valueEl.textContent = formatExpires(key.expires_at_ms);
        updateUsageInfo();

        const inputsUnchanged = isSameEditInputState(requestInputs, getEditInputState());
        if (inputsUnchanged && !expiresMismatch) {
          syncEditInputsFromKey();
        }

        editSnapshot = {
          name: key.name || "",
          usage_limit_requests: key.usage_limit_requests,
          window_ms: resolveKeyWindowMs(),
          expires_at_ms: key.expires_at_ms,
          paid_fallback_enabled: key.paid_fallback_enabled === true,
          paid_fallback_limit_credits: normalizeFiniteNumber(key.paid_fallback_limit_credits) ?? 0,
        };
        editDirty = expiresMismatch || !inputsUnchanged;
        if (expiresMismatch) {
          setEditBadge("bad", "Save conflict — retry");
        } else if (editDirty) {
          setEditBadge("unknown", "Editing...");
        } else {
          setEditBadge("ok", "Saved");
        }
      } catch {
        setEditBadge("bad", "Offline");
      } finally {
        editSaving = false;
        if (editQueued) {
          editQueued = false;
          scheduleEditSave();
        }
      }
    };

    const scheduleEditSave = debounce(() => {
      void saveEdits();
    }, 1000);

    const markEditDirty = () => {
      editDirty = true;
      if (editSaving) {
        editQueued = true;
        return;
      }
      scheduleEditSave();
    };

    editBtn.addEventListener("click", () => {
      const willOpen = editPanel.hidden;
      editPanel.hidden = !willOpen;
      applyIconButton(editBtn, willOpen ? "close" : "edit", willOpen ? "Close editor" : "Edit API key");
    });

    nameInput.addEventListener("input", () => {
      markEditDirty();
    });
    limitInput.addEventListener("input", () => {
      markEditDirty();
    });
    windowInput.addEventListener("input", () => {
      markEditDirty();
    });
    expiresInput.addEventListener("input", () => {
      markEditDirty();
    });
    neverInput.addEventListener("change", () => {
      expiresInput.disabled = neverInput.checked;
      if (neverInput.checked) expiresInput.value = "";
      markEditDirty();
    });
    paidFallbackInput.addEventListener("change", () => {
      syncPaidFallbackEditorVisibility();
      markEditDirty();
      const paidFallbackLimit = Number(paidFallbackLimitInput.value);
      if (paidFallbackInput.checked && paidFallbackLimit !== -1 && !(paidFallbackLimit > 0)) {
        paidFallbackLimitInput.focus();
      }
    });
    paidFallbackLimitInput.addEventListener("input", () => {
      markEditDirty();
    });

    preset1m.addEventListener("click", () => {
      windowInput.value = "60000";
      markEditDirty();
    });
    preset1h.addEventListener("click", () => {
      windowInput.value = "3600000";
      markEditDirty();
    });
    preset1d.addEventListener("click", () => {
      windowInput.value = "86400000";
      markEditDirty();
    });
    preset1w.addEventListener("click", () => {
      windowInput.value = "604800000";
      markEditDirty();
    });

    main.appendChild(editPanel);

    if (key?.id) {
      const { panel } = buildApiKeyRequestLogsPanel(key.id);
      panel.addEventListener("toggle", () => {
        if (panel.open) {
          void hydrateApiKeyRequestLogs(panel, key.id);
        }
      });
      main.appendChild(panel);
    }

    const hasUsageField = Object.prototype.hasOwnProperty.call(key, "usage");
    const usage = hasUsageField ? key.usage : undefined;

    if (hasUsageField) {
      main.appendChild(buildUsageDetails(usage, {
        label: "Request limit",
        unavailable: "Request count unavailable.",
        empty: "No request count available.",
      }));
    }

    row.appendChild(main);
    keysList.appendChild(row);
  });
  setKeysBadge("ok", `${formatPlural(filteredKeys.length, "API key")}`);
  updateAccessApiKeysSummary();
};

const renderPasskeyUsers = (users) => {
  passkeyUsersList.textContent = "";
  if (!Array.isArray(users) || users.length === 0) {
    setPasskeyUsersMessage("No passkey users yet.");
    setPasskeyUsersBadge("ok", "No users");
    return;
  }

  users.forEach((user, index) => {
    const row = document.createElement("article");
    row.dataset.key = "passkey-user";
    row.dataset.state = user.is_admin ? "active" : "warning";
    row.style.setProperty("--i", index);
    row.setAttribute("role", "listitem");

    const main = document.createElement("div");
    main.dataset.keyMain = "main";

    const header = document.createElement("header");
    header.dataset.keyHeader = "header";

    const title = document.createElement("h3");
    title.dataset.keyTitle = "title";
    title.textContent = typeof user.handle === "string" && user.handle ? user.handle : "Unknown username";

    const controls = document.createElement("div");
    controls.dataset.keyControls = "controls";

    const status = document.createElement("span");
    status.dataset.badge = "role";
    status.dataset.state = user.is_admin ? "ok" : "unknown";
    status.textContent = user.is_admin ? "Admin" : "User";

    const adminLabel = document.createElement("label");
    adminLabel.dataset.check = "admin";
    const adminCheckbox = document.createElement("input");
    adminCheckbox.type = "checkbox";
    adminCheckbox.checked = user.is_admin === true;
    adminCheckbox.dataset.passkeyUserAdmin = user.id;
    const adminText = document.createElement("span");
    adminText.textContent = "Admin";
    adminLabel.appendChild(adminCheckbox);
    adminLabel.appendChild(adminText);

    adminCheckbox.addEventListener("change", () => {
      void updatePasskeyUserAdmin(user.id, adminCheckbox.checked, adminCheckbox);
    });

    controls.appendChild(status);
    controls.appendChild(adminLabel);
    header.appendChild(title);
    header.appendChild(controls);

    const infoRow = document.createElement("div");
    infoRow.dataset.keyInfo = "info";
    appendKeyInfo(infoRow, "User ID", user.id ?? "unknown", { mono: true });
    appendKeyInfo(infoRow, "Passkeys", formatNumber(user.credential_count ?? 0));
    appendKeyInfo(infoRow, "Updated", formatDate(user.updated_at_ms));
    appendKeyInfo(infoRow, "Created", formatDate(user.created_at_ms));

    main.appendChild(header);
    main.appendChild(infoRow);
    row.appendChild(main);
    passkeyUsersList.appendChild(row);
  });

  setPasskeyUsersBadge("ok", formatPlural(users.length, "user"));
};

const refreshPasskeyUsers = async () => {
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setPasskeyUsersBadge("bad", "Missing token");
    setPasskeyUsersMessage("Paste a fallback admin token to manage passkey users.");
    return;
  }

  if (passkeyUsersLoading) return;
  passkeyUsersLoading = true;
  setPasskeyUsersBadge("unknown", passkeyUsersLoadedAt ? "Cached · refreshing" : "Loading...");

  try {
    const res = await fetch(apiUrl("/admin/passkey-users"), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      if (passkeyUsersLoadedAt) {
        setPasskeyUsersBadge("unknown", "Cached · refresh unavailable");
        return;
      }
      setPasskeyUsersBadge("bad", data?.error?.message ?? "Error");
      setPasskeyUsersMessage(
        res.status === 403
          ? "Super admin token required. Paste a Deno/admin token in the fallback token field."
          : "Failed to load passkey users.",
      );
      return;
    }

    passkeyUsers = Array.isArray(data?.data) ? data.data : [];
    passkeyUsersLoadedAt = Date.now();
    renderPasskeyUsers(passkeyUsers);
  } catch {
    if (passkeyUsersLoadedAt) {
      setPasskeyUsersBadge("unknown", "Cached · offline");
      return;
    }
    passkeyUsers = [];
    setPasskeyUsersBadge("bad", "Offline");
    setPasskeyUsersMessage("Failed to load passkey users.");
  } finally {
    passkeyUsersLoading = false;
  }
};

const ensurePasskeyUsersLoaded = async () => {
  if (currentAdminView !== "users") return;
  if (passkeyUsersLoading) return;
  if (passkeyUsersLoadedAt && Date.now() - passkeyUsersLoadedAt < 10_000) {
    renderPasskeyUsers(passkeyUsers);
    return;
  }
  await refreshPasskeyUsers();
};

const updatePasskeyUserAdmin = async (id, isAdmin, checkbox) => {
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setPasskeyUsersBadge("bad", "Missing token");
    checkbox.checked = !isAdmin;
    tokenInput.focus();
    return;
  }

  checkbox.disabled = true;
  setPasskeyUsersBadge("unknown", "Saving...");
  try {
    const res = await fetch(apiUrl("/admin/passkey-users"), {
      method: "PATCH",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${token}`,
      },
      body: JSON.stringify({ id, is_admin: isAdmin }),
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      checkbox.checked = !isAdmin;
      setPasskeyUsersBadge("bad", data?.error?.message ?? "Error");
      return;
    }
    const updated = data?.user;
    passkeyUsers = passkeyUsers.map((user) => (user.id === id && updated ? updated : user));
    renderPasskeyUsers(passkeyUsers);
    setPasskeyUsersBadge("ok", "Saved");
  } catch {
    checkbox.checked = !isAdmin;
    setPasskeyUsersBadge("bad", "Offline");
  } finally {
    checkbox.disabled = false;
  }
};

const ADMIN_VIEW_DEFAULT = "loading";
const ADMIN_VIEW_AUTHENTICATED_DEFAULT = "keys";
const VIEW_HASHES = {
  loading: "loading",
  keys: "keys",
  users: "users",
  kernel: "kernel",
  pubkeys: "pubkeys",
  defaults: "defaults",
  providers: "providers",
  errors: "errors",
};
const VIEW_REQUIREMENTS = {
  keys: "admin",
  users: "super-admin",
  kernel: "admin",
  pubkeys: "admin",
  defaults: "admin",
  providers: "admin",
  errors: "admin",
};
const VIEW_HASH_ALIASES = new Map([
  ["loading", "loading"],
  ["view-loading", "loading"],
  ["api-keys", "keys"],
  ["keys", "keys"],
  ["view-keys", "keys"],
  ["users", "users"],
  ["view-users", "users"],
  ["github-access", "kernel"],
  ["kernel", "kernel"],
  ["view-kernel", "kernel"],
  ["kernel-attestation", "pubkeys"],
  ["pubkeys", "pubkeys"],
  ["view-pubkeys", "pubkeys"],
  ["defaults", "defaults"],
  ["view-defaults", "defaults"],
  ["providers", "providers"],
  ["view-providers", "providers"],
  ["errors", "errors"],
  ["view-errors", "errors"],
  ["auth", "session"],
  ["session", "session"],
  ["view-session", "session"],
]);

const normalizeAdminView = (view) => viewSections[view] ? view : ADMIN_VIEW_DEFAULT;

const canAccessView = (view) => {
  if (view === "loading") return true;
  const requirement = VIEW_REQUIREMENTS[view];
  if (requirement === "super-admin") return adminAccessState.isSuperAdmin === true;
  if (requirement === "admin") return adminAccessState.isAdmin === true;
  return false;
};

const setTabState = (tab, selected, enabled = true) => {
  tab.setAttribute("aria-selected", selected ? "true" : "false");
  tab.setAttribute("aria-disabled", enabled ? "false" : "true");
  tab.disabled = !enabled;
  tab.tabIndex = enabled && selected ? 0 : -1;
  if (!enabled) {
    const label = tab.id === "view-tab-users" ? "Super admin required" : "Admin sign-in required";
    tab.title = label;
  } else {
    tab.removeAttribute("title");
  }
};

const revealTabInList = (tab) => {
  const tablist = tab?.closest('[role="tablist"]');
  if (!tablist) return;
  globalThis.requestAnimationFrame(() => {
    const targetLeft = tab.offsetLeft - (tablist.clientWidth - tab.offsetWidth) / 2;
    tablist.scrollTo({
      left: Math.max(0, targetLeft),
      behavior: "auto",
    });
  });
};

const bindTablistKeyboard = (tablist) => {
  if (!tablist) return;
  tablist.addEventListener("keydown", (event) => {
    if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) return;
    const tabs = [...tablist.querySelectorAll('[role="tab"]:not(:disabled)')];
    if (!tabs.length) return;
    const currentIndex = Math.max(0, tabs.indexOf(document.activeElement));
    const nextIndex = event.key === "Home"
      ? 0
      : event.key === "End"
      ? tabs.length - 1
      : event.key === "ArrowRight"
      ? (currentIndex + 1) % tabs.length
      : (currentIndex - 1 + tabs.length) % tabs.length;
    event.preventDefault();
    tabs[nextIndex].focus();
    tabs[nextIndex].click();
  });
};

const viewTabs = {
  keys: viewTabKeys,
  users: viewTabUsers,
  kernel: viewTabKernel,
  pubkeys: viewTabPubkeys,
  defaults: viewTabDefaults,
  providers: viewTabProviders,
  errors: viewTabErrors,
};

const viewSections = {
  loading: viewLoading,
  keys: viewKeys,
  users: viewUsers,
  kernel: viewKernel,
  pubkeys: viewPubkeys,
  defaults: viewDefaults,
  providers: viewProviders,
  errors: viewErrors,
};

const setErrorsMessage = (message) => {
  errorsList.textContent = "";
  const element = document.createElement("p");
  element.dataset.empty = "errors";
  element.textContent = message;
  errorsList.appendChild(element);
};

const invalidateAdminErrors = (message) => {
  errorsLoadId += 1;
  errorsLoading = false;
  errorsLoadedAt = 0;
  setBadge(errorsBadge, "unknown", "Not loaded");
  errorsUpdated.textContent = "";
  setErrorsMessage(message);
};

const renderAdminErrors = (records) => {
  errorsList.textContent = "";
  if (!records.length) {
    setErrorsMessage("No errors recorded in the last seven days.");
    return;
  }
  records.forEach((record) => {
    const row = document.createElement("article");
    row.dataset.key = "gateway-error";
    row.setAttribute("role", "listitem");
    const header = document.createElement("header");
    const title = document.createElement("strong");
    title.textContent = record.failure_kind || `HTTP ${record.status}`;
    const timestamp = document.createElement("span");
    timestamp.dataset.muted = "";
    timestamp.textContent = formatDate(record.created_at_ms);
    header.append(title, timestamp);
    const details = document.createElement("div");
    details.dataset.meta = "usage";
    appendMetaItem(details, "Status", String(record.status), { state: "bad" });
    appendMetaItem(details, "Route", record.route || "unknown");
    appendMetaItem(details, "Provider", record.provider || "gateway");
    appendMetaItem(details, "Model", record.model || "—");
    appendMetaItem(details, "Terminal", record.terminal_type || "error");
    appendMetaItem(details, "Request", record.request_id || "—", { mono: true });
    appendMetaItem(details, "Revision", record.deno_revision || "—", { mono: true });
    row.append(header, details);
    errorsList.appendChild(row);
  });
};

const loadAdminErrors = async () => {
  if (errorsLoading) return;
  const loadId = ++errorsLoadId;
  errorsLoading = true;
  setBadge(errorsBadge, "unknown", errorsLoadedAt ? "Cached · refreshing" : "Loading");
  try {
    const token = getAdminToken();
    const res = await fetch(apiUrl("/admin/errors?limit=200"), {
      headers: token ? { Authorization: `Bearer ${token}` } : {},
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (loadId !== errorsLoadId) return;
    if (!res.ok) throw new Error(data?.error?.message || "Error history is unavailable");
    const records = Array.isArray(data?.data) ? data.data : [];
    renderAdminErrors(records);
    errorsLoadedAt = Date.now();
    setBadge(errorsBadge, records.length ? "bad" : "ok", `${records.length} errors`);
    errorsUpdated.textContent = `Updated ${formatDate(errorsLoadedAt)}`;
  } catch (error) {
    if (loadId !== errorsLoadId) return;
    if (errorsLoadedAt) {
      setBadge(errorsBadge, "unknown", "Cached · refresh unavailable");
      return;
    }
    setBadge(errorsBadge, "bad", "Unavailable");
    setErrorsMessage(error?.message || "Error history is unavailable");
  } finally {
    if (loadId === errorsLoadId) errorsLoading = false;
  }
};

const getHashView = () => {
  const raw = globalThis.location.hash.replace(/^#/, "").trim().toLowerCase();
  if (!raw) return null;
  return VIEW_HASH_ALIASES.get(raw) ?? null;
};

const syncAdminHash = (view, mode) => {
  if (!mode) return;
  const hash = VIEW_HASHES[view];
  if (!hash) return;
  const nextUrl = `${globalThis.location.pathname}${globalThis.location.search}#${hash}`;
  const current = `${globalThis.location.pathname}${globalThis.location.search}${globalThis.location.hash}`;
  if (current === nextUrl) return;
  const fn = mode === "push" ? "pushState" : "replaceState";
  globalThis.history[fn](null, "", nextUrl);
};

const getAdminPrefetchSignature = () => `${resolveBaseUrl()}::${getAdminToken()}`;

const resetAdminPrefetchState = (summary = "Sign in to prepare the admin views.") => {
  adminPrefetchRunId += 1;
  adminPrefetchSignature = "";
  adminPrefetchPromise = null;
  setLoadingSummary(summary);
  updateLoadingAuthStatus();
  resetLoadingPrefetchStatuses();
};

const runAdminPrefetchTask = async (runId, task) => {
  if (typeof task.allowed === "function" && !task.allowed()) {
    setLoadingStatus(task.key, "unknown", task.skippedText ?? "Skipped");
    return "skipped";
  }
  setLoadingStatus(task.key, "unknown", "Loading");
  try {
    await task.load();
  } catch {
    // The loaders normally own their error UI. This keeps prefetch progress isolated.
  }
  if (runId !== adminPrefetchRunId) return "stale";
  if (task.ready()) {
    setLoadingStatus(task.key, "ok", "Ready");
    return "ready";
  }
  setLoadingStatus(task.key, "bad", "Unavailable");
  return "failed";
};

const startAdminPrefetch = () => {
  if (!adminAccessState.isAdmin) {
    return Promise.resolve({ ready: 0, failed: 0, skipped: 0 });
  }

  const signature = getAdminPrefetchSignature();
  if (adminPrefetchSignature === signature && adminPrefetchPromise) return adminPrefetchPromise;

  const runId = ++adminPrefetchRunId;
  adminPrefetchSignature = signature;
  setLoadingSummary("Preparing admin views...");
  updateLoadingAuthStatus();
  resetLoadingPrefetchStatuses("Queued");

  const tasks = [
    {
      key: "keys",
      load: refreshKeys,
      ready: () => keysLoadedAt > 0,
    },
    {
      key: "users",
      allowed: () => adminAccessState.isSuperAdmin === true,
      skippedText: "Super admin",
      load: refreshPasskeyUsers,
      ready: () => passkeyUsersLoadedAt > 0,
    },
    {
      key: "kernel",
      load: loadKernelList,
      ready: () => kernelListLoadedAt > 0,
    },
    {
      key: "queue",
      load: refreshKernelPolicyQueue,
      ready: () => kernelQueueLoadedAt > 0,
    },
    {
      key: "pubkeys",
      load: refreshKernelPubKeys,
      ready: () => kernelPubKeysLoadedAt > 0,
    },
    {
      key: "defaults",
      load: loadDefaults,
      ready: () => defaultsLoaded === true,
    },
    {
      key: "upstream",
      load: refreshAccessUpstreamSummary,
      ready: () => accessUpstreamLoadedAt > 0,
    },
    {
      key: "providers",
      load: loadProviders,
      ready: () => providersLoadedAt > 0,
    },
  ];

  adminPrefetchPromise = Promise.all(tasks.map((task) => runAdminPrefetchTask(runId, task))).then((results) => {
    if (runId !== adminPrefetchRunId) return { ready: 0, failed: 0, skipped: 0 };
    updateAccessApiKeysSummary();
    updateAccessGithubSummary();
    updateAccessPubkeysSummary();
    const ready = results.filter((result) => result === "ready").length;
    const failed = results.filter((result) => result === "failed").length;
    const skipped = results.filter((result) => result === "skipped").length;
    setLoadingSummary(failed > 0 ? "Admin views prepared with unavailable data." : "Admin views prepared.");
    return { ready, failed, skipped };
  });

  return adminPrefetchPromise;
};

const updateViewAccess = () => {
  Object.entries(viewTabs).forEach(([key, tab]) => {
    setTabState(tab, key === currentAdminView, canAccessView(key));
  });
  if (!Object.keys(viewTabs).some((key) => key === currentAdminView && canAccessView(key))) {
    const firstAvailableTab = Object.entries(viewTabs).find(([key]) => canAccessView(key))?.[1];
    if (firstAvailableTab) firstAvailableTab.tabIndex = 0;
  }
};

const syncVisibleAdminView = () => {
  const visibleView = canAccessView(currentAdminView) ? currentAdminView : ADMIN_VIEW_DEFAULT;
  Object.entries(viewSections).forEach(([key, section]) => {
    section.hidden = key !== visibleView;
  });
};

const loadAdminView = (view) => {
  if (!canAccessView(view)) return;
  if (view === "keys") {
    void ensureKeysLoaded();
  }
  if (view === "users") {
    void ensurePasskeyUsersLoaded();
  }
  if (view === "defaults") {
    void loadDefaults();
  }
  if (view === "kernel") {
    void loadKernelList();
    void ensureKernelPolicyQueueLoaded();
    void refreshAccessOverview();
  }
  if (view === "pubkeys") {
    void ensureKernelPubKeysLoaded();
  }
  if (view === "providers") {
    void loadProviders();
    if (!providerCapacityLoadedForOpen) {
      providerCapacityLoadedForOpen = true;
      void loadProviderCapacity().then((loaded) => {
        if (!loaded) providerCapacityLoadedForOpen = false;
      });
    }
    if (!quotaProjectionLoadedForOpen) {
      quotaProjectionLoadedForOpen = true;
      void loadQuotaProjection().then((loaded) => {
        if (!loaded) quotaProjectionLoadedForOpen = false;
      });
    }
  } else {
    providerCapacityLoadedForOpen = false;
    quotaProjectionLoadedForOpen = false;
  }
  if (view === "errors" && (!errorsLoadedAt || Date.now() - errorsLoadedAt >= 10_000)) {
    void loadAdminErrors();
  }
};

const setAdminAccessState = (next) => {
  adminAccessState = {
    checked: next?.checked === true,
    isAdmin: next?.isAdmin === true,
    isSuperAdmin: next?.isSuperAdmin === true,
  };
  document.body.dataset.authScope = adminAccessState.isSuperAdmin
    ? "super-admin"
    : adminAccessState.isAdmin
    ? "admin"
    : "none";
  updateLoadingAuthStatus();
  syncLoadingGate();
  updateViewAccess();
  if (adminAccessState.isAdmin) {
    closeAutoOpenedAuthWidget();
  } else if (adminAccessState.checked) {
    closeAutoOpenedAuthWidget();
  }
  if (pendingAdminView && canAccessView(pendingAdminView)) {
    const view = pendingAdminView;
    pendingAdminView = null;
    setAdminView(view, { hashMode: "replace" });
  } else if (
    pendingAdminView && adminAccessState.checked && adminAccessState.isAdmin && !canAccessView(pendingAdminView)
  ) {
    pendingAdminView = null;
    setAdminView(ADMIN_VIEW_AUTHENTICATED_DEFAULT, { hashMode: "replace", allowInaccessible: false });
  } else if (adminAccessState.checked && !adminAccessState.isAdmin && !canAccessView(currentAdminView)) {
    currentAdminView = ADMIN_VIEW_DEFAULT;
    syncVisibleAdminView();
    updateViewAccess();
  } else if (adminAccessState.checked && adminAccessState.isAdmin && currentAdminView === ADMIN_VIEW_DEFAULT) {
    setAdminView(ADMIN_VIEW_AUTHENTICATED_DEFAULT, { allowInaccessible: false });
  } else if (adminAccessState.checked && adminAccessState.isAdmin && !canAccessView(currentAdminView)) {
    setAdminView(ADMIN_VIEW_AUTHENTICATED_DEFAULT, { hashMode: "replace", allowInaccessible: false });
  } else {
    syncVisibleAdminView();
    loadAdminView(currentAdminView);
  }
  if (adminAccessState.isAdmin) {
    globalThis.requestAnimationFrame(() => {
      globalThis.setTimeout(() => {
        if (adminAccessState.isAdmin) void startAdminPrefetch();
      }, 0);
    });
  }
};

const setAdminView = (view, options = {}) => {
  const nextView = normalizeAdminView(view);
  const allowInaccessible = options.allowInaccessible === true;
  if (!allowInaccessible && !canAccessView(nextView)) {
    if (adminAccessState.checked && adminAccessState.isAdmin) {
      pendingAdminView = null;
      setAdminView(ADMIN_VIEW_AUTHENTICATED_DEFAULT, { hashMode: "replace", allowInaccessible: false });
      return false;
    }
    pendingAdminView = nextView;
    if (!canAccessView(currentAdminView)) currentAdminView = ADMIN_VIEW_DEFAULT;
    syncVisibleAdminView();
    updateViewAccess();
    if (!isAdminAuthCheckPending()) {
      openAuthWidgetForAuth({ focus: options.focusAuth === true });
    }
    return false;
  }
  currentAdminView = nextView;
  pendingAdminView = null;
  syncVisibleAdminView();
  updateViewAccess();
  if (viewTabs[nextView]) revealTabInList(viewTabs[nextView]);
  storage.set(STORAGE_KEYS.view, nextView);
  syncAdminHash(nextView, options.hashMode);
  loadAdminView(nextView);
  return true;
};

const switchKeysView = (view) => {
  currentKeyView = view;
  setTabState(keysTabAll, view === "all");
  setTabState(keysTabActive, view === "active");
  setTabState(keysTabRevoked, view === "revoked");
  if (allKeys.length) {
    renderKeys(allKeys, view);
  } else {
    setKeyListMessage("Paste an admin token to load API keys.");
    setKeysBadge("unknown", "Not loaded");
  }
};

const computeExpiresAtMs = (preset) => {
  if (preset === "forever") return -1;
  const dayMs = 24 * 60 * 60 * 1000;
  const now = Date.now();
  switch (preset) {
    case "day":
      return now + dayMs;
    case "week":
      return now + 7 * dayMs;
    case "month":
      return now + 30 * dayMs;
    case "quarter":
      return now + 91 * dayMs;
    case "year":
      return now + 365 * dayMs;
    default:
      return -1;
  }
};

const testAdminToken = async ({ allowBearerFallback = true } = {}) => {
  const token = getAdminToken();
  setAuthBadge("unknown", "Checking...");
  try {
    const requestAuth = async (headers) => {
      const res = await fetchWithCredentials(apiUrl("/uos/auth"), {
        headers,
        cache: "no-store",
      });
      return { res, data: await res.json().catch(() => null) };
    };
    let authResult;
    if (isRemoteRelayOrigin()) {
      authResult = await requestAuth({});
      relaySessionActive = authResult.res.ok && authResult.data?.auth?.is_admin === true &&
        authResult.data?.auth?.method?.kind === "passkey_session";
      if (!relaySessionActive && token && allowBearerFallback) {
        authResult = await requestAuth({ Authorization: `Bearer ${token}` });
      }
    } else {
      authResult = await requestAuth(token ? { Authorization: `Bearer ${token}` } : {});
    }
    const { res, data } = authResult;
    if (!res.ok) {
      clearAdminSnapshotScope();
      setAuthBadge("bad", data?.error?.message ?? "Unauthorized");
      setAdminAccessState({ checked: true, isAdmin: false, isSuperAdmin: false });
      setSignedInState(false);
      return false;
    }
    if (!data?.auth?.is_admin) {
      clearAdminSnapshotScope();
      setAuthBadge("bad", "Not admin");
      setAdminAccessState({ checked: true, isAdmin: false, isSuperAdmin: false });
      setSignedInState(false);
      return false;
    }
    const kind = data?.auth?.method?.kind;
    const isSuperAdmin = data?.auth?.is_super_admin === true;
    setAuthBadge("ok", kind ? `OK (${formatAuthMethodLabel(kind)})` : "OK");
    setAdminSnapshotScopeFromAuth(data.auth);
    setAdminAccessState({ checked: true, isAdmin: true, isSuperAdmin });
    void hydrateAdminSnapshots();
    setSignedInState(true, {
      canRegisterPasskey: true,
      deviceRegistered: hasAuthPasskeyCredential(data?.auth) || hasStoredPasskeyCredentials(),
      statusText: formatAuthSessionLabel(data?.auth),
    });
    return true;
  } catch {
    clearAdminSnapshotScope();
    setAuthBadge("bad", "Offline");
    setAdminAccessState({ checked: true, isAdmin: false, isSuperAdmin: false });
    setSignedInState(false);
    return false;
  }
};

const scheduleTokenCheck = debounce(() => {
  void testAdminToken();
}, 500);

const createKey = async () => {
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setCreateBadge("bad", "Missing token");
    setAuthBadge("bad", "Missing token");
    tokenInput.focus();
    return;
  }

  const name = keyNameInput.value.trim();
  if (!name) {
    setCreateBadge("bad", "Name required");
    keyNameInput.focus();
    return;
  }

  const expiresPreset = keyExpiresSelect.value;
  const expiresAtMs = computeExpiresAtMs(expiresPreset);
  const usageLimit = parseInt(keyUsageLimitInput.value, 10);
  const windowResult = parseKernelWindowValue(keyUsageWindowInput.value, setCreateBadge);
  if (!windowResult.ok) return;
  const paidFallbackEnabled = keyPaidFallbackEnabledInput.checked;
  let paidFallbackLimitCredits = 0;
  if (paidFallbackEnabled) {
    const paidFallbackLimitRaw = keyPaidFallbackLimitInput.value.trim();
    const parsed = Number(paidFallbackLimitRaw);
    const scaled = parsed * MICROCREDITS_PER_CREDIT;
    if (!paidFallbackLimitRaw || !Number.isFinite(parsed) || (parsed !== -1 && parsed <= 0)) {
      setCreateBadge("bad", "Fallback cap must be positive or -1");
      keyPaidFallbackLimitInput.focus();
      return;
    }
    if (parsed !== -1 && Math.abs(scaled - Math.round(scaled)) > 0.000001) {
      setCreateBadge("bad", "Fallback cap supports 6 decimals");
      keyPaidFallbackLimitInput.focus();
      return;
    }
    paidFallbackLimitCredits = parsed === -1 ? -1 : Math.round(scaled) / MICROCREDITS_PER_CREDIT;
  }
  const payload = {
    name,
    expires_at_ms: expiresAtMs,
    usage_limit_requests: isNaN(usageLimit) ? 50 : usageLimit,
    paid_fallback_enabled: paidFallbackEnabled,
    paid_fallback_limit_credits: paidFallbackLimitCredits,
  };
  if (windowResult.value !== null) payload.window_ms = windowResult.value;

  clearCreateResult();
  setCreateBadge("unknown", paidFallbackEnabled ? "Initializing Metered..." : "Creating...");
  createKeyBtn.disabled = true;

  try {
    const res = await fetch(apiUrl("/admin/api-keys"), {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${token}`,
      },
      body: JSON.stringify(payload),
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      setCreateBadge("bad", "Error");
      createResult.textContent = data?.error?.message ?? "Request failed.";
      createResult.hidden = false;
      toast.error("Create failed", { description: data?.error?.message ?? "Request failed." });
      return;
    }

    const lines = [
      `token: ${data?.token ?? ""}`,
      `id: ${data?.id ?? ""}`,
      `name: ${data?.name ?? name}`,
      `prefix: ${data?.prefix ?? ""}`,
      `rate_limit: ${data?.usage_limit_requests === -1 ? "unlimited" : (data?.usage_limit_requests ?? "")}`,
      `window_ms: ${formatWindowMs(data?.window_ms)}`,
      `paid_fallback: ${
        (typeof data?.paid_fallback_enabled === "boolean" ? data.paid_fallback_enabled : paidFallbackEnabled)
          ? "enabled"
          : "disabled"
      }`,
      `paid_fallback_limit: ${
        formatPaidFallbackLimit(
          typeof data?.paid_fallback_limit_credits === "number"
            ? data.paid_fallback_limit_credits
            : paidFallbackLimitCredits,
        )
      }`,
      `created_at: ${formatDate(data?.created_at_ms)}`,
      `expires_at: ${formatExpires(data?.expires_at_ms)}`,
    ];
    createResult.textContent = lines.join("\n").trim();
    createResult.hidden = false;
    setCreateBadge("ok", "Created");
    toast.success("API key created", { description: "Copy the token now — it won't be shown again." });
    keyNameInput.value = "";
    keyUsageWindowInput.value = "";
    keyPaidFallbackEnabledInput.checked = false;
    keyPaidFallbackLimitInput.value = "";
    syncCreatePaidFallbackControls();
    void refreshKeys();
  } catch {
    setCreateBadge("bad", "Error");
    createResult.textContent = "Request failed.";
    createResult.hidden = false;
    toast.error("Create failed", { description: "Request failed." });
  } finally {
    createKeyBtn.disabled = false;
  }
};

const refreshKeys = async () => {
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setKeysBadge("bad", "Missing token");
    setKeyListMessage("Paste an admin token to load API keys.");
    updateAccessApiKeysSummary();
    return;
  }

  if (keysLoading) return;
  keysLoading = true;
  if (keysLoadedAt) {
    setKeysBadge("unknown", "Cached · refreshing");
  } else {
    setKeysBadge("unknown", "Loading...");
    setKeysListLoading();
  }

  try {
    const res = await fetch(apiUrl("/admin/api-keys?include_usage=1"), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      if (keysLoadedAt) {
        setKeysBadge("unknown", "Cached · refresh unavailable");
        return;
      }
      setKeysBadge("bad", data?.error?.message ?? "Error");
      setKeyListMessage("Failed to load API keys.");
      return;
    }
    const keys = Array.isArray(data?.data) ? data.data : [];
    allKeys = keys;
    keysLoadedAt = Date.now();
    renderKeys(allKeys, currentKeyView);
  } catch {
    if (keysLoadedAt) {
      setKeysBadge("unknown", "Cached · offline");
      return;
    }
    allKeys = [];
    setKeysBadge("bad", "Offline");
    setKeyListMessage("Failed to load API keys.");
  } finally {
    keysLoading = false;
    updateAccessApiKeysSummary();
  }
};

const ensureKeysLoaded = async () => {
  if (currentAdminView !== "keys") return;
  if (keysLoading) return;
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setKeysBadge("bad", "Missing token");
    setKeyListMessage("Paste an admin token to load API keys.");
    return;
  }
  if (keysLoadedAt && Date.now() - keysLoadedAt < 10_000) {
    renderKeys(allKeys, currentKeyView);
    return;
  }
  await refreshKeys();
};

const updateKeyRevocationState = (id, revokedAtMs) => {
  let updated = false;
  allKeys = allKeys.map((key) => {
    if (key?.id !== id) return key;
    updated = true;
    return { ...key, revoked_at_ms: revokedAtMs };
  });
  if (updated) {
    keysLoadedAt = Date.now();
    renderKeys(allKeys, currentKeyView);
  }
  return updated;
};

const revokeKey = async (id, name, button) => {
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setKeysBadge("bad", "Missing token");
    tokenInput.focus();
    return;
  }

  setKeysBadge("unknown", "Revoking...");
  if (button) button.disabled = true;

  try {
    const res = await fetch(apiUrl("/admin/api-keys/revoke"), {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${token}`,
      },
      body: JSON.stringify({ id }),
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      setKeysBadge("bad", data?.error?.message ?? "Error");
      return;
    }
    updateKeyRevocationState(id, typeof data?.revoked_at_ms === "number" ? data.revoked_at_ms : Date.now());
    setKeysBadge("ok", `Revoked ${name}`);
    void refreshKeys();
  } catch {
    setKeysBadge("bad", "Offline");
  } finally {
    if (button) button.disabled = false;
  }
};

const unrevokeKey = async (id, name, button) => {
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setKeysBadge("bad", "Missing token");
    tokenInput.focus();
    return;
  }

  setKeysBadge("unknown", "Unrevoking...");
  if (button) button.disabled = true;

  try {
    const res = await fetch(apiUrl("/admin/api-keys/unrevoke"), {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${token}`,
      },
      body: JSON.stringify({ id }),
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      setKeysBadge("bad", data?.error?.message ?? "Error");
      return;
    }
    updateKeyRevocationState(id, null);
    setKeysBadge("ok", `Unrevoked ${name}`);
    void refreshKeys();
  } catch {
    setKeysBadge("bad", "Offline");
  } finally {
    if (button) button.disabled = false;
  }
};

const deleteKey = async (id, name, button) => {
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setKeysBadge("bad", "Missing token");
    tokenInput.focus();
    return;
  }
  if (!confirm(`Permanently delete ${name}?`)) return;

  setKeysBadge("unknown", "Deleting...");
  if (button) button.disabled = true;

  try {
    const res = await fetch(apiUrl("/admin/api-keys"), {
      method: "DELETE",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${token}`,
      },
      body: JSON.stringify({ id }),
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      setKeysBadge("bad", data?.error?.message ?? "Error");
      toast.error("Delete failed", { description: data?.error?.message ?? "Error" });
      return;
    }
    await refreshKeys();
    toast.success(`Deleted ${name}`);
  } catch {
    setKeysBadge("bad", "Offline");
    toast.error("Delete failed", { description: "Offline" });
  } finally {
    if (button) button.disabled = false;
  }
};

const formatModelLabel = (model) => {
  const label = typeof model?.display_name === "string" ? model.display_name : model?.slug;
  return label && label.trim() ? label : "unknown";
};

const clearMeteredQuotaDiagnostics = () => {
  meteredQuotaRemaining.textContent = "—";
  meteredQuotaProgress.hidden = true;
  meteredQuotaProgress.value = 0;
  meteredQuotaProgress.removeAttribute("aria-valuetext");
  meteredQuotaBalance.textContent = "—";
  meteredQuotaGranted.textContent = "—";
  meteredQuotaTokenUsage.textContent = "—";
  meteredQuotaBaseline.textContent = "—";
  meteredQuotaLatestRefill.textContent = "—";
  meteredQuotaLatestRefill.removeAttribute("title");
  meteredQuotaInferredCredit.textContent = "—";
  meteredQuotaCache.textContent = "—";
  meteredQuotaConfidence.textContent = "—";
  meteredQuotaObserved.textContent = "—";
  meteredQuotaCycleStarted.textContent = "—";
};

const formatQuotaCredits = (value) =>
  typeof value === "number" && Number.isFinite(value) ? `${creditFormatter.format(value)} credits` : "—";

const formatQuotaTokens = (value) =>
  typeof value === "number" && Number.isFinite(value) ? numberFormatter.format(value) : "—";

const formatQuotaLabel = (value) => {
  if (typeof value !== "string" || !value) return "—";
  return value.replaceAll("_", " ").replace(/^./, (first) => first.toUpperCase());
};

const renderMeteredQuotaDiagnostics = (diagnostics) => {
  clearMeteredQuotaDiagnostics();
  if (!diagnostics || typeof diagnostics !== "object") {
    setMeteredQuotaBadge("bad", "Unavailable");
    return;
  }
  if (diagnostics.configured !== true) {
    setMeteredQuotaBadge("unknown", "Not configured");
    return;
  }
  if (diagnostics.available !== true) {
    setMeteredQuotaBadge("bad", "Unavailable");
    return;
  }

  const tokenUsage = typeof diagnostics.total_available === "number" ||
    typeof diagnostics.total_granted === "number" ||
    typeof diagnostics.total_used === "number";
  const remaining = !tokenUsage && typeof diagnostics.remaining_percent === "number" &&
      Number.isFinite(diagnostics.remaining_percent)
    ? Math.min(100, Math.max(0, diagnostics.remaining_percent))
    : null;
  if (remaining !== null) {
    const formatted = quotaPercentFormatter.format(remaining);
    meteredQuotaRemaining.textContent = `${formatted}%`;
    meteredQuotaProgress.value = remaining;
    meteredQuotaProgress.hidden = false;
    meteredQuotaProgress.setAttribute("aria-valuetext", `${formatted}% remaining`);
  }

  const unlimited = diagnostics.unlimited_quota === true;
  if (tokenUsage) {
    meteredQuotaRemaining.textContent = unlimited ? "Unlimited quota" : "Reported quota";
    meteredQuotaBalance.textContent = formatQuotaTokens(diagnostics.total_available);
    meteredQuotaGranted.textContent = formatQuotaTokens(diagnostics.total_granted);
    meteredQuotaBaseline.textContent = "Not applicable";
    meteredQuotaInferredCredit.textContent = "Not applicable";
  } else if (unlimited) {
    meteredQuotaRemaining.textContent = "Unlimited quota";
    meteredQuotaBalance.textContent = "—";
    meteredQuotaGranted.textContent = "—";
    meteredQuotaBaseline.textContent = "Not applicable";
    meteredQuotaInferredCredit.textContent = "Not applicable";
  } else {
    meteredQuotaBalance.textContent = formatQuotaCredits(diagnostics.balance_credits);
    meteredQuotaGranted.textContent = "—";
    meteredQuotaBaseline.textContent = formatQuotaCredits(diagnostics.baseline_credits);
    meteredQuotaInferredCredit.textContent = formatQuotaCredits(diagnostics.last_inferred_credit_credits);
  }
  meteredQuotaTokenUsage.textContent = diagnostics.total_used === null || diagnostics.total_used === undefined
    ? "—"
    : formatQuotaTokens(diagnostics.total_used);
  meteredQuotaCache.textContent = formatQuotaLabel(diagnostics.cache_state);
  meteredQuotaConfidence.textContent = formatQuotaLabel(diagnostics.confidence);
  meteredQuotaObserved.textContent = typeof diagnostics.observed_at_ms === "number"
    ? formatDate(diagnostics.observed_at_ms)
    : "—";
  meteredQuotaCycleStarted.textContent = typeof diagnostics.cycle_started_at_ms === "number"
    ? formatDate(diagnostics.cycle_started_at_ms)
    : "—";

  const refillAmount = formatQuotaCredits(diagnostics.latest_refill_amount_credits);
  const refillTime = typeof diagnostics.latest_refill_completed_at_ms === "number"
    ? formatDate(diagnostics.latest_refill_completed_at_ms)
    : "—";
  meteredQuotaLatestRefill.textContent = refillAmount === "—" && refillTime === "—"
    ? "—"
    : `${refillAmount} · ${refillTime}`;
  if (typeof diagnostics.latest_refill_id === "string" && diagnostics.latest_refill_id) {
    meteredQuotaLatestRefill.title = `Refill ${diagnostics.latest_refill_id}`;
  }

  const stale = diagnostics.cache_state === "stale";
  setMeteredQuotaBadge(stale ? "unknown" : "ok", stale ? "Stale cache" : unlimited ? "Unlimited" : "Available");
};

const applyDefaultsSnapshot = (snapshot, defaults, options = {}) => {
  if (!Array.isArray(snapshot?.models) || !snapshot.models.length) return null;
  const models = snapshot.models;
  defaultsModelMap = new Map(models.map((model) => [model.slug, model]));
  if (snapshot.meta) updateDefaultsMeta(snapshot.meta, models);
  const modelOptions = models.map((model) => ({ value: model.slug, label: formatModelLabel(model) }));
  const preferredModel = options.preserveInputs ? defaultsModelSelect.value : defaults?.model ?? "";
  const selectedModel = setSelectOptions(defaultsModelSelect, modelOptions, preferredModel, "No models available");
  const preferredReasoning = options.preserveInputs ? defaultsReasoningSelect.value : defaults?.reasoning_effort ?? "";
  const selectedReasoning = updateReasoningOptions(selectedModel, preferredReasoning);
  if (!options.preserveInputs) {
    if (typeof defaults?.kernel_policy_limit_requests === "number") {
      defaultsKernelLimitInput.value = String(Math.trunc(defaults.kernel_policy_limit_requests));
    }
    if (typeof defaults?.kernel_policy_window_ms === "number") {
      defaultsKernelWindowInput.value = String(Math.trunc(defaults.kernel_policy_window_ms));
    }
  }
  return { selectedModel, selectedReasoning };
};

const setSelectOptions = (select, options, selected, emptyLabel) => {
  select.textContent = "";
  if (!options.length) {
    const opt = document.createElement("option");
    opt.value = "";
    opt.textContent = emptyLabel;
    select.appendChild(opt);
    select.disabled = true;
    return "";
  }
  select.disabled = false;
  options.forEach((option) => {
    const opt = document.createElement("option");
    opt.value = option.value;
    opt.textContent = option.label;
    select.appendChild(opt);
  });
  const next = options.some((option) => option.value === selected) ? selected : options[0].value;
  select.value = next;
  return next;
};

const updateDefaultsMeta = () => {};

const updateReasoningOptions = (modelSlug, preferred) => {
  const model = defaultsModelMap.get(modelSlug);
  return updateReasoningSelectForModel(defaultsReasoningSelect, model, preferred);
};

const loadDefaults = async (options = {}) => {
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setDefaultsBadge("bad", "Missing token");
    clearMeteredQuotaDiagnostics();
    setMeteredQuotaBadge("unknown", "Not loaded");
    return;
  }

  const loadId = ++defaultsLoadId;
  const preserveInputs = options.preserveInputs === true;
  if (!preserveInputs) defaultsTouched = false;
  defaultsLoaded = false;
  setDefaultsBadge("unknown", defaultsLoadedAt ? "Cached · refreshing" : "Loading...");
  setMeteredQuotaBadge("unknown", defaultsLoadedAt ? "Cached · refreshing" : "Loading...");
  try {
    const [modelsRes, defaultsRes] = await Promise.all([
      fetch(apiUrl("/admin/codex/models"), {
        headers: { Authorization: `Bearer ${token}` },
        cache: "no-store",
      }),
      fetch(apiUrl("/admin/defaults"), {
        headers: { Authorization: `Bearer ${token}` },
        cache: "no-store",
      }),
    ]);
    if (loadId !== defaultsLoadId) return;
    const modelsPayload = await modelsRes.json().catch(() => null);
    if (!modelsRes.ok) {
      if (defaultsLoadedAt) {
        defaultsLoaded = true;
        setDefaultsBadge("unknown", "Cached · refresh unavailable");
        setMeteredQuotaBadge("unknown", "Cached · refresh unavailable");
        return;
      }
      setDefaultsBadge("bad", modelsPayload?.error?.message ?? "Error");
      return;
    }
    const snapshot = modelsPayload?.data ?? null;
    const models = Array.isArray(snapshot?.models)
      ? snapshot.models.filter((model) => typeof model?.slug === "string")
      : [];
    defaultsModelMap = new Map(models.map((model) => [model.slug, model]));
    updateDefaultsMeta(snapshot, models);

    const defaultsPayload = await defaultsRes.json().catch(() => null);
    if (!defaultsRes.ok) {
      if (defaultsLoadedAt) {
        defaultsLoaded = true;
        setDefaultsBadge("unknown", "Cached · refresh unavailable");
        setMeteredQuotaBadge("unknown", "Cached · refresh unavailable");
        return;
      }
      setDefaultsBadge("bad", defaultsPayload?.error?.message ?? "Error");
      renderMeteredQuotaDiagnostics(null);
      return;
    }
    renderMeteredQuotaDiagnostics(defaultsPayload?.metered_quota);

    if (!models.length) {
      setDefaultsBadge("bad", "No models");
      setSelectOptions(defaultsModelSelect, [], "", "No models available");
      setReasoningPlaceholder(defaultsReasoningSelect, "No reasoning levels");
      return;
    }

    const serverDefaults = {
      model: typeof defaultsPayload?.defaults?.model === "string" ? defaultsPayload.defaults.model : "",
      reasoning_effort: typeof defaultsPayload?.defaults?.reasoning_effort === "string"
        ? defaultsPayload.defaults.reasoning_effort
        : "",
      kernel_policy_limit_requests: typeof defaultsPayload?.defaults?.kernel_policy_limit_requests === "number"
        ? Math.trunc(defaultsPayload.defaults.kernel_policy_limit_requests)
        : DEFAULT_KERNEL_POLICY_LIMIT,
      kernel_policy_window_ms: typeof defaultsPayload?.defaults?.kernel_policy_window_ms === "number"
        ? Math.trunc(defaultsPayload.defaults.kernel_policy_window_ms)
        : DEFAULT_KERNEL_POLICY_WINDOW_MS,
    };
    const result = applyDefaultsSnapshot(
      { models, meta: snapshot },
      serverDefaults,
      { preserveInputs: preserveInputs || defaultsTouched },
    );
    defaultsLoaded = true;
    defaultsLoadedAt = Date.now();
    if (!defaultsTouched && result) {
      setDefaultsBadge("ok", `${result.selectedModel} · ${result.selectedReasoning}`);
    }
  } catch {
    if (defaultsLoadedAt) {
      defaultsLoaded = true;
      setDefaultsBadge("unknown", "Cached · offline");
      setMeteredQuotaBadge("unknown", "Cached · offline");
      return;
    }
    setDefaultsBadge("bad", "Offline");
    renderMeteredQuotaDiagnostics(null);
  }
};

const saveDefaults = async () => {
  if (!defaultsLoaded) return;
  const token = getAdminToken();
  if (!token && !hasAdminCredential()) {
    setDefaultsBadge("bad", "Missing token");
    return;
  }

  if (defaultsSaving) return;
  defaultsSaving = true;
  const model = defaultsModelSelect.value;
  const reasoning = defaultsReasoningSelect.value;
  const limitValue = parseKernelLimitValue(defaultsKernelLimitInput.value, setDefaultsBadge);
  if (limitValue === null) {
    defaultsSaving = false;
    return;
  }
  const windowResult = parseKernelWindowValue(defaultsKernelWindowInput.value, setDefaultsBadge);
  if (!windowResult.ok || windowResult.value === null) {
    setDefaultsBadge("bad", "Window required");
    defaultsSaving = false;
    return;
  }
  setDefaultsBadge("unknown", "Saving...");

  try {
    const res = await fetch(apiUrl("/admin/defaults"), {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${token}`,
      },
      body: JSON.stringify({
        model,
        reasoning_effort: reasoning,
        kernel_policy_limit_requests: limitValue,
        kernel_policy_window_ms: windowResult.value,
      }),
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      setDefaultsBadge("bad", data?.error?.message ?? "Error");
      return;
    }
    const saved = data?.defaults;
    const summary = saved?.model && saved?.reasoning_effort ? `${saved.model} · ${saved.reasoning_effort}` : "Saved";
    if (typeof saved?.kernel_policy_limit_requests === "number") {
      defaultsKernelLimitInput.value = String(Math.trunc(saved.kernel_policy_limit_requests));
    }
    if (typeof saved?.kernel_policy_window_ms === "number") {
      defaultsKernelWindowInput.value = String(Math.trunc(saved.kernel_policy_window_ms));
    }
    defaultsTouched = false;
    setDefaultsBadge("ok", summary);
  } catch {
    setDefaultsBadge("bad", "Offline");
  } finally {
    defaultsSaving = false;
  }
};

const cachedPayload = (snapshot) => snapshot?.payload && typeof snapshot.payload === "object" ? snapshot.payload : null;

const cachedList = (snapshot, key = "data") => {
  const payload = cachedPayload(snapshot);
  return Array.isArray(payload?.[key]) ? payload[key] : null;
};

const cachedKernelResult = (snapshot, key) => {
  const records = cachedList(snapshot, key);
  return records
    ? { ok: true, message: "", records }
    : { ok: false, message: "Cached response unavailable", records: [] };
};

const hydrateAdminSnapshots = async () => {
  const scope = adminCacheScope;
  const epoch = adminCacheEpoch;
  if (!scope || !adminAccessState.isAdmin) return;

  const paths = [
    "/admin/api-keys?include_usage=1",
    "/admin/passkey-users",
    "/admin/kernel-policy-queue",
    "/admin/kernel-pubkeys",
    "/admin/kernel-usage?scope=org&list=1&inventory=1",
    "/admin/kernel-usage?scope=repo&list=1&inventory=1",
    "/admin/kernel-usage?scope=org&list=1",
    "/admin/kernel-usage?scope=repo&list=1",
    "/admin/codex/models",
    "/admin/defaults",
    "/health/providers",
    "/admin/providers",
    "/admin/providers/capacity",
    "/admin/errors?limit=1",
    "/admin/providers/quota-projection?window_days=30&balance_window_days=365",
    "/admin/errors?limit=200",
  ];
  const snapshots = await Promise.all(paths.map((path) => readAdminSnapshot(path)));
  if (scope !== adminCacheScope || epoch !== adminCacheEpoch || !adminAccessState.isAdmin) return;

  const [
    keysSnapshot,
    usersSnapshot,
    queueSnapshot,
    pubkeysSnapshot,
    orgUsageSnapshot,
    repoUsageSnapshot,
    orgPolicySnapshot,
    repoPolicySnapshot,
    modelsSnapshot,
    defaultsSnapshot,
    upstreamSnapshot,
    providersSnapshot,
    capacitySnapshot,
    capacityErrorsSnapshot,
    quotaProjectionSnapshot,
    errorsSnapshot,
  ] = snapshots;

  const keys = cachedList(keysSnapshot);
  if (keys && keysLoadedAt <= keysSnapshot.savedAt) {
    allKeys = keys;
    keysLoadedAt = keysSnapshot.savedAt;
    renderKeys(allKeys, currentKeyView);
    setKeysBadge("unknown", "Cached · refreshing");
  }

  const users = cachedList(usersSnapshot);
  if (adminAccessState.isSuperAdmin && users && passkeyUsersLoadedAt <= usersSnapshot.savedAt) {
    passkeyUsers = users;
    passkeyUsersLoadedAt = usersSnapshot.savedAt;
    renderPasskeyUsers(passkeyUsers);
    setPasskeyUsersBadge("unknown", "Cached · refreshing");
  }

  const queue = cachedList(queueSnapshot);
  if (queue && kernelQueueLoadedAt <= queueSnapshot.savedAt) {
    kernelQueueItems = queue;
    kernelQueueLoadedAt = queueSnapshot.savedAt;
    renderKernelPolicyQueue(kernelQueueItems);
    setKernelQueueBadge("unknown", "Cached · refreshing");
  }

  const pubkeys = cachedList(pubkeysSnapshot);
  if (pubkeys && kernelPubKeysLoadedAt <= pubkeysSnapshot.savedAt) {
    kernelPubKeys = pubkeys;
    kernelPubKeysLoadedAt = pubkeysSnapshot.savedAt;
    renderKernelPubKeys(kernelPubKeys);
    setKernelPubKeysBadge("unknown", "Cached · refreshing");
  }

  const cachedKernelResults = [
    cachedKernelResult(orgUsageSnapshot, "usage"),
    cachedKernelResult(repoUsageSnapshot, "usage"),
    cachedKernelResult(orgPolicySnapshot, "limits"),
    cachedKernelResult(repoPolicySnapshot, "limits"),
  ];
  if (cachedKernelResults.every((result) => result.ok)) {
    const [orgUsageResult, repoUsageResult, orgPolicyResult, repoPolicyResult] = cachedKernelResults;
    const cachedAt = Math.min(
      orgUsageSnapshot.savedAt,
      repoUsageSnapshot.savedAt,
      orgPolicySnapshot.savedAt,
      repoPolicySnapshot.savedAt,
    );
    if (kernelListLoadedAt <= cachedAt) {
      kernelPolicyState = buildKernelPolicyStateFromLists(orgPolicyResult, repoPolicyResult);
      kernelListRecords = {
        org: mergeKernelRecords(orgUsageResult.records, orgPolicyResult.records, "org"),
        repo: mergeKernelRecords(repoUsageResult.records, repoPolicyResult.records, "repo"),
      };
      const result = renderKernelList(kernelListRecords, kernelPolicyState);
      kernelListLoadedAt = cachedAt;
      setKernelListBadge("unknown", `Cached · ${formatKernelListBadge(result)}`);
    }
  }

  const modelsPayload = cachedPayload(modelsSnapshot);
  const defaultsPayload = cachedPayload(defaultsSnapshot);
  const models = Array.isArray(modelsPayload?.data?.models)
    ? modelsPayload.data.models.filter((model) => typeof model?.slug === "string")
    : [];
  if (
    models.length && defaultsPayload && defaultsLoadedAt <= Math.min(modelsSnapshot.savedAt, defaultsSnapshot.savedAt)
  ) {
    const serverDefaults = {
      model: typeof defaultsPayload?.defaults?.model === "string" ? defaultsPayload.defaults.model : "",
      reasoning_effort: typeof defaultsPayload?.defaults?.reasoning_effort === "string"
        ? defaultsPayload.defaults.reasoning_effort
        : "",
      kernel_policy_limit_requests: typeof defaultsPayload?.defaults?.kernel_policy_limit_requests === "number"
        ? Math.trunc(defaultsPayload.defaults.kernel_policy_limit_requests)
        : DEFAULT_KERNEL_POLICY_LIMIT,
      kernel_policy_window_ms: typeof defaultsPayload?.defaults?.kernel_policy_window_ms === "number"
        ? Math.trunc(defaultsPayload.defaults.kernel_policy_window_ms)
        : DEFAULT_KERNEL_POLICY_WINDOW_MS,
    };
    const result = applyDefaultsSnapshot({ models, meta: modelsPayload.data }, serverDefaults, {
      preserveInputs: defaultsTouched,
    });
    renderMeteredQuotaDiagnostics(defaultsPayload?.metered_quota);
    defaultsLoaded = true;
    defaultsLoadedAt = Math.min(modelsSnapshot.savedAt, defaultsSnapshot.savedAt);
    if (result && !defaultsTouched) setDefaultsBadge("unknown", "Cached · refreshing");
  }

  const upstreamPayload = cachedPayload(upstreamSnapshot);
  if (upstreamPayload && accessUpstreamLoadedAt <= upstreamSnapshot.savedAt) {
    const source = upstreamPayload?.codex?.source ?? "unknown";
    const expirations = Array.isArray(upstreamPayload?.codex?.accounts)
      ? upstreamPayload.codex.accounts
        .map((account) => account?.access_token_exp_ms)
        .filter((value) => typeof value === "number")
      : [];
    setAccessValue(accessUpstreamSource, source === "none" ? "None" : source);
    setAccessValue(accessUpstreamExpiry, expirations.length ? formatDate(Math.min(...expirations)) : "Unknown");
    accessUpstreamLoadedAt = upstreamSnapshot.savedAt;
  }

  const providersPayload = cachedPayload(providersSnapshot);
  if (providersPayload && providersLoadedAt <= providersSnapshot.savedAt) {
    latestProviderHealth = providersPayload;
    providersLoadedAt = providersSnapshot.savedAt;
  }

  const capacityPayload = cachedPayload(capacitySnapshot);
  const capacityErrorsPayload = cachedPayload(capacityErrorsSnapshot);
  if (capacityPayload && providerCapacityLoadedAt <= capacitySnapshot.savedAt) {
    renderProviderCapacity(
      capacityPayload,
      Array.isArray(capacityErrorsPayload?.five_xx_buckets) ? capacityErrorsPayload.five_xx_buckets : [],
    );
    providerCapacityLoadedAt = capacitySnapshot.savedAt;
    providerCapacityUpdated.textContent = `Cached ${formatDate(capacitySnapshot.savedAt)} · refreshing`;
    setBadge(providerCapacityBadge, "unknown", "Cached · refreshing");
  } else if (latestProviderCapacityChartState?.sources) {
    renderProviderCapacityList(latestProviderCapacityChartState.sources);
  }

  const quotaProjectionPayload = cachedPayload(quotaProjectionSnapshot);
  if (quotaProjectionPayload && quotaProjectionLoadedAt <= quotaProjectionSnapshot.savedAt) {
    renderQuotaProjection(quotaProjectionPayload);
    quotaProjectionLoadedAt = quotaProjectionSnapshot.savedAt;
    quotaRunwayUpdated.textContent = `Cached ${formatDate(quotaProjectionSnapshot.savedAt)} · refreshing`;
    setBadge(quotaRunwayBadge, "unknown", "Cached · refreshing");
  }

  const errors = cachedList(errorsSnapshot);
  if (errors && errorsLoadedAt <= errorsSnapshot.savedAt) {
    renderAdminErrors(errors);
    errorsUpdated.textContent = `Cached ${formatDate(errorsSnapshot.savedAt)} · refreshing`;
    setBadge(errorsBadge, "unknown", "Cached · refreshing");
    errorsLoadedAt = errorsSnapshot.savedAt;
  }

  updateAccessApiKeysSummary();
  updateAccessGithubSummary();
  updateAccessPubkeysSummary();
};

const scheduleDefaultsSave = debounce(() => {
  void saveDefaults();
}, 500);

restoreSettings();
syncCreatePaidFallbackControls();
setAuthBadge("unknown", "Not checked");
setSignedInState(false);
setCreateBadge("unknown", "Idle");
setKeysBadge("unknown", "Not loaded");
setPasskeyUsersBadge("unknown", "Not loaded");
setBadge(errorsBadge, "unknown", "Not loaded");
setErrorsMessage("Sign in to load gateway errors.");
setDefaultsBadge("unknown", "Idle");
clearMeteredQuotaDiagnostics();
setMeteredQuotaBadge("unknown", "Idle");
setKernelListBadge("unknown", "Not loaded");
setKernelNewBadge("unknown", "Idle");
setKernelQueueBadge("unknown", "Not loaded");
setKernelPubKeysBadge("unknown", "Not loaded");
setKernelPubKeyCreateBadge("unknown", "Idle");
setKeyListMessage("Paste an admin token to load API keys.");
setPasskeyUsersMessage("Paste a fallback admin token to manage passkey users.");
setKernelListMessage(getKernelListMissingTokenMessage());
setKernelQueueMessage(getKernelQueueMissingTokenMessage());
setKernelPubKeysMessage("Paste an admin token to load kernel attestation keys.");
kernelFilterInput.value = "";
kernelShowSelect.value = "all";
switchKeysView("active");
setKernelNewPanelOpen(false);
resetKernelPubKeyForm();
const initialHashView = getHashView();
if (initialHashView === "session") setAuthWidgetOpen(true);
resetAdminPrefetchState("Checking admin session...");
setAdminView(ADMIN_VIEW_DEFAULT, { allowInaccessible: true });
if (initialHashView && initialHashView !== "session") setAdminView(initialHashView, { focusAuth: false });
setAuthBadge("unknown", "Checking...");
setAdminAccessState({ checked: false, isAdmin: false, isSuperAdmin: false });
scheduleTokenCheck();

bindForegroundRefresh(() => {
  if (currentAdminView !== "defaults" || !adminAccessState.isAdmin || !hasAdminCredential()) return;
  void loadDefaults({ preserveInputs: true });
});

authWidgetToggle.addEventListener("click", () => {
  setAuthWidgetOpen(authWidgetPanel.hidden, { focus: authWidgetPanel.hidden });
});

authGateOpen.addEventListener("click", () => {
  setAuthWidgetOpen(true, { focus: true });
});

authWidgetClose.addEventListener("click", () => {
  setAuthWidgetOpen(false);
  authWidgetToggle.focus();
});

showTokenInput.addEventListener("change", () => {
  tokenInput.type = showTokenInput.checked ? "text" : "password";
});

tokenInput.addEventListener("input", () => {
  clearAdminSnapshotScope();
  if (localDevelopmentAutoAuth && tokenInput.value.trim() !== LOCAL_DEVELOPMENT_ADMIN_TOKEN) {
    localDevelopmentAutoAuth = false;
  }
  persistTokenIfEnabled();
  invalidateAdminErrors("Sign in to load gateway errors.");
  keysLoadedAt = 0;
  passkeyUsersLoadedAt = 0;
  defaultsLoaded = false;
  defaultsLoadedAt = 0;
  kernelListLoadedAt = 0;
  kernelQueueLoadedAt = 0;
  kernelPubKeysLoadedAt = 0;
  accessUpstreamLoadedAt = 0;
  providersLoadId += 1;
  providersLoading = false;
  providersLoadedAt = 0;
  providerCapacityLoadedForOpen = false;
  providerCapacityLoadedAt = 0;
  quotaProjectionLoadedForOpen = false;
  quotaProjectionLoading = false;
  quotaProjectionLoadedAt = 0;
  quotaProjectionLoadId += 1;
  quotaRunwayBadge.setAttribute("data-state", "unknown");
  quotaRunwayBadge.textContent = "Not loaded";
  quotaRunwayUpdated.textContent = "Waiting for projection";
  quotaRunwaySummary.replaceChildren();
  quotaRunwayList.replaceChildren();
  quotaRunwayNote.textContent = "";
  latestProviderCapacityChartState = null;
  latestProviderHealth = null;
  providerCapacityChart.replaceChildren();
  clearApiKeyRequestLogCaches();
  if (!hasAdminCredential()) {
    setAuthBadge("bad", "Missing token");
    setAdminAccessState({ checked: true, isAdmin: false, isSuperAdmin: false });
    setSignedInState(false);
    setKeysBadge("unknown", "Not loaded");
    setKeyListMessage("Paste an admin token to load API keys.");
    setPasskeyUsersBadge("unknown", "Not loaded");
    setPasskeyUsersMessage("Paste a fallback admin token to manage passkey users.");
    setKernelListBadge("unknown", "Not loaded");
    setKernelListMessage(getKernelListMissingTokenMessage());
    setKernelQueueBadge("unknown", "Not loaded");
    setKernelQueueMessage(getKernelQueueMissingTokenMessage());
    setKernelPubKeysBadge("unknown", "Not loaded");
    setKernelPubKeysMessage("Paste an admin token to load kernel attestation keys.");
    allKeys = [];
    passkeyUsers = [];
    kernelQueueItems = [];
    kernelPubKeys = [];
    kernelPubKeysLoadedAt = 0;
    keysLoadedAt = 0;
    resetAdminPrefetchState("Sign in to prepare the admin views.");
  } else {
    setAuthBadge("unknown", "Checking...");
    setAdminAccessState({ checked: false, isAdmin: false, isSuperAdmin: false });
    resetAdminPrefetchState("Checking admin session...");
  }
  scheduleTokenCheck();
  if (currentAdminView === "keys") {
    void ensureKeysLoaded();
  }
  if (currentAdminView === "users") {
    void ensurePasskeyUsersLoaded();
  }
  if (currentAdminView === "defaults") {
    void loadDefaults();
  }
  if (currentAdminView === "kernel") {
    void loadKernelList();
    void ensureKernelPolicyQueueLoaded();
    void refreshAccessOverview();
  }
  if (currentAdminView === "pubkeys") {
    void ensureKernelPubKeysLoaded();
  }
});

rememberTokenInput.addEventListener("change", () => {
  if (rememberTokenInput.checked) {
    storage.set(STORAGE_KEYS.rememberToken, "1");
    persistTokenIfEnabled();
    return;
  }
  storage.remove(STORAGE_KEYS.rememberToken);
  storage.remove(STORAGE_KEYS.token);
  setSignedInState(hasAdminCredential());
});

passkeyHandleInput.addEventListener("input", () => {
  schedulePasskeyHandlePersist();
});

passkeyLoginBtn.addEventListener("click", () => {
  void runPasskeyLogin();
});

passkeyRegisterBtn.addEventListener("click", async () => {
  const passkeyBaseUrl = getPasskeyBaseUrl();
  setPasskeyStatus("unknown", "Registering...");
  passkeyLoginBtn.disabled = true;
  passkeyRegisterBtn.disabled = true;
  try {
    const registrationToken = await getRegistrationAdminToken();
    const result = await registerPasskey({
      handle: passkeyHandleInput.value,
      token: registrationToken,
      baseUrl: passkeyBaseUrl,
    });
    if (result.handle) setPasskeyHandleValue(result.handle);
    if (isRemoteRelayOrigin() && relaySessionActive) {
      setSignedInState(true, { canRegisterPasskey: true, deviceRegistered: true });
    } else {
      applySignedInToken(result.token, { deviceRegistered: true });
    }
    setPasskeyStatus("ok", "Passkey registered");
    postAuthRelayResult(result);
  } catch (error) {
    setPasskeyStatus("bad", error?.message ?? "Passkey registration failed");
  } finally {
    passkeyLoginBtn.disabled = false;
    passkeyRegisterBtn.disabled = false;
  }
});

signOutBtn.addEventListener("click", async () => {
  const token = getAdminToken();
  signOutBtn.disabled = true;
  try {
    await signOut({
      token: isRemoteRelayOrigin() && relaySessionActive ? "" : token,
      baseUrl: resolveBaseUrl(),
      corsOrigin: isRemoteRelayOrigin() ? globalThis.location.origin : "",
    });
  } finally {
    relaySessionActive = false;
    tokenInput.value = "";
    rememberTokenInput.checked = false;
    setAuthBadge("bad", "Missing token");
    setSignedInState(false);
    tokenInput.dispatchEvent(new Event("input", { bubbles: true }));
    signOutBtn.disabled = false;
  }
});

globalThis.addEventListener("storage", (event) => {
  if (event.key === STORAGE_KEYS.rememberToken) {
    rememberTokenInput.checked = event.newValue === "1";
    return;
  }
  if (event.key === STORAGE_KEYS.passkeyHandle) {
    passkeyHandleInput.value = event.newValue ?? "";
    return;
  }
  if (event.key === STORAGE_KEYS.passkeyCredentialIds) {
    setSignedInState(hasAdminCredential());
    return;
  }
  if (event.key !== STORAGE_KEYS.token) return;
  if (event.newValue === null) {
    tokenInput.value = "";
    setAuthBadge("bad", "Missing token");
    setAdminAccessState({ checked: true, isAdmin: false, isSuperAdmin: false });
    setSignedInState(false);
    tokenInput.dispatchEvent(new Event("input", { bubbles: true }));
    return;
  }
  if (!rememberTokenInput.checked) return;
  tokenInput.value = event.newValue ?? "";
  setAdminAccessState({ checked: false, isAdmin: false, isSuperAdmin: false });
  tokenInput.dispatchEvent(new Event("input", { bubbles: true }));
});

globalThis.addEventListener("hashchange", () => {
  const hashView = getHashView();
  if (!hashView) return;
  if (hashView === "session") {
    setAuthWidgetOpen(true, { focus: true });
    return;
  }
  setAdminView(hashView, { focusAuth: true });
});

baseSelect.addEventListener("change", () => {
  clearAdminSnapshotScope();
  storage.set(STORAGE_KEYS.base, getBaseChoice());
  if (canUseLocalDevelopmentAuth()) applyLocalDevelopmentAuth();
  else clearLocalDevelopmentAuth();
  updateBasePreview();
  setAuthBadge("unknown", "Not checked");
  setAdminAccessState({ checked: false, isAdmin: false, isSuperAdmin: false });
  setSignedInState(false);
  setCreateBadge("unknown", "Idle");
  setKeysBadge("unknown", "Not loaded");
  setPasskeyUsersBadge("unknown", "Not loaded");
  invalidateAdminErrors("Target changed. Sign in to load gateway errors.");
  setDefaultsBadge("unknown", "Idle");
  setKernelListBadge("unknown", "Not loaded");
  setKernelNewBadge("unknown", "Idle");
  setKernelQueueBadge("unknown", "Not loaded");
  setKernelPubKeysBadge("unknown", "Not loaded");
  setKernelPubKeyCreateBadge("unknown", "Idle");
  setKeyListMessage("Target changed. Loading API keys...");
  setPasskeyUsersMessage("Target changed. Loading passkey users...");
  setKernelListMessage(getKernelListTargetChangedMessage());
  setKernelQueueMessage(getKernelQueueTargetChangedMessage());
  setKernelPubKeysMessage("Target changed. Loading kernel attestation keys...");
  clearCreateResult();
  setKernelNewPanelOpen(false);
  resetKernelPubKeyForm();
  allKeys = [];
  keysLoadedAt = 0;
  passkeyUsers = [];
  passkeyUsersLoadedAt = 0;
  defaultsLoaded = false;
  defaultsLoadedAt = 0;
  defaultsLoadId += 1;
  kernelListLoadedAt = 0;
  kernelListLoadId += 1;
  kernelQueueItems = [];
  kernelQueueLoadedAt = 0;
  kernelPubKeys = [];
  kernelPubKeysLoadedAt = 0;
  accessUpstreamLoadedAt = 0;
  providersLoadId += 1;
  providersLoading = false;
  providersLoadedAt = 0;
  providerCapacityLoadedForOpen = false;
  providerCapacityLoadedAt = 0;
  quotaProjectionLoadedForOpen = false;
  quotaProjectionLoading = false;
  quotaProjectionLoadedAt = 0;
  quotaProjectionLoadId += 1;
  quotaRunwayBadge.setAttribute("data-state", "unknown");
  quotaRunwayBadge.textContent = "Not loaded";
  quotaRunwayUpdated.textContent = "Waiting for projection";
  quotaRunwaySummary.replaceChildren();
  quotaRunwayList.replaceChildren();
  quotaRunwayNote.textContent = "";
  latestProviderCapacityChartState = null;
  latestProviderHealth = null;
  providerCapacityChart.replaceChildren();
  resetAdminPrefetchState(hasAdminCredential() ? "Checking admin session..." : "Sign in to prepare the admin views.");
  scheduleTokenCheck();
  if (currentAdminView === "keys") {
    void ensureKeysLoaded();
  }
  if (currentAdminView === "users") {
    void ensurePasskeyUsersLoaded();
  }
  if (currentAdminView === "defaults") {
    void loadDefaults();
  }
  if (currentAdminView === "kernel") {
    void loadKernelList();
    void ensureKernelPolicyQueueLoaded();
    void refreshAccessOverview();
  }
  if (currentAdminView === "pubkeys") {
    void ensureKernelPubKeysLoaded();
  }
});

keyExpiresSelect.addEventListener("change", () => {
  storage.set(STORAGE_KEYS.expiresPreset, keyExpiresSelect.value);
});
keyWindowPreset1m.addEventListener("click", () => {
  keyUsageWindowInput.value = "60000";
});
keyWindowPreset1h.addEventListener("click", () => {
  keyUsageWindowInput.value = "3600000";
});
keyWindowPreset1d.addEventListener("click", () => {
  keyUsageWindowInput.value = "86400000";
});
keyWindowPreset1w.addEventListener("click", () => {
  keyUsageWindowInput.value = "604800000";
});
keyPaidFallbackEnabledInput.addEventListener("change", () => {
  syncCreatePaidFallbackControls();
  setCreateBadge("unknown", "Editing...");
  if (keyPaidFallbackEnabledInput.checked) keyPaidFallbackLimitInput.focus();
});

viewTabKeys.addEventListener("click", () => setAdminView("keys", { hashMode: "push", focusAuth: true }));
viewTabUsers.addEventListener("click", () => setAdminView("users", { hashMode: "push", focusAuth: true }));
viewTabKernel.addEventListener("click", () => setAdminView("kernel", { hashMode: "push", focusAuth: true }));
viewTabPubkeys.addEventListener("click", () => setAdminView("pubkeys", { hashMode: "push", focusAuth: true }));
viewTabDefaults.addEventListener("click", () => setAdminView("defaults", { hashMode: "push", focusAuth: true }));
viewTabProviders.addEventListener("click", () => setAdminView("providers", { hashMode: "push", focusAuth: true }));
viewTabErrors.addEventListener("click", () => setAdminView("errors", { hashMode: "push", focusAuth: true }));
bindTablistKeyboard(viewTabKeys.closest('[role="tablist"]'));
bindTablistKeyboard(keysTabActive.closest('[role="tablist"]'));

globalThis.setInterval(() => {
  if (currentAdminView !== "providers" || document.visibilityState !== "visible") return;
  void loadProviders();
  void loadProviderCapacity();
  void loadQuotaProjection();
}, 30_000);

createKeyBtn.addEventListener("click", () => {
  void createKey();
});

kernelNewToggle.addEventListener("click", () => {
  const nextOpen = kernelNewPanel.hidden;
  setKernelNewPanelOpen(nextOpen);
  if (nextOpen) kernelNewOwnerInput.focus();
});

kernelNewPreset1m.addEventListener("click", () => {
  setKernelNewWindowPreset(60000);
  setKernelNewBadge("unknown", "Editing...");
});
kernelNewPreset1h.addEventListener("click", () => {
  setKernelNewWindowPreset(3600000);
  setKernelNewBadge("unknown", "Editing...");
});
kernelNewPreset1d.addEventListener("click", () => {
  setKernelNewWindowPreset(86400000);
  setKernelNewBadge("unknown", "Editing...");
});
kernelNewPreset1w.addEventListener("click", () => {
  setKernelNewWindowPreset(604800000);
  setKernelNewBadge("unknown", "Editing...");
});

kernelNewSaveBtn.addEventListener("click", () => {
  void saveNewKernelLimit();
});

kernelNewOwnerInput.addEventListener("input", () => setKernelNewBadge("unknown", "Editing..."));
kernelNewRepoInput.addEventListener("input", () => setKernelNewBadge("unknown", "Editing..."));
kernelNewLimitInput.addEventListener("input", () => setKernelNewBadge("unknown", "Editing..."));
kernelNewWindowInput.addEventListener("input", () => setKernelNewBadge("unknown", "Editing..."));
kernelNewExpiresInput.addEventListener("input", () => setKernelNewBadge("unknown", "Editing..."));
kernelNewNeverInput.addEventListener("change", () => {
  kernelNewExpiresInput.disabled = kernelNewNeverInput.checked;
  if (kernelNewNeverInput.checked) kernelNewExpiresInput.value = "";
  setKernelNewBadge("unknown", "Editing...");
});

kernelFilterInput.addEventListener("input", () => {
  refreshKernelListDebounced();
});
kernelShowSelect.addEventListener("change", () => {
  refreshKernelList();
});
kernelSortSelect.addEventListener("change", () => {
  refreshKernelList();
});

kernelPubKeyCreateBtn.addEventListener("click", () => {
  void createKernelPubKey();
});
kernelPubKeyAppIdInput.addEventListener("input", () => setKernelPubKeyCreateBadge("unknown", "Editing..."));
kernelPubKeyOwnerInput.addEventListener("input", () => setKernelPubKeyCreateBadge("unknown", "Editing..."));
kernelPubKeyPemInput.addEventListener("input", () => setKernelPubKeyCreateBadge("unknown", "Editing..."));

keysTabAll.addEventListener("click", () => switchKeysView("all"));
keysTabActive.addEventListener("click", () => switchKeysView("active"));
keysTabRevoked.addEventListener("click", () => switchKeysView("revoked"));

defaultsModelSelect.addEventListener("change", () => {
  if (!defaultsLoaded) return;
  defaultsTouched = true;
  const model = defaultsModelSelect.value;
  updateReasoningOptions(model, defaultsReasoningSelect.value);
  scheduleDefaultsSave();
});

defaultsReasoningSelect.addEventListener("change", () => {
  if (!defaultsLoaded) return;
  defaultsTouched = true;
  scheduleDefaultsSave();
});

const markDefaultsEditing = () => {
  if (!defaultsLoaded) return;
  defaultsTouched = true;
  setDefaultsBadge("unknown", "Editing...");
  scheduleDefaultsSave();
};

defaultsKernelLimitInput.addEventListener("input", () => {
  markDefaultsEditing();
});

defaultsKernelWindowInput.addEventListener("input", () => {
  markDefaultsEditing();
});

defaultsKernelWindowPreset1m.addEventListener("click", () => {
  if (!defaultsLoaded) return;
  defaultsKernelWindowInput.value = "60000";
  markDefaultsEditing();
});
defaultsKernelWindowPreset1h.addEventListener("click", () => {
  if (!defaultsLoaded) return;
  defaultsKernelWindowInput.value = "3600000";
  markDefaultsEditing();
});
defaultsKernelWindowPreset1d.addEventListener("click", () => {
  if (!defaultsLoaded) return;
  defaultsKernelWindowInput.value = "86400000";
  markDefaultsEditing();
});
defaultsKernelWindowPreset1w.addEventListener("click", () => {
  if (!defaultsLoaded) return;
  defaultsKernelWindowInput.value = "604800000";
  markDefaultsEditing();
});

const startAuthRelayIfRequested = async () => {
  if (!isAuthRelayMode) return;
  setAuthWidgetOpen(true);
  passkeyRegisterBtn.hidden = true;
  setAuthBadge("unknown", "Relay sign-in");
  setPasskeyStatus("unknown", "Starting passkey sign-in...");
  passkeyLoginBtn.disabled = true;
  try {
    await runPasskeyLogin({ automatic: true });
  } catch (error) {
    setPasskeyStatus("bad", `${formatPasskeyLoginError(error)} Click the sign-in button to continue.`);
  } finally {
    passkeyLoginBtn.disabled = false;
  }
};

void startAuthRelayIfRequested();
