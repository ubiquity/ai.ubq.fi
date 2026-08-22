import assert from "node:assert/strict";
import { buildImageResponsesRequest, extractImagesFromResponses, handleImages } from "../src/openai.ts";

/** Pin the tool host so the suite never depends on ambient deployment config. */
const withBaseModel = async (model: string, run: () => Promise<void>): Promise<void> => {
  const previous = Deno.env.get("IMAGE_BASE_MODEL");
  Deno.env.set("IMAGE_BASE_MODEL", model);
  try {
    await run();
  } finally {
    if (previous === undefined) Deno.env.delete("IMAGE_BASE_MODEL");
    else Deno.env.set("IMAGE_BASE_MODEL", previous);
  }
};

const imagesRequest = (body: unknown, path = "/v1/images/generations"): Request =>
  new Request(`http://127.0.0.1${path}`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });

Deno.test("an images request becomes a tool-bearing Responses request", () => {
  const built = buildImageResponsesRequest(
    { model: "gpt-image-2", prompt: "a green triangle", size: "1024x1024", quality: "high" },
    "gpt-5.6-sol",
    "generations",
  );
  // The tool runs on a text model; the requested image model is a tool option.
  assert.equal(built.model, "gpt-5.6-sol");
  assert.equal(built.input, "a green triangle");
  assert.deepEqual(built.tools, [{
    type: "image_generation",
    model: "gpt-image-2",
    size: "1024x1024",
    quality: "high",
  }]);
});

Deno.test("edits carry their source images as Responses input content", () => {
  const built = buildImageResponsesRequest(
    { model: "gpt-image-2", prompt: "make it blue", images: [{ image_url: "data:image/png;base64,AAAA" }] },
    "gpt-5.6-sol",
    "edits",
  );
  assert.deepEqual(built.input, [{
    role: "user",
    content: [
      { type: "input_text", text: "make it blue" },
      { type: "input_image", image_url: "data:image/png;base64,AAAA" },
    ],
  }]);
});

Deno.test("images are read from image_generation_call output items", () => {
  // Shape captured from a real Responses reply carrying the image tool.
  const payload = {
    id: "resp_1",
    output: [
      { id: "rs_1", type: "reasoning", summary: [] },
      { id: "ig_1", type: "image_generation_call", status: "completed", output_format: "png", result: "AAAA" },
    ],
  };
  assert.deepEqual(extractImagesFromResponses(payload), [{ b64_json: "AAAA", output_format: "png" }]);
  assert.deepEqual(extractImagesFromResponses({ output: [] }), []);
  assert.deepEqual(extractImagesFromResponses({ output: [{ type: "image_generation_call" }] }), []);
  assert.deepEqual(extractImagesFromResponses(null), []);
});

Deno.test("a successful generation returns the OpenAI images shape", async () => {
  await withBaseModel("gpt-5.6-sol", async () => {
    const seen: Array<Record<string, unknown>> = [];
    const dispatch = async (request: Request): Promise<Response> => {
      seen.push(await request.json());
      return new Response(
        JSON.stringify({
          created_at: 1787431659,
          output: [{ type: "image_generation_call", result: "BBBB", output_format: "png" }],
        }),
        { status: 200, headers: { "x-uos-upstream": "chatgpt_codex" } },
      );
    };
    const response = await handleImages(
      imagesRequest({ model: "gpt-image-2", prompt: "a circle" }),
      "generations",
      undefined,
      { dispatch },
    );
    assert.equal(response.status, 200);
    // The waterfall's provider label must survive so callers can see who served it.
    assert.equal(response.headers.get("x-uos-upstream"), "chatgpt_codex");
    assert.deepEqual(await response.json(), {
      created: 1787431659,
      data: [{ b64_json: "BBBB", output_format: "png" }],
    });
    assert.equal(seen.length, 1);
    assert.equal(seen[0]?.model, "gpt-5.6-sol", "dispatch must target the base text model");
  });
});

Deno.test("upstream quota and roster errors pass through unchanged", async () => {
  await withBaseModel("gpt-5.6-sol", async () => {
    const dispatch = () =>
      Promise.resolve(
        new Response(
          JSON.stringify({ error: { message: "quota exhausted", type: "invalid_request_error" } }),
          { status: 429, headers: { "x-uos-upstream": "chatgpt_codex" } },
        ),
      );
    const response = await handleImages(
      imagesRequest({ model: "gpt-image-2", prompt: "a circle" }),
      "generations",
      undefined,
      { dispatch },
    );
    assert.equal(response.status, 429);
    const body = await response.json() as { error?: { message?: string } };
    assert.equal(body.error?.message, "quota exhausted");
  });
});

Deno.test("a reply with no image is reported instead of returning an empty success", async () => {
  await withBaseModel("gpt-5.6-sol", async () => {
    const dispatch = () =>
      Promise.resolve(
        new Response(JSON.stringify({ output: [{ type: "message", content: [] }] }), { status: 200 }),
      );
    const response = await handleImages(
      imagesRequest({ model: "gpt-image-2", prompt: "a circle" }),
      "generations",
      undefined,
      { dispatch },
    );
    assert.equal(response.status, 502);
    const body = await response.json() as { error?: { code?: string } };
    assert.equal(body.error?.code, "image_generation_failed");
  });
});

Deno.test("malformed requests are rejected before any dispatch", async () => {
  await withBaseModel("gpt-5.6-sol", async () => {
    let dispatched = false;
    const dispatch = () => {
      dispatched = true;
      return Promise.resolve(new Response("{}", { status: 200 }));
    };
    for (const body of [{ model: "gpt-image-2" }, { model: "gpt-image-2", prompt: "   " }]) {
      const response = await handleImages(imagesRequest(body), "generations", undefined, { dispatch });
      assert.equal(response.status, 400);
    }
    const notJson = new Request("http://127.0.0.1/v1/images/generations", { method: "POST", body: "not json" });
    assert.equal((await handleImages(notJson, "generations", undefined, { dispatch })).status, 400);
    assert.equal(dispatched, false);
  });
});
