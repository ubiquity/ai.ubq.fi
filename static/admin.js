const STORAGE_KEYS = {
  rememberToken: "ubq_ai.admin.remember_token",
  token: "ubq_ai.admin.token",
  expiresPreset: "ubq_ai.admin.expires_preset",
  base: "ubq_ai.admin.base",
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

const mustGet = (id) => {
  const el = document.getElementById(id);
  if (!el) throw new Error(`Missing element: #${id}`);
  return el;
};

const tokenInput = mustGet("admin-token");
const rememberTokenInput = mustGet("remember-admin-token");
const showTokenInput = mustGet("show-admin-token");
const clearTokenBtn = mustGet("clear-admin-token");
const testTokenBtn = mustGet("test-admin-token");
const authBadge = mustGet("admin-auth-badge");
const baseSelect = mustGet("admin-base");
const basePreview = mustGet("base-preview");

const keyNameInput = mustGet("key-name");
const keyUsageLimitInput = mustGet("key-usage-limit");
const keyExpiresSelect = mustGet("key-expires");
const createKeyBtn = mustGet("create-key");
const createBadge = mustGet("create-badge");
const createResult = mustGet("create-result");

const refreshKeysBtn = mustGet("refresh-keys");
const keysBadge = mustGet("keys-badge");
const keysList = mustGet("keys-list");

const keysTabAll = mustGet("keys-tab-all");
const keysTabActive = mustGet("keys-tab-active");
const keysTabRevoked = mustGet("keys-tab-revoked");

let currentView = "all";
let allKeys = [];

const reasoningLevelSelect = mustGet("reasoning-level");
const getReasoningBtn = mustGet("get-reasoning-level");
const setReasoningBtn = mustGet("set-reasoning-level");
const reasoningBadge = mustGet("reasoning-badge");

const setBadge = (badge, state, text) => {
  badge.dataset.state = state;
  badge.textContent = text;
};

const setAuthBadge = (state, text) => setBadge(authBadge, state, text);
const setCreateBadge = (state, text) => setBadge(createBadge, state, text);
const setKeysBadge = (state, text) => setBadge(keysBadge, state, text);
const setReasoningBadge = (state, text) => setBadge(reasoningBadge, state, text);

const getBaseChoice = () => (baseSelect.value === "ai" ? "ai" : "local");

const resolveBaseUrl = () => (getBaseChoice() === "ai" ? "https://ai.ubq.fi" : window.location.origin);

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
  keyExpiresSelect.value = storage.get(STORAGE_KEYS.expiresPreset) ?? "forever";
  baseSelect.value = storage.get(STORAGE_KEYS.base) ?? "local";
  updateBasePreview();
};

const clearCreateResult = () => {
  createResult.textContent = "";
  createResult.hidden = true;
};

