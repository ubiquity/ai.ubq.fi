# Codex supervisor panel

The supervisor panel is a read-only, super-admin-only view of Codex sessions on the machines an operator has configured.
It answers one question: which Codex sessions exist, which are running right now, and what is each one waiting on.

It never steers a session. The panel connects to each machine's existing Codex app-server control socket, sends only the
read methods listed below, and always closes its own websocket. It does not resume, start, load, unsubscribe, approve,
interrupt, archive, or otherwise mutate a thread, and it never launches SSH, manages daemons, or reads history databases
or rollout files.

Reach it at `/admin#supervisor`. The tab is hidden for anyone who is not a super admin, and both backend routes
(`GET /admin/codex/supervisor/sessions` and `GET /admin/codex/supervisor/output`) enforce `requireSuperAdminAuth`
independently of the UI.

## Configuration

The panel reads one optional, non-secret file relative to the runtime working directory: `.data/codex-supervisor.json`.

```json
{
  "sources": [
    {
      "id": "local",
      "name": "This Mac",
      "socketPath": "/Users/example/.codex/app-server-control/app-server-control.sock",
      "codexHome": "/Users/example/.codex"
    },
    { "id": "vps", "name": "VPS", "socketPath": "/tmp/uos-supervisor/vps.sock" }
  ]
}
```

Rules the loader enforces:

- `id` must match `/^[a-z0-9][a-z0-9_-]{0,31}$/` and be unique. `name` is 1-64 characters. `socketPath` must be
  absolute.
- `codexHome` is optional and must be absolute when present. It is the only way to read that machine's persisted
  `branch` and accumulated token usage; it never gates live fields. A source without it is treated as remote, and its
  token usage stays unavailable rather than guessed.
- At most 8 sources are used. Invalid entries are skipped and reported in the panel notice line.
- Without the file, the panel falls back to a single local source derived from `CODEX_HOME`, else `$HOME/.codex`, with
  the socket at `<codexHome>/app-server-control/app-server-control.sock`.

HTTP input can select a configured source id but can never add or change a path. There is no new environment variable,
secret, CLI argument, or flag. The transport's `ws` dependency stays a pinned bare import-map alias
(`"ws": "npm:ws@8.18.3"`) so header parsing cannot drift; it stays in Knip's `ignoreDependencies` list with the other
packages Deno owns, because the Deno manifest and lockfile are the dependency authority here.

A remote source requires an operator-provided Unix-socket forward (for example an SSH `-L` style tunnel) to that
machine's app-server control socket. Creating and supervising that forward is deliberately outside the product: the
panel only dials the socket path that configuration names.

## Required runtime permissions

Codex control sockets are Unix domain sockets, and Deno requires **both read and write access to the socket path** to
connect to one, even for a connection that only sends read methods. The state database needs read access to the Codex
home. A serving process that lacks these grants starts normally and reports every source as unavailable with a reason,
so the failure is visible rather than silent.

A local preview therefore needs, in addition to the existing serve permissions:

```
--allow-read=/Users/example/.codex --allow-write=/Users/example/.codex/app-server-control,/tmp/uos-supervisor
--allow-sys=hostname   # optional: without it the machine name reports as unavailable
```

Deployment service files (`ops/`) do not currently grant those paths; granting them is a separate, explicit operations
change.

## Read methods

One JSON-RPC allowlist is enforced in code, so a caller cannot reach anything else:

- `initialize` and the `initialized` notification
- `thread/list` (paginated, `useStateDbOnly: true`, `sourceKinds` set to every value including all `subAgent*` kinds so
  subagent sessions are never hidden)
- `thread/loaded/list`
- `thread/read` (summary only, `includeTurns: false`)
- `thread/turns/list` (newest turn, bounded item views)
- `account/rateLimits/read`

Payloads are capped at 1 MiB, the handshake is capped at 3 seconds, and every call has its own 4-second deadline. A
sampling round has a 9-second overall deadline and probes at most 24 threads per source with a concurrency of 4, so a
large store cannot stall an admin request; the panel reports the listed, sampled, and returned counts plus a truncation
flag.

## What the panel reports

- **State** comes from the runtime status in `thread/read`, never from recency. `active` means the runtime says active.
  `systemError` is its own value. `idle` requires an idle runtime status or a `notLoaded` thread whose newest turn
  reached a terminal state. A newest turn that is still in progress while the thread is not loaded is `stale`
  (persisted, not live). Anything unreadable is `unknown` — the panel never fabricates `idle`, and `updatedAt` is
  display-only.
- **Waiting** flags come from `activeFlags` and are shown verbatim, with `approval` and `input` recognized as distinct
  waits.
- **Title, model, effort, cwd, and lineage** come from each machine's own `thread/list` and `thread/read` results, for
  local and remote sources alike. Live values win; the local state database never overrides them.
