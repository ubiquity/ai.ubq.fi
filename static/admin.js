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
} from "./auth.js?v=provider-capacity-20260805-consolidated";
import { AUTH_RELAY_MESSAGE_TYPE, parseAuthRelayAction, parseTrustedAuthRelayOrigin } from "./auth-relay.js";
import { bindForegroundRefresh } from "./foreground-refresh.js";
import { setReasoningPlaceholder, updateReasoningSelectForModel } from "./reasoning-select.js";

const STORAGE_KEYS = {
  rememberToken: AUTH_STORAGE_KEYS.rememberToken,
  token: AUTH_STORAGE_KEYS.token,
  passkeyHandle: AUTH_STORAGE_KEYS.passkeyHandle,
  passkeyCredentialIds: AUTH_STORAGE_KEYS.passkeyCredentialIds,
  expiresPreset: "uos_ai.admin.expires_preset",
  base: AUTH_STORAGE_KEYS.base,
  view: "uos_ai.admin.view",
  defaultsSnapshot: "uos_ai.admin.defaults_snapshot",
  defaultsModels: "uos_ai.admin.defaults_models",
};

const AUTH_RELAY_TIMEOUT_MS = 120_000;
const API_KEY_REQUEST_LOGS_LIMIT = 20;
const API_KEY_REQUEST_LOGS_TTL_MS = 10_000;

const readStorageJson = (key) => {
  const raw = storage.get(key);
  if (!raw) return null;
  try {
    return JSON.parse(raw);
  } catch {
    return null;
  }
};

const writeStorageJson = (key, value) => {
  try {
    storage.set(key, JSON.stringify(value));
  } catch {
    // ignore
  }
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
const authWidgetClose = mustGet("auth-widget-close");
const loadingSummary = mustGet("admin-loading-summary");
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

const viewLoading = mustGet("view-loading");
const viewKeys = mustGet("view-keys");
const viewUsers = mustGet("view-users");
const viewKernel = mustGet("view-kernel");
const viewPubkeys = mustGet("view-pubkeys");
const viewDefaults = mustGet("view-defaults");
const viewProviders = mustGet("view-providers");

const providerCapacityBadge = mustGet("provider-capacity-badge");
const providerCapacityUpdated = mustGet("provider-capacity-updated");
const providerCapacityChart = mustGet("provider-capacity-chart");
const providerCapacityList = mustGet("provider-capacity-list");

let currentKeyView = "active";
let currentAdminView = "loading";
let pendingAdminView = null;
let adminAccessState = { checked: false, isAdmin: false, isSuperAdmin: false };
let localDevelopmentAutoAuth = false;
let adminPrefetchRunId = 0;
let adminPrefetchSignature = "";
let adminPrefetchPromise = null;
let authWidgetAutoOpened = false;
let allKeys = [];
let keysLoading = false;
let keysLoadedAt = 0;
let providersLoading = false;
let providersLoadedAt = 0;
let providerCapacityLoading = false;
let providerCapacityLoadedForOpen = false;
let latestProviderCapacityChartState = null;
let capacityChartResizeFrame = 0;
let latestProviderHealth = null;
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
const yunwuQuotaBadge = mustGet("yunwu-quota-badge");
const yunwuQuotaRemaining = mustGet("yunwu-quota-remaining");
const yunwuQuotaProgress = mustGet("yunwu-quota-progress");
const yunwuQuotaBalance = mustGet("yunwu-quota-balance");
const yunwuQuotaBaseline = mustGet("yunwu-quota-baseline");
const yunwuQuotaLatestRefill = mustGet("yunwu-quota-latest-refill");
const yunwuQuotaInferredCredit = mustGet("yunwu-quota-inferred-credit");
const yunwuQuotaCache = mustGet("yunwu-quota-cache");
const yunwuQuotaConfidence = mustGet("yunwu-quota-confidence");
const yunwuQuotaObserved = mustGet("yunwu-quota-observed");
const yunwuQuotaCycleStarted = mustGet("yunwu-quota-cycle-started");
let defaultsLoaded = false;
let defaultsSaving = false;
let defaultsModelMap = new Map();
let defaultsTouched = false;
let defaultsLoadId = 0;

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
  if (expanded && options.focus) {
    globalThis.requestAnimationFrame(() => authWidgetPanel.focus({ preventScroll: true }));
  }
};

const setSignedInState = (signedIn, options = {}) => {
  const deviceRegistered = options.deviceRegistered ?? hasStoredPasskeyCredentials();
  const canRegisterPasskey = options.canRegisterPasskey ?? false;
  passkeyLoginBtn.hidden = signedIn;
  passkeyRegisterBtn.hidden = isAuthRelayMode || deviceRegistered || (signedIn && !canRegisterPasskey);
  signOutBtn.hidden = !signedIn;
  if (signedIn) setPasskeyStatus("ok", options.statusText ?? "Token active");
  else setPasskeyStatus("unknown", "Passkey idle");
};
const setCreateBadge = (state, text) => setBadge(createBadge, state, text);
const setKeysBadge = (state, text) => setBadge(keysBadge, state, text);
const setPasskeyUsersBadge = (state, text) => setBadge(passkeyUsersBadge, state, text);
const setDefaultsBadge = (state, text) => setBadge(defaultsBadge, state, text);
const setYunwuQuotaBadge = (state, text) => setBadge(yunwuQuotaBadge, state, text);
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

const apiUrl = (path) => buildBackendUrl(path, resolveBaseUrl());

const updateBasePreview = () => {
  basePreview.textContent = resolveBaseUrl();
};

const pageUrl = new URL(globalThis.location.href);
const authRelayOrigin = parseTrustedAuthRelayOrigin(pageUrl.searchParams.get("auth_relay_origin"));
const authRelayAction = parseAuthRelayAction(pageUrl.searchParams.get("auth_relay_action"));
const isAuthRelayMode = Boolean(authRelayOrigin && authRelayAction && globalThis.opener);

const getPasskeyBaseUrl = () => isAuthRelayMode ? globalThis.location.origin : resolveBaseUrl();

const isRemoteAiTarget = () => new URL(resolveBaseUrl()).origin === "https://ai.ubq.fi";

const isCrossOriginTarget = () => new URL(resolveBaseUrl()).origin !== globalThis.location.origin;

const getAdminToken = () => tokenInput.value.trim();

const canUseLocalDevelopmentAuth = () => isLocalDevelopmentOrigin() && getBaseChoice() === "local";

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
  if (!isAuthRelayMode || !result?.token) return false;
  globalThis.opener.postMessage({
    type: AUTH_RELAY_MESSAGE_TYPE,
    token: result.token,
    handle: result.handle ?? getPasskeyHandle(),
    expires_at_ms: result.expires_at_ms ?? null,
  }, authRelayOrigin);
  setPasskeyStatus("ok", "Signed in. Returning to local admin...");
  setTimeout(() => globalThis.close(), 300);
  return true;
};

const formatPasskeyLoginError = (error) => {
  const message = error?.message ?? "Passkey sign-in failed";
  if (
    !isAuthRelayMode && !isRemoteAiTarget() &&
    /invalid passkey assertion|unknown passkey|passkey account not found|no passkeys registered/i.test(message)
  ) {
    return `${message}. This origin uses localhost passkeys; choose ai.ubq.fi or add a local passkey with the fallback token.`;
  }
  return message;
};

