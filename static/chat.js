import "./network.js";

const STORAGE_KEYS = {
  rememberToken: "ubq_ai.playground.remember_token",
  token: "ubq_ai.playground.token",
  model: "ubq_ai.playground.model",
  reasoningEffort: "ubq_ai.playground.reasoning_effort",
  systemPrompt: "ubq_ai.playground.system_prompt",
  stream: "ubq_ai.playground.stream",
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

const tokenInput = mustGet("token");
const rememberTokenInput = mustGet("remember-token");
const showTokenInput = mustGet("show-token");
const modelInput = mustGet("model");
const reasoningSelect = mustGet("reasoning-effort");
const systemInput = mustGet("system");
const streamInput = mustGet("stream");
const authBadge = mustGet("auth-badge");
const messagesEl = mustGet("messages");
const resetChatBtn = mustGet("reset-chat");
const stopBtn = mustGet("stop");
const chatForm = mustGet("chat-form");
const promptInput = mustGet("prompt");
const sendBtn = mustGet("send");

const setAuthBadge = (state, text) => {
  authBadge.dataset.state = state;
  authBadge.textContent = text;
};

let authCheckId = 0;
const checkAuthToken = async () => {
  const token = tokenInput.value.trim();
  if (!token) {
    setAuthBadge("bad", "Missing token");
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
      return;
    }
    const mode = data?.auth?.mode;
    setAuthBadge("ok", mode ? `OK (${mode})` : "OK");
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

  modelInput.value = storage.get(STORAGE_KEYS.model) ?? "gpt-5.2-chat-latest";
  reasoningSelect.value = storage.get(STORAGE_KEYS.reasoningEffort) ?? "";
  systemInput.value = storage.get(STORAGE_KEYS.systemPrompt) ?? "";
  streamInput.checked = storage.get(STORAGE_KEYS.stream) === "1";
};

restoreSettings();
if (tokenInput.value.trim()) {
  setAuthBadge("unknown", "Checking...");
  void checkAuthToken();
} else {
  setAuthBadge("bad", "Missing token");
}

showTokenInput.addEventListener("change", () => {
  tokenInput.type = showTokenInput.checked ? "text" : "password";
});

tokenInput.addEventListener("input", () => {
  authCheckId += 1;
  scheduleTokenPersist();
  const token = tokenInput.value.trim();
  if (!token) {
    setAuthBadge("bad", "Missing token");
    return;
  }
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

modelInput.addEventListener("input", () => scheduleModelPersist());
systemInput.addEventListener("input", () => scheduleSystemPersist());
reasoningSelect.addEventListener("change", () => persistSetting(STORAGE_KEYS.reasoningEffort, reasoningSelect.value));
streamInput.addEventListener("change", () => storage.set(STORAGE_KEYS.stream, streamInput.checked ? "1" : "0"));

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

  promptInput.value = "";
  conversation.push({ role: "user", content: text });
  appendMessage("user", text);

  const systemPrompt = systemInput.value.trim();
  const requestMessages = [];
  if (systemPrompt) requestMessages.push({ role: "system", content: systemPrompt });
  requestMessages.push(...conversation);

  const payload = {
    model: modelInput.value.trim() || undefined,
    messages: requestMessages,
    stream: streamInput.checked,
  };

  const reasoningEffort = reasoningSelect.value.trim();
  if (reasoningEffort) payload.reasoning_effort = reasoningEffort;
  if (!payload.model) delete payload.model;

  const assistantEl = appendMessage("assistant", streamInput.checked ? "" : "…");
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

    if (!streamInput.checked || !res.headers.get("content-type")?.includes("text/event-stream")) {
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
