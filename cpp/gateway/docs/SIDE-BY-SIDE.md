# Native gateway side-by-side runbook

Run the sealed C++ package as a manually started, IPv4 loopback service with fresh synthetic KV. Keep Bun as the sole production listener behind Caddy and the sole writer of production KV throughout the observation. Use a small sample of public reads on both listeners, plus the packaged mock harness for authenticated and stateful behavior.

## 1. Qualify the package and host

Start with an accepted Linux ARM64 package from [BUILD.md](BUILD.md). Record its full source SHA, binary and `manifest.sha256` digests and `runtime-closure.json`. Compare these to the build and package receipts. The launcher independently checks the package, and linked host libraries before it opens KV or binds the port. Copy the sealed package as a unit. Keep this runbook, rendered unit and any receipts outside the sealed directory. The current producer packages `ops/launch-native.sh`, so obtain this kit from the source checkout. [Native runtime and package contract](RUNTIME.md)

Use a Linux host with the package's ARM64 loader and library closure, Landlock ABI 8 or newer, seccomp thread synchronization, and systemd with cgroup v2 CPU and memory controllers and cgroup BPF IP and socket-bind filtering. Check available memory, CPU and disk headroom against the current Bun baseline before starting. Qualify a Raspberry Pi on its actual 64-bit OS, loader, library closure and memory. Apple Silicon requires a separately qualified package and launchd plan. [Platform evidence](MACOS.md)

Record the current unit, proxy upstream, health identity and proxy configuration. Choose a free, unprivileged
native port other than `7999` and `8000`. Use the current listener's actual port for public-read replay. [Existing VPS operations](https://github.com/ubiquity/ai.ubq.fi/blob/2b23fb6837e2b5c0dc9ffa50771f52d322dd413a/ops/README.md)

## 2. Prepare isolated state and render the unit

Set the shell variables below to operator-selected values. Use canonical absolute paths with simple path components for unit substitution. Choose three disjoint roots with no symlink, hard-link or bind-mount aliases between them. Put the native paths outside locations hidden by `ProtectHome` or `PrivateTmp`.

| Template field   | Shell variable | Value                                                                                        |
| ---------------- | -------------- | -------------------------------------------------------------------------------------------- |
| `@SHADOW_USER@`  | `shadow_user`  | Dedicated unprivileged local account with no login and no access to Bun's account or secrets |
| `@SHADOW_GROUP@` | `shadow_group` | Dedicated group for that account                                                             |
| `@PACKAGE_ROOT@` | `package_root` | Verified, administrator-owned immutable package readable by the shadow account               |
| `@STATE_ROOT@`   | `state_root`   | New private directory on local storage, owned by the shadow account                          |
| `@BUN_ROOT@`     | `bun_root`     | Canonical running Bun checkout root to hide from the native unit                             |
| `@NATIVE_PORT@`  | `native_port`  | Selected free loopback port, identical in `launch.json` and the rendered unit                |

Set `bun_unit` to the running Bun unit name, `bun_port` to its loopback port, and `native_unit` to `uos-gateway-side-by-side.service`. Use a private operator directory `receipt_dir` for evidence. Set `unit_file` to the absolute path of the rendered unit in an administrator-owned operations directory, with basename `$native_unit`.

Create fresh directories and seed the empty `bun-jsc-v1` marker using `sqlite3`. The launcher requires a regular, owner-private database at startup and the normal storage layer creates its native schema when it opens the marked file. Use this seed only in the new shadow root. Keep the production Bun and legacy Deno databases in their existing custody. Run each block after the previous block succeeds.

```bash
sudo mkdir -m 0700 -- "$state_root" &&
  sudo chown -- "$shadow_user:$shadow_group" "$state_root" &&
  sudo install -d -m 0700 -o "$shadow_user" -g "$shadow_group" \
    "$state_root/.data" "$state_root/home" "$state_root/home/.codex"
```

```bash
sudo -u "$shadow_user" -g "$shadow_group" \
  env -i PATH=/usr/bin:/bin HOME="$state_root/home" TMPDIR="$state_root/.data" \
  /bin/sh -eu -c '
    umask 077
    set -C
    : > "$1"
    exec sqlite3 -batch -bail -init /dev/null "$1" "$2"
  ' sh "$state_root/.data/kv-bun.sqlite3" "
    CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
    INSERT INTO uos_kv_metadata (key, value) VALUES ('codec', 'bun-jsc-v1');
  "
```

