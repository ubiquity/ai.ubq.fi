// ubq-ai admin commands, split out of scripts/ubq-ai.ts.

import type { UbqAiAdminContext } from "./ubq-ai_core.ts";
import {
  errorText,
  expandTilde,
  extractCreatedToken,
  getFlagString,
  listCodexBinaryCandidates,
  lookupFlag,
  parseApiKeyExpiresAtMs,
  parseUsageLimitRequests,
  parseWindowMs,
  resolveCodexClientVersion,
  writeErrText,
  writeOutText,
  writeRequestFailure,
  writeUsageError,
} from "./ubq-ai_core.ts";

const runAdminUploadAuthCommand = async (ctx: UbqAiAdminContext): Promise<number> => {
  const { runtime, flags, homeDir, endpoint, adminToken, doFetchWithDebug } = ctx;
  const authJsonPath = expandTilde(getFlagString(flags, "auth-json") ?? "~/.codex/auth.json", homeDir);
  let authJsonText: string;
  try {
    authJsonText = await runtime.readTextFile(authJsonPath);
  } catch (error) {
    await writeErrText(runtime, `Failed to read auth.json at ${authJsonPath}:\n`);
    await writeErrText(runtime, `${errorText(error)}\n`);
    return 2;
  }

  let authJson: unknown;
  try {
    authJson = JSON.parse(authJsonText) as unknown;
  } catch (error) {
    await writeErrText(runtime, `auth.json at ${authJsonPath} is not valid JSON:\n`);
    await writeErrText(runtime, `${errorText(error)}\n`);
    return 2;
  }

  const skipModelsFlag = lookupFlag(flags, "skip-models");
  const noModelsFlag = lookupFlag(flags, "no-models");
  if (skipModelsFlag !== undefined || noModelsFlag !== undefined) {
    await writeErrText(runtime, "--skip-models is obsolete; upload-auth always stores the live upstream Codex model catalog.\n");
    return 2;
  }
  const codexBinFlag = getFlagString(flags, "codex-bin");
  const clientVersion = (await resolveCodexClientVersion(runtime, listCodexBinaryCandidates(runtime, codexBinFlag, homeDir), homeDir)) ?? undefined;
  const modelsPayload: Record<string, unknown> = {
    source: "chatgpt_codex",
    client_version: clientVersion,
    updated_at_ms: Date.now(),
  };

  const payload = { auth: authJson, models: modelsPayload };
  const req = new Request(endpoint("/admin/codex/auth"), {
    method: "POST",
    headers: {
      Authorization: `Bearer ${adminToken}`,
      "Content-Type": "application/json",
      Accept: "application/json",
    },
    body: JSON.stringify(payload),
  });

  const result = await doFetchWithDebug(req);
  if (!result.ok) return await writeRequestFailure(runtime, result.status, result.body);
  await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
  return 0;
};

