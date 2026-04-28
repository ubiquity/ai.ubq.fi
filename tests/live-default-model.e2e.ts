import assert from "node:assert/strict";

const getBaseUrl = (): URL => {
  const value = Deno.env.get("BASE_URL")?.trim() || "https://ai.ubq.fi";
  return new URL(value);
};

const getToken = (): string => {
  const token = Deno.env.get("UOS_AI_TOKEN")?.trim() || Deno.env.get("DENO_DEPLOY_TOKEN")?.trim() || "";
  if (!token) {
    throw new Error("Set UOS_AI_TOKEN or DENO_DEPLOY_TOKEN to run the live default-model e2e test.");
  }
  return token;
};

const fetchJson = async (baseUrl: URL, path: string, init: RequestInit): Promise<Record<string, unknown>> => {
  const response = await fetch(new URL(path, baseUrl), init);
  const text = await response.text();
  let payload: unknown = null;
  try {
    payload = text ? JSON.parse(text) : null;
  } catch {
    payload = null;
  }
  if (!response.ok) {
    const detail = typeof payload === "object" && payload !== null ? JSON.stringify(payload) : text;
    throw new Error(`${path} failed with ${response.status}: ${detail}`);
  }
  assert.equal(typeof payload, "object");
  assert.notEqual(payload, null);
  return payload as Record<string, unknown>;
};

Deno.test({
  name: "live: default model is listed and used by no-model requests",
  permissions: { env: ["BASE_URL", "UOS_AI_TOKEN", "DENO_DEPLOY_TOKEN"], net: true },
  async fn() {
    const baseUrl = getBaseUrl();
    const token = getToken();
    const authHeaders = { "Authorization": `Bearer ${token}` };

    const responsesPayload = await fetchJson(baseUrl, "/v1/responses", {
      method: "POST",
      headers: { ...authHeaders, "Content-Type": "application/json" },
      body: JSON.stringify({ input: "Reply exactly with pong." }),
    });
    assert.equal(typeof responsesPayload.model, "string");
    assert.notEqual((responsesPayload.model as string).trim(), "");
    const defaultModel = responsesPayload.model as string;

    const modelsPayload = await fetchJson(baseUrl, "/v1/models", { headers: authHeaders });
    const models = Array.isArray(modelsPayload.data) ? modelsPayload.data : [];
    assert.ok(
      models.some((model) =>
        typeof model === "object" && model !== null &&
        (model as { id?: unknown }).id === defaultModel
      ),
      `/v1/models did not include default response model ${defaultModel}`,
    );

    const chatPayload = await fetchJson(baseUrl, "/v1/chat/completions", {
      method: "POST",
      headers: { ...authHeaders, "Content-Type": "application/json" },
      body: JSON.stringify({ messages: [{ role: "user", content: "Reply exactly with pong." }] }),
    });
    assert.equal(chatPayload.model, defaultModel);
  },
});
