# Rust Worker Codex subscription tier through the native owner - 2026-11-05

Branch `rust-codex-worker-owner`, based on `origin/rust-migration` at `405b5bd`. This note closes the Worker-side part of
promotion blocker B6 in `docs/research/rust-promotion-runbook-2026-11-05.md`: "No Codex credential owner for the
Worker". It records the design the existing decisions already authorize, what this branch implements, the exact operator
steps, and what remains open.

## Design: the native owner keeps custody (option b)

The design question was whether (a) the Worker holds the Codex account pool itself and refreshes it, or (b) the Worker
delegates credential custody and refresh to a native owner over the `UOS_NATIVE_CARRIER` HTTPS carrier. The decisions
already settle it in favour of (b), and they rule out (a).

| Evidence                                                                                                                                                                                                                                                                     | Effect                                                                                                      |
| ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| DECISIONS 2026-10-19 "Scoped native connector and native-direct imports approved": credential and refresh-bearing documents "go directly to the trusted native receiver ... not through Cloudflare".                                                                         | Refresh tokens must not reach the Worker, so (a) is excluded.                                               |
| DECISIONS 2026-10-21 "Isolated local operator bootstrap ...": "Refresh-bearing documents remain native"; only "the validated access projection reaches the same common Durable Object".                                                                                      | The Durable Object holds access-only projections.                                                           |
| DECISIONS 2026-10-21 "Native Worker HTTPS carrier is constructor-pinned ...": the Worker reads only `UOS_NATIVE_CARRIER`, which names a fixed HTTPS origin and transport identity; it "does not install a native owner, grant renewal ownership".                            | The carrier exists. Renewal ownership must come from explicit trusted configuration.                        |
| DECISIONS 2026-10-22 "Workers Free core-state migration ...": "Raw managed Codex credentials still require current native custody and access-only sanitization before preparation".                                                                                          | Import keeps native custody.                                                                                |
| DECISIONS 2026-10-02 "The Mac gateway delegates a shared CLI credential lineage to native Codex": an account bound to a durable `native_owner` is refreshed by the native Codex daemon, never by a second OAuth writer.                                                      | One refresh writer per lineage: the Codex app-server in that home.                                          |
| AGENTS.md "Local Credential Recovery Safety": OAuth refresh is a credential mutation; shared refresh tokens must not get a second writer.                                                                                                                                    | Copying the VPS refresh tokens into Cloudflare would create two writers. (a) is excluded again.             |
| Code: `gateway-authority/src/native_control/{owners,receiver,managed}.rs`, `gateway-worker/src/native_host.rs` (`refresh_routing`, `complete_renewal`, `renew`), `gateway-native/src/owner_renewal.rs` and `bin/uos-native.rs` (`owner_renewal` daemon configuration v2/v3). | The renewal protocol, fences, publication receipt and native owner daemon are already ported on both sides. |

Option (b) also adds the least new trust surface. The Worker learns only a fixed owner-to-home mapping and two
capabilities for the transport key it already holds. Refresh tokens never leave the native host. The native side reuses
the Codex app-server that the VPS gateway already delegates to, so the Codex app-server stays the only writer of
`auth.json`.

## What was missing

Every Worker path that renews or publishes Codex access calls `NativeBindings::renewal_owners()`. These paths are:

- `refresh_routing`, the 401 retry in `/v1/responses`;
- `managed_publication`, where the owner publishes the renewed access projection;
- the native import authority and publication dispatch.

The exported shipping entrypoint built `NativeBindings` only from `UOS_CANDIDATE_NATIVE_V1` or `UOS_NATIVE_CARRIER`. In
both cases the registry was never set, because only the Rust fixture called `with_renewal_owners`. As a result, even
with a carrier configured, every Codex 401 ended with `native_unavailable` and no access projection could be renewed.
`cloudflare.config.ts` also did not declare the carrier secret.

The remaining parts were already ported and are reused unchanged:

