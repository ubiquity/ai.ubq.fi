# Mac gateway service

The Mac companion listens on `http://127.0.0.1:8000`. `com.ubiquity.ai.local` is a per-user launchd agent: it starts at
login and restarts after exit. It does not run before the user logs in or keep a sleeping Mac awake.

Configuration lives in `ops/com.ubiquity.ai.local.plist`, linked from `~/Library/LaunchAgents/`. The repository-root
`.env` contains the existing upstream credentials. Local client and admin authentication are disabled for loopback
requests using the listener and peer checks; the admin dashboard opens without sign-in. Local KV is `.data/kv.sqlite3`.
Code runs from the immutable release selected by `.data/current`, including that release's Deno configuration. Runtime
identity is `mac-<full-git-sha>`.

Provider quota is sampled at startup and every fifteen minutes into local KV so the Providers dashboard has current
capacity and accumulates its own history.

The daemon can read the existing synced `~/.codex/auth.json` through the gateway's normal local credential loader. Keep
the existing sign-in and cross-machine sync. This uses the gateway's existing KV credential-pool behavior after initial
loading; it does not change the synced auth file. Production scheduled billing remains on the VPS. Local capped paid
billing maintenance is tracked in [#266](https://github.com/ubiquity/ai.ubq.fi/issues/266).

## Codex credential repair

Both hosts keep their own KV auth pool seeded from the same synced `~/.codex/auth.json`, and each refreshes its own
access token. One host can therefore rotate a token out from under the other, after which the host left behind answers
401 for that account while the other still works.

`com.ubiquity.ai.codex-auth-repair` is a second per-user launchd agent that heals exactly that split. It runs
`scripts/codex-auth-repair.ts --apply` at minutes 2, 17, 32 and 47 — just after each capacity sample — and macOS runs a
schedule missed during sleep at the next wake.

What one run does:

1. Probes each local pool account with an account-bound `GET` on the Codex usage endpoint. That is not inference, and it
   never calls the OAuth token endpoint, so a check can never rotate a shared refresh token or break the other host.
2. Stops there when every account authenticates. No SSH connection is opened and no credential is rewritten.
3. Otherwise reads candidate credentials from the local `~/.codex/auth.json`, the VPS pool, and the VPS
   `~/.codex/auth.json`, probes each candidate the same way, and adopts the longest-lived candidate that authenticates.
4. Writes only the affected account into local KV, guarded by a versionstamp check, so a rotation the gateway performs
   concurrently wins and the repair simply retries on its next run.

A credential is replaced only when the local one is rejected and the replacement is accepted. An account with no
authenticating candidate is reported as blocked rather than guessed at, and a transport failure is never grounds for a
write. Inspect it with:

```sh
deno run --env-file=.env --unstable-kv --allow-env --allow-read --allow-write=.data \
  --allow-net=chatgpt.com --allow-run=/usr/bin/ssh scripts/codex-auth-repair.ts
deno run --env-file=.env --unstable-kv --allow-env --allow-read --allow-write=.data \
  --allow-net=chatgpt.com --allow-run=/usr/bin/ssh scripts/codex-auth-repair.ts --diff
tail -n 20 .data/codex-auth-repair.log
```

Without `--apply` a run reports the plan and writes nothing. `--diff` also compares the other host's credentials when
nothing is broken, which is how a divergence is seen before it starts failing. The gateway caches the pool for at most
`CODEX_AUTH_CACHE_TTL_MS` (five minutes), so an out-of-process repair is picked up within that window.

From the clean Mac repository root, `deno task deploy:mac` installs the committed release and loads the launch agent.
Verify authenticated local inference after deployment before changing Codex routing. The task's health check alone does
not establish provider readiness. The deployment also registers the repair agent and reports `repair_agent` in its
output; a registration failure is reported without failing an otherwise verified service deployment.

```sh
launchctl print gui/501/com.ubiquity.ai.local
launchctl kickstart -k gui/501/com.ubiquity.ai.local
launchctl print gui/501/com.ubiquity.ai.codex-auth-repair
curl --fail http://127.0.0.1:8000/health
tail -n 50 .data/mac.stderr.log
```

Use a custom Codex provider with base URL `http://127.0.0.1:8000/v1`, the Responses wire API, and the existing
`UOS_AI_TOKEN`. Keep the remote `uos` provider available for an explicit remote profile. Local inference avoids the VPS
round trip; traffic between the Mac and upstream model providers still uses the internet. The two gateways have
independent usage and routing state.