const runAdminKeysCreateCommand = async (ctx: UbqAiAdminContext): Promise<number> => {
  const { runtime, flags, endpoint, wantsJson, adminToken, subArgs, doFetchWithDebug } = ctx;
  const positionalName = subArgs.slice(1).join(" ").trim();
  const name = (getFlagString(flags, "name") ?? positionalName).trim();
  if (!name.trim()) {
    await writeErrText(runtime, "Missing key name. Pass it as an argument or via --name.\n");
    return 2;
  }
  const token = (getFlagString(flags, "token") ?? "").trim();
  const tokenOnlyFlag = flags["token-only"];
  const tokenOnly = !wantsJson && tokenOnlyFlag !== false;

  const expiresAt = parseApiKeyExpiresAtMs(flags);
  if (!expiresAt.ok) {
    await writeErrText(runtime, `${expiresAt.message}\n`);
    return 2;
  }

  const usageLimitRaw = getFlagString(flags, "usage-limit");
  let usageLimit: number | undefined;
  if (usageLimitRaw) {
    const parsedLimit = parseUsageLimitRequests(usageLimitRaw);
    if (!parsedLimit.ok) {
      await writeErrText(runtime, "--usage-limit must be a positive number, -1, or 'unlimited'\n");
      return 2;
    }
    usageLimit = parsedLimit.value;
  }

  const body: Record<string, unknown> = token ? { name, token } : { name };
  if (expiresAt.value !== undefined) body.expires_at_ms = expiresAt.value;
  if (usageLimit !== undefined) body.usage_limit_requests = usageLimit;

  const req = new Request(endpoint("/admin/api-keys"), {
    method: "POST",
    headers: {
      Authorization: `Bearer ${adminToken}`,
      "Content-Type": "application/json",
      Accept: "application/json",
    },
    body: JSON.stringify(body),
  });

  const result = await doFetchWithDebug(req);
  if (!result.ok) return await writeRequestFailure(runtime, result.status, result.body);

  if (tokenOnly) {
    const tokenValue = extractCreatedToken(result.json);
    if (tokenValue !== null) {
      await writeOutText(runtime, `${tokenValue}\n`);
      return 0;
    }
    await writeErrText(runtime, "Create succeeded but response was missing token.\n");
    return 1;
  }

  await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
  return 0;
};

const runAdminKeysListCommand = async (ctx: UbqAiAdminContext): Promise<number> => {
  const { runtime, endpoint, adminToken, doFetchWithDebug } = ctx;
  const req = new Request(endpoint("/admin/api-keys"), {
    method: "GET",
    headers: {
      Authorization: `Bearer ${adminToken}`,
      Accept: "application/json",
    },
  });
  const result = await doFetchWithDebug(req);
  if (!result.ok) return await writeRequestFailure(runtime, result.status, result.body);
  await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
  return 0;
};

const runAdminKeysRevokeCommand = async (ctx: UbqAiAdminContext): Promise<number> => {
  const { runtime, flags, endpoint, adminToken, doFetchWithDebug } = ctx;
  const id = (getFlagString(flags, "id") ?? "").trim();
  if (!id) {
    await writeErrText(runtime, "Missing --id\n");
    return 2;
  }
  const req = new Request(endpoint("/admin/api-keys/revoke"), {
    method: "POST",
    headers: {
      Authorization: `Bearer ${adminToken}`,
      "Content-Type": "application/json",
      Accept: "application/json",
    },
    body: JSON.stringify({ id }),
  });
  const result = await doFetchWithDebug(req);
  if (!result.ok) return await writeRequestFailure(runtime, result.status, result.body);
  await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
  return 0;
};

const runAdminKeysCommand = async (ctx: UbqAiAdminContext): Promise<number> => {
  const { runtime, subArgs } = ctx;
  const action = subArgs[0] ?? "";

  if (action === "create") return await runAdminKeysCreateCommand(ctx);
  if (action === "list") return await runAdminKeysListCommand(ctx);
  if (action === "revoke") return await runAdminKeysRevokeCommand(ctx);

  return await writeUsageError(runtime, `Unknown admin keys command: ${action || "(missing)"}\n`);
};

/** Parses `--app-id`, printing the diagnostics for a missing or non-numeric value. */
const resolveAppIdFlag = async (ctx: UbqAiAdminContext): Promise<{ ok: true; value: number } | { ok: false }> => {
  const { runtime, flags } = ctx;
  const appIdRaw = getFlagString(flags, "app-id");
  if (!appIdRaw) {
    await writeErrText(runtime, "Missing --app-id\n");
    return { ok: false };
  }
  const appId = parseInt(appIdRaw, 10);
  if (isNaN(appId)) {
    await writeErrText(runtime, "--app-id must be a number\n");
    return { ok: false };
  }
  return { ok: true, value: appId };
};