- routing, quota, banked-reset and overage logic (`gateway-authority::routing`, `reset`, `manual_reset`, `probes`, `gateway-routing-runtime`);
- `none` preservation and `ultra` to `max` (`gateway-providers/src/source_request.rs`, `source_controls.rs`);
- Codex dispatch to `https://chatgpt.com/backend-api/codex/responses` with the admitted account's access projection (`gateway-worker/src/routing_host.rs::credential`);
- the renewal fences and publication verification (`native_control::receiver`, `managed`).

## Implemented on this branch

1. **Carrier renewal registry** (`rust/crates/gateway-worker/src/native_host.rs`). The `UOS_NATIVE_CARRIER` JSON accepts
   an optional `renewal_owners: [{"owner_id","codex_home"}]` and builds `RegisteredRenewalOwners`. That type still
   enforces one or two owners, distinct ids and homes, and absolute POSIX homes. The registry must pair exactly with the
   `owner_renewal` and `access_publish` capabilities. A registry without both capabilities fails closed, and so do the
   capabilities without a registry. A partial pairing would otherwise silently disable refresh or accept unregistered
   renewal. The configuration shape used before this change still parses and still grants no renewal ownership.
   `renewal_owners()` is now `pub` so the integration test can observe it. The crate shrank by 10 lines; the ceiling was
   tightened to 20209.
2. **Secret declaration** (`cloudflare.config.ts`). `UOS_NATIVE_CARRIER: bindings.secret()` replaces the comment "this
   Worker never claims a native owner". An absent secret keeps the carrier unavailable.
3. **Tests**
   - `rust/crates/gateway-worker/tests/native_carrier_config.rs`: the registry configures renewal; the registry and the
     capabilities must pair; entries fail closed (relative home, empty id, unknown field, missing home, duplicate id or
     home, three owners).
   - `rust/crates/gateway-routing-runtime/tests/codex_native_refresh.rs` (the refresh fence at the routing boundary): a
     Codex 401 asks the host to renew exactly the dispatched `CredentialFence::Codex` (account hash, credential version,
     source revision, native generation) once.
     - A published renewal retries the same global active account without marking it invalid.
     - A second 401 on the already-renewed version marks that credential invalid and lets the pool advance.
     - An unpublished renewal marks only that credential invalid.
     - 500, 502 and 503 neither refresh nor move the active account.
   - The routing test host (`gateway-routing-runtime-test-support/src/host.rs`) now scripts the renewal outcome and
     records the requested fences. The default is unchanged (`Unavailable`).

The Worker-side check that the owner renewed exactly the admitted credential already existed:
`refresh_routing` compares the prepared action fence with the admission's account hash, credential version, source
revision and access generation. `verify_action_publication` then records the published source-native and access
generations, which are the refreshed credential fingerprint, under the existing renewal fences.

No new crate, gateway-native change or authority change was needed: the native owner endpoint already exists.
`uos-native` serves `POST /v1/native/control` (`codex_renew_access`) when started with the `owner_renewal` block of its
v2 or v3 configuration.

## Operator steps

These steps require owner authorization at each marked point. None was executed by this work, and no VPS, credential or
live data was touched.

### 1. Preconditions (read-only checks on the VPS)

```sh
ssh codex@vps.pavlovcik.com 'uname -r; test -S /home/codex/.codex/app-server-control/app-server-control.sock && echo socket-ok; test -f /home/codex/.codex/auth.json && echo auth-ok'
```

- `uos-native` enforces Landlock ABI 9 and a seccomp no-new-process filter on Linux (`gateway-native/src/confinement.rs`).
  It exits `native_confinement_unavailable` on an older kernel. That exit is the expected fail-closed result, not a bug
  to work around.
- Every pool account the Worker must renew needs a `native_owner` binding whose `codex_home` is a registered home. On
  Linux the TypeScript gateway binds at most the single home `/home/codex/.codex` (`src/codex/native-auth.ts`), and the
  authority registry admits at most two owners. Uploaded accounts without `native_owner` keep working on the VPS. On the
  Worker, however, a 401 for such an account fails with an owner error (`503`) and does not mark the account invalid
  (DECISIONS 2026-10-02: owner failures "do not establish current-credential invalidity"). Do not enable the tier on the
  Worker while the active account is not native-owned.

