import "./network.js";

const STORAGE_KEYS = {
  rememberToken: "uos_ai.playground.remember_token",
  token: "uos_ai.playground.token",
  model: "uos_ai.playground.model",
  reasoningEffort: "uos_ai.playground.reasoning_effort",
  systemPrompt: "uos_ai.playground.system_prompt",
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

const PANEL_STATE_PREFIX = "uos_ai.playground.panel.";

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

const tokenInput = mustGet("token");
const rememberTokenInput = mustGet("remember-token");
const showTokenInput = mustGet("show-token");
const modelInput = mustGet("model");
const reasoningSelect = mustGet("reasoning-effort");
const systemInput = mustGet("system");
const authBadge = mustGet("auth-badge");
const messagesEl = mustGet("messages");
const resetChatBtn = mustGet("reset-chat");
const stopBtn = mustGet("stop");
const chatForm = mustGet("chat-form");
const promptInput = mustGet("prompt");
const sendBtn = mustGet("send");
const panels = Array.from(document.querySelectorAll("details[data-chat-panel][data-panel-key]"));

const DEFAULT_MODEL = "";

const setAuthBadge = (state, text) => {
  authBadge.dataset.state = state;
  authBadge.textContent = text;
};

let modelsRequestId = 0;
let modelsLoadedToken = "";
let modelCatalog = new Map();
let preferredModel = DEFAULT_MODEL;

const normalizeModelId = (model) => {
  if (!model || typeof model !== "object") return "";
  const id = typeof model.id === "string" ? model.id : typeof model.slug === "string" ? model.slug : "";
  return id.trim();
};

const formatModelLabel = (model, fallback) => {
  const display = typeof model?.display_name === "string" ? model.display_name.trim() : "";
  if (display) return display;
  const name = typeof model?.name === "string" ? model.name.trim() : "";
  if (name) return name;
  return fallback;
};

const setModelPlaceholder = (label) => {
  modelInput.textContent = "";
  const option = document.createElement("option");
  option.value = "";
  option.textContent = label;
  option.disabled = true;
  modelInput.appendChild(option);
  modelInput.disabled = true;
};

const setReasoningPlaceholder = (label) => {
  reasoningSelect.textContent = "";
  const option = document.createElement("option");
  option.value = "";
  option.textContent = label;
  option.disabled = true;
  reasoningSelect.appendChild(option);
  reasoningSelect.disabled = true;
};

const setModelOptions = (models, preferred) => {
  modelInput.textContent = "";
  const options = models
    .map((model) => {
      const id = normalizeModelId(model);
      if (!id) return null;
      return { value: id, label: formatModelLabel(model, id), model };
    })
    .filter(Boolean);

  if (!options.length) {
    setModelPlaceholder("No models available");
    return "";
  }

  modelInput.disabled = false;
  options.forEach((option) => {
    const opt = document.createElement("option");
    opt.value = option.value;
    opt.textContent = option.label;
    modelInput.appendChild(opt);
  });

  const preferredValue = typeof preferred === "string" ? preferred.trim() : "";
  const fallback = options[0].value;
  const hasStored = options.some((option) => option.value === preferredValue);
  const next = hasStored ? preferredValue : fallback;
  modelInput.value = next;
  return next;
};

const setReasoningOptions = (levels, preferred) => {
  const trimmedPreferred = (preferred ?? "").trim();
  const uniqueLevels = Array.from(
    new Set(levels.filter((level) => typeof level === "string").map((level) => level.trim()).filter(Boolean)),
  );

  reasoningSelect.textContent = "";
  const defaultOption = document.createElement("option");
  defaultOption.value = "";
  defaultOption.textContent = "Default";
  reasoningSelect.appendChild(defaultOption);

  if (!uniqueLevels.length) {
    reasoningSelect.disabled = true;
    reasoningSelect.value = "";
    return "";
  }

  reasoningSelect.disabled = false;

  uniqueLevels.forEach((level) => {
    const option = document.createElement("option");
    option.value = level;
    option.textContent = level;
    reasoningSelect.appendChild(option);
  });

  reasoningSelect.value = uniqueLevels.includes(trimmedPreferred) ? trimmedPreferred : "";
};

const getReasoningLevelsForModel = (modelId) => {
  const model = modelCatalog.get(modelId);
  const levels = Array.isArray(model?.supported_reasoning_levels) ? model.supported_reasoning_levels : [];
  const normalized = [];
  levels.forEach((level) => {
    if (typeof level === "string") {
      const trimmed = level.trim();
      if (trimmed) normalized.push(trimmed);
      return;
    }
    if (level && typeof level === "object" && "effort" in level) {
      const effort = typeof level.effort === "string" ? level.effort.trim() : "";
      if (effort) normalized.push(effort);
    }
  });
  const defaultReasoning = typeof model?.default_reasoning_level === "string"
    ? model.default_reasoning_level.trim()
    : "";
  const unique = Array.from(new Set(normalized));
  if (unique.length) return unique;
  return defaultReasoning ? [defaultReasoning] : [];
};

const updateReasoningForModel = (modelId, preferred) => {
  const levels = getReasoningLevelsForModel(modelId);
  const model = modelCatalog.get(modelId);
  const defaultReasoning = typeof model?.default_reasoning_level === "string"
    ? model.default_reasoning_level.trim()
    : "";
  const selected = levels.includes(preferred)
    ? preferred
    : levels.includes(defaultReasoning)
    ? defaultReasoning
    : levels.length
    ? levels[0]
    : "";
  return setReasoningOptions(levels, selected);
};

const resetModelCatalog = (label = "Authenticate to load models") => {
  modelCatalog = new Map();
  modelsLoadedToken = "";
  setModelPlaceholder(label);
  setReasoningPlaceholder("No model selected");
};

const loadModels = async (token) => {
  const trimmed = token.trim();
  if (!trimmed) return;
  if (trimmed === modelsLoadedToken) return;
  const requestId = ++modelsRequestId;
  try {
    const res = await fetch("/v1/models", { headers: { Authorization: `Bearer ${trimmed}` }, cache: "no-store" });
    const data = await res.json().catch(() => null);
    if (requestId !== modelsRequestId) return;
    if (trimmed !== tokenInput.value.trim()) return;
    if (!res.ok) {
      const message = typeof data?.error?.message === "string" && data?.error?.message.trim().length > 0
        ? data.error.message
        : `Failed to load models (${res.status} ${res.statusText})`;
      setModelPlaceholder(message);
      return;
    }
    const models = Array.isArray(data?.data) ? data.data : [];
    if (!models.length) {
      setModelPlaceholder("No models available");
      return;
    }
    modelCatalog = new Map(
      models
        .map((model) => ({ id: normalizeModelId(model), model }))
        .filter((entry) => entry.id)
        .map((entry) => [entry.id, entry.model]),
    );
    const selected = setModelOptions(models, preferredModel);
    if (selected && selected !== preferredModel) {
      preferredModel = selected;
      persistSetting(STORAGE_KEYS.model, selected);
    } else if (selected) {
      preferredModel = selected;
    }
    modelsLoadedToken = trimmed;
    updateReasoningForModel(selected || modelInput.value.trim(), reasoningSelect.value);
  } catch {
    if (requestId !== modelsRequestId) return;
  }
};

let authCheckId = 0;
const checkAuthToken = async () => {
  const token = tokenInput.value.trim();
  if (!token) {
    setAuthBadge("bad", "Missing token");
    resetModelCatalog();
    setReasoningPlaceholder("Missing token");
    return;
  }

  const requestId = ++authCheckId;
  setAuthBadge("unknown", "Checking...");
  try {
    const res = await fetch("/v1/auth", { headers: { Authorization: `Bearer ${token}` }, cache: "no-store" });
    const data = await res.json().catch(() => null);
    if (requestId !== authCheckId) return;
    if (!res.ok) {
      setAuthBadge("bad", data?.error?.message ?? "Unauthorized");
      resetModelCatalog();
      setReasoningPlaceholder("Invalid token");
      return;
    }
    const mode = data?.auth?.mode;
    setAuthBadge("ok", mode ? `OK (${mode})` : "OK");
    void loadModels(token);
  } catch {
    if (requestId !== authCheckId) return;
    setAuthBadge("bad", "Offline");
  }
};

const scheduleAuthCheck = debounce(() => {
  void checkAuthToken();
}, 500);

const persistTokenIfEnabled = () => {
  if (!rememberTokenInput.checked) return;
  const token = tokenInput.value.trim();
  if (token) storage.set(STORAGE_KEYS.token, token);
  else storage.remove(STORAGE_KEYS.token);
};

const persistSetting = (key, value) => {
  const trimmed = value.trim();
  if (trimmed) storage.set(key, trimmed);
  else storage.remove(key);
};

const scheduleTokenPersist = debounce(() => {
  persistTokenIfEnabled();
}, 500);

const scheduleModelPersist = debounce(() => {
  persistSetting(STORAGE_KEYS.model, modelInput.value);
}, 500);

const scheduleSystemPersist = debounce(() => {
  persistSetting(STORAGE_KEYS.systemPrompt, systemInput.value);
}, 500);

const restoreSettings = () => {
  const remember = storage.get(STORAGE_KEYS.rememberToken) === "1";
  rememberTokenInput.checked = remember;
  if (remember) tokenInput.value = storage.get(STORAGE_KEYS.token) ?? "";

  preferredModel = storage.get(STORAGE_KEYS.model) ?? DEFAULT_MODEL;
  setModelPlaceholder("Loading models...");
  setReasoningPlaceholder("Loading models...");
  systemInput.value = storage.get(STORAGE_KEYS.systemPrompt) ?? "";
};

const restorePanelStates = () => {
  panels.forEach((panel) => {
    const key = panel.dataset.panelKey;
    if (!key) return;
    const stored = storage.get(`${PANEL_STATE_PREFIX}${key}`);
    if (stored === "open") panel.open = true;
    if (stored === "closed") panel.open = false;
  });
};

const bindPanelPersistence = () => {
  panels.forEach((panel) => {
    const key = panel.dataset.panelKey;
    if (!key) return;
    panel.addEventListener("toggle", () => {
      storage.set(`${PANEL_STATE_PREFIX}${key}`, panel.open ? "open" : "closed");
    });
  });
};

restoreSettings();
restorePanelStates();
bindPanelPersistence();
if (tokenInput.value.trim()) {
  setAuthBadge("unknown", "Checking...");
  void checkAuthToken();
} else {
  setAuthBadge("bad", "Missing token");
  resetModelCatalog();
}

showTokenInput.addEventListener("change", () => {
  tokenInput.type = showTokenInput.checked ? "text" : "password";
});

tokenInput.addEventListener("input", () => {
  authCheckId += 1;
  modelsRequestId += 1;
  scheduleTokenPersist();
  const token = tokenInput.value.trim();
  if (!token) {
    setAuthBadge("bad", "Missing token");
    resetModelCatalog();
    setReasoningPlaceholder("No model selected");
    return;
  }
  if (token !== modelsLoadedToken) resetModelCatalog("Checking token...");
  setAuthBadge("unknown", "Checking...");
  scheduleAuthCheck();
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

const handleModelChange = () => {
  const nextModel = modelInput.value.trim();
  if (nextModel) preferredModel = nextModel;
  scheduleModelPersist();
  updateReasoningForModel(nextModel, reasoningSelect.value);
};

modelInput.addEventListener("input", handleModelChange);
modelInput.addEventListener("change", handleModelChange);
systemInput.addEventListener("input", () => scheduleSystemPersist());
reasoningSelect.addEventListener("change", () => persistSetting(STORAGE_KEYS.reasoningEffort, reasoningSelect.value));

const appendMessage = (role, text) => {
  const el = document.createElement("div");
  el.dataset.message = role;
  el.textContent = text;
  messagesEl.appendChild(el);
  messagesEl.scrollTop = messagesEl.scrollHeight;
  return el;
};

const setBusy = (busy) => {
  sendBtn.disabled = busy;
  stopBtn.disabled = !busy;
  promptInput.disabled = busy;
  resetChatBtn.disabled = busy;
};

let abortController = null;
let conversation = [];

resetChatBtn.addEventListener("click", () => {
  if (abortController) abortController.abort();
  abortController = null;
  conversation = [];
  messagesEl.textContent = "";
  setBusy(false);
  promptInput.focus();
});

stopBtn.addEventListener("click", () => {
  if (!abortController) return;
  abortController.abort();
});

const streamSse = async (response, onEvent) => {
  const reader = response.body?.getReader();
  if (!reader) return;

  const decoder = new TextDecoder();
  let buffer = "";

  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    buffer += decoder.decode(value, { stream: true });

    for (;;) {
      const boundary = buffer.indexOf("\n\n");
      if (boundary < 0) break;
      const rawEvent = buffer.slice(0, boundary);
      buffer = buffer.slice(boundary + 2);
      onEvent(rawEvent);
    }
  }

  if (buffer.trim()) onEvent(buffer);
};

const extractAssistantDelta = (payload) => {
  const delta = payload?.choices?.[0]?.delta;
  if (typeof delta?.content === "string") return delta.content;
  return "";
};

const sendPrompt = async () => {
  const token = tokenInput.value.trim();
  if (!token) {
    appendMessage("error", "Missing gateway token. Paste one under “Auth” first.");
    tokenInput.focus();
    return;
  }

  const text = promptInput.value.trim();
  if (!text) return;

  const systemPrompt = systemInput.value.trim();

  promptInput.value = "";
  conversation.push({ role: "user", content: text });
  appendMessage("user", text);

  const requestMessages = [];
  if (systemPrompt) requestMessages.push({ role: "system", content: systemPrompt });
  requestMessages.push(...conversation);

  const payload = {
    model: modelInput.value.trim() || undefined,
    messages: requestMessages,
    stream: true,
  };

  const reasoningEffort = reasoningSelect.value.trim();
  if (reasoningEffort) payload.reasoning_effort = reasoningEffort;
  if (!payload.model) delete payload.model;

  const assistantEl = appendMessage("assistant", "");
  setBusy(true);

  abortController = new AbortController();
  try {
    const res = await fetch("/v1/chat/completions", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${token}`,
      },
      body: JSON.stringify(payload),
      signal: abortController.signal,
    });

    if (!res.ok) {
      const errorPayload = await res.json().catch(() => null);
      assistantEl.dataset.message = "error";
      assistantEl.textContent = errorPayload?.error?.message ?? `${res.status} ${res.statusText}`;
      return;
    }

    if (!res.headers.get("content-type")?.includes("text/event-stream")) {
      const data = await res.json().catch(() => null);
      const content = data?.choices?.[0]?.message?.content;
      if (typeof content === "string" && content.length > 0) {
        assistantEl.textContent = content;
        conversation.push({ role: "assistant", content });
      } else {
        assistantEl.textContent = JSON.stringify(data, null, 2);
      }
      return;
    }

    let assistantText = "";
    await streamSse(res, (rawEvent) => {
      const dataLines = rawEvent
        .split("\n")
        .map((line) => line.trimEnd())
        .filter((line) => line.startsWith("data:"))
        .map((line) => line.slice("data:".length).trimStart());

      const dataText = dataLines.join("\n").trim();
      if (!dataText) return;
      if (dataText === "[DONE]") return;

      try {
        const payload = JSON.parse(dataText);
        const delta = extractAssistantDelta(payload);
        if (!delta) return;
        assistantText += delta;
        assistantEl.textContent = assistantText;
        messagesEl.scrollTop = messagesEl.scrollHeight;
      } catch {
        // ignore invalid chunks
      }
    });

    if (assistantText.trim()) conversation.push({ role: "assistant", content: assistantText });
  } catch (error) {
    if (error?.name === "AbortError") {
      assistantEl.dataset.message = "system";
      assistantEl.textContent = "[stopped]";
      return;
    }
    assistantEl.dataset.message = "error";
    assistantEl.textContent = "Request failed.";
  } finally {
    abortController = null;
    setBusy(false);
    promptInput.focus();
  }
};

promptInput.addEventListener("keydown", (event) => {
  if (event.key !== "Enter" || event.shiftKey) return;
  event.preventDefault();
  chatForm.requestSubmit();
});

chatForm.addEventListener("submit", (event) => {
  event.preventDefault();
  void sendPrompt();
});

setBusy(false);
promptInput.focus();