- **Branch and persisted tokens** additionally come from the read-only Codex state database `threads` table (highest
  `state_<n>.sqlite` in the configured `codexHome`), opened with `readOnly: true` and queried only for the sampled
  thread ids. They fill only what the live thread omitted. Without a configured `codexHome` (a remote source) token
  usage stays `unavailable` instead of `0` or blank, and branch is only shown when it is genuinely available. Tokens are
  cumulative persisted usage and are labelled as such.
- **Quota** comes from `account/rateLimits/read` per source and is rendered as the one shared account-wide window:
  machine, used percent, window length, and reset time. Every machine signed in to the account reports the same reading,
  so the panel labels it as shared and never adds the machines together or attributes it to a session.
- **Coverage and staleness** are explicit: last sampled time per source, source-level availability with a reason, and
  persisted-only rows when a local app-server is down (marked as not live).

If one source is offline, the others still render. Notes at the top of the list name each unavailable source and why.

## Follow recorded output

Selecting a session and choosing **Follow output** opens a server-sent event stream that polls the newest persisted turn
about every 2 seconds and appends newly recorded assistant messages and command output. It is not a token-level stream,
and the panel says so in the status line: it updates as Codex records output.

Only `agentMessage` text and `commandExecution` command/output are rendered. Reasoning, system, developer, user, and
tool-internal items are never converted into panel entries. The stream is bounded (120 entries per update, 4,000
characters per field, 10 minutes per connection), deduplicates by turn and item id, and stops when the selection
changes, the panel is deselected, the tab is hidden, or the operator signs out. The endpoint validates the source id and
thread id against the trusted sampled inventory, so it cannot be used to probe arbitrary thread ids, and it refuses to
start when the source is unavailable.

## Catch me up brief

Each session row has a **Catch me up** button. Clicking it makes one bounded, on-demand summarizer call and shows a
stable brief with two sections: **About this session** and **Current status**. The normal inventory poll never calls a
model; the summarizer runs only for an explicit click, never repeatedly, and never writes to, resumes, or steers a
session.

The data flow is:

1. The panel POSTs `{ "source": "<configured source id>", "id": "<thread id>" }` to `POST /admin/codex/supervisor/brief`
   (super-admin only, `no-store`, one request per click).
2. The route validates the source id and thread id against the trusted configured inventory, then opens the same
   read-only app-server connection the panel uses and calls only `thread/read` and `thread/turns/list`: one page for the
   first recorded turn, one page of the most recent turns, and at most two `itemsView: "full"` enrichments when a
   summary turn carries no text.
3. Collected items are limited to user messages, assistant messages, command executions, and tool/status markers.
   System, developer, and reasoning items are omitted; the transcript is capped at 32 KiB, 6 recent turns, 12 items per
   turn, and 1,200 characters per item, and credential-shaped text is replaced with `[redacted]` before anything is
   sent.
4. The prompt carries the session metadata and transcript inside `<session_metadata>`/`<session_log>` delimiters and
   instructs the model to treat that content as untrusted data. The summarizer calls the existing DeepSeek Chat
   Completions client with the `deepseek-flash` model, `reasoning_effort: "max"`, JSON response mode, no tools, and a
   45-second deadline, and validates the returned `{ "about": string, "status": string }` object before rendering it.
5. A partial or missing transcript is stated plainly in the brief instead of being guessed at; when no recorded turn was
   available at all, the route answers from the live thread metadata without calling the model. The brief line shows the
   inventory snapshot timestamp, the generation timestamp, truncation, and redaction counts.
6. When the projected turn history lags a still-active local session, the route additionally reads at most the last 256
   KiB of that selected session's own rollout file, after validating the path belongs to the configured `codexHome`
   `sessions`/`archived_sessions` tree and to that thread id. A partial leading JSONL line is discarded; only visible
   user/assistant messages and safe tool progress are normalized (reasoning, system, and developer payloads are never
   collected), each text is redacted before it is bounded, and the fresh events fill the same 32 KiB model context with
   budget priority over older records. This is read-only: no Codex file, database, or session is written. Remote sources
   have no local rollout to read, so their brief says the recorded history may lag the live session instead.

## Known limitations

- Follow output reads persisted turn history, so a turn that has not yet been written to that store shows "waiting for
  Codex to record new output" rather than live tokens.
- Remote sources read the same live thread fields as local ones through the tunnel, but their token usage is not
  reported by the remote app-server, so it stays unavailable. Branch is shown only where a local state database provides
  it. The brief reads the same recorded turn history through the same tunnel, so a remote session with no recorded turns
  produces the "no recorded transcript was available" brief.
- The state database is a cache that Codex maintains; a value can lag the live session. It is labelled as persisted data
  wherever it is shown.
- The panel samples the most recently updated threads per source (60 listed, 24 live-probed) rather than an entire
  history; the coverage line reports when that bound was reached. The list refreshes every 15 seconds while the tab is
  visible and keeps existing rows in place, so an activity reorder does not move a card the operator is reading.
