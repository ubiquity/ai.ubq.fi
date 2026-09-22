import assert from "node:assert/strict";
import { openSupervisorConnection } from "../src/codex_supervisor_transport.ts";

Deno.test("openSupervisorConnection rejects immediately if signal is already aborted", async () => {
  const controller = new AbortController();
  controller.abort();
  await assert.rejects(
    async () => {
      await openSupervisorConnection("/nonexistent/socket.sock", controller.signal);
    },
    {
      name: "Error",
      message: "app-server connection aborted",
    }
  );
});

Deno.test("openSupervisorConnection rejects and terminates socket when connection fails", async () => {
  const controller = new AbortController();
  const timer = setTimeout(() => {
    controller.abort();
  }, 100);

  try {
    await assert.rejects(
      async () => {
        await openSupervisorConnection("/tmp/nonexistent-test-supervisor.sock", controller.signal);
      }
    );
  } finally {
    clearTimeout(timer);
  }
});
