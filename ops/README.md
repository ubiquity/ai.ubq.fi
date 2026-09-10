# VPS operations

Production runs as `ai-ubq-fi.service` on `codex@vps.pavlovcik.com` (129.158.58.222). All service files and data belong
to `/home/codex/repos/ubiquity/ai.ubq.fi`.

- `ops/ai-ubq-fi.service`: enabled systemd service; starts at boot and restarts after exit.
- `scripts/serve-vps.ts`: authenticated listener on `127.0.0.1:8001`, with graceful shutdown.
- `.env`: existing production credentials, mode 0600. `DENO_DEPLOY_TOKEN` remains the application's admin-token name.
- `.data/kv.sqlite3`: persistent local KV. Never replace it during a code deployment.
- `.data/releases/<full-git-sha>`: immutable code and assets; `.data/current` selects the release.
- `ops/Caddyfile`: HTTPS reverse proxy, imported by `/etc/caddy/Caddyfile`.
- `ops/caddy-ai-ubq-fi.conf`: narrow read-only mounts for Caddy, which otherwise cannot access home directories.
- `.data/caddy/`: origin TLS key and certificate; the key stays on the VPS. The certificate expires on 2041-09-06.

The systemd unit and Caddy drop-in are symlinks to their files in `ops/`. Caddy reads its configuration and TLS material
through the two read-only mounts. The existing Caddy sites remain in the system Caddyfile.

## Operate the service

From the VPS repository root:

```sh
sudo systemctl status ai-ubq-fi.service
sudo journalctl -u ai-ubq-fi.service -n 100 --no-pager
sudo systemctl restart ai-ubq-fi.service
curl --fail http://127.0.0.1:8001/health
```

Systemd loads the repository-root `.env` through Deno's dotenv parser. Keep `DENO_TIMELINE=production` in the service
unit so paid billing reconciliation, capacity sampling, and telemetry pruning run on the VPS. Do not add Deno Deploy
identity variables to `.env`; they cause the VPS launcher to reject startup.

## Deploy a change

Required GitHub CI checks must pass first. GitHub no longer deploys to Deno Deploy.

```sh
git fetch origin development
git merge --ff-only origin/development
deno task deploy:vps
```

The command requires a clean tracked checkout, takes an exclusive deployment lock, archives the exact commit, stamps its
Git identity, atomically selects the new release, restarts only the gateway, and checks the exact local health identity.
It refuses to overwrite an existing release. Use `systemctl restart` to restart the current release.

Verify authenticated inference and the public health route after deployment. A health response alone does not prove
provider inference. The startup message, health body, and response headers identify the same immutable Git revision.

To roll back code, atomically point `.data/current` to a previously accepted release and restart `ai-ubq-fi.service`.
Preserve `.env` and `.data/kv.sqlite3`.

## Data migration and recovery

The 2026-09-10 migration retained the complete Deno export privately in `.kv-migration/production-20260910.ndjson`. The
import preserved 366,442 records and verified a matching full checksum. Four transient records were omitted: two login
sessions, one rate window, and one backfill cursor. Existing API keys, passkey credentials, provider credentials, model
configuration, and usage history were retained. Users can sign in again with their existing passkeys.

The export format does not include Deno's native expiry metadata. Imported historical rows are retained locally; new
writes use the application's normal expiry rules. The private import receipt records the count and checksum.

Back up `.env`, `.data/kv.sqlite3`, and `.data/caddy/`. Use a consistent SQLite backup or stop only the gateway while
copying the database; a plain copy of a live SQLite file can omit its WAL. The original export is a migration snapshot,
not an ongoing backup of new VPS writes.

## DNS and TLS

Cloudflare proxies the explicit `ai.ubq.fi` A record to 129.158.58.222. A Worker route for `ai.ubq.fi/*` with no script
excludes this hostname from `*.ubq.fi/*`; both are required. Leave other hostnames and the wildcard Worker unchanged.
The origin uses a Cloudflare Origin CA certificate, so direct Mac tests must explicitly trust the Cloudflare Origin CA
root and use `curl --resolve ai.ubq.fi:443:129.158.58.222`. Public clients use the normal Cloudflare edge certificate.
