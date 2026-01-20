import "./network.js";

const STORAGE_KEYS = {
  rememberToken: "uos_ai.admin.remember_token",
  token: "uos_ai.admin.token",
  expiresPreset: "uos_ai.admin.expires_preset",
  base: "uos_ai.admin.base",
  view: "uos_ai.admin.view",
};

const storage = {
  get(key) {
    try {
      return localStorage.getItem(key);
    } catch {
      return null;
    }
  },
  set(key, value) {
    try {
      localStorage.setItem(key, value);
    } catch {
      // ignore
    }
  },
  remove(key) {
    try {
      localStorage.removeItem(key);
    } catch {
      // ignore
    }
  },
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
const baseSelect = mustGet("admin-base");
const basePreview = mustGet("base-preview");

const keyNameInput = mustGet("key-name");
const keyUsageLimitInput = mustGet("key-usage-limit");
const keyExpiresSelect = mustGet("key-expires");
const createKeyBtn = mustGet("create-key");
const createBadge = mustGet("create-badge");
const createResult = mustGet("create-result");

const keysBadge = mustGet("keys-badge");
const keysList = mustGet("keys-list");

const keysTabAll = mustGet("keys-tab-all");
const keysTabActive = mustGet("keys-tab-active");
const keysTabRevoked = mustGet("keys-tab-revoked");

const viewTabSession = mustGet("view-tab-session");
const viewTabKeys = mustGet("view-tab-keys");
const viewTabKernel = mustGet("view-tab-kernel");
const viewTabPubkeys = mustGet("view-tab-pubkeys");
const viewTabDefaults = mustGet("view-tab-defaults");

const viewSession = mustGet("view-session");
const viewKeys = mustGet("view-keys");
const viewKernel = mustGet("view-kernel");
const viewPubkeys = mustGet("view-pubkeys");
const viewDefaults = mustGet("view-defaults");

let currentKeyView = "all";
let currentAdminView = "session";
let allKeys = [];
let keysLoading = false;
let keysLoadedAt = 0;

const accessApiKeys = mustGet("access-api-keys");
const accessGithubRepos = mustGet("access-github-repos");
const accessGithubQueue = mustGet("access-github-queue");
const accessKernelPubkeys = mustGet("access-kernel-pubkeys");
const accessUpstreamSource = mustGet("access-upstream-source");
const accessUpstreamExpiry = mustGet("access-upstream-expiry");

const defaultsModelSelect = mustGet("defaults-model");
const defaultsReasoningSelect = mustGet("defaults-reasoning");
const defaultsBadge = mustGet("defaults-badge");
const defaultsMeta = mustGet("defaults-meta");
let defaultsLoaded = false;
let defaultsSaving = false;
let defaultsModelMap = new Map();

const kernelListBadge = mustGet("kernel-list-badge");
const kernelList = mustGet("kernel-list");
const kernelQueueBadge = mustGet("kernel-queue-badge");
const kernelQueueList = mustGet("kernel-queue-list");
const kernelFilterInput = mustGet("kernel-filter");
const kernelShowSelect = mustGet("kernel-show");
const kernelSortSelect = mustGet("kernel-sort");
const kernelAttention = mustGet("kernel-attention");
const kernelNewToggle = mustGet("kernel-new-toggle");
const kernelNewPanel = mustGet("kernel-new-panel");
const kernelNewOwnerInput = mustGet("kernel-new-owner");
const kernelNewRepoField = mustGet("kernel-new-repo-field");
const kernelNewRepoInput = mustGet("kernel-new-repo");
const kernelNewLimitInput = mustGet("kernel-new-limit");
const kernelNewWindowInput = mustGet("kernel-new-window");
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
let kernelNewSaving = false;
let kernelListRecords = { org: [], repo: [] };
let kernelQueueItems = [];
let kernelQueueLoading = false;
let kernelQueueLoadedAt = 0;
let kernelQueuePoller = null;
let kernelPubKeys = [];
let kernelPubKeysLoading = false;
let kernelPubKeysLoadedAt = 0;
let kernelPubKeysSaving = false;
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

const setAuthBadge = (state, text) => setBadge(authBadge, state, text);
const setCreateBadge = (state, text) => setBadge(createBadge, state, text);
const setKeysBadge = (state, text) => setBadge(keysBadge, state, text);
const setDefaultsBadge = (state, text) => setBadge(defaultsBadge, state, text);
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

const resolveBaseUrl = () =>
  (getBaseChoice() === "ai" ? "https://ai.ubq.fi" : globalThis.location?.origin ?? "http://localhost");

const apiUrl = (path) => new URL(path, resolveBaseUrl()).toString();

const updateBasePreview = () => {
  basePreview.textContent = resolveBaseUrl();
};

const getAdminToken = () => tokenInput.value.trim();

const persistTokenIfEnabled = () => {
  if (!rememberTokenInput.checked) return;
  const token = getAdminToken();
  if (token) storage.set(STORAGE_KEYS.token, token);
  else storage.remove(STORAGE_KEYS.token);
};

const restoreSettings = () => {
  const remember = storage.get(STORAGE_KEYS.rememberToken) === "1";
  rememberTokenInput.checked = remember;
  if (remember) tokenInput.value = storage.get(STORAGE_KEYS.token) ?? "";
  keyExpiresSelect.value = storage.get(STORAGE_KEYS.expiresPreset) ?? "quarter";
  baseSelect.value = storage.get(STORAGE_KEYS.base) ?? "local";
  updateBasePreview();
};

const clearCreateResult = () => {
  createResult.textContent = "";
  createResult.hidden = true;
};

const setKeyListMessage = (text) => {
  keysList.textContent = "";
  const message = document.createElement("p");
  message.dataset.empty = "keys";
  message.textContent = text;
  keysList.appendChild(message);
};

const dateFormatter = new Intl.DateTimeFormat(undefined, {
  year: "numeric",
  month: "short",
  day: "2-digit",
  hour: "2-digit",
  minute: "2-digit",
});
const dateFormatterWithZone = new Intl.DateTimeFormat(undefined, {
  year: "numeric",
  month: "short",
  day: "2-digit",
  hour: "2-digit",
  minute: "2-digit",
  timeZoneName: "short",
});

const formatDate = (ms) => {
  if (typeof ms !== "number" || !Number.isFinite(ms)) return "unknown";
  return dateFormatter.format(new Date(ms));
};
const formatDateWithZone = (ms) => {
  if (typeof ms !== "number" || !Number.isFinite(ms)) return "unknown";
  return dateFormatterWithZone.format(new Date(ms));
};

const formatExpires = (ms) => (ms === -1 ? "Never" : formatDate(ms));

const numberFormatter = new Intl.NumberFormat();
const compactNumberFormatter = new Intl.NumberFormat(undefined, { notation: "compact", maximumFractionDigits: 1 });

const toNumber = (value) => {
  if (typeof value !== "number" || !Number.isFinite(value)) return 0;
  return Math.trunc(value);
};

const formatNumber = (value) => numberFormatter.format(toNumber(value));
const formatCompactNumber = (value) => compactNumberFormatter.format(toNumber(value));

const formatOptionalText = (value) => {
  if (typeof value !== "string") return "unknown";
  const trimmed = value.trim();
  return trimmed ? trimmed : "unknown";
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
  };
  return lookup[method] ?? method.replace(/_/g, " ");
};

