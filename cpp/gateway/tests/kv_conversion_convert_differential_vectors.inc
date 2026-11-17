// Recorded from src/kv-conversion/convert.ts under Bun 1.4.0 (scratch scripts rec-convert.ts); paths are
// "<root>", the fake codec digest "<sha>", volatile file identities "<file>".
const char* const CONVERT_RECORDED = R"JSON({
 "script": "#!/bin/sh\nlast=\"\"\nfor arg in \"$@\"; do last=\"$arg\"; done\nif [ \"$last\" = \"--identity\" ]; then\n  printf '%s\\n' '2.9.7'\n  exit 0\nfi\nwhile IFS= read -r line; do :; done\nexit 0\n",
 "deno-to-bun copies counters and bytes": {
  "receipt": {
   "schema": "uos-kv-runtime-conversion-v1",
   "direction": "deno-to-bun",
   "source": {
    "path": "<root>/source.sqlite3",
    "codec": "deno-v8",
    "file": "<file>",
    "snapshot": "<file>",
    "row_count": 2,
    "content_sha256": "5aa93a3970e8cae941772d26b1268c9eefae95515f438d2cb8e2f5e31db1691a"
   },
   "destination": {
    "path": "<root>/destination.sqlite3",
    "codec": "bun-jsc-v1",
    "file": "<file>",
    "row_count": 2,
    "content_sha256": "5aa93a3970e8cae941772d26b1268c9eefae95515f438d2cb8e2f5e31db1691a"
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
     "version": "9",
     "seq": "0"
    },
    "user_version": "0",
    "application_id": "0"
   },
   "last_native_revision": {
    "version": "9",
    "seq": "0"
   },
   "absolute_expiry_sha256": "7d3850757f023485d6d9e2ddb74482d9a55c2340a338bad4a0df15a26d3e8030",
   "transcoded_cursors": 0,
   "deno": {
    "version": "2.9.7",
    "binary": "<root>/fake-deno",
    "sha256": "be4859cfb9a112a6e22a512a9dd699950d6b7c56c5cbe348f9c696cb1f9b6878"
   },
   "bun_version": "1.4.0",
   "completed_at": "<completed_at>"
  },
  "receiptFile": true,
  "rows": [
   [
    "02636f756e74657200",
    "2a00000000000000",
    "2",
    "3",
    "0",
    "-1"
   ],
   [
    "0272617700",
    "0800ff",
    "3",
    "5",
    "1",
    "1700000000000"
   ]
  ],
  "metadata": [
   {
    "key": "codec",
    "value": "bun-jsc-v1"
   },
   {
    "key": "created_bun_version",
    "value": "1.4.0"
   }
  ],
  "identityMatches": true,
  "sourceUnchanged": true,
  "entries": [
   "destination.sqlite3",
   "fake-deno",
   "receipt.json",
   "source.sqlite3"
  ]
 },
 "bun-to-deno copies counters and bytes": {
  "receipt": {
   "schema": "uos-kv-runtime-conversion-v1",
   "direction": "bun-to-deno",
   "source": {
    "path": "<root>/source.sqlite3",
    "codec": "bun-jsc-v1",
    "file": "<file>",
    "snapshot": "<file>",
    "row_count": 2,
    "content_sha256": "5aa93a3970e8cae941772d26b1268c9eefae95515f438d2cb8e2f5e31db1691a"
   },
   "destination": {
    "path": "<root>/destination.sqlite3",
    "codec": "deno-v8",
    "file": "<file>",
    "row_count": 2,
    "content_sha256": "5aa93a3970e8cae941772d26b1268c9eefae95515f438d2cb8e2f5e31db1691a"
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
     },
     {
      "type": "table",
      "name": "uos_kv_metadata",
      "sql": "CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID"
     }
    ],
    "migration_state": {
     "k": "0",
     "version": "3"
    },
    "data_version": {
     "k": "0",
     "version": "9",
     "seq": "0"
    },
    "user_version": "0",
    "application_id": "0"
   },
   "last_native_revision": {
    "version": "9",
    "seq": "0"
   },
   "absolute_expiry_sha256": "7d3850757f023485d6d9e2ddb74482d9a55c2340a338bad4a0df15a26d3e8030",
   "transcoded_cursors": 0,
   "deno": {
    "version": "2.9.7",
    "binary": "<root>/fake-deno",
    "sha256": "be4859cfb9a112a6e22a512a9dd699950d6b7c56c5cbe348f9c696cb1f9b6878"
   },
   "bun_version": "1.4.0",
   "completed_at": "<completed_at>"
  },
  "receiptFile": true,
  "rows": [
   [
    "02636f756e74657200",
    "2a00000000000000",
    "2",
    "3",
    "0",
    "-1"
   ],
   [
    "0272617700",
    "0800ff",
    "3",
    "5",
    "1",
    "1700000000000"
   ]
  ],
  "metadata": null,
  "identityMatches": true,
  "sourceUnchanged": true,
  "entries": [
   "destination.sqlite3",
   "fake-deno",
   "receipt.json",
   "source.sqlite3"
  ]
 },
 "empty database": {
  "receipt": {
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
    "path": "<root>/destination.sqlite3",
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
     "version": "0",
     "seq": "0"
    },
    "user_version": "0",
    "application_id": "0"
   },
   "last_native_revision": {
    "version": "0",
    "seq": "0"
   },
   "absolute_expiry_sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
   "transcoded_cursors": 0,
   "deno": {
    "version": "2.9.7",
    "binary": "<root>/fake-deno",
    "sha256": "be4859cfb9a112a6e22a512a9dd699950d6b7c56c5cbe348f9c696cb1f9b6878"
   },
   "bun_version": "1.4.0",
   "completed_at": "<completed_at>"
  },
  "receiptFile": true,
  "rows": [],
  "metadata": [
   {
    "key": "codec",
    "value": "bun-jsc-v1"
   },
   {
    "key": "created_bun_version",
    "value": "1.4.0"
   }
  ],
  "identityMatches": true,
  "sourceUnchanged": true,
  "entries": [
   "destination.sqlite3",
   "fake-deno",
   "receipt.json",
   "source.sqlite3"
  ]
 },
 "malformed counter": {
  "error": {
   "name": "Error",
   "message": "Malformed native unsigned counter"
  },
  "sourceUnchanged": true,
  "entries": [
   "fake-deno",
   "source.sqlite3"
  ]
 },
 "row newer than revision": {
  "error": {
   "name": "Error",
   "message": "Invalid native revision or absolute expiration"
  },
  "sourceUnchanged": true,
  "entries": [
   "fake-deno",
   "source.sqlite3"
  ]
 },
 "negative expiration": {
  "error": {
   "name": "Error",
   "message": "Invalid native revision or absolute expiration"
  },
  "sourceUnchanged": true,
  "entries": [
   "fake-deno",
   "source.sqlite3"
  ]
 },
 "unsupported encoding": {
  "error": {
   "name": "Error",
   "message": "Unsupported native value encoding"
  },
  "sourceUnchanged": true,
  "entries": [
   "fake-deno",
   "source.sqlite3"
  ]
 },
 "truncated key": {
  "error": {
   "name": "TypeError",
   "message": "Truncated KV bytes or string part"
  },
  "sourceUnchanged": true,
  "entries": [
   "fake-deno",
   "source.sqlite3"
  ]
 },
 "negative NaN key": {
  "error": {
   "name": "TypeError",
   "message": "Negative NaN KV key parts cannot retain their identity in Bun; the source database was not converted"
  },
  "sourceUnchanged": true,
  "entries": [
   "fake-deno",
   "source.sqlite3"
  ]
 },
 "wrong source codec": {
  "error": {
   "name": "Error",
   "message": "Unsupported native KV tables or codec marker"
  },
  "sourceUnchanged": true,
  "entries": [
   "fake-deno",
   "source.sqlite3"
  ]
 },
 "codec pin mismatch": {
  "error": {
   "name": "Error",
   "message": "Pinned legacy Deno binary does not match its SHA-256: expected 0000000000000000000000000000000000000000000000000000000000000000, observed <sha>"
  },
  "sourceUnchanged": true,
  "entries": [
   "fake-deno",
   "source.sqlite3"
  ]
 },
 "aborted before start": {
  "error": {
   "name": "Error",
   "message": "synthetic interruption"
  },
  "sourceUnchanged": true,
  "entries": [
   "fake-deno",
   "source.sqlite3"
  ]
 },
 "unknown direction": {
  "error": {
   "name": "Error",
   "message": "Unknown conversion direction"
  },
  "sourceUnchanged": true,
  "entries": [
   "fake-deno",
   "source.sqlite3"
  ]
 }
})JSON";