const setKeyListMessage = (text) => {
  keysList.textContent = "";
  const message = document.createElement("div");
  message.className = "key-empty";
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

const formatDate = (ms) => {
  if (typeof ms !== "number" || !Number.isFinite(ms)) return "unknown";
  return dateFormatter.format(new Date(ms));
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

const compactId = (value) => {
  if (typeof value !== "string") return "";
  const trimmed = value.trim();
  if (trimmed.length <= 16) return trimmed;
  return `${trimmed.slice(0, 8)}...${trimmed.slice(-4)}`;
};

const appendMetaItem = (container, label, value, options = {}) => {
  const item = document.createElement("div");
  item.className = "key-meta-item";
  if (options.state) item.dataset.state = options.state;

  const labelEl = document.createElement("span");
  labelEl.className = "key-meta-label";
  labelEl.textContent = label;

  const valueEl = document.createElement("span");
  valueEl.className = `key-meta-value${options.mono ? " mono" : ""}`;
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
  item.className = "key-info-item";
  if (options.state) item.dataset.state = options.state;

  const labelEl = document.createElement("span");
  labelEl.className = "key-info-label";
  labelEl.textContent = label;

  const valueEl = document.createElement("span");
  valueEl.className = `key-info-value${options.mono ? " mono" : ""}`;
  valueEl.textContent = value;
  if (options.title) valueEl.title = options.title;

  item.appendChild(labelEl);
  item.appendChild(valueEl);
  container.appendChild(item);
};

const appendUsagePill = (container, label, value = "", options = {}) => {
  const pill = document.createElement("span");
  pill.className = "key-usage-pill";
  if (options.state) pill.dataset.state = options.state;
  pill.textContent = value ? `${label} ${value}` : label;
  if (options.title) pill.title = options.title;
  container.appendChild(pill);
};

const buildUsageSummary = (usage) => {
  const summary = document.createElement("div");
  summary.className = "key-usage-summary";

  if (!usage || typeof usage !== "object") {
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

  for (const key of filteredKeys) {
    const row = document.createElement("div");
    row.className = "key-row";
    row.dataset.state = key.revoked_at_ms ? "revoked" : "active";

    const main = document.createElement("div");
    main.className = "key-main";

    const header = document.createElement("div");
    header.className = "key-header";

    const title = document.createElement("div");
    title.className = "key-title";
    title.textContent = key.name || "Untitled";

    const controls = document.createElement("div");
    controls.className = "key-controls";

    const infoRow = document.createElement("div");
    infoRow.className = "key-info-row";
    appendKeyInfo(infoRow, "ID", compactId(key.id), { title: key.id, mono: true });
    appendKeyInfo(infoRow, "Prefix", key.prefix, { title: key.prefix, mono: true });
    appendKeyInfo(infoRow, "Created", formatDate(key.created_at_ms));
    appendKeyInfo(infoRow, "Expires", formatExpires(key.expires_at_ms));
    if (key.revoked_at_ms) {
      appendKeyInfo(infoRow, "Revoked", formatDate(key.revoked_at_ms), { state: "bad" });
    }

    // Display usage limit information
    if (typeof key.usage_limit_requests === "number") {
      const limit = key.usage_limit_requests === -1 ? "Unlimited" : formatNumber(key.usage_limit_requests);
      const current = typeof key.usage_requests === "number" ? key.usage_requests : 0;
      const usageText = key.usage_limit_requests === -1 ? `${current}` : `${current}/${limit}`;
      const isNearLimit = key.usage_limit_requests !== -1 && current / key.usage_limit_requests >= 0.8;
      const isAtLimit = key.usage_limit_requests !== -1 && current >= key.usage_limit_requests;
      appendKeyInfo(infoRow, "Usage", usageText, {
        state: isAtLimit ? "bad" : (isNearLimit ? "warning" : ""),
        title: `${formatNumber(current)} requests${key.usage_limit_requests !== -1 ? ` of ${formatNumber(key.usage_limit_requests)} limit` : ""} (resets ${formatDate(key.usage_reset_at_ms)})`
      });
    }

    const status = document.createElement("span");
    status.className = "status-badge";
    status.dataset.state = key.revoked_at_ms ? "bad" : "ok";
    status.textContent = key.revoked_at_ms ? "Revoked" : "Active";
    controls.appendChild(status);

    if (!key.revoked_at_ms) {
      const revokeBtn = document.createElement("button");
      revokeBtn.type = "button";
      revokeBtn.className = "btn danger";
      revokeBtn.textContent = "Revoke";
      revokeBtn.addEventListener("click", () => {
        void revokeKey(key.id, key.name || "this key", revokeBtn);
      });
      controls.appendChild(revokeBtn);
    }

    header.appendChild(title);
    header.appendChild(controls);

    main.appendChild(header);
    main.appendChild(infoRow);

    const hasUsageField = Object.prototype.hasOwnProperty.call(key, "usage");
    const usage = hasUsageField ? key.usage : undefined;

    if (hasUsageField) {
      const usageSection = document.createElement("details");
      usageSection.className = "key-usage";

      const usageTitle = document.createElement("summary");
      usageTitle.className = "key-usage-title";

      const usageLabel = document.createElement("span");
      usageLabel.className = "key-usage-label";
      usageLabel.textContent = "Usage";

      usageTitle.appendChild(usageLabel);
      usageTitle.appendChild(buildUsageSummary(usage));
      usageSection.appendChild(usageTitle);

      if (!usage || typeof usage !== "object") {
        const empty = document.createElement("div");
        empty.className = "key-usage-empty";
        empty.textContent = "Usage unavailable.";
        usageSection.appendChild(empty);
      } else if (Object.keys(usage).length === 0) {
        const empty = document.createElement("div");
        empty.className = "key-usage-empty";
        empty.textContent = "No usage yet.";
        usageSection.appendChild(empty);
      } else {
        const usageList = document.createElement("div");
        usageList.className = "key-usage-list";
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

      main.appendChild(usageSection);
    }

    row.appendChild(main);
    keysList.appendChild(row);
  }
  setKeysBadge("ok", `${filteredKeys.length} keys`);
};

const switchKeysView = (view) => {
  currentView = view;
  keysTabAll.classList.toggle("tab-active", view === "all");
  keysTabActive.classList.toggle("tab-active", view === "active");
  keysTabRevoked.classList.toggle("tab-active", view === "revoked");
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

  setKeysBadge("unknown", "Loading...");
  refreshKeysBtn.disabled = true;

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
    renderKeys(allKeys, currentView);
  } catch {
    allKeys = [];
    setKeysBadge("bad", "Offline");
    setKeyListMessage("Failed to load keys.");
  } finally {
    refreshKeysBtn.disabled = false;
  }
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

const getReasoningLevel = async () => {
  const token = getAdminToken();
  if (!token) {
    setReasoningBadge("bad", "Missing token");
    return;
  }

  setReasoningBadge("unknown", "Loading...");

  try {
    const res = await fetch(apiUrl("/admin/reasoning-level"), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      setReasoningBadge("bad", data?.error?.message ?? "Error");
      return;
    }
    reasoningLevelSelect.value = data.effort;
    setReasoningBadge("ok", data.effort);
  } catch {
    setReasoningBadge("bad", "Offline");
  }
};

const setReasoningLevel = async () => {
  const token = getAdminToken();
  if (!token) {
    setReasoningBadge("bad", "Missing token");
    return;
  }

  const effort = reasoningLevelSelect.value;
  setReasoningBadge("unknown", "Setting...");

  try {
    const res = await fetch(apiUrl("/admin/reasoning-level"), {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${token}`,
      },
      body: JSON.stringify({ effort }),
    });
    const data = await res.json().catch(() => null);
    if (!res.ok) {
      setReasoningBadge("bad", data?.error?.message ?? "Error");
      return;
    }
    setReasoningBadge("ok", data.effort);
  } catch {
    setReasoningBadge("bad", "Offline");
  }
};

restoreSettings();
setAuthBadge("unknown", "Not checked");
setCreateBadge("unknown", "Idle");
setKeysBadge("unknown", "Not loaded");
setReasoningBadge("unknown", "Idle");
setKeyListMessage("Paste an admin token to load keys.");
switchKeysView("all");

showTokenInput.addEventListener("change", () => {
  tokenInput.type = showTokenInput.checked ? "text" : "password";
});

tokenInput.addEventListener("input", () => {
  persistTokenIfEnabled();
  setAuthBadge("unknown", "Not checked");
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

clearTokenBtn.addEventListener("click", () => {
  tokenInput.value = "";
  rememberTokenInput.checked = false;
  showTokenInput.checked = false;
  tokenInput.type = "password";
  storage.remove(STORAGE_KEYS.rememberToken);
  storage.remove(STORAGE_KEYS.token);
  setAuthBadge("unknown", "Not checked");
  setKeysBadge("unknown", "Not loaded");
  setReasoningBadge("unknown", "Idle");
  setKeyListMessage("Paste an admin token to load keys.");
  allKeys = [];
  currentView = "all";
  tokenInput.focus();
});

baseSelect.addEventListener("change", () => {
  storage.set(STORAGE_KEYS.base, getBaseChoice());
  updateBasePreview();
  setAuthBadge("unknown", "Not checked");
  setCreateBadge("unknown", "Idle");
  setKeysBadge("unknown", "Not loaded");
  setReasoningBadge("unknown", "Idle");
  setKeyListMessage("Target changed. Refresh keys.");
  clearCreateResult();
  allKeys = [];
});

keyExpiresSelect.addEventListener("change", () => {
  storage.set(STORAGE_KEYS.expiresPreset, keyExpiresSelect.value);
});

testTokenBtn.addEventListener("click", () => {
  void testAdminToken();
});

createKeyBtn.addEventListener("click", () => {
  void createKey();
});

refreshKeysBtn.addEventListener("click", () => {
  void refreshKeys();
});

getReasoningBtn.addEventListener("click", () => {
  void getReasoningLevel();
});

setReasoningBtn.addEventListener("click", () => {
  void setReasoningLevel();
});

keysTabAll.addEventListener("click", () => switchKeysView("all"));
keysTabActive.addEventListener("click", () => switchKeysView("active"));
keysTabRevoked.addEventListener("click", () => switchKeysView("revoked"));