const runAdminKernelPubkeysListCommand = async (ctx: UbqAiAdminContext): Promise<number> => {
  const { runtime, endpoint, adminToken, doFetchWithDebug } = ctx;
  const req = new Request(endpoint("/admin/kernel-pubkeys"), {
    method: "GET",
    headers: {
      Authorization: `Bearer ${adminToken}`,
      Accept: "application/json",
    },
  });
  const result = await doFetchWithDebug(req);
  if (!result.ok) return await writeRequestFailure(runtime, result.status, result.body);
  await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
  return 0;
};

const runAdminKernelPubkeysAddCommand = async (ctx: UbqAiAdminContext): Promise<number> => {
  const { runtime, flags, endpoint, adminToken, doFetchWithDebug } = ctx;
  const appIdResult = await resolveAppIdFlag(ctx);
  if (!appIdResult.ok) return 2;

  let pem = getFlagString(flags, "pem");
  if (!pem && !runtime.stdinIsTerminal) {
    pem = (await runtime.readStdin()).trim();
  }
  if (!pem) {
    await writeErrText(runtime, "Missing --pem. Pass it as a flag or pipe via stdin.\n");
    return 2;
  }

  const owner = getFlagString(flags, "owner") ?? "cli";

  const req = new Request(endpoint("/admin/kernel-pubkeys"), {
    method: "POST",
    headers: {
      Authorization: `Bearer ${adminToken}`,
      "Content-Type": "application/json",
      Accept: "application/json",
    },
    body: JSON.stringify({ app_id: appIdResult.value, pem, owner }),
  });
  const result = await doFetchWithDebug(req);
  if (!result.ok) return await writeRequestFailure(runtime, result.status, result.body);
  await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
  return 0;
};

const runAdminKernelPubkeysRemoveCommand = async (ctx: UbqAiAdminContext): Promise<number> => {
  const { runtime, endpoint, adminToken, doFetchWithDebug } = ctx;
  const appIdResult = await resolveAppIdFlag(ctx);
  if (!appIdResult.ok) return 2;

  const url = endpoint("/admin/kernel-pubkeys");
  url.searchParams.set("app_id", String(appIdResult.value));

  const req = new Request(url, {
    method: "DELETE",
    headers: {
      Authorization: `Bearer ${adminToken}`,
      Accept: "application/json",
    },
  });
  const result = await doFetchWithDebug(req);
  if (!result.ok) return await writeRequestFailure(runtime, result.status, result.body);
  await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
  return 0;
};

const runAdminKernelPubkeysCommand = async (ctx: UbqAiAdminContext): Promise<number> => {
  const { runtime, subArgs } = ctx;
  const action = subArgs[0] ?? "";

  if (action === "list") return await runAdminKernelPubkeysListCommand(ctx);
  if (action === "add") return await runAdminKernelPubkeysAddCommand(ctx);
  if (action === "remove") return await runAdminKernelPubkeysRemoveCommand(ctx);

  return await writeUsageError(runtime, `Unknown admin kernel-pubkeys command: ${action || "(missing)"}\n`);
};

/**
 * Resolves `--owner`, `--repo` and `--scope` for the kernel-usage commands.
 * With no `defaultScope` the scope falls back to repo only when `--repo` was passed.
 */
const resolveKernelUsageTarget = async (
  ctx: UbqAiAdminContext,
  defaultScope?: "repo" | "org"
): Promise<Readonly<{ owner: string; repo: string; scope: "repo" | "org" }> | null> => {
  const { runtime, flags } = ctx;
  const owner = (getFlagString(flags, "owner") ?? "").trim();
  const repo = (getFlagString(flags, "repo") ?? "").trim();
  const fallbackScope = defaultScope ?? (repo ? "repo" : "org");
  const scopeRaw = (getFlagString(flags, "scope") ?? fallbackScope).trim().toLowerCase();
  const scope = scopeRaw === "org" ? "org" : "repo";
  if (!owner) {
    await writeErrText(runtime, "Missing --owner\n");
    return null;
  }
  if (scope === "repo" && !repo) {
    await writeErrText(runtime, "Missing --repo for scope=repo\n");
    return null;
  }
  if (scope === "org" && repo) {
    await writeErrText(runtime, "--repo must be omitted for scope=org\n");
    return null;
  }
  return { owner, repo, scope };
};