const getValidCachedRelayAuth = async () => {
  const token = getAdminToken();
  if (!token) return null;
  try {
    const res = await fetch(apiUrl("/uos/auth"), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (!res.ok || !data?.auth?.is_admin) return null;
    return { token, handle: getPasskeyHandle() };
  } catch {
    return null;
  }
};

let authRelayRequest = null;
const requestRemotePasskeySession = () => {
  if (authRelayRequest) return authRelayRequest;
  const targetOrigin = new URL(resolveBaseUrl()).origin;
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
      if (!data || data.type !== AUTH_RELAY_MESSAGE_TYPE || typeof data.token !== "string") return;
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

const getRegistrationAdminToken = async () => {
  const token = getAdminToken();
  if (token || !isCrossOriginTarget() || !isRemoteAiTarget()) return token;
  setPasskeyStatus("unknown", "Sign in on ai.ubq.fi to authorize registration...");
  const relay = await requestRemotePasskeySession();
  if (relay.handle) setPasskeyHandleValue(relay.handle);
  applySignedInToken(relay.token, { deviceRegistered: true });
  return relay.token;
};

const restoreSettings = () => {
  const remember = storage.get(STORAGE_KEYS.rememberToken) === "1";
  rememberTokenInput.checked = remember;
  if (remember) tokenInput.value = storage.get(STORAGE_KEYS.token) ?? "";
  passkeyHandleInput.value = storage.get(STORAGE_KEYS.passkeyHandle) ?? "";
  keyExpiresSelect.value = storage.get(STORAGE_KEYS.expiresPreset) ?? "quarter";
  baseSelect.value = storage.get(STORAGE_KEYS.base) ?? "local";
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
const formatLatency = (value) => {
  if (typeof value !== "number" || !Number.isFinite(value) || value < 0) return "unknown";
  return `${numberFormatter.format(Math.trunc(value))} ms`;
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
  if (state === "stale") return "Stale";
  return "Unavailable";
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
    progress.style.setProperty("--capacity-progress", `${remainingPercent}%`);
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
  if (source.state !== "unavailable" && hasState) {
    badgeState = providerBadgeState(state);
    if (source.state === "stale" && badgeState === "ok") badgeState = "unknown";
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
  } else if (source.source === "yunwu") {
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
  title.textContent = `Codex account ${source.slot}`;
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

const renderYunwuCapacitySource = (source, provider = null) => {
  const row = document.createElement("article");
  row.dataset.capacitySource = "yunwu";
  row.dataset.state = source.state;
  row.setAttribute("role", "listitem");

  const header = document.createElement("header");
  const title = document.createElement("h3");
  title.textContent = "YunWu fallback";
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
    "Balance",
    wallet.balance_credits === null ? "Not available" : formatCredits(wallet.balance_credits),
  );
  appendProviderFact(facts, "Refill remaining", formatCapacityPercent(wallet.refill_cycle_remaining_percent));
  appendProviderFact(
    facts,
    "Refill baseline",
    wallet.baseline_credits === null ? "Not available" : formatCredits(wallet.baseline_credits),
  );
  appendProviderFact(facts, "Confidence", wallet.confidence ?? "Not available");
  appendProviderFact(facts, "Cycle started", formatCapacityTimestamp(wallet.cycle_started_at_ms));
  appendProviderFact(facts, "Reset", "Not provided for refill cycle");
  row.append(header, facts);
  appendCapacitySourceMeta(row, source, provider);
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
      source.source === "yunwu"
        ? renderYunwuCapacitySource(source, latestProviderHealth?.yunwu)
        : renderCodexCapacitySource(source, providerForCodexSlot(source.slot)),
    );
  }
};

const unavailableCapacitySource = (source, slot = null) =>
  source === "yunwu"
    ? {
      source: "yunwu",
      state: "unavailable",
      source_observed_at_ms: null,
      snapshot_at_ms: null,
      wallet: {
        balance_credits: null,
        baseline_credits: null,
        refill_cycle_remaining_percent: null,
        refill_cycle_used_percent: null,
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
const CAPACITY_CHART_DAY_MS = 24 * 60 * 60 * 1_000;
const CAPACITY_CHART_HOUR_MS = 60 * 60 * 1_000;
const CAPACITY_CHART_MINUTE_MS = 60 * 1_000;
const CAPACITY_CHART_PLOT_HEIGHT = 100;
const CAPACITY_CHART_PLOT_LEFT = 48;
const CAPACITY_CHART_PLOT_TOP = 24;
const CAPACITY_CHART_PLOT_RIGHT = 12;
const CAPACITY_CHART_PLOT_BOTTOM = 56;
const CAPACITY_CHART_MAX_PIXELS_PER_PERCENT = 4;
const CAPACITY_CHART_MEDIUM_PIXELS_PER_PERCENT = 2;
const CAPACITY_CHART_MIN_PIXELS_PER_PERCENT = 1;
const CAPACITY_CHART_VIEWPORT_GAP_PX = 16;
const CAPACITY_CHART_FIGURE_OVERHEAD_PX = 48;
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
  { key: "available-capacity", label: "Available capacity", source: "aggregate" },
];

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

const capacityChartContentWidth = () => {
  const chartStyles = getComputedStyle(providerCapacityChart);
  const horizontalPadding = Number.parseFloat(chartStyles.paddingLeft) + Number.parseFloat(chartStyles.paddingRight);
  const width = providerCapacityChart.getBoundingClientRect().width - horizontalPadding;
  return Number.isFinite(width) && width > 0
    ? Math.max(CAPACITY_CHART_PLOT_LEFT + CAPACITY_CHART_PLOT_RIGHT + 1, width)
    : 740;
};

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

const capacityChartAggregateRemainingPercent = (sample) => {
  const sources = Array.isArray(sample?.sources) ? sample.sources : [];
  const remaining = [];
  for (const source of sources) {
    if (source?.state === "unavailable") continue;
    if (source?.source === "codex") {
      const usedPercent = source.windows?.primary?.used_percent;
      if (typeof usedPercent === "number" && Number.isFinite(usedPercent)) {
        remaining.push(capacityRemainingPercent(usedPercent));
      }
    } else if (source?.source === "yunwu") {
      const refillRemaining = source.wallet?.refill_cycle_remaining_percent;
      if (typeof refillRemaining === "number" && Number.isFinite(refillRemaining)) {
        remaining.push(clampCapacityChartPercent(refillRemaining));
      }
    }
  }
  const reported = remaining.filter((value) => typeof value === "number" && Number.isFinite(value));
  return reported.length ? reported.reduce((total, value) => total + value, 0) / reported.length : null;
};

const capacityChartPoint = (sample, series, activeInterval = null, chartWindow = null) => {
  if (series.source === "aggregate") {
    const displayInterval = chartWindow ?? activeInterval;
    const sampledAtMs = sample?.sampled_at_ms;
    const remainingPercent = capacityChartAggregateRemainingPercent(sample);
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
      candidate?.source === series.source && (series.source === "yunwu" || candidate.slot === series.slot)
    )
    : null;
  const window = series.source === "codex" ? source?.windows?.[series.windowKey] : null;
  const interval = series.source === "yunwu" ? chartWindow : activeInterval ?? capacityChartWindow(window);
  const displayInterval = chartWindow ?? interval;
  const reportedPercent = series.source === "yunwu" ? source?.wallet?.[series.valueKey] : window?.used_percent;
  const remainingPercent = series.source === "yunwu" ? reportedPercent : capacityRemainingPercent(reportedPercent);
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
    (series.source === "codex" && (sampledAtMs < interval.startAtMs || sampledAtMs > interval.resetAtMs)) ||
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
    sampledAtMs < interval.startAtMs || sampledAtMs > interval.resetAtMs ||
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
) => {
  const runs = [];
  let run = [];
  const pushRun = () => {
    if (run.length) runs.push(run);
    run = [];
  };
  for (const sample of history) {
    const point = capacityChartPoint(sample, series, activeInterval, chartWindow);
    if (!point) {
      pushRun();
      continue;
    }
    run.push({ sampledAtMs: sample.sampled_at_ms, point, synthetic: false });
  }
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

const capacityChartPath = (points, plot) => {
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
    if (runIndex === 0) anchored.push({ elapsedPercent: 0, remainingPercent: 100 });
    anchored.push(...runPoints);
    if (runIndex === runs.length - 1) {
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

const capacityChartReferenceWindow = (sources, nowMs) => {
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

const renderProviderCapacityChart = (snapshot, sources) => {
  const history = Array.isArray(snapshot?.history) ? snapshot.history : [];
  const nowMs = Date.now();
  const chartWindow = capacityChartReferenceWindow(sources, nowMs);
  const width = capacityChartContentWidth();
  const plot = {
    left: CAPACITY_CHART_PLOT_LEFT,
    top: CAPACITY_CHART_PLOT_TOP,
    width: Math.max(1, width - CAPACITY_CHART_PLOT_LEFT - CAPACITY_CHART_PLOT_RIGHT),
    height: capacityChartPlotHeight(),
  };
  const height = plot.top + plot.height + CAPACITY_CHART_PLOT_BOTTOM;
  const chartTicks = capacityChartTickConfig(chartWindow, plot.width);
  const chartSectionMs = chartWindow.durationMs / chartTicks.count;
  const figure = document.createElement("figure");
  figure.dataset.capacityChartFigure = "";
  figure.style.setProperty("--capacity-chart-height-px", `${height}px`);
  const title = document.createElement("h3");
  title.textContent = "Available capacity history";
  figure.appendChild(title);

  const svg = capacityChartSvgElement("svg", {
    viewBox: `0 0 ${width} ${height}`,
    preserveAspectRatio: "xMidYMid meet",
    role: "img",
    "aria-label": "Available provider capacity history across the active usage period",
    focusable: "false",
  });
  svg.dataset.capacityChartSvg = "";

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
  xAxisTitle.textContent = `Usage period · ${capacityChartIntervalLabel(chartWindow.durationMs)}`;
  xAxisTitle.dataset.capacityChartAxisTitle = "x";
  svg.appendChild(xAxisTitle);
  const yAxisTitle = capacityChartSvgElement("text", {
    x: 12,
    y: plot.top + plot.height / 2,
    transform: `rotate(-90 12 ${plot.top + plot.height / 2})`,
    "text-anchor": "middle",
  });
  yAxisTitle.textContent = "Available capacity remaining";
  yAxisTitle.dataset.capacityChartAxisTitle = "y";
  svg.appendChild(yAxisTitle);

  const optimalSpendTrend = capacityChartSvgElement("line", {
    x1: plot.left,
    y1: plot.top,
    x2: plot.left + plot.width,
    y2: plot.top + plot.height,
  });
  optimalSpendTrend.dataset.capacityTrend = "optimal-spend";
  optimalSpendTrend.setAttribute("aria-label", "Optimal spend trend");
  svg.appendChild(optimalSpendTrend);

  const currentElapsedPercent = chartWindow.durationMs > 0
    ? clampCapacityChartPercent(((nowMs - chartWindow.startAtMs) / chartWindow.durationMs) * 100)
    : 0;
  const currentX = plot.left + (currentElapsedPercent / 100) * plot.width;
  const reticule = capacityChartSvgElement("line", {
    x1: currentX,
    y1: plot.top,
    x2: currentX,
    y2: plot.top + plot.height,
  });
  reticule.dataset.capacityReticule = "current-time";
  reticule.setAttribute("aria-label", "Current time in usage period");
  svg.appendChild(reticule);

  const currentSample = { sampled_at_ms: nowMs, sources };
  for (const series of CAPACITY_CHART_SERIES) {
    const activeInterval = chartWindow;
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
      )
      : [];
    const path = capacityChartSvgElement("path", {
      d: capacityChartPath(chartPoints, plot),
      fill: "none",
    });
    path.style.fill = "none";
    path.dataset.capacitySeries = series.key;
    path.setAttribute("aria-label", series.label);
    svg.appendChild(path);
  }

  figure.appendChild(svg);
  const caption = document.createElement("figcaption");
  caption.dataset.capacityChartMeta = "";
  const samples = history.filter((sample) => typeof sample?.sampled_at_ms === "number");
  const staleNotes = [
    sources.some((source) => source?.source === "codex" && source?.state === "stale") ? "Codex samples stale" : null,
    sources.some((source) => source?.source === "yunwu" && source?.state === "stale") ? "YunWu sample stale" : null,
  ].filter((note) => note !== null);
  const staleSuffix = staleNotes.length ? ` · ${staleNotes.join(" · ")}` : "";
  const resetEvents = Array.isArray(snapshot?.reset_events) ? snapshot.reset_events : [];
  const resetSuffix = resetEvents.length
    ? ` · ${resetEvents.length} verified reset${resetEvents.length === 1 ? "" : "s"}`
    : "";
  caption.textContent = samples.length
    ? `15-minute buckets · ${formatCapacityTimestamp(samples[0].sampled_at_ms)} → ${
      formatCapacityTimestamp(samples[samples.length - 1].sampled_at_ms)
    } · ${samples.length} sample${samples.length === 1 ? "" : "s"}${resetSuffix}${staleSuffix}`
    : `No retained samples yet · current Codex usage period${resetSuffix}${staleSuffix}`;
  figure.appendChild(caption);
  providerCapacityChart.replaceChildren(figure);
};

const renderProviderCapacity = (snapshot) => {
  const rawSources = Array.isArray(snapshot?.sources) ? snapshot.sources : [];
  const sourceForSlot = (slot) =>
    rawSources.find((source) => source?.source === "codex" && source.slot === slot) ??
      unavailableCapacitySource("codex", slot);
  const sources = [
    sourceForSlot(1),
    sourceForSlot(2),
    rawSources.find((source) => source?.source === "yunwu") ?? unavailableCapacitySource("yunwu"),
  ];
  latestProviderCapacityChartState = { snapshot, sources };
  renderProviderCapacityChart(snapshot, sources);
  renderProviderCapacityList(sources);

  const unavailableCount = sources.filter((source) => source.state === "unavailable").length;
  const staleCount = sources.filter((source) => source.state === "stale").length;
  if (unavailableCount === sources.length) {
    setBadge(providerCapacityBadge, "bad", "Unavailable");
  } else if (unavailableCount > 0) {
    setBadge(providerCapacityBadge, "unknown", `Partial · ${unavailableCount} unavailable`);
  } else if (staleCount > 0) {
    setBadge(providerCapacityBadge, "unknown", `Stale · ${staleCount} source${staleCount === 1 ? "" : "s"}`);
  } else {
    setBadge(providerCapacityBadge, "ok", "Live");
  }
  const snapshotAt = typeof snapshot?.snapshot_at_ms === "number" ? snapshot.snapshot_at_ms : null;
  const cacheState = typeof snapshot?.cache_state === "string" ? snapshot.cache_state : "unavailable";
  providerCapacityUpdated.textContent = `Snapshot ${formatCapacityTimestamp(snapshotAt)} · ${cacheState}`;
};

const loadProviderCapacity = async ({ live = true } = {}) => {
  if (providerCapacityLoading) return false;
  const token = getAdminToken();
  if (!adminAccessState.isAdmin || !token) {
    setBadge(providerCapacityBadge, "bad", "Sign in required");
    return false;
  }
  providerCapacityLoading = true;
  setBadge(providerCapacityBadge, "unknown", "Loading capacity");
  try {
    const response = await fetch(apiUrl(`/admin/providers/capacity${live ? "?refresh=live" : ""}`), {
      cache: "no-store",
      headers: { Authorization: `Bearer ${token}` },
    });
    const payload = await response.json().catch(() => null);
    if (!response.ok || !payload) {
      setBadge(providerCapacityBadge, "bad", payload?.error?.message ?? "Capacity unavailable");
      providerCapacityUpdated.textContent = "Snapshot unavailable";
      return false;
    }
    renderProviderCapacity(payload);
    return true;
  } catch {
    setBadge(providerCapacityBadge, "bad", "Offline");
    providerCapacityUpdated.textContent = "Snapshot unavailable";
    return false;
  } finally {
    providerCapacityLoading = false;
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
    );
  });
};

globalThis.addEventListener("resize", scheduleProviderCapacityChartResize);

const loadProviders = async () => {
  if (providersLoading) return;
  const token = getAdminToken();
  if (!adminAccessState.isAdmin || !token) {
    return;
  }
  providersLoading = true;
  try {
    const response = await fetch(apiUrl("/admin/providers"), {
      cache: "no-store",
      headers: { Authorization: `Bearer ${token}` },
    });
    const payload = await response.json().catch(() => null);
    if (!response.ok || !payload) {
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
    latestProviderHealth = null;
    if (latestProviderCapacityChartState?.sources) renderProviderCapacityList(latestProviderCapacityChartState.sources);
  } finally {
    providersLoading = false;
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

const KERNEL_ACTIVITY_WINDOW_MS = 30 * 24 * 60 * 60 * 1000;
const KERNEL_NEAR_LIMIT_RATIO = 0.8;
const DEFAULT_API_KEY_WINDOW_MS = 604800000;
const DEFAULT_KERNEL_POLICY_LIMIT = -1;
const DEFAULT_KERNEL_POLICY_WINDOW_MS = 7 * 24 * 60 * 60 * 1000;

applyIconButton(kernelNewSaveBtn, "save", "Save rate limit");
applyIconButton(kernelPubKeyCreateBtn, "plus", "Add kernel attestation key");

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
  if (!getAdminToken()) {
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
  if (!getAdminToken()) {
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
  if (!getAdminToken()) {
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
  if (!token) {
    setAccessValue(accessUpstreamSource, "Missing token");
    setAccessValue(accessUpstreamExpiry, "Missing token");
    return;
  }
  accessUpstreamLoading = true;
  setAccessValue(accessUpstreamSource, "Loading...");
  setAccessValue(accessUpstreamExpiry, "Loading...");
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
      setAccessValue(accessUpstreamSource, source === "none" ? "None" : source);
      setAccessValue(accessUpstreamExpiry, data?.error ?? "Unavailable");
      return;
    }
    setAccessValue(accessUpstreamSource, source === "none" ? "None" : source);
    setAccessValue(accessUpstreamExpiry, typeof expMs === "number" ? formatDate(expMs) : "Unknown");
    accessUpstreamLoadedAt = Date.now();
  } catch {
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
  if (!token) {
    setKernelQueueBadge("bad", "Missing token");
    setKernelQueueMessage(getKernelQueueMissingTokenMessage());
    updateAccessGithubSummary();
    return;
  }

  if (kernelQueueLoading) return;
  kernelQueueLoading = true;
  setKernelQueueBadge("unknown", "Loading...");

  try {
    const res = await fetch(apiUrl("/admin/kernel-policy-queue"), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
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
  if (!token) {
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
  applyIconButton(kernelNewToggle, open ? "close" : "plus", open ? "Close new rate limit" : "New rate limit");
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
  if (!token) {
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
  if (!token) {
    setKernelListBadge("bad", "Missing token");
    setAuthBadge("bad", "Missing token");
    setKernelListMessage(getKernelListMissingTokenMessage());
    resetKernelPolicyState();
    setKernelAttention("");
    updateAccessGithubSummary();
    return;
  }

  const loadId = ++kernelListLoadId;
  setKernelListBadge("unknown", "Loading...");
  resetKernelPolicyState();
  setKernelAttention("");

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
    if (!token) {
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
    } catch {
      setEditBadge("bad", "Offline");
    } finally {
      saveBtn.disabled = false;
    }
  };

  const deleteLimit = async () => {
    const token = getAdminToken();
    if (!token) {
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
        return;
      }
      await loadKernelList();
    } catch {
      setKernelListBadge("bad", "Offline");
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
  if (!token) {
    setKernelPubKeysBadge("bad", "Missing token");
    setKernelPubKeysMessage("Paste an admin token to load kernel attestation keys.");
    updateAccessPubkeysSummary();
    return;
  }

  if (kernelPubKeysLoading) return;
  kernelPubKeysLoading = true;
  setKernelPubKeysBadge("unknown", "Loading...");

  try {
    const res = await fetch(apiUrl("/admin/kernel-pubkeys"), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
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
  if (!token) {
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
  if (!token) {
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
  if (!token) {
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

  if (Object.prototype.hasOwnProperty.call(usage, "yunwu_fallback_requests")) {
    appendUsagePill(summary, "YunWu", formatCompactNumber(usage.yunwu_fallback_requests), {
      title: `${formatNumber(usage.yunwu_fallback_requests)} fallback requests`,
    });
  }
  if (Object.prototype.hasOwnProperty.call(usage, "yunwu_spend_microcredits")) {
    appendUsagePill(summary, "Spend", formatMicrocreditsAsCredits(usage.yunwu_spend_microcredits), {
      title: `${formatNumber(usage.yunwu_spend_microcredits)} microcredits`,
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
    "Daily YunWu fallbacks",
    buildUsageSparkline(usage, {
      dailyKey: "daily_yunwu_fallback_requests",
      ariaLabel: "YunWu fallback requests",
    }),
  );
  appendUsageSeries(
    usageSection,
    "Daily YunWu spend",
    buildUsageSparkline(usage, {
      dailyKey: "daily_yunwu_spend_microcredits",
      ariaLabel: "YunWu spend",
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
    if (Object.prototype.hasOwnProperty.call(usage, "yunwu_fallback_requests")) {
      appendMetaItem(usageList, "YunWu fallbacks", formatNumber(usage.yunwu_fallback_requests));
    }
    if (Object.prototype.hasOwnProperty.call(usage, "yunwu_input_tokens")) {
      appendMetaItem(usageList, "YunWu tokens in", formatNumber(usage.yunwu_input_tokens));
    }
    if (Object.prototype.hasOwnProperty.call(usage, "yunwu_output_tokens")) {
      appendMetaItem(usageList, "YunWu tokens out", formatNumber(usage.yunwu_output_tokens));
    }
    if (Object.prototype.hasOwnProperty.call(usage, "yunwu_total_tokens")) {
      appendMetaItem(usageList, "YunWu tokens total", formatNumber(usage.yunwu_total_tokens));
    }
    if (Object.prototype.hasOwnProperty.call(usage, "yunwu_spend_microcredits")) {
      appendMetaItem(
        usageList,
        "YunWu spend",
        formatMicrocreditsAsCredits(usage.yunwu_spend_microcredits),
        { title: `${formatNumber(usage.yunwu_spend_microcredits)} microcredits` },
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

const loadApiKeyRequestLogs = (keyId) => {
  const cacheKey = getApiKeyRequestLogCacheKey(keyId);
  if (!cacheKey) return { ok: false, records: [], error: "Missing key id" };

  const token = getAdminToken();
  if (!token) return { ok: false, records: [], error: "Missing token" };

  const now = Date.now();
  const cached = apiKeyRequestLogCache.get(cacheKey);
  if (cached && now - cached.fetchedAt < API_KEY_REQUEST_LOGS_TTL_MS) {
    return { ok: true, records: cached.records, fromCache: true };
  }

  const inFlight = apiKeyRequestLogPromises.get(cacheKey);
  if (inFlight) return inFlight;

  const request = (async () => {
    try {
      const url = new URL(apiUrl(`/admin/api-keys/${encodeURIComponent(cacheKey)}/paid-fallbacks`));
      url.searchParams.set("limit", String(API_KEY_REQUEST_LOGS_LIMIT));

      const res = await fetch(url.toString(), {
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

      const rawRecords = Array.isArray(data?.data) ? data.data : [];
      const records = rawRecords
        .map((record) => normalizeApiKeyRequestLogRecord(record))
        .filter((record) => record && record.created_at_ms !== null);

      apiKeyRequestLogCache.set(cacheKey, {
        records,
        fetchedAt: Date.now(),
      });

      return { ok: true, records };
    } catch {
      return { ok: false, records: [], error: API_KEY_REQUEST_LOG_STATUS_UNAVAILABLE };
    } finally {
      apiKeyRequestLogPromises.delete(cacheKey);
    }
  })();

  apiKeyRequestLogPromises.set(cacheKey, request);
  return request;
};

const hydrateApiKeyRequestLogs = async (panel, keyId) => {
  if (!panel?.dataset) return;
  const currentKeyId = panel.dataset.keyId;
  if (!currentKeyId || currentKeyId !== keyId) return;

  const summary = panel.querySelector("[data-usage-summary]");
  const list = panel.querySelector("[data-api-key-request-list]");
  if (!summary || !list) return;
  if (panel.dataset.requestLogsState === "ready" && panel.dataset.requestLogsLoading !== "1") {
    const cacheKey = getApiKeyRequestLogCacheKey(currentKeyId);
    if (cacheKey) {
      const cached = apiKeyRequestLogCache.get(cacheKey);
      if (cached && Date.now() - cached.fetchedAt < API_KEY_REQUEST_LOGS_TTL_MS) return;
    }
  }

  if (panel.dataset.requestLogsLoading === "1") return;
  panel.dataset.requestLogsLoading = "1";
  setRequestLogsPanelMessage(list, summary, "Loading...", API_KEY_REQUEST_LOG_STATUS_OK);
  const response = await loadApiKeyRequestLogs(currentKeyId);

  if (!panel.isConnected || panel.dataset.keyId !== currentKeyId) {
    panel.dataset.requestLogsLoading = "0";
    return;
  }

  if (!response.ok) {
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

  panel.dataset.requestLogsState = "ready";
  const records = response.records || [];
  if (!records.length) {
    setRequestLogsPanelMessage(list, summary, "No paid fallbacks recorded");
    panel.dataset.requestLogsLoading = "0";
    return;
  }

  const countText = records.length === 1 ? "1 paid fallback" : `${records.length} paid fallbacks`;
  summary.textContent = `${countText} (showing last ${API_KEY_REQUEST_LOGS_LIMIT})`;
  list.textContent = "";
  records.forEach((record) => {
    list.appendChild(createRequestLogRow(record));
  });
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
    paidFallbackTitle.textContent = "YunWu paid overflow";
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

    paidFallbackSummary.appendChild(paidFallbackHeader);
    paidFallbackSummary.appendChild(paidFallbackInfo);

    const updatePaidFallbackInfo = () => {
      const enabled = key.paid_fallback_enabled === true;
      const limit = normalizeFiniteNumber(key.paid_fallback_limit_credits) ?? 0;
      const spent = normalizeFiniteNumber(key.paid_fallback_spent_credits) ?? 0;
      const reserved = normalizeFiniteNumber(key.paid_fallback_reserved_credits) ?? 0;
      const usage = key.usage && typeof key.usage === "object" ? key.usage : null;

      paidFallbackSummary.hidden = !enabled;
      paidFallbackStatus.dataset.state = enabled ? "ok" : "unknown";
      paidFallbackStatus.textContent = enabled ? "Enabled" : "Disabled";
      paidFallbackSummary.dataset.state = enabled ? "enabled" : "disabled";
      paidLimitInfo.valueEl.textContent = formatPaidFallbackLimit(limit);
      paidSpentInfo.valueEl.textContent = formatCredits(spent);
      paidReservedInfo.valueEl.textContent = formatCredits(reserved);
      paidResetInfo.valueEl.textContent = formatDate(key.usage_reset_at_ms);
      lifetimeSpendInfo.valueEl.textContent = usage &&
          Object.prototype.hasOwnProperty.call(usage, "yunwu_spend_microcredits")
        ? formatMicrocreditsAsCredits(usage.yunwu_spend_microcredits)
        : "unknown";
      fallbackCountInfo.valueEl.textContent = usage &&
          Object.prototype.hasOwnProperty.call(usage, "yunwu_fallback_requests")
        ? formatNumber(usage.yunwu_fallback_requests)
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
    paidFallbackToggleTitle.textContent = "YunWu paid overflow";
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
      "Enabling fallback can send prompts, code, tools, and attachments to YunWu. Pricing is checked only when this key is enabled; re-enabling checks it again. Ordinary requests never recheck it.";

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
      if (!token) {
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
      setEditBadge("unknown", initializingPaidFallback ? "Initializing YunWu..." : "Saving...");
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
  if (!token) {
    setPasskeyUsersBadge("bad", "Missing token");
    setPasskeyUsersMessage("Paste a fallback admin token to manage passkey users.");
    return;
  }

  if (passkeyUsersLoading) return;
  passkeyUsersLoading = true;
  setPasskeyUsersBadge("unknown", "Loading...");

  try {
    const res = await fetch(apiUrl("/admin/passkey-users"), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
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
  if (!token) {
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
const VIEW_HASHES = {
  loading: "loading",
  keys: "keys",
  users: "users",
  kernel: "kernel",
  pubkeys: "pubkeys",
  defaults: "defaults",
  providers: "providers",
};
const VIEW_REQUIREMENTS = {
  keys: "admin",
  users: "super-admin",
  kernel: "admin",
  pubkeys: "admin",
  defaults: "admin",
  providers: "admin",
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
  tab.tabIndex = enabled ? 0 : -1;
  if (!enabled) {
    const label = tab.id === "view-tab-users" ? "Super admin required" : "Admin sign-in required";
    tab.title = label;
  } else {
    tab.removeAttribute("title");
  }
};

const viewTabs = {
  keys: viewTabKeys,
  users: viewTabUsers,
  kernel: viewTabKernel,
  pubkeys: viewTabPubkeys,
  defaults: viewTabDefaults,
  providers: viewTabProviders,
};

const viewSections = {
  loading: viewLoading,
  keys: viewKeys,
  users: viewUsers,
  kernel: viewKernel,
  pubkeys: viewPubkeys,
  defaults: viewDefaults,
  providers: viewProviders,
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
  const token = getAdminToken();
  if (!adminAccessState.isAdmin || !token) {
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

const showPendingAdminViewAfterPrefetch = (view) => {
  startAdminPrefetch().finally(() => {
    if (pendingAdminView !== view || !canAccessView(view)) return;
    pendingAdminView = null;
    setAdminView(view, { hashMode: "replace", allowInaccessible: false });
  });
};

const updateViewAccess = () => {
  Object.entries(viewTabs).forEach(([key, tab]) => {
    setTabState(tab, key === currentAdminView, canAccessView(key));
  });
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
      void loadProviderCapacity({ live: true }).then((loaded) => {
        if (!loaded) providerCapacityLoadedForOpen = false;
      });
    }
  } else {
    providerCapacityLoadedForOpen = false;
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
  updateViewAccess();
  let prefetchPromise = null;
  if (adminAccessState.isAdmin) {
    closeAutoOpenedAuthWidget();
    prefetchPromise = startAdminPrefetch();
  } else if (adminAccessState.checked) {
    openAuthWidgetForAuth();
  }
  if (pendingAdminView && canAccessView(pendingAdminView)) {
    const view = pendingAdminView;
    if (prefetchPromise) {
      showPendingAdminViewAfterPrefetch(view);
      return;
    }
    pendingAdminView = null;
    setAdminView(view, { hashMode: "replace" });
    return;
  }
  if (pendingAdminView && adminAccessState.checked && adminAccessState.isAdmin && !canAccessView(pendingAdminView)) {
    pendingAdminView = null;
    setAdminView(ADMIN_VIEW_DEFAULT, { hashMode: "replace", allowInaccessible: false });
    return;
  }
  if (adminAccessState.checked && !adminAccessState.isAdmin && !canAccessView(currentAdminView)) {
    currentAdminView = ADMIN_VIEW_DEFAULT;
  } else if (adminAccessState.checked && adminAccessState.isAdmin && !canAccessView(currentAdminView)) {
    setAdminView(ADMIN_VIEW_DEFAULT, { hashMode: "replace", allowInaccessible: false });
    return;
  }
  syncVisibleAdminView();
  loadAdminView(currentAdminView);
};

const setAdminView = (view, options = {}) => {
  const nextView = normalizeAdminView(view);
  const allowInaccessible = options.allowInaccessible === true;
  if (!allowInaccessible && !canAccessView(nextView)) {
    if (adminAccessState.checked && adminAccessState.isAdmin) {
      pendingAdminView = null;
      setAdminView(ADMIN_VIEW_DEFAULT, { hashMode: "replace", allowInaccessible: false });
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

const testAdminToken = async () => {
  const token = getAdminToken();
  if (!token) {
    setAuthBadge("bad", "Missing token");
    setAdminAccessState({ checked: true, isAdmin: false, isSuperAdmin: false });
    setSignedInState(false);
    tokenInput.focus();
    return;
  }

  setAuthBadge("unknown", "Checking...");
  try {
    const res = await fetch(apiUrl("/uos/auth"), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      setAuthBadge("bad", data?.error?.message ?? "Unauthorized");
      setAdminAccessState({ checked: true, isAdmin: false, isSuperAdmin: false });
      setSignedInState(false);
      return;
    }
    if (!data?.auth?.is_admin) {
      setAuthBadge("bad", "Not admin");
      setAdminAccessState({ checked: true, isAdmin: false, isSuperAdmin: false });
      setSignedInState(false);
      return;
    }
    const kind = data?.auth?.method?.kind;
    const isSuperAdmin = data?.auth?.is_super_admin === true;
    setAuthBadge("ok", kind ? `OK (${formatAuthMethodLabel(kind)})` : "OK");
    setAdminAccessState({ checked: true, isAdmin: true, isSuperAdmin });
    setSignedInState(true, {
      canRegisterPasskey: true,
      deviceRegistered: hasAuthPasskeyCredential(data?.auth) || hasStoredPasskeyCredentials(),
      statusText: formatAuthSessionLabel(data?.auth),
    });
  } catch {
    setAuthBadge("bad", "Offline");
    setAdminAccessState({ checked: true, isAdmin: false, isSuperAdmin: false });
    setSignedInState(false);
  }
};

const scheduleTokenCheck = debounce(() => {
  if (!getAdminToken()) {
    setAuthBadge("bad", "Missing token");
    setAdminAccessState({ checked: true, isAdmin: false, isSuperAdmin: false });
    setSignedInState(false);
    clearApiKeyRequestLogCaches();
    return;
  }
  void testAdminToken();
}, 500);

const createKey = async () => {
  const token = getAdminToken();
  if (!token) {
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
  setCreateBadge("unknown", paidFallbackEnabled ? "Initializing YunWu..." : "Creating...");
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
  } finally {
    createKeyBtn.disabled = false;
  }
};

const refreshKeys = async () => {
  const token = getAdminToken();
  if (!token) {
    setKeysBadge("bad", "Missing token");
    setKeyListMessage("Paste an admin token to load API keys.");
    updateAccessApiKeysSummary();
    return;
  }

  if (keysLoading) return;
  keysLoading = true;
  setKeysBadge("unknown", "Loading...");
  setKeysListLoading();

  try {
    const res = await fetch(apiUrl("/admin/api-keys?include_usage=1"), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      setKeysBadge("bad", data?.error?.message ?? "Error");
      setKeyListMessage("Failed to load API keys.");
      return;
    }
    const keys = Array.isArray(data?.data) ? data.data : [];
    allKeys = keys;
    keysLoadedAt = Date.now();
    renderKeys(allKeys, currentKeyView);
  } catch {
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
  if (!token) {
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
  if (!token) {
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
  if (!token) {
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
  if (!token) {
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
      return;
    }
    await refreshKeys();
  } catch {
    setKeysBadge("bad", "Offline");
  } finally {
    if (button) button.disabled = false;
  }
};

const formatModelLabel = (model) => {
  const label = typeof model?.display_name === "string" ? model.display_name : model?.slug;
  return label && label.trim() ? label : "unknown";
};

const coerceFiniteInt = (value) => (typeof value === "number" && Number.isFinite(value) ? Math.trunc(value) : null);

const extractCachedModels = (cached) => {
  if (!cached || typeof cached !== "object") return { snapshot: null, models: [] };
  const rawModels = Array.isArray(cached.models) ? cached.models : [];
  const models = rawModels.filter((model) => typeof model?.slug === "string" && model.slug.trim().length > 0);
  if (!models.length) return { snapshot: null, models: [] };
  const snapshot = {
    updated_at_ms: coerceFiniteInt(cached.updated_at_ms),
    source: typeof cached.source === "string" && cached.source.trim() ? cached.source.trim() : "cached",
    client_version: typeof cached.client_version === "string" && cached.client_version.trim()
      ? cached.client_version.trim()
      : "",
  };
  return { snapshot, models };
};

const extractCachedDefaults = (cached) => {
  if (!cached || typeof cached !== "object") return null;
  const model = typeof cached.model === "string" ? cached.model : "";
  const reasoning = typeof cached.reasoning_effort === "string" ? cached.reasoning_effort : "";
  const limit = coerceFiniteInt(cached.kernel_policy_limit_requests);
  const windowMs = coerceFiniteInt(cached.kernel_policy_window_ms);
  return { model, reasoning_effort: reasoning, kernel_policy_limit_requests: limit, kernel_policy_window_ms: windowMs };
};

const clearYunwuQuotaDiagnostics = () => {
  yunwuQuotaRemaining.textContent = "—";
  yunwuQuotaProgress.hidden = true;
  yunwuQuotaProgress.value = 0;
  yunwuQuotaProgress.removeAttribute("aria-valuetext");
  yunwuQuotaBalance.textContent = "—";
  yunwuQuotaBaseline.textContent = "—";
  yunwuQuotaLatestRefill.textContent = "—";
  yunwuQuotaLatestRefill.removeAttribute("title");
  yunwuQuotaInferredCredit.textContent = "—";
  yunwuQuotaCache.textContent = "—";
  yunwuQuotaConfidence.textContent = "—";
  yunwuQuotaObserved.textContent = "—";
  yunwuQuotaCycleStarted.textContent = "—";
};

const formatQuotaCredits = (value) =>
  typeof value === "number" && Number.isFinite(value) ? `${creditFormatter.format(value)} credits` : "—";

const formatQuotaLabel = (value) => {
  if (typeof value !== "string" || !value) return "—";
  return value.replaceAll("_", " ").replace(/^./, (first) => first.toUpperCase());
};

const renderYunwuQuotaDiagnostics = (diagnostics) => {
  clearYunwuQuotaDiagnostics();
  if (!diagnostics || typeof diagnostics !== "object") {
    setYunwuQuotaBadge("bad", "Unavailable");
    return;
  }
  if (diagnostics.configured !== true) {
    setYunwuQuotaBadge("unknown", "Not configured");
    return;
  }
  if (diagnostics.available !== true) {
    setYunwuQuotaBadge("bad", "Unavailable");
    return;
  }

  const remaining = typeof diagnostics.remaining_percent === "number" && Number.isFinite(diagnostics.remaining_percent)
    ? Math.min(100, Math.max(0, diagnostics.remaining_percent))
    : null;
  if (remaining !== null) {
    const formatted = quotaPercentFormatter.format(remaining);
    yunwuQuotaRemaining.textContent = `${formatted}%`;
    yunwuQuotaProgress.value = remaining;
    yunwuQuotaProgress.hidden = false;
    yunwuQuotaProgress.setAttribute("aria-valuetext", `${formatted}% remaining`);
  }

  yunwuQuotaBalance.textContent = formatQuotaCredits(diagnostics.balance_credits);
  yunwuQuotaBaseline.textContent = formatQuotaCredits(diagnostics.baseline_credits);
  yunwuQuotaInferredCredit.textContent = formatQuotaCredits(diagnostics.last_inferred_credit_credits);
  yunwuQuotaCache.textContent = formatQuotaLabel(diagnostics.cache_state);
  yunwuQuotaConfidence.textContent = formatQuotaLabel(diagnostics.confidence);
  yunwuQuotaObserved.textContent = typeof diagnostics.observed_at_ms === "number"
    ? formatDate(diagnostics.observed_at_ms)
    : "—";
  yunwuQuotaCycleStarted.textContent = typeof diagnostics.cycle_started_at_ms === "number"
    ? formatDate(diagnostics.cycle_started_at_ms)
    : "—";

  const refillAmount = formatQuotaCredits(diagnostics.latest_refill_amount_credits);
  const refillTime = typeof diagnostics.latest_refill_completed_at_ms === "number"
    ? formatDate(diagnostics.latest_refill_completed_at_ms)
    : "—";
  yunwuQuotaLatestRefill.textContent = refillAmount === "—" && refillTime === "—"
    ? "—"
    : `${refillAmount} · ${refillTime}`;
  if (typeof diagnostics.latest_refill_id === "string" && diagnostics.latest_refill_id) {
    yunwuQuotaLatestRefill.title = `Refill ${diagnostics.latest_refill_id}`;
  }

  const stale = diagnostics.cache_state === "stale";
  setYunwuQuotaBadge(stale ? "unknown" : "ok", stale ? "Stale cache" : "Available");
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

const persistDefaultsSnapshot = (snapshot) => {
  if (!snapshot) return;
  writeStorageJson(STORAGE_KEYS.defaultsSnapshot, snapshot);
};

const persistDefaultsModels = (snapshot, models) => {
  if (!Array.isArray(models) || models.length === 0) return;
  writeStorageJson(STORAGE_KEYS.defaultsModels, {
    updated_at_ms: snapshot?.updated_at_ms ?? null,
    source: snapshot?.source ?? "unknown",
    client_version: snapshot?.client_version ?? "",
    models,
  });
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
  if (!token) {
    setDefaultsBadge("bad", "Missing token");
    clearYunwuQuotaDiagnostics();
    setYunwuQuotaBadge("unknown", "Not loaded");
    return;
  }

  const loadId = ++defaultsLoadId;
  const preserveInputs = options.preserveInputs === true;
  if (!preserveInputs) defaultsTouched = false;
  defaultsLoaded = false;
  setDefaultsBadge("unknown", "Loading...");
  setYunwuQuotaBadge("unknown", "Loading...");
  let cacheApplied = false;
  const cachedDefaults = extractCachedDefaults(readStorageJson(STORAGE_KEYS.defaultsSnapshot));
  const cachedModels = extractCachedModels(readStorageJson(STORAGE_KEYS.defaultsModels));
  if (cachedModels.models.length) {
    const cachedSnapshot = {
      models: cachedModels.models,
      meta: cachedModels.snapshot,
    };
    const cachedResult = applyDefaultsSnapshot(cachedSnapshot, cachedDefaults, {
      preserveInputs: preserveInputs || defaultsTouched,
    });
    if (cachedResult) {
      cacheApplied = true;
      defaultsLoaded = true;
      setDefaultsBadge("unknown", "Cached");
    }
  }
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
      setDefaultsBadge("bad", modelsPayload?.error?.message ?? "Error");
      return;
    }
    const snapshot = modelsPayload?.data ?? null;
    const models = Array.isArray(snapshot?.models)
      ? snapshot.models.filter((model) => typeof model?.slug === "string")
      : [];
    defaultsModelMap = new Map(models.map((model) => [model.slug, model]));
    updateDefaultsMeta(snapshot, models);
    persistDefaultsModels(snapshot, models);

    const defaultsPayload = await defaultsRes.json().catch(() => null);
    if (!defaultsRes.ok) {
      setDefaultsBadge("bad", defaultsPayload?.error?.message ?? "Error");
      renderYunwuQuotaDiagnostics(null);
      return;
    }
    renderYunwuQuotaDiagnostics(defaultsPayload?.yunwu_quota);

    if (!models.length) {
      setDefaultsBadge("bad", "No models");
      if (!cacheApplied) {
        setSelectOptions(defaultsModelSelect, [], "", "No models available");
        setReasoningPlaceholder(defaultsReasoningSelect, "No reasoning levels");
      }
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
    persistDefaultsSnapshot(serverDefaults);
    const result = applyDefaultsSnapshot(
      { models, meta: snapshot },
      serverDefaults,
      { preserveInputs: preserveInputs || defaultsTouched },
    );
    defaultsLoaded = true;
    if (!defaultsTouched && result) {
      setDefaultsBadge("ok", `${result.selectedModel} · ${result.selectedReasoning}`);
    }
  } catch {
    setDefaultsBadge("bad", "Offline");
    renderYunwuQuotaDiagnostics(null);
  }
};

const saveDefaults = async () => {
  if (!defaultsLoaded) return;
  const token = getAdminToken();
  if (!token) {
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
    persistDefaultsSnapshot({
      model: typeof saved?.model === "string" && saved.model ? saved.model : model,
      reasoning_effort: typeof saved?.reasoning_effort === "string" && saved.reasoning_effort
        ? saved.reasoning_effort
        : reasoning,
      kernel_policy_limit_requests: typeof saved?.kernel_policy_limit_requests === "number"
        ? Math.trunc(saved.kernel_policy_limit_requests)
        : limitValue,
      kernel_policy_window_ms: typeof saved?.kernel_policy_window_ms === "number"
        ? Math.trunc(saved.kernel_policy_window_ms)
        : windowResult.value,
    });
    defaultsTouched = false;
    setDefaultsBadge("ok", summary);
  } catch {
    setDefaultsBadge("bad", "Offline");
  } finally {
    defaultsSaving = false;
  }
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
setDefaultsBadge("unknown", "Idle");
clearYunwuQuotaDiagnostics();
setYunwuQuotaBadge("unknown", "Idle");
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
const hasInitialAdminToken = Boolean(getAdminToken());
resetAdminPrefetchState(hasInitialAdminToken ? "Checking admin session..." : "Sign in to prepare the admin views.");
setAdminView(ADMIN_VIEW_DEFAULT, { allowInaccessible: true });
if (initialHashView && initialHashView !== "session") setAdminView(initialHashView, { focusAuth: false });
if (hasInitialAdminToken) {
  setAuthBadge("unknown", "Checking...");
  scheduleTokenCheck();
} else {
  setAuthBadge("bad", "Missing token");
  setAdminAccessState({ checked: true, isAdmin: false, isSuperAdmin: false });
}

bindForegroundRefresh(() => {
  if (currentAdminView !== "defaults" || !adminAccessState.isAdmin || !getAdminToken()) return;
  void loadDefaults({ preserveInputs: true });
});

authWidgetToggle.addEventListener("click", () => {
  setAuthWidgetOpen(authWidgetPanel.hidden, { focus: authWidgetPanel.hidden });
});

authWidgetClose.addEventListener("click", () => {
  setAuthWidgetOpen(false);
  authWidgetToggle.focus();
});

showTokenInput.addEventListener("change", () => {
  tokenInput.type = showTokenInput.checked ? "text" : "password";
});

tokenInput.addEventListener("input", () => {
  if (localDevelopmentAutoAuth && tokenInput.value.trim() !== LOCAL_DEVELOPMENT_ADMIN_TOKEN) {
    localDevelopmentAutoAuth = false;
  }
  persistTokenIfEnabled();
  keysLoadedAt = 0;
  passkeyUsersLoadedAt = 0;
  defaultsLoaded = false;
  kernelListLoadedAt = 0;
  kernelQueueLoadedAt = 0;
  kernelPubKeysLoadedAt = 0;
  accessUpstreamLoadedAt = 0;
  providersLoadedAt = 0;
  providerCapacityLoadedForOpen = false;
  latestProviderCapacityChartState = null;
  latestProviderHealth = null;
  providerCapacityChart.replaceChildren();
  clearApiKeyRequestLogCaches();
  if (!getAdminToken()) {
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
  setSignedInState(Boolean(getAdminToken()));
});

passkeyHandleInput.addEventListener("input", () => {
  schedulePasskeyHandlePersist();
});

passkeyLoginBtn.addEventListener("click", async () => {
  const passkeyBaseUrl = getPasskeyBaseUrl();
  setPasskeyStatus("unknown", "Signing in...");
  passkeyLoginBtn.disabled = true;
  passkeyRegisterBtn.disabled = true;
  try {
    if (!isAuthRelayMode && isCrossOriginTarget() && isRemoteAiTarget() && !hasStoredPasskeyCredentials()) {
      const relay = await requestRemotePasskeySession();
      if (relay.handle) setPasskeyHandleValue(relay.handle);
      applySignedInToken(relay.token, { deviceRegistered: true });
      setPasskeyStatus("ok", "Passkey signed in");
      return;
    }

    const passkeyHandle = getPasskeyHandle();
    const result = await signInWithPasskey({
      baseUrl: passkeyBaseUrl,
      handle: passkeyHandle,
      useHandle: Boolean(passkeyHandle),
    });
    if (result.handle) setPasskeyHandleValue(result.handle);
    applySignedInToken(result.token, { deviceRegistered: true });
    setPasskeyStatus("ok", "Passkey signed in");
    postAuthRelayResult(result);
  } catch (error) {
    setSignedInState(false);
    setPasskeyStatus("bad", formatPasskeyLoginError(error));
  } finally {
    passkeyLoginBtn.disabled = false;
    passkeyRegisterBtn.disabled = false;
  }
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
    applySignedInToken(result.token, { deviceRegistered: true });
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
    await signOut({ token, baseUrl: resolveBaseUrl() });
  } finally {
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
    setSignedInState(Boolean(getAdminToken()));
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
  defaultsLoadId += 1;
  kernelListLoadedAt = 0;
  kernelListLoadId += 1;
  kernelQueueItems = [];
  kernelQueueLoadedAt = 0;
  kernelPubKeys = [];
  kernelPubKeysLoadedAt = 0;
  accessUpstreamLoadedAt = 0;
  providersLoadedAt = 0;
  providerCapacityLoadedForOpen = false;
  latestProviderCapacityChartState = null;
  latestProviderHealth = null;
  providerCapacityChart.replaceChildren();
  resetAdminPrefetchState(getAdminToken() ? "Checking admin session..." : "Sign in to prepare the admin views.");
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

globalThis.setInterval(() => {
  if (currentAdminView === "providers" && document.visibilityState === "visible") void loadProviders();
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
  setPasskeyStatus("unknown", "Checking signed-in session...");
  passkeyLoginBtn.disabled = true;
  try {
    const cachedAuth = await getValidCachedRelayAuth();
    if (cachedAuth) {
      postAuthRelayResult(cachedAuth);
      return;
    }
    setPasskeyStatus("unknown", "Use the sign-in button to continue.");
  } catch (error) {
    setPasskeyStatus("bad", `${error?.message ?? "Passkey sign-in failed"} Use the sign-in button to try again.`);
  } finally {
    passkeyLoginBtn.disabled = false;
  }
};

void startAuthRelayIfRequested();