### 2. Generate the transport identity (on the operator's machine; secret)

The candidate's installed control owner is the `UOS_CANDIDATE_NATIVE_V1` identity (`InitialControlInstaller`, B3). Renewal
authority checks that the transport identity equals that installed owner (`native_control/facts.rs::read_control`). The
key that installs the pool must therefore be the same key that later renews it.

```sh
umask 077; mkdir -p "$TASK/native-owner"
KEY=$(head -c 32 /dev/urandom | basenc --base64url | tr -d '=')
printf '%s' "$KEY" > "$TASK/native-owner/key"
```

### 3. Import the pool (existing private installer; B3)

The Codex pool is imported only through the existing private initial-control installer. That installer is an
`InitialControlInstaller` RPC entrypoint with the `UOS_CANDIDATE_NATIVE_V1` and `UOS_CANDIDATE_INITIAL_SEAL_V1` secrets,
fed by the runbook step G pipeline: capture, then verify, then core-state manifest, then stage frames with native custody
registrations for every Codex account. The installer receives an access-only projection whose accounts carry
`native_owner.codex_home`. It never sees refresh tokens. Its operator path is the B3 work owned by the state-migration
stream; this branch does not change it. Use `UOS_CANDIDATE_NATIVE_V1`:

```json
{
  "v": 1,
  "role": "readonly_live",
  "origin": "https://native-owner.ai.ubq.fi",
  "key_id": "vps-owner-1",
  "owner_id": "vps-native",
  "owner_epoch": "2026-11-05",
  "key_base64url": "<KEY>"
}
```

### 4. Start the native owner on the VPS (authorization: new VPS service)

Build `uos-native` from the exact accepted source in the task-local toolchain. This compiles the native companion, not
the application.

```sh
SRC=/home/codex/rust-worker-prep/checkout-$SHA
env -i HOME="$HOME" bash -c '. /home/codex/rust-worker-prep/worker-build-env.sh; cd '"$SRC"'/rust && CARGO_INCREMENTAL=0 cargo build --locked --offline --release -p gateway-native --bin uos-native'
install -d -m 700 /home/codex/.uos-native /home/codex/.uos-native/replay
install -m 755 "$SRC/rust/target/release/uos-native" /home/codex/.uos-native/uos-native
```

Write the private configuration in `/home/codex/.uos-native/config.json` with mode 600. The daemon reads this JSON on
stdin, binds loopback only, and keeps its replay database in a dedicated directory. The `identity` must be byte-identical
to step 2. The owner id and home must match the pool's `native_owner` and the Worker registry in step 5.

```json
{
  "v": 2,
  "listen": "127.0.0.1:7990",
  "identity": {
    "key_id": "vps-owner-1",
    "owner_id": "vps-native",
    "owner_epoch": "2026-11-05",
    "key_base64url": "<KEY>"
  },
  "nonce_database": "/home/codex/.uos-native/replay/nonces.sqlite3",
  "sources": [],
  "owner_renewal": {
    "authority_origin": "https://rust-candidate.ai.ubq.fi/",
    "owners": [{ "owner_id": "vps-codex-home", "codex_home": "/home/codex/.codex" }]
  }
}
```

Use a systemd unit as follows. Restart is required: the owner preconnects one TLS socket to the Worker authority before
confinement and never reconnects.

```ini
[Unit]
Description=uos native Codex owner (Rust Worker renewal)
After=network-online.target
[Service]
User=codex
ExecStart=/bin/sh -c 'exec /home/codex/.uos-native/uos-native < /home/codex/.uos-native/config.json'
Restart=always
RestartSec=5
NoNewPrivileges=yes
[Install]
WantedBy=multi-user.target
```