const runAdminKernelUsageGetCommand = async (ctx: UbqAiAdminContext): Promise<number> => {
  const { runtime, endpoint, adminToken, doFetchWithDebug } = ctx;
  const target = await resolveKernelUsageTarget(ctx, "repo");
  if (!target) return 2;

  const url = endpoint("/admin/kernel-usage");
  url.searchParams.set("owner", target.owner);
  url.searchParams.set("scope", target.scope);
  if (target.scope === "repo") url.searchParams.set("repo", target.repo);

  const req = new Request(url, {
    method: "GET",
    headers: {
      Authorization: `Bearer ${adminToken}`,
      Accept: "application/json",
    },
  });
  const result = await doFetchWithDebug(req);
  if (!result.ok) return await writeRequestFailure(runtime, result.status, result.body);
  await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
  return 0;
};

const runAdminKernelUsageSetCommand = async (ctx: UbqAiAdminContext): Promise<number> => {
  const { runtime, flags, endpoint, adminToken, doFetchWithDebug } = ctx;
  const target = await resolveKernelUsageTarget(ctx);
  if (!target) return 2;

  const usageLimitRaw = getFlagString(flags, "usage-limit");
  if (!usageLimitRaw) {
    await writeErrText(runtime, "Missing --usage-limit\n");
    return 2;
  }
  const parsedLimit = parseUsageLimitRequests(usageLimitRaw);
  if (!parsedLimit.ok) {
    await writeErrText(runtime, "--usage-limit must be a non-negative number, -1, or 'unlimited'\n");
    return 2;
  }
  const usageLimit = parsedLimit.value;

  const resetUsage = flags["reset-usage"] === true;
  const windowMsRaw = getFlagString(flags, "window-ms");
  let windowMs: number | undefined;
  if (windowMsRaw) {
    const parsedWindow = parseWindowMs(windowMsRaw);
    if (!parsedWindow.ok) {
      await writeErrText(runtime, "--window-ms must be a positive number\n");
      return 2;
    }
    windowMs = parsedWindow.value;
  }

  const body: Record<string, unknown> = {
    owner: target.owner,
    usage_limit_requests: usageLimit,
    reset_usage: resetUsage,
    scope: target.scope,
  };
  if (target.scope === "repo") body.repo = target.repo;
  if (windowMs !== undefined) body.window_ms = windowMs;

  const req = new Request(endpoint("/admin/kernel-usage"), {
    method: "POST",
    headers: {
      Authorization: `Bearer ${adminToken}`,
      "Content-Type": "application/json",
      Accept: "application/json",
    },
    body: JSON.stringify(body),
  });
  const result = await doFetchWithDebug(req);
  if (!result.ok) return await writeRequestFailure(runtime, result.status, result.body);
  await writeOutText(runtime, `${JSON.stringify(result.json, null, 2)}\n`);
  return 0;
};

const runAdminKernelUsageCommand = async (ctx: UbqAiAdminContext): Promise<number> => {
  const { runtime, subArgs } = ctx;
  const action = subArgs[0] ?? "";

  if (action === "get") return await runAdminKernelUsageGetCommand(ctx);
  if (action === "set") return await runAdminKernelUsageSetCommand(ctx);

  return await writeUsageError(runtime, `Unknown admin kernel-usage command: ${action || "(missing)"}\n`);
};

export {
  runAdminKernelPubkeysAddCommand,
  runAdminKernelPubkeysCommand,
  runAdminKernelPubkeysListCommand,
  runAdminKernelPubkeysRemoveCommand,
  runAdminKernelUsageCommand,
  runAdminKernelUsageGetCommand,
  runAdminKernelUsageSetCommand,
  runAdminKeysCommand,
  runAdminKeysCreateCommand,
  runAdminKeysListCommand,
  runAdminKeysRevokeCommand,
  runAdminUploadAuthCommand,
};
