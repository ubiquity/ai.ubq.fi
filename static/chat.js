import "./network.js";
import {
  buildBackendUrl,
  formatAuthSessionLabel,
  hasAuthPasskeyCredential,
  hasStoredPasskeyCredentials,
  registerPasskey,
  resolveBackendBase,
  signInWithPasskey,
  signOut,
  storage,
  STORAGE_KEYS as AUTH_STORAGE_KEYS,
} from "./auth.js";
import {
  getReasoningEffortForChatRequest,
  setReasoningPlaceholder as setSharedReasoningPlaceholder,
  updateReasoningSelectForModel,
} from "./reasoning-select.js?v=20260713-none-ultra";
import { bindForegroundRefresh } from "./foreground-refresh.js";

const STORAGE_KEYS = {
  rememberToken: AUTH_STORAGE_KEYS.rememberToken,
  token: AUTH_STORAGE_KEYS.token,
  passkeyHandle: AUTH_STORAGE_KEYS.passkeyHandle,
  passkeyCredentialIds: AUTH_STORAGE_KEYS.passkeyCredentialIds,
  model: "uos_ai.playground.model",
  reasoningEffort: "uos_ai.playground.reasoning_effort",
  systemPrompt: "uos_ai.playground.system_prompt",
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
const passkeyHandleInput = mustGet("passkey-handle");
const passkeyLoginBtn = mustGet("passkey-login");
const passkeyRegisterBtn = mustGet("passkey-register");
const signOutBtn = mustGet("sign-out");
const passkeyStatus = mustGet("passkey-status");
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

const setPasskeyStatus = (state, text) => {
  passkeyStatus.dataset.state = state;
  passkeyStatus.textContent = text;
};

const getActiveBackendBase = () => resolveBackendBase();

const formatBackendLabel = (baseUrl) => {
  try {
    return new URL(baseUrl).origin;
  } catch {
    return baseUrl;
  }
};

const buildBackendAwareMessage = (baseUrl, fallback) => {
  const target = formatBackendLabel(baseUrl);
  return `${fallback} Re-sign in on the active backend at ${target}.`;
};

const setSignedInState = (signedIn, options = {}) => {
  const deviceRegistered = options.deviceRegistered ?? hasStoredPasskeyCredentials();
  const canRegisterPasskey = options.canRegisterPasskey ?? false;
  passkeyLoginBtn.hidden = signedIn;
  passkeyRegisterBtn.hidden = deviceRegistered || (signedIn && !canRegisterPasskey);
  signOutBtn.hidden = !signedIn;
  if (signedIn) setPasskeyStatus("ok", options.statusText ?? "Token active");
  else setPasskeyStatus("unknown", "Passkey idle");
};

const logPasskeyUsername = (handle) => {
  const username = (handle ?? "").trim();
  if (username) console.info("[ai.ubq.fi] passkey username:", username);
};

let modelsRequestId = 0;
let modelsLoadedToken = "";
let modelCatalog = new Map();
let preferredModel = DEFAULT_MODEL;
let preferredReasoningEffort = "";

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

const mergeModelCapabilities = (models, capabilities) => {
  const capabilitiesById = new Map(
    capabilities
      .map((capability) => ({ id: normalizeModelId(capability), capability }))
      .filter((entry) => entry.id)
      .map((entry) => [entry.id, entry.capability]),
  );

  return models.map((model) => {
    const id = normalizeModelId(model);
    const capability = capabilitiesById.get(id);
    if (!capability) return model;

    const merged = { ...model };
    if (typeof capability.display_name === "string" && capability.display_name.trim()) {
      merged.display_name = capability.display_name;
    }
    if (Array.isArray(capability.supported_reasoning_levels)) {
      merged.supported_reasoning_levels = capability.supported_reasoning_levels;
    }
    const defaultReasoning = typeof capability.default_reasoning_effort === "string"
      ? capability.default_reasoning_effort.trim()
      : typeof capability.default_reasoning_level === "string"
      ? capability.default_reasoning_level.trim()
      : "";
    if (defaultReasoning) {
      merged.default_reasoning_effort = defaultReasoning;
      merged.default_reasoning_level = defaultReasoning;
    }
    for (
      const key of [
        "context_window_tokens",
        "max_context_window_tokens",
        "auto_compact_token_limit_tokens",
      ]
    ) {
      if (typeof capability[key] === "number" || capability[key] === null) {
        merged[key] = capability[key];
      }
    }
    return merged;
  });
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
  setSharedReasoningPlaceholder(reasoningSelect, label);
};

const loadDefaultModelFromAdmin = async (token, baseUrl) => {
  try {
    const response = await fetch(buildBackendUrl("/admin/defaults", baseUrl), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    if (!response.ok) return "";
    const payload = await response.json().catch(() => null);
    const raw = payload?.defaults?.model;
    if (typeof raw !== "string") return "";
    const model = raw.trim();
    return model;
  } catch {
    return "";
  }
};

const setModelOptions = (models, preferred, fallbackModel = "") => {
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
  const fallbackValue = typeof fallbackModel === "string" ? fallbackModel.trim() : "";
  const firstValue = options[0].value;
  const hasStored = options.some((option) => option.value === preferredValue);
  const hasFallback = fallbackValue ? options.some((option) => option.value === fallbackValue) : false;
  const next = hasStored ? preferredValue : hasFallback ? fallbackValue : firstValue;
  modelInput.value = next;
  return next;
};

const updateReasoningForModel = (modelId, preferred) => {
  const model = modelCatalog.get(modelId);
  return updateReasoningSelectForModel(reasoningSelect, model, preferred);
};

const resetModelCatalog = (label = "Authenticate to load models") => {
  modelCatalog = new Map();
  modelsLoadedToken = "";
  setModelPlaceholder(label);
  setReasoningPlaceholder("No model selected");
};

const loadModels = async (token, options = {}) => {
  const trimmed = token.trim();
  if (!trimmed) return;
  if (!options.force && trimmed === modelsLoadedToken) return;
  const requestId = ++modelsRequestId;
  const backendBase = getActiveBackendBase();
  try {
    const [modelsResponse, capabilitiesResponse, defaultsResponse] = await Promise.all([
      fetch(buildBackendUrl("/v1/models", backendBase), {
        headers: { Authorization: `Bearer ${trimmed}` },
        cache: "no-store",
      }).then(
        async (res) => ({ res, data: await res.json().catch(() => null) }),
      ),
      fetch(
        buildBackendUrl("/uos/models/capabilities", backendBase),
        { headers: { Authorization: `Bearer ${trimmed}` }, cache: "no-store" },
      ).then(
        async (res) => ({ res, data: await res.json().catch(() => null) }),
      ).catch((error) => ({ error })),
      loadDefaultModelFromAdmin(trimmed, backendBase).then((model) => ({ ok: true, model })).catch(() => ({
        ok: false,
        model: "",
      })),
    ]);
    const { res, data } = modelsResponse;
    if (requestId !== modelsRequestId) return;
    if (trimmed !== tokenInput.value.trim()) return;
    if (!res.ok) {
      if (res.status === 401) {
        setModelPlaceholder(buildBackendAwareMessage(backendBase, "Auth failed for the configured backend."));
        return;
      }
      const message = typeof data?.error?.message === "string" && data?.error?.message.trim().length > 0
        ? data.error.message
        : `Failed to load models (${res.status} ${res.statusText})`;
      setModelPlaceholder(message);
      return;
    }
    const rawModels = Array.isArray(data?.data) ? data.data : [];
    const capabilities = "res" in capabilitiesResponse && capabilitiesResponse.res.ok &&
        Array.isArray(capabilitiesResponse.data?.data)
      ? capabilitiesResponse.data.data
      : [];
    if (!capabilities.length && "res" in capabilitiesResponse && !capabilitiesResponse.res.ok) {
      console.warn("[ai.ubq.fi] model capabilities unavailable:", capabilitiesResponse.res.status);
    } else if ("error" in capabilitiesResponse) {
      console.warn("[ai.ubq.fi] model capabilities unavailable:", capabilitiesResponse.error);
    }
    const models = mergeModelCapabilities(rawModels, capabilities);
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
    const defaultsModel = defaultsResponse.ok ? defaultsResponse.model : "";
    const selected = setModelOptions(models, preferredModel, defaultsModel);
    if (selected && selected !== preferredModel) {
      preferredModel = selected;
      persistSetting(STORAGE_KEYS.model, selected);
    } else if (selected) {
      preferredModel = selected;
    }
    modelsLoadedToken = trimmed;
    preferredReasoningEffort = updateReasoningForModel(selected || modelInput.value.trim(), preferredReasoningEffort) ??
      "";
  } catch {
    if (requestId !== modelsRequestId) return;
  }
};

let authCheckId = 0;
const checkAuthToken = async () => {
  const token = tokenInput.value.trim();
  if (!token) {
    setAuthBadge("bad", "Missing token");
    setSignedInState(false);
    resetModelCatalog();
    setReasoningPlaceholder("Missing token");
    return;
  }

  const requestId = ++authCheckId;
  setAuthBadge("unknown", "Checking...");
  const backendBase = getActiveBackendBase();
  try {
    const res = await fetch(buildBackendUrl("/uos/auth", backendBase), {
      headers: { Authorization: `Bearer ${token}` },
      cache: "no-store",
    });
    const data = await res.json().catch(() => null);
    if (requestId !== authCheckId) return;
    if (!res.ok) {
      if (res.status === 401) {
        setAuthBadge("bad", buildBackendAwareMessage(backendBase, "Token was rejected by the active backend."));
      } else {
        setAuthBadge("bad", data?.error?.message ?? "Unauthorized");
      }
      setSignedInState(false);
      resetModelCatalog();
      setReasoningPlaceholder("Invalid token");
      return;
    }
    const mode = data?.auth?.mode;
    setAuthBadge("ok", mode ? `OK (${mode})` : "OK");
    setSignedInState(true, {
      canRegisterPasskey: data?.auth?.is_admin === true,
      deviceRegistered: hasAuthPasskeyCredential(data?.auth) || hasStoredPasskeyCredentials(),
      statusText: formatAuthSessionLabel(data?.auth),
    });
    void loadModels(token);
  } catch {
    if (requestId !== authCheckId) return;
    setAuthBadge("bad", "Offline");
    setSignedInState(false);
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

const persistPasskeyHandle = () => {
  persistSetting(STORAGE_KEYS.passkeyHandle, passkeyHandleInput.value);
};

const schedulePasskeyHandlePersist = debounce(() => {
  persistPasskeyHandle();
}, 500);

const setPasskeyHandleValue = (handle) => {
  passkeyHandleInput.value = handle ?? "";
  persistPasskeyHandle();
  logPasskeyUsername(handle);
};

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
  passkeyHandleInput.value = storage.get(STORAGE_KEYS.passkeyHandle) ?? "";

  preferredModel = storage.get(STORAGE_KEYS.model) ?? DEFAULT_MODEL;
  preferredReasoningEffort = storage.get(STORAGE_KEYS.reasoningEffort) ?? "";
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
setSignedInState(false);
if (tokenInput.value.trim()) {
  setAuthBadge("unknown", "Checking...");
  void checkAuthToken();
} else {
  setAuthBadge("bad", "Missing token");
  resetModelCatalog();
}

bindForegroundRefresh(() => {
  const token = tokenInput.value.trim();
  if (token) void loadModels(token, { force: true });
});

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
    setSignedInState(false);
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
  setSignedInState(Boolean(tokenInput.value.trim()));
});

passkeyHandleInput.addEventListener("input", () => {
  schedulePasskeyHandlePersist();
});

const applySignedInToken = (token, options = {}) => {
  tokenInput.value = token;
  rememberTokenInput.checked = true;
  storage.set(STORAGE_KEYS.rememberToken, "1");
  storage.set(STORAGE_KEYS.token, token);
  setSignedInState(true, options);
  authCheckId += 1;
  modelsRequestId += 1;
  setAuthBadge("unknown", "Checking...");
  void checkAuthToken();
};

passkeyLoginBtn.addEventListener("click", async () => {
  setPasskeyStatus("unknown", "Signing in...");
  passkeyLoginBtn.disabled = true;
  passkeyRegisterBtn.disabled = true;
  try {
    const passkeyHandle = passkeyHandleInput.value.trim();
    const result = await signInWithPasskey({
      baseUrl: getActiveBackendBase(),
      handle: passkeyHandle,
      useHandle: Boolean(passkeyHandle),
    });
    if (result.handle) setPasskeyHandleValue(result.handle);
    applySignedInToken(result.token, { deviceRegistered: true });
    setPasskeyStatus("ok", "Passkey signed in");
  } catch (error) {
    setSignedInState(false);
    setPasskeyStatus("bad", error?.message ?? "Passkey sign-in failed");
  } finally {
    passkeyLoginBtn.disabled = false;
    passkeyRegisterBtn.disabled = false;
  }
});

passkeyRegisterBtn.addEventListener("click", async () => {
  setPasskeyStatus("unknown", "Registering...");
  passkeyLoginBtn.disabled = true;
  passkeyRegisterBtn.disabled = true;
  try {
    const result = await registerPasskey({
      handle: passkeyHandleInput.value,
      token: tokenInput.value,
      baseUrl: getActiveBackendBase(),
    });
    if (result.handle) setPasskeyHandleValue(result.handle);
    applySignedInToken(result.token, { deviceRegistered: true });
    setPasskeyStatus("ok", "Passkey registered");
  } catch (error) {
    setPasskeyStatus("bad", error?.message ?? "Passkey registration failed");
  } finally {
    passkeyLoginBtn.disabled = false;
    passkeyRegisterBtn.disabled = false;
  }
});

signOutBtn.addEventListener("click", async () => {
  const token = tokenInput.value.trim();
  signOutBtn.disabled = true;
  try {
    await signOut({ token, baseUrl: getActiveBackendBase() });
  } finally {
    tokenInput.value = "";
    rememberTokenInput.checked = false;
    authCheckId += 1;
    modelsRequestId += 1;
    setAuthBadge("bad", "Missing token");
    setSignedInState(false);
    resetModelCatalog();
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
    setSignedInState(Boolean(tokenInput.value.trim()));
    return;
  }
  if (event.key !== STORAGE_KEYS.token) return;
  if (event.newValue === null) {
    tokenInput.value = "";
    authCheckId += 1;
    modelsRequestId += 1;
    setAuthBadge("bad", "Missing token");
    setSignedInState(false);
    resetModelCatalog();
    return;
  }
  if (!rememberTokenInput.checked) return;
  tokenInput.value = event.newValue ?? "";
  authCheckId += 1;
  modelsRequestId += 1;
  if (tokenInput.value.trim()) {
    setAuthBadge("unknown", "Checking...");
    void checkAuthToken();
    return;
  }
  setAuthBadge("bad", "Missing token");
  setSignedInState(false);
  resetModelCatalog();
});

const handleModelChange = () => {
  const nextModel = modelInput.value.trim();
  if (nextModel) preferredModel = nextModel;
  scheduleModelPersist();
  preferredReasoningEffort = updateReasoningForModel(nextModel, reasoningSelect.value) ?? "";
};

modelInput.addEventListener("input", handleModelChange);
modelInput.addEventListener("change", handleModelChange);
systemInput.addEventListener("input", () => scheduleSystemPersist());
reasoningSelect.addEventListener("change", () => {
  preferredReasoningEffort = reasoningSelect.value;
  persistSetting(STORAGE_KEYS.reasoningEffort, reasoningSelect.value);
});

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
    appendMessage("error", "Sign in with a passkey first, or use the fallback gateway token under Auth.");
    passkeyLoginBtn.focus();
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

  const reasoningEffort = getReasoningEffortForChatRequest(reasoningSelect.value);
  if (reasoningEffort) payload.reasoning_effort = reasoningEffort;
  if (!payload.model) delete payload.model;

  const assistantEl = appendMessage("assistant", "");
  setBusy(true);

  abortController = new AbortController();
  const backendBase = getActiveBackendBase();
  try {
    const res = await fetch(buildBackendUrl("/v1/chat/completions", backendBase), {
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
      if (res.status === 401) {
        assistantEl.textContent = buildBackendAwareMessage(backendBase, "Auth failed for this chat request.");
      } else {
        assistantEl.textContent = errorPayload?.error?.message ?? `${res.status} ${res.statusText}`;
      }
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
