// Recorded from src/kv-conversion/cli.ts under Bun 1.4.0 (scratch scripts rec-cli.ts); paths are
// "<root>", the fake codec digest "<sha>", volatile file identities "<file>".
const char* const CLI_RECORDED = R"JSON({
 "script": "#!/bin/sh\nlast=\"\"\nfor arg in \"$@\"; do last=\"$arg\"; done\nif [ \"$last\" = \"--identity\" ]; then\n  printf '%s\\n' '2.9.7'\n  exit 0\nfi\nwhile IFS= read -r line; do :; done\nexit 0\n",
 "no flags": {
  "args": [],
  "stdout": "",
  "stderr": "--direction is required\n",
  "code": 1
 },
 "flag without value": {
  "args": [
   "--direction"
  ],
  "stdout": "",
  "stderr": "Expected unique --flag value pairs\n",
  "code": 1
 },
 "key without dashes": {
  "args": [
   "direction",
   "deno-to-bun"
  ],
  "stdout": "",
  "stderr": "Expected unique --flag value pairs\n",
  "code": 1
 },
 "value that is a flag": {
  "args": [
   "--direction",
   "--source"
  ],
  "stdout": "",
  "stderr": "Expected unique --flag value pairs\n",
  "code": 1
 },
 "repeated flag": {
  "args": [
   "--direction",
   "a",
   "--direction",
   "b"
  ],
  "stdout": "",
  "stderr": "Expected unique --flag value pairs\n",
  "code": 1
 },
 "empty value": {
  "args": [
   "--direction",
   ""
  ],
  "stdout": "",
  "stderr": "Expected unique --flag value pairs\n",
  "code": 1
 },
 "unknown direction": {
  "args": [
   "--direction",
   "sideways",
   "--source",
   "<root>/source.sqlite3",
   "--destination",
   "<root>/out.sqlite3",
   "--receipt",
   "<root>/out.json",
   "--deno",
   "<root>/fake-deno",
   "--deno-sha256",
   "<sha>"
  ],
  "stdout": "",
  "stderr": "Unknown conversion direction\n",
  "code": 1
 },
 "missing pin": {
  "args": [
   "--direction",
   "deno-to-bun",
   "--source",
   "<root>/source.sqlite3",
   "--destination",
   "<root>/out.sqlite3",
   "--receipt",
   "<root>/out.json",
   "--deno",
   "<root>/fake-deno"
  ],
  "stdout": "",
  "stderr": "--deno-sha256 is required\n",
  "code": 1
 },
 "missing source": {
  "args": [
   "--direction",
   "deno-to-bun",
   "--source",
   "<root>/missing.sqlite3",
   "--destination",
   "<root>/x.sqlite3",
   "--receipt",
   "<root>/x.json",
   "--deno",
   "<root>/fake-deno",
   "--deno-sha256",
   "<sha>"
  ],
  "stdout": "",
  "stderr": "ENOENT: no such file or directory, lstat '<root>/missing.sqlite3'\n",
  "code": 1
 },
 "same source and destination": {
  "args": [
   "--direction",
   "deno-to-bun",
   "--source",
   "<root>/source.sqlite3",
   "--destination",
   "<root>/source.sqlite3",
   "--receipt",
   "<root>/y.json",
   "--deno",
   "<root>/fake-deno",
   "--deno-sha256",
   "<sha>"
  ],
  "stdout": "",
  "stderr": "Conversion destination and receipt must be NEW paths\n",
  "code": 1
 },
 "repeated pin flag": {
  "args": [
   "--direction",
   "deno-to-bun",
   "--source",
   "<root>/source.sqlite3",
   "--destination",
   "<root>/out.sqlite3",
   "--receipt",
   "<root>/out.json",
   "--deno",
   "<root>/fake-deno",
   "--deno-sha256",
   "<sha>",
   "--deno-sha256",
   "0000000000000000000000000000000000000000000000000000000000000000"
  ],
  "stdout": "",
  "stderr": "Expected unique --flag value pairs\n",
  "code": 1
 },
 "unused extra flag": {
  "args": [
   "--direction",
   "deno-to-bun",
   "--source",
   "<root>/source.sqlite3",
   "--destination",
   "<root>/out.sqlite3",
   "--receipt",
   "<root>/out.json",
   "--deno",
   "<root>/fake-deno",
   "--deno-sha256",
   "0000000000000000000000000000000000000000000000000000000000000000",
   "--extra",
   "1"
  ],
  "stdout": "",
  "stderr": "Pinned legacy Deno binary does not match its SHA-256: expected 0000000000000000000000000000000000000000000000000000000000000000, observed <sha>\n",
  "code": 1
 },
 "converts an empty database": {
  "args": [
   "--direction",
   "deno-to-bun",
   "--source",
   "<root>/source.sqlite3",
   "--destination",
   "<root>/out.sqlite3",
   "--receipt",
   "<root>/out.json",
   "--deno",
   "<root>/fake-deno",
   "--deno-sha256",
   "<sha>"
  ],
  "stderr": "",
  "code": 0,
  "stdout_normalized": {
   "schema": "uos-kv-runtime-conversion-v1",
   "direction": "deno-to-bun",
   "source": {
    "path": "<root>/source.sqlite3",
    "codec": "deno-v8",
    "file": "<file>",
    "snapshot": "<file>",
    "row_count": 0,
    "content_sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
   },
   "destination": {
    "path": "<root>/out.sqlite3",
    "codec": "bun-jsc-v1",
    "file": "<file>",
    "row_count": 0,
    "content_sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
   },
   "native_schema_version": 3,
   "schema_metadata": {
    "source": [
     {
      "type": "table",
      "name": "data_version",
      "sql": "CREATE TABLE data_version (k INTEGER PRIMARY KEY, version INTEGER NOT NULL, seq INTEGER NOT NULL DEFAULT 0)"
     },
     {
      "type": "table",
      "name": "kv",
      "sql": "CREATE TABLE kv (k BLOB PRIMARY KEY, v BLOB NOT NULL, v_encoding INTEGER NOT NULL, version INTEGER NOT NULL, seq INTEGER NOT NULL DEFAULT 0, expiration_ms INTEGER NOT NULL DEFAULT -1) WITHOUT ROWID"
     },
     {
      "type": "index",
      "name": "kv_expiration_ms_idx",
      "sql": "CREATE INDEX kv_expiration_ms_idx ON kv (expiration_ms)"
     },
     {
      "type": "table",
      "name": "migration_state",
      "sql": "CREATE TABLE migration_state (k INTEGER NOT NULL PRIMARY KEY, version INTEGER NOT NULL)"
     },
     {
      "type": "table",
      "name": "queue",
      "sql": "CREATE TABLE queue (ts INTEGER NOT NULL, id TEXT NOT NULL, data BLOB NOT NULL, backoff_schedule TEXT NOT NULL, keys_if_undelivered BLOB NOT NULL, PRIMARY KEY (ts, id))"
     },
     {
      "type": "table",
      "name": "queue_running",
      "sql": "CREATE TABLE queue_running (deadline INTEGER NOT NULL, id TEXT NOT NULL, data BLOB NOT NULL, backoff_schedule TEXT NOT NULL, keys_if_undelivered BLOB NOT NULL, PRIMARY KEY (deadline, id))"
     },
     {
      "type": "index",
      "name": "sqlite_autoindex_queue_1",
      "sql": null
     },
     {
      "type": "index",
      "name": "sqlite_autoindex_queue_running_1",
      "sql": null
     }
    ],
    "migration_state": {
     "k": "0",
     "version": "3"
    },
    "data_version": {
     "k": "0",
     "version": "4",
     "seq": "0"
    },
    "user_version": "0",
    "application_id": "0"
   },
   "last_native_revision": {
    "version": "4",
    "seq": "0"
   },
   "absolute_expiry_sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
   "transcoded_cursors": 0,
   "deno": {
    "version": "2.9.7",
    "binary": "<root>/fake-deno",
    "sha256": "<sha>"
   },
   "bun_version": "1.4.0",
   "completed_at": "<completed_at>"
  }
 }
})JSON";
