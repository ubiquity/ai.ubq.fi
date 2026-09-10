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

From the clean Mac repository root, `deno task deploy:mac` installs the committed release and loads the launch agent.
Verify authenticated local inference after deployment before changing Codex routing. The task's health check alone does
not establish provider readiness.

```sh
launchctl print gui/501/com.ubiquity.ai.local
launchctl kickstart -k gui/501/com.ubiquity.ai.local
curl --fail http://127.0.0.1:8000/health
tail -n 50 .data/mac.stderr.log
```

Use a custom Codex provider with base URL `http://127.0.0.1:8000/v1`, the Responses wire API, and the existing
`UOS_AI_TOKEN`. Keep the remote `uos` provider available for an explicit remote profile. Local inference avoids the VPS
round trip; traffic between the Mac and upstream model providers still uses the internet. The two gateways have
independent usage and routing state.