const pad2 = (value) => String(value).padStart(2, "0");

const toDateTimeLocalValue = (ms) => {
  if (typeof ms !== "number" || !Number.isFinite(ms) || ms <= 0) return "";
  const date = new Date(ms);
  return `${date.getFullYear()}-${pad2(date.getMonth() + 1)}-${pad2(date.getDate())}T${pad2(date.getHours())}:${pad2(date.getMinutes())}`;
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

applyIconButton(kernelNewSaveBtn, "save", "Save policy");
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
const refreshAccessUpstreamSummary = async () => {
  if (accessUpstreamLoading) return;
  accessUpstreamLoading = true;
  setAccessValue(accessUpstreamSource, "Loading...");
  setAccessValue(accessUpstreamExpiry, "Loading...");
  try {
    const res = await fetch(apiUrl("/health/auth"), { cache: "no-store" });
    const data = await res.json().catch(() => null);
    const source = data?.auth?.source ?? "unknown";
    const expMs = data?.auth?.access_token_exp_ms ?? null;
    if (!res.ok) {
      setAccessValue(accessUpstreamSource, source === "none" ? "None" : source);
      setAccessValue(accessUpstreamExpiry, data?.error ?? "Unavailable");
      return;
    }
    setAccessValue(accessUpstreamSource, source === "none" ? "None" : source);
    setAccessValue(accessUpstreamExpiry, typeof expMs === "number" ? formatDate(expMs) : "Unknown");
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

const getKernelListMissingTokenMessage = () => "Paste an admin token to load GitHub access analytics and policies.";

const getKernelListTargetChangedMessage = () => "Target changed. Loading GitHub access analytics and policies...";

const getKernelQueueMissingTokenMessage = () => "Paste an admin token to load the policy queue.";

const getKernelQueueTargetChangedMessage = () => "Target changed. Loading the policy queue...";

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
    const message = policyState?.message ? `Policy data unavailable: ${policyState.message}` : "Policy data unavailable.";
    setKernelAttention(message);
    return;
  }
  if (!summary || summary.total === 0 || summary.unbounded === 0) {
    setKernelAttention("");
    return;
  }
  const label = "repos";
  const countText = `${summary.unbounded} of ${summary.total}`;
  setKernelAttention(`Attention: ${countText} ${label} with analytics have no caps (policies unset or unlimited).`);
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
  if (total === 0) return "No analytics or policies";

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
    setKernelListMessage("No analytics or policies yet.");
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
    setKernelQueueMessage("No policy gaps yet.");
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
    status.textContent = "Needs policy";

    const actionRow = document.createElement("div");
    actionRow.dataset.actionRow = "actions";
    const addBtn = document.createElement("button");
    addBtn.type = "button";
    addBtn.dataset.variant = "primary";
    applyIconButton(addBtn, "plus", `Add policy for ${owner}/${repo}`);
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
    setKernelQueueMessage("No policy gaps yet.");
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
      setKernelQueueMessage("Failed to load the policy queue.");
      return;
    }

    const records = Array.isArray(data?.data) ? data.data : [];
    kernelQueueItems = records;
    kernelQueueLoadedAt = Date.now();
    renderKernelPolicyQueue(records);
    setKernelQueueBadge("ok", records.length === 0 ? "No requests" : `${records.length} request${records.length === 1 ? "" : "s"}`);
  } catch {
    kernelQueueItems = [];
    setKernelQueueBadge("bad", "Offline");
    setKernelQueueMessage("Failed to load the policy queue.");
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

const startKernelQueuePolling = () => {
  if (kernelQueuePoller) return;
  kernelQueuePoller = setInterval(() => {
    if (currentAdminView === "kernel") {
      void refreshKernelPolicyQueue();
    }
  }, 15000);
};

const stopKernelQueuePolling = () => {
  if (!kernelQueuePoller) return;
  clearInterval(kernelQueuePoller);
  kernelQueuePoller = null;
};

const setKernelNewPanelOpen = (open) => {
  kernelNewPanel.hidden = !open;
  applyIconButton(kernelNewToggle, open ? "close" : "plus", open ? "Close new policy" : "New policy");
  if (!open) {
    resetKernelNewForm();
  }
};

const resetKernelNewForm = () => {
  kernelNewOwnerInput.value = "";
  kernelNewRepoInput.value = "";
  kernelNewLimitInput.value = "-1";
  kernelNewWindowInput.value = "";
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
    setBadgeFn("bad", "Policy limit required");
    return null;
  }
  if (trimmed === "unlimited" || trimmed === "-1") return -1;
  const parsed = Number(trimmed);
  if (!Number.isFinite(parsed) || parsed < 0) {
    setBadgeFn("bad", "Invalid policy limit");
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

  kernelNewSaving = true;
  kernelNewSaveBtn.disabled = true;
  setKernelNewBadge("unknown", "Saving...");

  try {
    const payload = {
      owner,
      scope,
      usage_limit_requests: limitValue,
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
  if (!orgResult.ok) messageParts.push(`Org policies unavailable: ${orgResult.message}`);
  if (!repoResult.ok) messageParts.push(`Repo policies unavailable: ${repoResult.message}`);

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
    const badgeText = formatKernelListBadge(result);
    const warnings = [];
    if (!orgUsageResult.ok) warnings.push(`Org analytics ${orgUsageResult.message}`);
    if (!repoUsageResult.ok) warnings.push(`Repo analytics ${repoUsageResult.message}`);
    if (!orgPolicyResult.ok) warnings.push(`Org policies ${orgPolicyResult.message}`);
    if (!repoPolicyResult.ok) warnings.push(`Repo policies ${repoPolicyResult.message}`);
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
  const policyLabel = scope === "repo" ? "Repo policy" : "Org policy";
  const policyValue = policyAvailable ? "Not set" : "Unavailable";
  const policyItem = appendKeyInfo(infoRow, policyLabel, policyValue, { state: "warning" });
  if (canAdd) {
    const addBtn = document.createElement("button");
    addBtn.type = "button";
    addBtn.dataset.variant = "primary";
    applyIconButton(addBtn, "plus", scope === "repo" ? "Add repo policy" : "Add org policy");
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
  appendKeyInfo(infoRow, "Updated", "—");
  main.appendChild(infoRow);

  const help = document.createElement("p");
  help.dataset.help = "true";
  if (options.helpText) {
    help.textContent = options.helpText;
  } else if (!policyAvailable) {
    help.textContent = "Policy data unavailable.";
  } else {
    help.textContent = scope === "repo" ? "No repo policy set yet." : "No org policy set yet.";
  }
  main.appendChild(help);

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
  row.dataset.state = record.usage_limit_requests === 0 ? "revoked" : "active";
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
  applyIconButton(editBtn, "edit", "Edit policy");

  const deleteBtn = document.createElement("button");
  deleteBtn.type = "button";
  deleteBtn.dataset.variant = "danger";
  applyIconButton(deleteBtn, "trash", "Delete policy");

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
    "Policy limit",
    formatLimitValue(record.usage_limit_requests),
    limitState ? { state: limitState } : {},
  );
  const windowInfo = appendKeyInfo(infoRow, "Window", formatWindowMs(record.window_ms));
  const usageInfo = appendKeyInfo(infoRow, "Window requests", formatNumber(record.usage_requests));
  const resetInfo = appendKeyInfo(infoRow, "Reset at", formatDate(record.usage_reset_at_ms));
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
  limitLabel.textContent = "Policy limit";
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

  editFields.appendChild(limitField);
  editFields.appendChild(windowField);

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

  const editHelp = document.createElement("p");
  editHelp.dataset.help = "true";
  editHelp.textContent = "Leave blank to keep the current interval. Updates reset analytics.";

  editPanel.appendChild(editFields);
  editPanel.appendChild(presetRow);
  editPanel.appendChild(editActions);
  editPanel.appendChild(editBadge);
  editPanel.appendChild(editHelp);

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
    setEditBadge("unknown", "Idle");
  };

  const updateInfo = (updated) => {
    const next = updated ?? record;
    limitInfo.valueEl.textContent = formatLimitValue(next.usage_limit_requests);
    if (next.usage_limit_requests === 0) {
      limitInfo.item.dataset.state = "bad";
      row.dataset.state = "revoked";
    } else {
      delete limitInfo.item.dataset.state;
      row.dataset.state = "active";
    }
    windowInfo.valueEl.textContent = formatWindowMs(next.window_ms);
    usageInfo.valueEl.textContent = formatNumber(next.usage_requests);
    resetInfo.valueEl.textContent = formatDate(next.usage_reset_at_ms);
    updatedInfo.valueEl.textContent = formatDate(next.updated_at_ms);
  };

  const saveEdits = async () => {
    const limitValue = parseKernelLimitValue(limitInput.value, setEditBadge);
    if (limitValue === null) return;
    const windowResult = parseKernelWindowValue(windowInput.value, setEditBadge);
    if (!windowResult.ok) return;

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
    if (!confirm(`Delete policy for ${confirmLabel}?`)) return;

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

  return row;
};

const renderKernelList = (records, policyState = kernelPolicyState) => {
  kernelList.textContent = "";
  const { org, repo } = normalizeKernelListRecords(records);
  const allGroups = buildKernelGroups(records);
  const totalOrgCount = allGroups.length;
  const totalRepoCount = repo.length;

  if (totalOrgCount === 0 && totalRepoCount === 0) {
    setKernelListMessage("No analytics or policies yet.");
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
      groupTile.appendChild(sublist);
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
const buildUsageSparkline = (usage) => {
  const daily = Array.isArray(usage?.daily_requests) ? usage.daily_requests : null;
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
  svg.setAttribute("aria-label", `Last ${count} days requests`);

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
  const maxLabel = maxValue > scaleMax ? `Max ${formatCompactNumber(scaleMaxLabel)}+` : `Max ${formatCompactNumber(scaleMaxLabel)}`;

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

  appendUsagePill(summary, "Requests", formatCompactNumber(usage.total_requests), {
    title: formatNumber(usage.total_requests),
  });
  appendUsagePill(summary, "Tokens", formatCompactNumber(usage.total_tokens), {
    title: formatNumber(usage.total_tokens),
  });
  appendUsagePill(summary, "Last", formatDate(usage.last_seen_at_ms));

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

  const sparkline = buildUsageSparkline(usage);
  if (sparkline) usageSection.appendChild(sparkline);

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
    usageSection.appendChild(usageList);
  }

  return usageSection;
};

const renderKeys = (keys, view = "all") => {
  keysList.textContent = "";
  let filteredKeys = keys;
  if (view === "active") {
    filteredKeys = keys.filter(k => !k.revoked_at_ms);
  } else if (view === "revoked") {
    filteredKeys = keys.filter(k => k.revoked_at_ms);
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

    const getUsageInfoData = () => {
      const limitValue = key.usage_limit_requests;
      const current = typeof key.usage_requests === "number" ? key.usage_requests : 0;
      const limitText = limitValue === -1 ? "Unlimited" : formatNumber(limitValue);
      const usageText = limitValue === -1 ? `${current}` : `${current}/${limitText}`;
      const hasLimit = limitValue !== -1;
      const isNearLimit = hasLimit && limitValue > 0 && current / limitValue >= 0.8;
      const isAtLimit = hasLimit && current >= limitValue;
      const title = `${formatNumber(current)} requests${
        hasLimit ? ` of ${formatNumber(limitValue)} policy limit` : ""
      } (resets ${formatDate(key.usage_reset_at_ms)})`;
      const state = isAtLimit ? "bad" : (isNearLimit ? "warning" : "");
      return { usageText, title, state };
    };

    // Display policy limit information
    let usageInfo = null;
    if (typeof key.usage_limit_requests === "number") {
      const usageData = getUsageInfoData();
      usageInfo = appendKeyInfo(infoRow, "Analytics", usageData.usageText, {
        state: usageData.state,
        title: usageData.title,
      });
    }

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
    limitLabel.textContent = "Policy limit";
    const limitInput = document.createElement("input");
    limitInput.type = "number";
    limitInput.inputMode = "numeric";
    limitInput.value = typeof key.usage_limit_requests === "number" ? String(key.usage_limit_requests) : "-1";
    limitField.appendChild(limitLabel);
    limitField.appendChild(limitInput);

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

    editFields.appendChild(nameField);
    editFields.appendChild(limitField);
    editFields.appendChild(expiresField);
    editPanel.appendChild(editFields);
    editPanel.appendChild(neverLabel);

    const editBadge = document.createElement("span");
    editBadge.dataset.badge = "status";
    editBadge.dataset.state = "unknown";
    editBadge.textContent = "Idle";

    const editHelp = document.createElement("p");
    editHelp.dataset.help = "true";
    editHelp.textContent = "Edits save automatically.";

    editPanel.appendChild(editBadge);
    editPanel.appendChild(editHelp);

    const setEditBadge = (state, text) => setBadge(editBadge, state, text);

    let editSaving = false;
    let editQueued = false;
    let editDirty = false;
    let editSnapshot = {
      name: key.name || "",
      usage_limit_requests: typeof key.usage_limit_requests === "number" ? key.usage_limit_requests : -1,
      expires_at_ms: typeof key.expires_at_ms === "number" ? key.expires_at_ms : -1,
    };

    const getEditInputState = () => ({
      name: nameInput.value,
      limit: limitInput.value,
      expires: expiresInput.value,
      never: neverInput.checked,
    });

    const isSameEditInputState = (left, right) =>
      left.name === right.name &&
      left.limit === right.limit &&
      left.expires === right.expires &&
      left.never === right.never;

    const syncEditInputsFromKey = () => {
      nameInput.value = key.name || "";
      limitInput.value = String(key.usage_limit_requests);
      neverInput.checked = key.expires_at_ms === -1;
      expiresInput.disabled = neverInput.checked;
      expiresInput.value = neverInput.checked ? "" : toDateTimeLocalValue(key.expires_at_ms);
    };

    const updateUsageInfo = () => {
      if (!usageInfo) return;
      const usageData = getUsageInfoData();
      usageInfo.valueEl.textContent = usageData.usageText;
      if (usageData.state) {
        usageInfo.item.dataset.state = usageData.state;
      } else {
        delete usageInfo.item.dataset.state;
      }
      usageInfo.valueEl.title = usageData.title;
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
        setEditBadge("bad", "Policy limit required");
        return null;
      }
      let nextLimit;
      if (limitRaw === "unlimited" || limitRaw === "-1") {
        nextLimit = -1;
      } else {
        const parsed = Number(limitRaw);
        if (!Number.isFinite(parsed) || parsed < 0) {
          setEditBadge("bad", "Invalid policy limit");
          return null;
        }
        nextLimit = Math.trunc(parsed);
      }
      if (nextLimit !== editSnapshot.usage_limit_requests) {
        payload.usage_limit_requests = nextLimit;
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

      if (!changed) {
        editDirty = false;
        setEditBadge("ok", "Saved");
        return null;
      }

      return { payload, nextSnapshot: { name: nextName, usage_limit_requests: nextLimit, expires_at_ms: nextExpiresAtMs } };
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
      const requestInputs = getEditInputState();
      editSaving = true;
      setEditBadge("unknown", "Saving...");
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
        const updatedExpires = typeof data?.expires_at_ms === "number" ? data.expires_at_ms : nextSnapshot.expires_at_ms;

        key.name = updatedName;
        key.usage_limit_requests = updatedLimit;
        key.expires_at_ms = updatedExpires;
        if (typeof data?.usage_requests === "number") key.usage_requests = data.usage_requests;
        if (typeof data?.usage_reset_at_ms === "number") key.usage_reset_at_ms = data.usage_reset_at_ms;

        title.textContent = key.name || "Untitled";
        expiresInfo.valueEl.textContent = formatExpires(key.expires_at_ms);
        updateUsageInfo();

        const inputsUnchanged = isSameEditInputState(requestInputs, getEditInputState());
        if (inputsUnchanged) {
          syncEditInputsFromKey();
        }

        editSnapshot = {
          name: key.name || "",
          usage_limit_requests: key.usage_limit_requests,
          expires_at_ms: key.expires_at_ms,
        };
        editDirty = !inputsUnchanged;
        if (editDirty) {
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
    expiresInput.addEventListener("input", () => {
      markEditDirty();
    });
    neverInput.addEventListener("change", () => {
      expiresInput.disabled = neverInput.checked;
      if (neverInput.checked) expiresInput.value = "";
      markEditDirty();
    });

    main.appendChild(editPanel);

    const hasUsageField = Object.prototype.hasOwnProperty.call(key, "usage");
    const usage = hasUsageField ? key.usage : undefined;

    if (hasUsageField) {
      main.appendChild(buildUsageDetails(usage));
    }

    row.appendChild(main);
    keysList.appendChild(row);
  });
  setKeysBadge("ok", `${formatPlural(filteredKeys.length, "API key")}`);
  updateAccessApiKeysSummary();
};

const setTabState = (tab, selected) => {
  tab.setAttribute("aria-selected", selected ? "true" : "false");
  tab.tabIndex = selected ? 0 : -1;
};

const viewTabs = {
  session: viewTabSession,
  keys: viewTabKeys,
  kernel: viewTabKernel,
  pubkeys: viewTabPubkeys,
  defaults: viewTabDefaults,
};

const viewSections = {
  session: viewSession,
  keys: viewKeys,
  kernel: viewKernel,
  pubkeys: viewPubkeys,
  defaults: viewDefaults,
};

const setAdminView = (view) => {
  const nextView = viewSections[view] ? view : "session";
  currentAdminView = nextView;
  Object.entries(viewSections).forEach(([key, section]) => {
    section.hidden = key !== nextView;
  });
  Object.entries(viewTabs).forEach(([key, tab]) => {
    setTabState(tab, key === nextView);
  });
  storage.set(STORAGE_KEYS.view, nextView);
  if (nextView === "keys") {
    void ensureKeysLoaded();
  }
  if (nextView === "defaults") {
    void loadDefaults();
  }
  if (nextView === "kernel") {
    void loadKernelList();
    void ensureKernelPolicyQueueLoaded();
    void refreshAccessOverview();
    startKernelQueuePolling();
  } else {
    stopKernelQueuePolling();
  }
  if (nextView === "pubkeys") {
    void ensureKernelPubKeysLoaded();
  }
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
    tokenInput.focus();
    return;
  }

  setAuthBadge("unknown", "Checking...");
  try {
    const res = await fetch(apiUrl("/v1/auth"), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      setAuthBadge("bad", data?.error?.message ?? "Unauthorized");
      return;
    }
    if (!data?.auth?.is_admin) {
      setAuthBadge("bad", "Not admin");
      return;
    }
    const kind = data?.auth?.method?.kind;
    setAuthBadge("ok", kind ? `OK (${formatAuthMethodLabel(kind)})` : "OK");
  } catch {
    setAuthBadge("bad", "Offline");
  }
};

const scheduleTokenCheck = debounce(() => {
  if (!getAdminToken()) {
    setAuthBadge("bad", "Missing token");
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
  const payload = {
    name,
    expires_at_ms: expiresAtMs,
    usage_limit_requests: isNaN(usageLimit) ? 50 : usageLimit,
  };

  clearCreateResult();
  setCreateBadge("unknown", "Creating...");
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
      `policy_limit: ${data?.usage_limit_requests === -1 ? "unlimited" : (data?.usage_limit_requests ?? "")}`,
      `created_at: ${formatDate(data?.created_at_ms)}`,
      `expires_at: ${formatExpires(data?.expires_at_ms)}`,
    ];
    createResult.textContent = lines.join("\n").trim();
    createResult.hidden = false;
    setCreateBadge("ok", "Created");
    keyNameInput.value = "";
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
  if (allKeys.length && Date.now() - keysLoadedAt < 10_000) {
    setKeysBadge("ok", `${formatPlural(allKeys.length, "API key")}`);
    return;
  }
  await refreshKeys();
};

const revokeKey = async (id, name, button) => {
  const token = getAdminToken();
  if (!token) {
    setKeysBadge("bad", "Missing token");
    tokenInput.focus();
    return;
  }
  if (!confirm(`Revoke ${name}?`)) return;

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
    await refreshKeys();
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
  if (!confirm(`Unrevoke ${name}?`)) return;

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
    await refreshKeys();
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

const getModelReasoningLevels = (model) => {
  const levels = Array.isArray(model?.supported_reasoning_levels) ? model.supported_reasoning_levels : [];
  return levels.filter((level) => typeof level === "string" && level.trim().length > 0);
};

const updateDefaultsMeta = (snapshot, models) => {
  if (!snapshot) {
    defaultsMeta.textContent = "No model snapshot available.";
    return;
  }
  const updatedAt = typeof snapshot.updated_at_ms === "number" ? formatDateWithZone(snapshot.updated_at_ms) : "unknown";
  const source = typeof snapshot.source === "string" ? snapshot.source : "unknown";
  const version =
    typeof snapshot.client_version === "string" && snapshot.client_version.trim() ? snapshot.client_version.trim() : "";
  const sourceLabel = version ? `${source} v${version}` : source;
  defaultsMeta.textContent = `Models: ${models.length} · Source: ${sourceLabel} · Updated: ${updatedAt}`;
};

const updateReasoningOptions = (modelSlug, preferred) => {
  const model = defaultsModelMap.get(modelSlug);
  const levels = getModelReasoningLevels(model);
  const options = levels.length ? levels.map((level) => ({ value: level, label: level })) : [
    { value: "none", label: "none" },
  ];
  const fallback = typeof model?.default_reasoning_level === "string" ? model.default_reasoning_level : "";
  const nextPreferred = levels.includes(preferred) ? preferred : levels.includes(fallback) ? fallback : options[0].value;
  const selected = setSelectOptions(defaultsReasoningSelect, options, nextPreferred, "No reasoning levels");
  defaultsReasoningSelect.disabled = levels.length === 0;
  return selected;
};

const loadDefaults = async () => {
  const token = getAdminToken();
  if (!token) {
    setDefaultsBadge("bad", "Missing token");
    return;
  }

  defaultsLoaded = false;
  setDefaultsBadge("unknown", "Loading...");
  defaultsMeta.textContent = "Loading model list...";

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
    const modelsPayload = await modelsRes.json().catch(() => null);
    if (!modelsRes.ok) {
      setDefaultsBadge("bad", modelsPayload?.error?.message ?? "Error");
      defaultsMeta.textContent = "Failed to load models.";
      return;
    }
    const snapshot = modelsPayload?.data ?? null;
    const models = Array.isArray(snapshot?.models) ? snapshot.models.filter((model) => typeof model?.slug === "string") : [];
    defaultsModelMap = new Map(models.map((model) => [model.slug, model]));
    updateDefaultsMeta(snapshot, models);

    const defaultsPayload = await defaultsRes.json().catch(() => null);
    if (!defaultsRes.ok) {
      setDefaultsBadge("bad", defaultsPayload?.error?.message ?? "Error");
      return;
    }

    if (!models.length) {
      setDefaultsBadge("bad", "No models");
      setSelectOptions(defaultsModelSelect, [], "", "No models available");
      setSelectOptions(defaultsReasoningSelect, [], "", "No reasoning levels");
      return;
    }

    const currentModel = typeof defaultsPayload?.defaults?.model === "string" ? defaultsPayload.defaults.model : "";
    const currentReasoning =
      typeof defaultsPayload?.defaults?.reasoning_effort === "string" ? defaultsPayload.defaults.reasoning_effort : "";
    const modelOptions = models.map((model) => ({ value: model.slug, label: formatModelLabel(model) }));
    const selectedModel = setSelectOptions(defaultsModelSelect, modelOptions, currentModel, "No models available");
    const selectedReasoning = updateReasoningOptions(selectedModel, currentReasoning);
    defaultsLoaded = true;
    setDefaultsBadge("ok", `${selectedModel} · ${selectedReasoning}`);
  } catch {
    setDefaultsBadge("bad", "Offline");
    defaultsMeta.textContent = "Offline.";
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
  setDefaultsBadge("unknown", "Saving...");

  try {
    const res = await fetch(apiUrl("/admin/defaults"), {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${token}`,
      },
      body: JSON.stringify({ model, reasoning_effort: reasoning }),
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      setDefaultsBadge("bad", data?.error?.message ?? "Error");
      return;
    }
    const saved = data?.defaults;
    const summary = saved?.model && saved?.reasoning_effort ? `${saved.model} · ${saved.reasoning_effort}` : "Saved";
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
setAuthBadge("unknown", "Not checked");
setCreateBadge("unknown", "Idle");
setKeysBadge("unknown", "Not loaded");
setDefaultsBadge("unknown", "Idle");
setKernelListBadge("unknown", "Not loaded");
setKernelNewBadge("unknown", "Idle");
setKernelQueueBadge("unknown", "Not loaded");
setKernelPubKeysBadge("unknown", "Not loaded");
setKernelPubKeyCreateBadge("unknown", "Idle");
setKeyListMessage("Paste an admin token to load API keys.");
setKernelListMessage(getKernelListMissingTokenMessage());
setKernelQueueMessage(getKernelQueueMissingTokenMessage());
setKernelPubKeysMessage("Paste an admin token to load kernel attestation keys.");
kernelFilterInput.value = "";
kernelShowSelect.value = "all";
switchKeysView("all");
setKernelNewPanelOpen(false);
resetKernelPubKeyForm();
setAdminView(storage.get(STORAGE_KEYS.view) ?? "session");
if (getAdminToken()) scheduleTokenCheck();

showTokenInput.addEventListener("change", () => {
  tokenInput.type = showTokenInput.checked ? "text" : "password";
});

tokenInput.addEventListener("input", () => {
  persistTokenIfEnabled();
  keysLoadedAt = 0;
  defaultsLoaded = false;
  kernelQueueLoadedAt = 0;
  kernelPubKeysLoadedAt = 0;
  if (!getAdminToken()) {
    setAuthBadge("bad", "Missing token");
    setKeysBadge("unknown", "Not loaded");
    setKeyListMessage("Paste an admin token to load API keys.");
    setKernelListBadge("unknown", "Not loaded");
    setKernelListMessage(getKernelListMissingTokenMessage());
    setKernelQueueBadge("unknown", "Not loaded");
    setKernelQueueMessage(getKernelQueueMissingTokenMessage());
    setKernelPubKeysBadge("unknown", "Not loaded");
    setKernelPubKeysMessage("Paste an admin token to load kernel attestation keys.");
    allKeys = [];
    kernelQueueItems = [];
    kernelPubKeys = [];
    kernelPubKeysLoadedAt = 0;
  } else {
    setAuthBadge("unknown", "Checking...");
  }
  scheduleTokenCheck();
  if (currentAdminView === "keys") {
    void ensureKeysLoaded();
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
});

baseSelect.addEventListener("change", () => {
  storage.set(STORAGE_KEYS.base, getBaseChoice());
  updateBasePreview();
  setAuthBadge("unknown", "Not checked");
  setCreateBadge("unknown", "Idle");
  setKeysBadge("unknown", "Not loaded");
  setDefaultsBadge("unknown", "Idle");
  setKernelListBadge("unknown", "Not loaded");
  setKernelNewBadge("unknown", "Idle");
  setKernelQueueBadge("unknown", "Not loaded");
  setKernelPubKeysBadge("unknown", "Not loaded");
  setKernelPubKeyCreateBadge("unknown", "Idle");
  setKeyListMessage("Target changed. Loading API keys...");
  setKernelListMessage(getKernelListTargetChangedMessage());
  setKernelQueueMessage(getKernelQueueTargetChangedMessage());
  setKernelPubKeysMessage("Target changed. Loading kernel attestation keys...");
  clearCreateResult();
  setKernelNewPanelOpen(false);
  resetKernelPubKeyForm();
  allKeys = [];
  keysLoadedAt = 0;
  defaultsLoaded = false;
  kernelQueueItems = [];
  kernelQueueLoadedAt = 0;
  kernelPubKeys = [];
  kernelPubKeysLoadedAt = 0;
  scheduleTokenCheck();
  if (currentAdminView === "keys") {
    void ensureKeysLoaded();
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

viewTabSession.addEventListener("click", () => setAdminView("session"));
viewTabKeys.addEventListener("click", () => setAdminView("keys"));
viewTabKernel.addEventListener("click", () => setAdminView("kernel"));
viewTabPubkeys.addEventListener("click", () => setAdminView("pubkeys"));
viewTabDefaults.addEventListener("click", () => setAdminView("defaults"));

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
  const model = defaultsModelSelect.value;
  updateReasoningOptions(model, defaultsReasoningSelect.value);
  scheduleDefaultsSave();
});

defaultsReasoningSelect.addEventListener("change", () => {
  if (!defaultsLoaded) return;
  scheduleDefaultsSave();
});