```sh
sudo install -m 644 "$TASK/uos-native-owner.service" /etc/systemd/system/uos-native-owner.service  # the unit above
sudo systemctl daemon-reload && sudo systemctl enable --now uos-native-owner.service
journalctl -u uos-native-owner.service -n 5   # expect {"event":"native_listener_ready",...}
```

Expose it to the Worker as a root HTTPS origin on port 443. Add a Caddy site `native-owner.ai.ubq.fi` with
`reverse_proxy 127.0.0.1:7990` and a proxied Cloudflare DNS record. The native HTTP server parses strictly bounded
headers; confirm that it accepts the proxy's forwarded headers with one signed `/v1/native/rpc` probe before relying
on it.

### 5. Configure the Worker carrier (authorization: candidate secret change)

After the installer receipt reports installed or reconciled, delete the candidate binding. The candidate binding takes
precedence and its `readonly_live` role carries no renewal capability. Then set `UOS_NATIVE_CARRIER` with the same
identity:

```sh
w secret delete UOS_CANDIDATE_NATIVE_V1      --name uos-ai-rust-candidate
w secret delete UOS_CANDIDATE_INITIAL_SEAL_V1 --name uos-ai-rust-candidate
w secret put UOS_NATIVE_CARRIER --name uos-ai-rust-candidate < "$TASK/native-owner/carrier.json"
```

`carrier.json`:

```json
{
  "origin": "https://native-owner.ai.ubq.fi",
  "identity": {
    "key_id": "vps-owner-1",
    "owner_id": "vps-native",
    "owner_epoch": "2026-11-05",
    "key_base64url": "<KEY>",
    "capabilities": ["supervisor_read", "owner_renewal", "access_publish"]
  },
  "renewal_owners": [{ "owner_id": "vps-codex-home", "codex_home": "/home/codex/.codex" }]
}
```

Add `live_sideband` to the capabilities only when the owner runs configuration v3 with `live_sideband`.

### 6. Acceptance

The routing tier is enabled implicitly: once the pool is installed, the subscription is first in the waterfall. Prove it:

```sh
curl -sS "${H[@]}" "$CAND/v1/responses" -d '{"model":"gpt-reserve","input":"ping","reasoning":{"effort":"none"}}' -D - | grep -i '^x-uos-upstream'
```

The header must show the Codex upstream. To exercise renewal, wait until the projection's access token is near expiry,
or let a 401 occur, and check that the owner journal shows one `codex_renew_access` and that the request still succeeds
on the same account. Then confirm that the existing VPS clients still work (AGENTS.md "Local Credential Recovery
Safety").

## What remains

- **B3 import path** (the state-migration stream): the private `InitialControlInstaller` operator path and the seal
  producer that step 3 depends on.
- **Pool coverage**: on the Worker, only native-owned accounts in at most two registered homes can be renewed. Covering
  uploaded VPS accounts requires moving each lineage into a Codex home whose app-server owns refresh, so the TypeScript
  gateway delegates too. Raising the two-owner cap is an authority invariant change and needs its own decision.
- **Reconnect**: the owner's reverse authority and publication sockets are preconnected once. After Cloudflare closes
  the connection, renewal fails until a restart. The unit's `Restart=always` does not trigger on that failure because
  the process keeps serving. A reconnect design that stays inside confinement (or a supervised periodic restart) is
  open.
- **Not run here**: workerd end-to-end fixture of `/v1/responses` with a fake Codex upstream and a signed native owner.
  The routing-boundary tests above cover the dispatch and refresh semantics natively, but the real carrier round trip
  needs the built WASM, `workerd` and the TLS fixture CA from `tools/cloudflare/shipping-native-carrier-conformance.mjs`.
  It was not run in this session.
- **Proxy acceptance**: Caddy or Cloudflare in front of the loopback owner (step 4) is unverified against the native
  server's header bounds.
- **Linux kernel**: whether the VPS kernel provides Landlock ABI 9 is unconfirmed.