Set `package_manifest_sha256` to the independently recorded lowercase SHA-256 of `manifest.sha256` from the seal receipt. Write a launch manifest for the ordinary packaged entrypoint. The synthetic upstream `fixture` field belongs to the namespace harness in step 4.

```bash
umask 077
jq -n --arg package "$package_root" --arg state "$state_root" \
  --arg package_hash "$package_manifest_sha256" \
  --argjson port "$native_port" '{
    schema: "uos-gateway-launch-v1", packageRoot: $package, stateRoot: $state,
    packageManifestSha256: $package_hash,
    hostname: "127.0.0.1", port: $port
  }' > "$receipt_dir/launch.json" &&
  sudo install -m 0600 -o "$shadow_user" -g "$shadow_group" \
    "$receipt_dir/launch.json" "$state_root/launch.json"
```

Replace each `@…@` field in a copy of the unit template. Review the resolved paths, owner and port, and run `systemd-analyze verify --recursive-errors=one "$unit_file"`. `Type=exec` acknowledges execution of `/usr/bin/env`. Use the HTTP checks below for application readiness. [Unit template](../ops/side-by-side/uos-gateway-side-by-side.service.in)

| Environment or config                            | Side-by-side value                                                                                                                                                                                                                                                |
| ------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `DENO_TIMELINE`                                  | Set to `production` as on Bun. This disables local authentication bypasses even on loopback.                                                                                                                                                                      |
| `HOME`, `CODEX_HOME`, `TMPDIR`                   | Set to the private shadow home, its empty `.codex` directory and the shadow `.data`. Native startup sets `SQLITE_TMPDIR` to that same `.data`.                                                                                                                    |
| `.env`, deployment markers and runtime overrides | Omit Bun's `.env`, `DENO_DEPLOY`, `DENO_DEPLOYMENT_ID`, `DENO_REGION`, `DENO_DEPLOY_BUILD_ID` and other Deploy markers. Keep the default host trust store and verified loader. The unit starts with `env -i`.                                                     |
| Gateway, admin and authority credentials         | Leave `UOS_AI_TOKEN`, `DENO_DEPLOY_TOKEN`, `UOS_AI_KERNEL_PUBLIC_KEY` and `SENTINEL_REPLAY_KEY` unset. The mock harness supplies only its own synthetic credentials.                                                                                              |
| Provider credentials and bindings                | Leave `CODEX_AUTH_JSON_B64` and all provider keys, OAuth credentials and workspace identifiers unset. Start with an empty account pool and no synchronized auth files or native control socket. [Full environment inventory](RUNTIME.md#launch-and-offline-tools) |

The template grants write access to the shadow `.data`, mounts the package read-only and hides the Bun tree and user homes. It keeps `NoNewPrivileges` and an empty capability bounding set while allowing the native runtime to apply Landlock and seccomp itself. The socket policy permits IPv4 loopback traffic and restricts explicit binds to the selected TCP port. Confirm the host applies both cgroup BPF policies and that no loopback proxy, tunnel or relay exposes the candidate or gives it a path to real upstreams. Qualify enforcement on the target OS before the observation window. Use local test endpoints to prove a permitted loopback connection, a denied non-loopback connection and a denied wrong-port bind. A successful `verify` checks unit syntax. Kernel enforcement requires its own receipt.

The template starts at `MemoryMax=512M`, `MemorySwapMax=0`, `CPUWeight=25` and `Nice=10`. Compare effective limits and scheduling weights with Bun on the target host. Bun's checked-in VPS unit leaves CPU weight and nice at system defaults. Ensure the native unit has lower priority within the effective slice and record the approved memory budget and minimum host headroom. [Bun unit](https://github.com/ubiquity/ai.ubq.fi/blob/2b23fb6837e2b5c0dc9ffa50771f52d322dd413a/ops/ai-ubq-fi.service)

## 3. Start and check the observation service

After the operator approves a side-by-side window, link and start only the rendered native unit. Confirm its port is free beforehand. The template has no boot target and uses `Restart=no` so a startup failure stays visible.

```bash
sudo ss -H -ltnp "sport = :$native_port"
started_at=$(date --iso-8601=seconds)
sudo systemctl link "$unit_file"
sudo systemctl start "$native_unit"
```

```bash
sudo ss -H -ltnp "sport = :$native_port"
systemctl show "$native_unit" --property=User,Group,MainPID,ControlGroup,Result,MemoryMax,MemorySwapMax,CPUWeight,Nice
cgroup_dir="/sys/fs/cgroup$(systemctl show --value --property=ControlGroup "$native_unit")"
sudo bpftool cgroup show "$cgroup_dir" effective
```

Check the `127.0.0.1:$native_port` listener belongs to the native unit, the effective cgroup has ingress, egress and IPv4 bind programs, and the process owns only the shadow KV files. Check both listeners directly. Capture status, response headers and body in the private receipt directory, using the same command for Bun with its own filenames and port.

```bash
curl -q --noproxy '*' --fail --silent --show-error --max-time 3 \
  --dump-header "$receipt_dir/native-health.headers" \
  --output "$receipt_dir/native-health.json" --write-out '%{http_code}\n' \
  "http://127.0.0.1:$native_port/health"
jq -e '.status == "available" and (.release.git_sha | type == "string") and
  (.release.deployment_id | type == "string")' "$receipt_dir/native-health.json"
```

Expect `200` and `status: "available"`. Match `release.git_sha` and `release.deployment_id` against `x-uos-git-sha` and `x-uos-deployment-id` and the expected release. A source-tree development or test build reports the checked-in `unknown` constant. Portable and macOS packages compile the full `HEAD` commit into the executable, so a package must report that full SHA and `vps-<full-git-sha>` in both headers and body. An `unknown` value from a package is an unbound build and fails production acceptance. The packaged launcher opens and checks the private KV before listening. Health confirms liveness and identity, while authenticated behavior and durable restart need the mock smoke. [Native health handler](../src/health.cpp) · [Release constant](../src/release.cpp)

### Health, readiness and metrics

| Route or source                                                                                       | What to observe                                                                                                                                                                                                                        |
| ----------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `GET` or `HEAD /health`                                                                               | Public liveness and identity. GET returns JSON and HEAD returns headers. This is the routine poll.                                                                                                                                     |
| `GET` or `HEAD /openapi.json`                                                                         | Public artifact/readiness check. A GET also compares the packaged API document.                                                                                                                                                        |
| `GET /health/providers` and `GET /admin/providers`                                                    | Admin-only passive cached provider state and staleness, with additional quota fields on the admin route. Use synthetic auth in isolation. A credential-free request to `/health/providers` on the native listener should return `401`. |
| `GET /health/upstream`                                                                                | Admin-only active provider probes that can use account credentials and reach upstream. Exercise only with mocks during the observation.                                                                                                |
| `GET /admin/providers/capacity`, `/admin/providers/capacity/rollups`, `/admin/prompt-cache-analytics` | Admin diagnostic views. Capacity accepts `?refresh=live` and can fetch upstream. Keep these out of replay and use synthetic evidence for stateful checks.                                                                              |

Use `/health` for HTTP liveness and the journal and cgroup for metrics. The current gateway has no separate readiness or Prometheus scrape route. Inspect the native journal with `journalctl -u "$native_unit" --since "$started_at" --no-pager` and the Bun journal with its own unit name. [Route dispatch](../src/handler/index.cpp)

| Journal event or source                                                   | What to track                                                                                                                                                                                                                                                                                                       |
| ------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Listening on http://…`, `Native startup refused`, `HTTP shutdown failed` | Expected interface and startup success, manifest or confinement failures, and graceful stop. Check systemd result, exit status and OOM events as well.                                                                                                                                                              |
| `auth`                                                                    | Status, scope, auth method and failure reason. Investigate any successful bypass or unexpected rejection in the negative and synthetic positive controls.                                                                                                                                                           |
| `request_terminal`                                                        | Route, status, release, `latency_ms`, first semantic commitment, `stream_terminal_type`, `failure_kind`, `delivery_outcome`, attempted provider and usage. Compare latency percentiles, 5xx, interrupted streams and settlement in the mock run. Public health and OpenAPI checks produce their own replay timings. |
| `quota_accounting_failed`                                                 | Investigate any occurrence. Tie each admitted mock request to the actual delivery outcome and exactly one durable settlement. Check persisted usage after restart.                                                                                                                                                  |
| `optional_telemetry_shutdown`                                             | When emitted, inspect failed, dropped, retained, drain-timeout and `shutdown_incomplete` fields. Verify durable work settled before telemetry and KV closure.                                                                                                                                                       |
| cgroup v2 and live process                                                | Record `memory.current`, `memory.peak`, `memory.events`, `memory.max`, `memory.swap.max`, `cpu.stat` and `cpu.weight` for both units while active. Sample gateway RSS and host `MemAvailable`, observe sustained CPU and disk growth, and compare their Bun baseline over the same window.                          |

Treat a wrong identity or listener, an unsuccessful auth or confinement control, any unexpected KV access, OOM, Bun SLO regression, or unexplained status/body mismatch as a stop signal. Agree on the window, a request ceiling and numeric latency/error/resource limits before starting. Short loopback timings help spot regressions in public reads. Use the mock harness for inference and stream metrics. [Terminal events](../src/handler/terminal_log.cpp) · [Shutdown lifecycle](../src/serve.cpp)

## 4. Replay a sample and run the mock acceptance

Select up to 12 method and path pairs from sanitized access-log counts or use a fixed mix of health and OpenAPI requests. Copy only the literal method and path. Exclude queries, headers, cookies, request bodies and user data. The script sends one fresh request to Bun and then to native over loopback. Run from the same host. Use Bash, `jq` and curl 8.4 or newer for response size enforcement, and pass an absolute private scratch directory you own.

```bash
samples=('GET /health' 'HEAD /health' 'GET /openapi.json' 'HEAD /openapi.json')
bash cpp/gateway/ops/side-by-side/replay.sh "$receipt_dir" "$bun_port" "$native_port" "${samples[@]}"
bash cpp/gateway/ops/side-by-side/replay.sh --run "$receipt_dir" "$bun_port" "$native_port" "${samples[@]}"
```

Review the dry-run list before using `--run`. Replay ignores curl configuration and proxies, follows zero redirects, and caps each response at 256 KiB with a one-second connect deadline and three-second total deadline. It compares status and requires `200`, sorts JSON object keys while preserving array order, and masks only the two health identity values. HEAD compares status. The summary reports timings and mismatches without response bodies. Interpret exit `0` as all pairs matched, `1` as a failed comparison or transport, and `2` as invalid input or missing replay prerequisites. Retain individual health identity receipts from step 3.

Keep the allowlist closed. Inference, embedding jobs, Responses operations, live calls and WebSocket upgrades can admit or bill work. Exclude `/v1/models` and versioned catalogs, active provider/capacity refresh, auth uploads, refresh or login, and account, reset, key, payment or quota operations. Even `GET /admin/api-keys/{keyId}/paid-fallbacks` can trigger ledger reconciliation. Reserve these for local mocks with synthetic accounts and their own durable KV. [Accounting read path](../src/admin/api_keys.cpp)

Run the [same-binary smoke harness](../tests/production_smoke/README.md) with the exact sealed native package, its matching receipts and output directory, and a real no-outbound fixture namespace. That lane uses a runtime manifest with `port: 0` and a `fixture.syntheticOrigin` local mock. Verify authenticated request and rejection paths, stream and WebSocket delivery, the SIGTERM-in-flight case, and usage after restart. Use matched mock workloads for latency and RSS evidence. Add isolated fixtures for any additional provider, account mutation, fallback or ledger path the owner requires for cutover.

## 5. Stop the observation and retain evidence

On any stop signal or at the end of the window, stop replay first and then stop only the native unit. Use systemd's normal SIGTERM so the server drains requests before telemetry and KV closure. Check the exit status and the absence of a native listener. Preserve the private state, any WAL, the exact logs and package receipts for diagnosis if the drain times out or is killed. Verify Bun still has its original listener, health identity and proxy route.

```bash
sudo systemctl stop "$native_unit"
systemctl show "$native_unit" --property=ActiveState,Result,ExecMainStatus
sudo ss -H -ltnp "sport = :$native_port"
sudo journalctl -u "$native_unit" --since "$started_at" --no-pager
```

To remove the observation unit, run `sudo systemctl disable "$native_unit"` to remove the systemd link and `sudo systemctl daemon-reload`. Retain the operator copy and receipts. For a retry, review the failure, qualify the next exact package and use a new private state root.

## 6. Owner cutover decision

1. **Release and window.** Select the exact accepted package, reviewed native production unit and rollback target. Set the maintenance window, stop limits and client acceptance budget. The observation unit's loopback egress policy and synthetic state are for replay. A production candidate needs its own reviewed confinement, provider access and full release identity.
2. **Storage and rollback.** Rehearse with a consistent stopped copy of `.data/kv-bun.sqlite3`. Verify native decoding of real values, revisions and expiry, and verify that the previous runtime can reopen the latest native writes. Keep the retained legacy `.data/kv.sqlite3` in its original custody. If it is authoritative, use the archived conversion procedure with a stopped source and a new destination. [Historical tooling](https://github.com/ubiquity/ai.ubq.fi/tree/ca045a83ebaab6c50c20567d1a825638abac0891) · [Cutover checklist](CUTOVER.md)
3. **Storage evidence.** Include sparse and cyclic values in the stopped-copy readback and rollback rehearsal. Verify journal disposition, durable quota and single-writer ownership before adopting the latest copy. [Durable KV](rt/kv-durable.md)
4. **Credentials and provider access.** Authorize credential custody, native home and control-socket access, per-account ownership and any live probe budget. Confirm the access token expiry and owner for each credential. Coordinate exclusive refresh ownership before moving a shared credential. Use the approved explicit environment and verify client/admin auth and paid settlement. Require the durable active Codex subscription to stay selected until the applicable transition is proved. Require fresh full-pool proof before Surplus, and an authoritative paid-tier quota or capacity signal before OpenLux. Exercise transient upstream errors in mocks and verify that they leave the waterfall unchanged. Obtain the applicable standalone Sentinel reader acceptance for the exact captured source and artifacts. [Repository operator policy](../../../AGENTS.md)
5. **Ingress and client acceptance.** Approve the Caddy upstream switch and save the prior config. For a same-host switch, keep the existing proxied A record and hostname-specific Cloudflare Worker exclusion. If DNS or proxy changes are needed, have the owner approve the exact diff. Under the deployment lock, drain the old writer, install only the approved latest-state candidate, and validate the new loopback identity before switching ingress. Prove direct Mac-to-VPS authenticated inference and full health identity in headers and body, then verify public `/health` after the proxy or DNS change. [DNS, TLS and direct client check](https://github.com/ubiquity/ai.ubq.fi/blob/2b23fb6837e2b5c0dc9ffa50771f52d322dd413a/ops/README.md#dns-and-tls)

If cutover has already occurred, close ingress to the candidate, drain and stop it, and preserve its **latest** database and WAL under the deployment lock. To return to Bun, prove the latest stopped native-written Bun/JSC state can reopen safely, then restore the approved Bun selector and that state. To return to legacy Deno, reverse-convert the latest stopped Bun/JSC state to a new Deno V8 destination and verify the private rollback receipt and supported legacy release/profile. A stale backup loses writes. Keep ingress closed if latest-state recovery is unproved. Start the reviewed rollback unit, check its loopback identity and restore the saved Caddy upstream. Verify direct Mac-to-origin authenticated inference and full release identity before reopening public ingress. Restore DNS if the cutover changed it and confirm public health. [Rollback procedure and receipt contract](https://github.com/ubiquity/ai.ubq.fi/blob/2b23fb6837e2b5c0dc9ffa50771f52d322dd413a/ops/README.md#data-migration-and-recovery)

| Decision area                     | Go evidence                                                                                                                                       | No-go                                                                                                                                                                              |
| --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Host and package                  | Exact immutable package, effective cgroup and native confinement, correct listener and release identity on the target host                        | Unknown or unbound identity, missing isolation or resource headroom, unsupported platform or closure                                                                               |
| Replay and client contract        | All sampled pairs match or have owner-accepted explanations, and same-binary mock auth, streaming, delivery and restart checks pass               | Unexplained wire, status, validation, quota, settlement or security difference                                                                                                     |
| Storage and recovery              | Full stopped-copy forward and reverse evidence, sparse and cyclic controls pass, and latest-state recovery rehearsed                              | Either storage blocker remains open, live/shared store, incomplete proof, or rollback would lose writes                                                                            |
| Credentials and provider behavior | Approved credential custody and refresh ownership, approved probe budget, routing and accounting receipts                                         | Unowned shared refresh, unresolved account state, weaker auth, or unauthorized spend                                                                                               |
| Production switch                 | Owner approval, accepted preview and monitoring, ingress rollback prepared, and the direct Mac/VPS client and public health acceptance plan ready | Missing approval, incomplete monitoring or recovery, missing external reader acceptance where applicable. Close the cutover as failed if live client or identity acceptance fails. |
