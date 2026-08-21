import assert from "node:assert/strict";
import { codexExecCliCompatibilityArgs, codexReviewCliCompatibilityArgs } from "../scripts/sentinel/codex.ts";

const runPermission = await Deno.permissions.query({ name: "run", command: "codex" });
const pathPermission = await Deno.permissions.query({ name: "env", variable: "PATH" });
const executablePath = pathPermission.state === "granted" ? Deno.env.get("PATH") ?? "" : "";

Deno.test({
  name: "pinned Codex CLI parses the exact Sentinel exec and native review arguments",
  ignore: runPermission.state !== "granted" || pathPermission.state !== "granted" || executablePath === "",
  async fn() {
    for (
      const [label, args] of [
        ["exec", codexExecCliCompatibilityArgs(Deno.cwd())],
        ["review", codexReviewCliCompatibilityArgs(Deno.cwd())],
      ] as const
    ) {
      const output = await new Deno.Command("codex", {
        args: [...args],
        clearEnv: true,
        env: {
          CODEX_HOME: "/tmp/sentinel-codex-cli-compatibility",
          HOME: "/tmp",
          PATH: executablePath,
        },
        stdout: "piped",
        stderr: "piped",
      }).output();
      assert.equal(
        output.success,
        true,
        `${label} arguments were rejected: ${new TextDecoder().decode(output.stderr).trim()}`,
      );
      assert.match(new TextDecoder().decode(output.stdout), /Usage:/u);
    }
  },
});
