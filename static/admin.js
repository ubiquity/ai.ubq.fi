import "./network.js";

const STORAGE_KEYS = {
  rememberToken: "ubq_ai.admin.remember_token",
  token: "ubq_ai.admin.token",
  expiresPreset: "ubq_ai.admin.expires_preset",
  base: "ubq_ai.admin.base",
  view: "ubq_ai.admin.view",
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
const viewTabDefaults = mustGet("view-tab-defaults");

const viewSession = mustGet("view-session");
const viewKeys = mustGet("view-keys");
const viewKernel = mustGet("view-kernel");
const viewDefaults = mustGet("view-defaults");

let currentKeyView = "all";
let currentAdminView = "session";
let allKeys = [];
let keysLoading = false;
let keysLoadedAt = 0;

const defaultsModelSelect = mustGet("defaults-model");
const defaultsReasoningSelect = mustGet("defaults-reasoning");
const defaultsBadge = mustGet("defaults-badge");
const defaultsMeta = mustGet("defaults-meta");
let defaultsLoaded = false;
let defaultsSaving = false;
let defaultsModelMap = new Map();

const kernelModeUsage = mustGet("kernel-mode-usage");
const kernelModeLimits = mustGet("kernel-mode-limits");
const kernelScopeRepo = mustGet("kernel-scope-repo");
const kernelScopeOrg = mustGet("kernel-scope-org");
const kernelListBadge = mustGet("kernel-list-badge");
const kernelList = mustGet("kernel-list");
const kernelFilterInput = mustGet("kernel-filter");
const kernelShowSelect = mustGet("kernel-show");
const kernelSortSelect = mustGet("kernel-sort");
const kernelHelpLimits = mustGet("kernel-help-limits");
const kernelHelpUsage = mustGet("kernel-help-usage");
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
const kernelPubKeyNewToggle = mustGet("kernel-pubkey-new-toggle");
const kernelPubKeyNewPanel = mustGet("kernel-pubkey-new-panel");
const kernelPubKeyAppIdInput = mustGet("kernel-pubkey-app-id");
const kernelPubKeyOwnerInput = mustGet("kernel-pubkey-owner");
const kernelPubKeyPemInput = mustGet("kernel-pubkey-pem");
const kernelPubKeyCreateBtn = mustGet("kernel-pubkey-create");
const kernelPubKeyCreateBadge = mustGet("kernel-pubkey-create-badge");
let kernelScope = "org";
let kernelMode = "usage";
let kernelListLoadId = 0;
let kernelNewSaving = false;
let kernelListRecords = [];
let kernelListScope = "org";
let kernelListMode = "usage";
let kernelPubKeys = [];
let kernelPubKeysLoading = false;
let kernelPubKeysLoadedAt = 0;
let kernelPubKeysSaving = false;
let kernelPolicyState = {
  available: false,
  message: "",
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
const KERNEL_SHOW_OPTIONS_LIMITS = [
  { value: "all", label: "All" },
  { value: "active", label: "Active (30d)" },
  { value: "near-limit", label: "Near limit" },
  { value: "no-usage", label: "No usage" },
];
const KERNEL_SHOW_OPTIONS_USAGE = [
  { value: "all", label: "All" },
  { value: "active", label: "Active (30d)" },
  { value: "no-usage", label: "No usage" },
];

applyIconButton(kernelNewSaveBtn, "save", "Save limit");
applyIconButton(kernelPubKeyNewToggle, "plus", "New key");
applyIconButton(kernelPubKeyCreateBtn, "save", "Save key");

const setKernelListMessage = (text) => {
  kernelList.textContent = "";
  const message = document.createElement("p");
  message.dataset.empty = "kernel";
  message.textContent = text;
  kernelList.appendChild(message);
};

const getKernelListMissingTokenMessage = () =>
  kernelMode === "usage" ? "Paste an admin token to load usage." : "Paste an admin token to load limits.";

const getKernelListTargetChangedMessage = () =>
  kernelMode === "usage" ? "Target changed. Loading usage..." : "Target changed. Loading limits...";

const setKernelShowOptions = (options) => {
  const current = kernelShowSelect.value;
  kernelShowSelect.textContent = "";
  options.forEach((option) => {
    const opt = document.createElement("option");
    opt.value = option.value;
    opt.textContent = option.label;
    kernelShowSelect.appendChild(opt);
  });
  const next = options.some((option) => option.value === current) ? current : options[0]?.value ?? "";
  kernelShowSelect.value = next;
};

const setKernelMode = (mode) => {
  kernelMode = mode === "limits" ? "limits" : "usage";
  setTabState(kernelModeUsage, kernelMode === "usage");
  setTabState(kernelModeLimits, kernelMode === "limits");
  kernelHelpUsage.hidden = kernelMode !== "usage";
  kernelHelpLimits.hidden = kernelMode !== "limits";
  kernelNewToggle.hidden = kernelMode !== "limits";
  if (kernelMode !== "usage") {
    setKernelAttention("");
  }
  if (kernelMode !== "limits") {
    setKernelNewPanelOpen(false);
  }
  setKernelShowOptions(kernelMode === "limits" ? KERNEL_SHOW_OPTIONS_LIMITS : KERNEL_SHOW_OPTIONS_USAGE);
  if (currentAdminView === "kernel") {
    void loadKernelList();
  }
};

const normalizeKernelFilterText = (value) => value.trim().toLowerCase();

const getKernelRecordLabel = (record, scope) => {
  const owner = typeof record.owner === "string" ? record.owner : "";
  const repo = typeof record.repo === "string" ? record.repo : "";
  return scope === "org" ? owner : `${owner}/${repo}`;
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

const getKernelUsageRequests = (record) => {
  if (typeof record?.usage_requests === "number" && Number.isFinite(record.usage_requests)) {
    return Math.trunc(record.usage_requests);
  }
  const usage = getKernelRecordUsage(record);
  if (!usage || typeof usage !== "object") return 0;
  return toNumber(usage.total_requests);
};

const isKernelUsageEmpty = (usage) => {
  if (usage === null) return true;
  if (!usage || typeof usage !== "object") return false;
  return toNumber(usage.total_requests) <= 0;
};

const getKernelLimitValue = (record) => {
  if (!record || typeof record !== "object") return null;
  const limit = record.usage_limit_requests;
  if (typeof limit !== "number" || !Number.isFinite(limit)) return null;
  return Math.trunc(limit);
};

const buildKernelPolicySummary = (record, options = {}) => {
  const warnWhenMissing = options.warnWhenMissing === true;
  const warnWhenUnlimited = options.warnWhenUnlimited === true;
  if (!record || typeof record !== "object") {
    return {
      text: "Not set",
      state: warnWhenMissing ? "warning" : "",
      title: "No limit policy set. Defaults to unlimited.",
    };
  }
  const limitValue = getKernelLimitValue(record);
  if (limitValue === null) {
    return {
      text: "Unknown",
      state: warnWhenMissing ? "warning" : "",
      title: "Limit data unavailable.",
    };
  }
  if (limitValue === 0) {
    return {
      text: "Blocked",
      state: "bad",
      title: "Limit set to 0. Requests blocked.",
    };
  }
  if (limitValue === -1) {
    return {
      text: "Unlimited",
      state: warnWhenUnlimited ? "warning" : "",
      title: `Unlimited (window ${formatWindowMs(record.window_ms)})`,
    };
  }
  const windowLabel = formatWindowShort(record.window_ms);
  return {
    text: `${formatNumber(limitValue)} / ${windowLabel}`,
    state: "",
    title: `${formatNumber(limitValue)} requests per ${formatWindowMs(record.window_ms)}`,
  };
};

const resolveKernelPolicyStatus = (record, scope, policyState) => {
  if (!policyState?.available) {
    return { org: null, repo: null, hasCap: false };
  }
  const owner = typeof record?.owner === "string" ? record.owner : "";
  const repo = typeof record?.repo === "string" ? record.repo : "";
  const orgLimit = owner ? policyState.orgLimits.get(owner) ?? null : null;
  const repoLimit = scope === "repo" && owner && repo ? policyState.repoLimits.get(`${owner}/${repo}`) ?? null : null;

  const orgLimitValue = getKernelLimitValue(orgLimit);
  const repoLimitValue = getKernelLimitValue(repoLimit);
  const orgHasCap = orgLimitValue !== null && orgLimitValue !== -1;
  const repoHasCap = repoLimitValue !== null && repoLimitValue !== -1;
  const hasCap = orgHasCap || repoHasCap;

  const summaryOptions = { warnWhenMissing: !hasCap, warnWhenUnlimited: !hasCap };
  return {
    org: buildKernelPolicySummary(orgLimit, summaryOptions),
    repo: scope === "repo" ? buildKernelPolicySummary(repoLimit, summaryOptions) : null,
    hasCap,
  };
};

const buildKernelPolicyRow = (record, scope, policyState) => {
  const policyRow = document.createElement("div");
  policyRow.dataset.keyInfo = "info";

  if (!policyState?.available) {
    appendKeyInfo(policyRow, "Policy", "Unavailable", { state: "warning" });
    return policyRow;
  }

  const policy = resolveKernelPolicyStatus(record, scope, policyState);
  if (policy.org) {
    appendKeyInfo(policyRow, "Org limit", policy.org.text, {
      state: policy.org.state,
      title: policy.org.title,
    });
  }
  if (scope === "repo") {
    const repoSummary = policy.repo;
    appendKeyInfo(policyRow, "Repo limit", repoSummary?.text ?? "Not set", {
      state: repoSummary?.state ?? "",
      title: repoSummary?.title,
    });
  }

  return policyRow;
};

const summarizeKernelPolicyCoverage = (records, scope, policyState) => {
  let total = 0;
  let unbounded = 0;

  records.forEach((record) => {
    if (getKernelUsageRequests(record) <= 0) return;
    total += 1;
    if (policyState?.available) {
      const policy = resolveKernelPolicyStatus(record, scope, policyState);
      if (!policy.hasCap) unbounded += 1;
    }
  });

  return { total, unbounded };
};

const updateKernelAttention = (summary, scope, listMode, policyState) => {
  if (listMode !== "usage") {
    setKernelAttention("");
    return;
  }
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
  const label = scope === "org" ? "orgs" : "repos";
  const countText = `${summary.unbounded} of ${summary.total}`;
  setKernelAttention(`Attention: ${countText} ${label} with usage have no caps (limits unset or unlimited).`);
};

const applyKernelFilters = (records, scope, listMode) => {
  const filterText = normalizeKernelFilterText(kernelFilterInput.value);
  const filterMode = kernelShowSelect.value;
  const nowMs = Date.now();

  return records.filter((record) => {
    const label = getKernelRecordLabel(record, scope).toLowerCase();
    if (filterText && !label.includes(filterText)) return false;

    const usage = getKernelRecordUsage(record);
    if (filterMode === "active") {
      const lastSeen = getKernelUsageLastSeen(usage);
      return lastSeen !== null && lastSeen >= nowMs - KERNEL_ACTIVITY_WINDOW_MS;
    }
    if (filterMode === "near-limit") {
      if (listMode !== "limits") return false;
      const limit = typeof record.usage_limit_requests === "number" ? record.usage_limit_requests : null;
      if (limit === null || limit <= 0 || limit === -1) return false;
      const current = getKernelUsageRequests(record);
      return current / limit >= KERNEL_NEAR_LIMIT_RATIO;
    }
    if (filterMode === "no-usage") {
      return isKernelUsageEmpty(usage);
    }
    return true;
  });
};

const sortKernelRecords = (records, scope) => {
  const mode = kernelSortSelect.value;
  const sorted = [...records];
  sorted.sort((a, b) => {
    if (mode === "recent") {
      const aSeen = getKernelUsageLastSeen(getKernelRecordUsage(a)) ?? 0;
      const bSeen = getKernelUsageLastSeen(getKernelRecordUsage(b)) ?? 0;
      if (aSeen !== bSeen) return bSeen - aSeen;
    } else if (mode === "usage") {
      const aUsage = getKernelUsageRequests(a);
      const bUsage = getKernelUsageRequests(b);
      if (aUsage !== bUsage) return bUsage - aUsage;
    }
    const ownerCmp = (typeof a.owner === "string" ? a.owner : "").localeCompare(
      typeof b.owner === "string" ? b.owner : "",
    );
    if (ownerCmp !== 0) return ownerCmp;
    if (scope === "org") return 0;
    return (typeof a.repo === "string" ? a.repo : "").localeCompare(typeof b.repo === "string" ? b.repo : "");
  });
  return sorted;
};

const formatKernelListBadge = (visible, total, listMode) => {
  const label = listMode === "usage" ? "entry" : "limit";
  const plural = listMode === "usage" ? "entries" : "limits";
  if (total === 0) return `No ${plural}`;
  if (visible === total) return `${total} ${total === 1 ? label : plural}`;
  return `${visible}/${total} shown`;
};

const refreshKernelList = () => {
  if (!Array.isArray(kernelListRecords)) {
    setKernelListMessage(kernelListMode === "usage" ? "No usage yet." : "No limits yet.");
    setKernelListBadge("ok", formatKernelListBadge(0, 0, kernelListMode));
    return;
  }
  const result = renderKernelList(kernelListRecords, kernelListScope, kernelListMode, kernelPolicyState);
  setKernelListBadge("ok", formatKernelListBadge(result.visible, result.total, kernelListMode));
};

const refreshKernelListDebounced = debounce(refreshKernelList, 250);

const setKernelPubKeysMessage = (text) => {
  kernelPubKeysList.textContent = "";
  const message = document.createElement("p");
  message.dataset.empty = "kernel-pubkeys";
  message.textContent = text;
  kernelPubKeysList.appendChild(message);
};

const setKernelScope = (scope) => {
  kernelScope = scope === "org" ? "org" : "repo";
  setTabState(kernelScopeRepo, kernelScope === "repo");
  setTabState(kernelScopeOrg, kernelScope === "org");
  const hideRepo = kernelScope === "org";
  kernelNewRepoField.hidden = hideRepo;
  kernelNewRepoInput.disabled = hideRepo;
  if (hideRepo) kernelNewRepoInput.value = "";
};

const setKernelNewPanelOpen = (open) => {
  kernelNewPanel.hidden = !open;
  applyIconButton(kernelNewToggle, open ? "close" : "plus", open ? "Close new limit" : "New limit");
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

const setKernelPubKeyNewPanelOpen = (open) => {
  kernelPubKeyNewPanel.hidden = !open;
  applyIconButton(kernelPubKeyNewToggle, open ? "close" : "plus", open ? "Close new key" : "New key");
  if (!open) {
    resetKernelPubKeyForm();
  }
};

const setKernelNewWindowPreset = (ms) => {
  kernelNewWindowInput.value = String(ms);
};

const parseKernelLimitValue = (raw, setBadgeFn) => {
  const trimmed = raw.trim();
  if (!trimmed) {
    setBadgeFn("bad", "Limit required");
    return null;
  }
  if (trimmed === "unlimited" || trimmed === "-1") return -1;
  const parsed = Number(trimmed);
  if (!Number.isFinite(parsed) || parsed < 0) {
    setBadgeFn("bad", "Invalid limit");
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
  if (kernelScope === "repo" && !repo) {
    setKernelNewBadge("bad", "Repo required");
    kernelNewRepoInput.focus();
    return;
  }
  if (kernelScope === "org" && repo) {
    setKernelNewBadge("bad", "Repo not allowed");
    kernelNewRepoInput.focus();
    return;
  }

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
      scope: kernelScope,
      usage_limit_requests: limitValue,
    };
    if (kernelScope === "repo") payload.repo = repo;
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

const fetchKernelLimitList = async (token, scope) => {
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
      return { ok: false, message: data?.error?.message ?? "Error", limits: [] };
    }
    return { ok: true, message: "", limits: Array.isArray(data?.limits) ? data.limits : [] };
  } catch {
    return { ok: false, message: "Offline", limits: [] };
  }
};

const loadKernelPolicyState = async (token, scope) => {
  const orgResult = await fetchKernelLimitList(token, "org");
  if (!orgResult.ok) {
    return { available: false, message: orgResult.message, orgLimits: new Map(), repoLimits: new Map() };
  }
  const orgLimits = new Map();
  orgResult.limits.forEach((record) => {
    const owner = typeof record?.owner === "string" ? record.owner : "";
    if (owner) orgLimits.set(owner, record);
  });

  const repoLimits = new Map();
  if (scope === "repo") {
    const repoResult = await fetchKernelLimitList(token, "repo");
    if (!repoResult.ok) {
      return { available: false, message: repoResult.message, orgLimits: new Map(), repoLimits: new Map() };
    }
    repoResult.limits.forEach((record) => {
      const owner = typeof record?.owner === "string" ? record.owner : "";
      const repo = typeof record?.repo === "string" ? record.repo : "";
      if (owner && repo) repoLimits.set(`${owner}/${repo}`, record);
    });
  }

  return { available: true, message: "", orgLimits, repoLimits };
};

const loadKernelList = async () => {
  const token = getAdminToken();
  if (!token) {
    setKernelListBadge("bad", "Missing token");
    setAuthBadge("bad", "Missing token");
    setKernelListMessage(getKernelListMissingTokenMessage());
    resetKernelPolicyState();
    setKernelAttention("");
    return;
  }

  const loadId = ++kernelListLoadId;
  setKernelListBadge("unknown", "Loading...");
  resetKernelPolicyState();
  setKernelAttention("");

  try {
    const url = new URL(apiUrl("/admin/kernel-usage"));
    url.searchParams.set("scope", kernelScope);
    url.searchParams.set("list", "1");
    if (kernelMode === "usage") {
      url.searchParams.set("inventory", "1");
    } else {
      url.searchParams.set("include_usage", "1");
    }

    const res = await fetch(url.toString(), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (loadId !== kernelListLoadId) return;
    if (!res.ok) {
      const message = data?.error?.message ?? "Error";
      setKernelListBadge("bad", message);
      setKernelListMessage(message);
      return;
    }

    const scope = data?.scope === "org" ? "org" : "repo";
    if (kernelMode === "usage") {
      const policyState = await loadKernelPolicyState(token, scope);
      if (loadId !== kernelListLoadId) return;
      kernelPolicyState = policyState;
    }
    const records = kernelMode === "usage"
      ? (Array.isArray(data?.usage) ? data.usage : [])
      : (Array.isArray(data?.limits) ? data.limits : []);
    kernelListRecords = records;
    kernelListScope = scope;
    kernelListMode = kernelMode;
    const result = renderKernelList(records, scope, kernelMode, kernelPolicyState);
    setKernelListBadge("ok", formatKernelListBadge(result.visible, result.total, kernelMode));
  } catch {
    if (loadId !== kernelListLoadId) return;
    setKernelListBadge("bad", "Offline");
    setKernelListMessage("Request failed.");
  }
};

const renderKernelUsageRow = (record, scope, policyState) => {
  if (!record || typeof record !== "object") return;
  const owner = typeof record.owner === "string" ? record.owner : "";
  const repo = typeof record.repo === "string" ? record.repo : "";
  const displayOwner = owner || "unknown";
  const displayRepo = repo || "unknown";
  const titleText = scope === "org" ? displayOwner : `${displayOwner}/${displayRepo}`;

  const row = document.createElement("article");
  row.dataset.key = "kernel-usage";

  const usage = getKernelRecordUsage(record);
  const lastSeen = getKernelUsageLastSeen(usage);
  if (lastSeen && lastSeen >= Date.now() - KERNEL_ACTIVITY_WINDOW_MS) {
    row.dataset.state = "active";
  }

  const main = document.createElement("div");
  main.dataset.keyMain = "main";

  const header = document.createElement("div");
  header.dataset.keyHeader = "header";

  const title = document.createElement("div");
  title.dataset.keyTitle = "title";
  title.textContent = titleText;

  header.appendChild(title);
  main.appendChild(header);

  const infoRow = document.createElement("div");
  infoRow.dataset.keyInfo = "info";
  appendKeyInfo(infoRow, "Requests", formatNumber(usage?.total_requests));
  appendKeyInfo(infoRow, "Tokens", formatNumber(usage?.total_tokens));
  const errorCount = toNumber(usage?.error_requests);
  appendKeyInfo(infoRow, "Errors", formatNumber(errorCount), { state: errorCount > 0 ? "bad" : "" });
  appendKeyInfo(infoRow, "Last seen", formatDate(usage?.last_seen_at_ms));
  main.appendChild(infoRow);
  main.appendChild(buildKernelPolicyRow(record, scope, policyState));

  main.appendChild(buildUsageDetails(usage));
  row.appendChild(main);
  kernelList.appendChild(row);
};

const renderKernelList = (records, scope, listMode, policyState = kernelPolicyState) => {
  kernelList.textContent = "";
  if (!Array.isArray(records) || records.length === 0) {
    setKernelListMessage(listMode === "usage" ? "No usage yet." : "No limits yet.");
    updateKernelAttention({ total: 0, unbounded: 0 }, scope, listMode, policyState);
    return { total: 0, visible: 0 };
  }

  const filtered = sortKernelRecords(applyKernelFilters(records, scope, listMode), scope);
  if (!filtered.length) {
    setKernelListMessage("No matches.");
    updateKernelAttention({ total: 0, unbounded: 0 }, scope, listMode, policyState);
    return { total: records.length, visible: 0 };
  }

  if (listMode === "usage") {
    filtered.forEach((record) => {
      renderKernelUsageRow(record, scope, policyState);
    });
    const summary = summarizeKernelPolicyCoverage(filtered, scope, policyState);
    updateKernelAttention(summary, scope, listMode, policyState);
    return { total: records.length, visible: filtered.length };
  }

  filtered.forEach((limit) => {
    if (!limit || typeof limit !== "object") return;
    const record = { ...limit };
    const owner = typeof record.owner === "string" ? record.owner : "";
    const repo = typeof record.repo === "string" ? record.repo : "";
    const displayOwner = owner || "unknown";
    const displayRepo = repo || "unknown";
    const titleText = scope === "org" ? displayOwner : `${displayOwner}/${displayRepo}`;

    const row = document.createElement("article");
    row.dataset.key = "kernel";
    row.dataset.state = record.usage_limit_requests === 0 ? "revoked" : "active";

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
    applyIconButton(editBtn, "edit", "Edit limit");

    const deleteBtn = document.createElement("button");
    deleteBtn.type = "button";
    deleteBtn.dataset.variant = "danger";
    applyIconButton(deleteBtn, "trash", "Delete limit");

    actionRow.appendChild(editBtn);
    actionRow.appendChild(deleteBtn);
    controls.appendChild(actionRow);
    header.appendChild(title);
    header.appendChild(controls);

    const infoRow = document.createElement("div");
    infoRow.dataset.keyInfo = "info";

    const limitState = record.usage_limit_requests === 0 ? "bad" : "";
    const limitInfo = appendKeyInfo(
      infoRow,
      "Limit",
      formatLimitValue(record.usage_limit_requests),
      limitState ? { state: limitState } : {},
    );
    const windowInfo = appendKeyInfo(infoRow, "Window", formatWindowMs(record.window_ms));
    const usageInfo = appendKeyInfo(infoRow, "Usage", formatNumber(record.usage_requests));
    const resetInfo = appendKeyInfo(infoRow, "Reset at", formatDate(record.usage_reset_at_ms));
    const updatedInfo = appendKeyInfo(infoRow, "Updated", formatDate(record.updated_at_ms));

    main.appendChild(header);
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
    limitLabel.textContent = "Usage Limit";
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
    editHelp.textContent = "Leave blank to keep the current interval. Updates reset usage.";

    editPanel.appendChild(editFields);
    editPanel.appendChild(presetRow);
    editPanel.appendChild(editActions);
    editPanel.appendChild(editBadge);
    editPanel.appendChild(editHelp);

    main.appendChild(editPanel);
    const hasUsageField = Object.prototype.hasOwnProperty.call(record, "usage");
    const usage = hasUsageField ? record.usage : undefined;
    if (hasUsageField) {
      main.appendChild(buildUsageDetails(usage));
    }
    row.appendChild(main);
    kernelList.appendChild(row);

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
      if (!confirm(`Delete kernel limit for ${titleText}?`)) return;

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
  });

  updateKernelAttention(null, scope, listMode, policyState);
  return { total: records.length, visible: filtered.length };
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
    setKernelPubKeysMessage("No public keys yet.");
    return;
  }

  const sorted = [...records].sort((a, b) => {
    const aId = typeof a?.app_id === "number" ? a.app_id : 0;
    const bId = typeof b?.app_id === "number" ? b.app_id : 0;
    return aId - bId;
  });

  sorted.forEach((record) => {
    if (!record || typeof record !== "object") return;
    const appId = typeof record.app_id === "number" ? Math.trunc(record.app_id) : null;
    const owner = typeof record.owner === "string" ? record.owner : "";
    const pem = typeof record.pem === "string" ? record.pem : "";
    const addedAt = typeof record.added_at_ms === "number" ? record.added_at_ms : null;

    const row = document.createElement("article");
    row.dataset.key = "kernel-pubkey";
    row.dataset.state = "active";

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
    applyIconButton(deleteBtn, "trash", appId ? `Delete App ${appId}` : "Delete public key");

    actionRow.appendChild(deleteBtn);
    controls.appendChild(actionRow);
    header.appendChild(title);
    header.appendChild(controls);

    const infoRow = document.createElement("div");
    infoRow.dataset.keyInfo = "info";
    appendKeyInfo(infoRow, "App ID", appId ? String(appId) : "unknown", { mono: true });
    appendKeyInfo(infoRow, "Owner", formatOptionalText(owner));
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
      label.textContent = "Public key";

      const summaryMeta = document.createElement("span");
      summaryMeta.dataset.usageSummary = "summary";
      summaryMeta.textContent = formatPemPreview(pem) || "PEM";

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
};

const refreshKernelPubKeys = async () => {
  const token = getAdminToken();
  if (!token) {
    setKernelPubKeysBadge("bad", "Missing token");
    setKernelPubKeysMessage("Paste an admin token to load public keys.");
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
      setKernelPubKeysMessage("Failed to load public keys.");
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
    setKernelPubKeysMessage("Failed to load public keys.");
  } finally {
    kernelPubKeysLoading = false;
  }
};

const ensureKernelPubKeysLoaded = async () => {
  if (currentAdminView !== "kernel") return;
  if (kernelPubKeysLoading) return;
  const token = getAdminToken();
  if (!token) {
    setKernelPubKeysBadge("bad", "Missing token");
    setKernelPubKeysMessage("Paste an admin token to load public keys.");
    return;
  }
  if (kernelPubKeysLoadedAt && Date.now() - kernelPubKeysLoadedAt < 10_000) {
    setKernelPubKeysBadge("ok", `${kernelPubKeys.length} key${kernelPubKeys.length === 1 ? "" : "s"}`);
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

    kernelPubKeyAppIdInput.value = "";
    kernelPubKeyOwnerInput.value = "";
    kernelPubKeyPemInput.value = "";
    setKernelPubKeyCreateBadge("ok", "Saved");
    setKernelPubKeyNewPanelOpen(false);
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
  if (!confirm(`Delete public key for App ${appId}?`)) return;

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

const SPARKLINE_DAY_WIDTH = 24;
const SPARKLINE_HEIGHT = 42;
let sparklineCounter = 0;
const buildUsageSparkline = (usage) => {
  const daily = Array.isArray(usage?.daily_requests) ? usage.daily_requests : null;
  if (!daily || daily.length === 0) return null;

  const width = daily.length * SPARKLINE_DAY_WIDTH;
  const height = SPARKLINE_HEIGHT;
  const maxValue = Math.max(...daily);
  const scaleMax = maxValue > 0 ? maxValue : 1;
  const padY = 6;
  const xOffset = SPARKLINE_DAY_WIDTH / 2;

  const points = daily.map((value, index) => {
    const x = index * SPARKLINE_DAY_WIDTH + xOffset;
    const ratio = Math.min(1, value / scaleMax);
    const y = height - padY - ratio * (height - padY * 2);
    return `${x},${y}`;
  });

  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  const gradientId = `sparkline-${++sparklineCounter}`;
  svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
  svg.setAttribute("width", String(width));
  svg.setAttribute("height", String(height));
  svg.setAttribute("role", "img");
  svg.setAttribute("aria-label", `Last ${daily.length} days requests`);

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
  defs.appendChild(gradient);
  svg.appendChild(defs);

  const polyline = document.createElementNS("http://www.w3.org/2000/svg", "polyline");
  polyline.setAttribute("points", points.join(" "));
  polyline.setAttribute("fill", "none");
  polyline.setAttribute("stroke", `url(#${gradientId})`);
  polyline.setAttribute("stroke-width", "2");
  polyline.setAttribute("stroke-linecap", "round");
  polyline.setAttribute("stroke-linejoin", "round");
  polyline.dataset.usageSparkLine = "line";
  svg.appendChild(polyline);

  if (points.length === 1) {
    const [x, y] = points[0].split(",").map(Number);
    const dot = document.createElementNS("http://www.w3.org/2000/svg", "circle");
    dot.setAttribute("cx", String(x));
    dot.setAttribute("cy", String(y));
    dot.setAttribute("r", "2.5");
    dot.setAttribute("fill", "rgba(255, 255, 255, 0.8)");
    svg.appendChild(dot);
  }

  const container = document.createElement("div");
  container.dataset.usageSpark = "spark";
  container.style.setProperty("--spark-days", String(daily.length));
  container.style.setProperty("--spark-width", `${width}px`);
  container.appendChild(svg);
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

const buildUsageDetails = (usage) => {
  const usageSection = document.createElement("details");
  usageSection.dataset.usage = "details";

  const usageTitle = document.createElement("summary");
  usageTitle.dataset.usageTitle = "title";

  const usageLabel = document.createElement("span");
  usageLabel.dataset.usageLabel = "label";
  usageLabel.textContent = "Usage";

  usageTitle.appendChild(usageLabel);
  usageTitle.appendChild(buildUsageSummary(usage));
  usageSection.appendChild(usageTitle);

  const sparkline = buildUsageSparkline(usage);
  if (sparkline) usageSection.appendChild(sparkline);

  if (usage === undefined) {
    const empty = document.createElement("div");
    empty.dataset.usageEmpty = "empty";
    empty.textContent = "Usage unavailable.";
    usageSection.appendChild(empty);
  } else if (usage === null) {
    const empty = document.createElement("div");
    empty.dataset.usageEmpty = "empty";
    empty.textContent = "No usage yet.";
    usageSection.appendChild(empty);
  } else if (typeof usage !== "object") {
    const empty = document.createElement("div");
    empty.dataset.usageEmpty = "empty";
    empty.textContent = "Usage unavailable.";
    usageSection.appendChild(empty);
  } else if (Object.keys(usage).length === 0) {
    const empty = document.createElement("div");
    empty.dataset.usageEmpty = "empty";
    empty.textContent = "No usage yet.";
    usageSection.appendChild(empty);
  } else {
    const usageList = document.createElement("div");
    usageList.dataset.usageList = "list";
    appendMetaItem(usageList, "Requests", formatNumber(usage.total_requests));
    appendMetaItem(usageList, "Completed", formatNumber(usage.completed_requests));
    const errorCount = toNumber(usage.error_requests);
    appendMetaItem(usageList, "Errors", formatNumber(errorCount), { state: errorCount > 0 ? "bad" : "" });
    appendMetaItem(usageList, "Stream", formatNumber(usage.stream_requests));
    appendMetaItem(usageList, "Non-stream", formatNumber(usage.non_stream_requests));
    appendMetaItem(usageList, "Tokens in", formatNumber(usage.input_tokens));
    appendMetaItem(usageList, "Tokens out", formatNumber(usage.output_tokens));
    appendMetaItem(usageList, "Tokens total", formatNumber(usage.total_tokens));
    appendMetaItem(usageList, "First seen", formatDate(usage.first_seen_at_ms));
    appendMetaItem(usageList, "Last seen", formatDate(usage.last_seen_at_ms));
    appendMetaItem(usageList, "Last model", formatOptionalText(usage.last_model), { mono: true });
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
    setKeyListMessage(view === "all" ? "No keys yet." : `No ${view} keys.`);
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
        hasLimit ? ` of ${formatNumber(limitValue)} limit` : ""
      } (resets ${formatDate(key.usage_reset_at_ms)})`;
      const state = isAtLimit ? "bad" : (isNearLimit ? "warning" : "");
      return { usageText, title, state };
    };

    // Display usage limit information
    let usageInfo = null;
    if (typeof key.usage_limit_requests === "number") {
      const usageData = getUsageInfoData();
      usageInfo = appendKeyInfo(infoRow, "Usage", usageData.usageText, {
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
    applyIconButton(editBtn, "edit", "Edit key");
    actionRow.appendChild(editBtn);

    if (!key.revoked_at_ms) {
      const revokeBtn = document.createElement("button");
      revokeBtn.type = "button";
      revokeBtn.dataset.variant = "danger";
      applyIconButton(revokeBtn, "revoke", "Revoke key");
      revokeBtn.addEventListener("click", () => {
        void revokeKey(key.id, key.name || "this key", revokeBtn);
      });
      actionRow.appendChild(revokeBtn);
    } else {
      const restoreBtn = document.createElement("button");
      restoreBtn.type = "button";
      applyIconButton(restoreBtn, "restore", "Unrevoke key");
      restoreBtn.addEventListener("click", () => {
        void unrevokeKey(key.id, key.name || "this key", restoreBtn);
      });
      actionRow.appendChild(restoreBtn);

      const deleteBtn = document.createElement("button");
      deleteBtn.type = "button";
      deleteBtn.dataset.variant = "danger";
      applyIconButton(deleteBtn, "trash", "Delete key");
      deleteBtn.addEventListener("click", () => {
        void deleteKey(key.id, key.name || "this key", deleteBtn);
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
    limitLabel.textContent = "Usage Limit";
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
        setEditBadge("bad", "Limit required");
        return null;
      }
      let nextLimit;
      if (limitRaw === "unlimited" || limitRaw === "-1") {
        nextLimit = -1;
      } else {
        const parsed = Number(limitRaw);
        if (!Number.isFinite(parsed) || parsed < 0) {
          setEditBadge("bad", "Invalid limit");
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

        nameInput.value = key.name || "";
        limitInput.value = String(key.usage_limit_requests);
        neverInput.checked = key.expires_at_ms === -1;
        expiresInput.disabled = neverInput.checked;
        expiresInput.value = neverInput.checked ? "" : toDateTimeLocalValue(key.expires_at_ms);

        editSnapshot = {
          name: key.name || "",
          usage_limit_requests: key.usage_limit_requests,
          expires_at_ms: key.expires_at_ms,
        };
        editDirty = false;
        setEditBadge("ok", "Saved");
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
    }, 650);

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
      applyIconButton(editBtn, willOpen ? "close" : "edit", willOpen ? "Close editor" : "Edit key");
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
  setKeysBadge("ok", `${filteredKeys.length} keys`);
};

const setTabState = (tab, selected) => {
  tab.setAttribute("aria-selected", selected ? "true" : "false");
  tab.tabIndex = selected ? 0 : -1;
};

const viewTabs = {
  session: viewTabSession,
  keys: viewTabKeys,
  kernel: viewTabKernel,
  defaults: viewTabDefaults,
};

const viewSections = {
  session: viewSession,
  keys: viewKeys,
  kernel: viewKernel,
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
    setKeyListMessage("Paste an admin token to load keys.");
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
    setAuthBadge("ok", kind ? `OK (${kind})` : "OK");
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
      `usage_limit: ${data?.usage_limit_requests === -1 ? "unlimited" : (data?.usage_limit_requests ?? "")}`,
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
    setKeyListMessage("Paste an admin token to load keys.");
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
      setKeyListMessage("Failed to load keys.");
      return;
    }
    const keys = Array.isArray(data?.data) ? data.data : [];
    allKeys = keys;
    keysLoadedAt = Date.now();
    renderKeys(allKeys, currentKeyView);
  } catch {
    allKeys = [];
    setKeysBadge("bad", "Offline");
    setKeyListMessage("Failed to load keys.");
  } finally {
    keysLoading = false;
  }
};

const ensureKeysLoaded = async () => {
  if (currentAdminView !== "keys") return;
  if (keysLoading) return;
  const token = getAdminToken();
  if (!token) {
    setKeysBadge("bad", "Missing token");
    setKeyListMessage("Paste an admin token to load keys.");
    return;
  }
  if (allKeys.length && Date.now() - keysLoadedAt < 10_000) {
    setKeysBadge("ok", `${allKeys.length} keys`);
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
setKernelPubKeysBadge("unknown", "Not loaded");
setKernelPubKeyCreateBadge("unknown", "Idle");
setKeyListMessage("Paste an admin token to load keys.");
setKernelListMessage(getKernelListMissingTokenMessage());
setKernelPubKeysMessage("Paste an admin token to load public keys.");
kernelFilterInput.value = "";
kernelShowSelect.value = "all";
switchKeysView("all");
setKernelMode("usage");
setKernelScope("org");
setKernelNewPanelOpen(false);
setKernelPubKeyNewPanelOpen(false);
setAdminView(storage.get(STORAGE_KEYS.view) ?? "session");
if (getAdminToken()) scheduleTokenCheck();

showTokenInput.addEventListener("change", () => {
  tokenInput.type = showTokenInput.checked ? "text" : "password";
});

tokenInput.addEventListener("input", () => {
  persistTokenIfEnabled();
  keysLoadedAt = 0;
  defaultsLoaded = false;
  kernelPubKeysLoadedAt = 0;
  if (!getAdminToken()) {
    setAuthBadge("bad", "Missing token");
    setKeysBadge("unknown", "Not loaded");
    setKeyListMessage("Paste an admin token to load keys.");
    setKernelListBadge("unknown", "Not loaded");
    setKernelListMessage(getKernelListMissingTokenMessage());
    setKernelPubKeysBadge("unknown", "Not loaded");
    setKernelPubKeysMessage("Paste an admin token to load public keys.");
    allKeys = [];
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
  setKernelPubKeysBadge("unknown", "Not loaded");
  setKernelPubKeyCreateBadge("unknown", "Idle");
  setKeyListMessage("Target changed. Loading keys...");
  setKernelListMessage(getKernelListTargetChangedMessage());
  setKernelPubKeysMessage("Target changed. Loading public keys...");
  clearCreateResult();
  setKernelNewPanelOpen(false);
  setKernelPubKeyNewPanelOpen(false);
  allKeys = [];
  keysLoadedAt = 0;
  defaultsLoaded = false;
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
    void ensureKernelPubKeysLoaded();
  }
});

keyExpiresSelect.addEventListener("change", () => {
  storage.set(STORAGE_KEYS.expiresPreset, keyExpiresSelect.value);
});

viewTabSession.addEventListener("click", () => setAdminView("session"));
viewTabKeys.addEventListener("click", () => setAdminView("keys"));
viewTabKernel.addEventListener("click", () => setAdminView("kernel"));
viewTabDefaults.addEventListener("click", () => setAdminView("defaults"));

kernelModeUsage.addEventListener("click", () => setKernelMode("usage"));
kernelModeLimits.addEventListener("click", () => setKernelMode("limits"));

createKeyBtn.addEventListener("click", () => {
  void createKey();
});

kernelScopeRepo.addEventListener("click", () => {
  setKernelScope("repo");
  setKernelNewPanelOpen(false);
  if (currentAdminView === "kernel") {
    void loadKernelList();
  }
});
kernelScopeOrg.addEventListener("click", () => {
  setKernelScope("org");
  setKernelNewPanelOpen(false);
  if (currentAdminView === "kernel") {
    void loadKernelList();
  }
});
kernelNewToggle.addEventListener("click", () => {
  const nextOpen = kernelNewPanel.hidden;
  setKernelNewPanelOpen(nextOpen);
  if (nextOpen) kernelNewOwnerInput.focus();
});

kernelPubKeyNewToggle.addEventListener("click", () => {
  const nextOpen = kernelPubKeyNewPanel.hidden;
  setKernelPubKeyNewPanelOpen(nextOpen);
  if (nextOpen) kernelPubKeyAppIdInput.focus();
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
