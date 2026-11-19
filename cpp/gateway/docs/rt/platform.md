# uos::rt platform: crypto, env, process, fs, path, console

Headers: `uos/rt/crypto.hpp`, `env.hpp`, `process.hpp`, `fs.hpp` (also node:path),
`console.hpp`. Oracle: Bun 1.4.0 on Linux arm64. Every expected value in
`tests/rt/{crypto,env,fs,console,process}_test.cpp` was recorded from bun.

## Conventions

- **Synchronous cores.** `crypto.subtle.*` and `node:fs/promises` calls are
  synchronous C++ functions with the JS results and errors. `await
crypto.subtle.digest(...)` becomes `subtle::digest(...)`; an exception is the
  rejection, thrown where the TS awaits. Where the TS awaits and the ordering
  against other tasks is observable, keep the suspension point with
  `co_await rt::microtask();` right after the call. When the TS starts the call
  and awaits it later (`const p = subtle.digest(...); ...; await p`), capture the
  result or the exception at the call and rethrow it at the await.
- **Errors** are `rt::JsError` subclasses with Bun's `name` and `message`:
  Web Crypto uses `rt::DOMException` (`OperationError`, `DataError`,
  `NotSupportedError`, `InvalidAccessError`) and `rt::TypeError`/`rt::SyntaxError`;
  failed system calls throw `rt::fs::ErrnoError` (`code`, `errno_`, `syscall`,
  `path`, `dest`, `fd`). Node's `code` on argument errors (`ERR_INVALID_ARG_VALUE`,
  `ERR_CRYPTO_HASH_FINALIZED`, ...) is not carried: the TS never reads it (except
  `utimesSync`'s TypeError, which sets `rt::JsError::code` to `ERR_INVALID_ARG_TYPE`).
  `catch (e) { if (e.code !== "ENOENT") throw e }` is
  `catch (const rt::fs::ErrnoError& e) { if (e.code != "ENOENT") throw; }`.
- Names that are libc macros get a trailing `_`: `ErrnoError::errno_`,
  `fs::constants::O_RDONLY_`, `O_NOFOLLOW_`, ... (`COPYFILE_EXCL` keeps its name).
- Byte data is `std::vector<std::uint8_t>` / `std::span<const std::uint8_t>`;
  strings are UTF-8 (WTF-8 for lone surrogates).

## TypeScript construct -> C++

| TypeScript                                                                                                    | C++                                                                                                                                                                                                                                                                                                                                                                             |
| ------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `crypto.randomUUID()`                                                                                         | `rt::crypto::randomUUID()`                                                                                                                                                                                                                                                                                                                                                      |
| `crypto.getRandomValues(new Uint8Array(n))`                                                                   | `rt::crypto::getRandomValues(std::span<std::uint8_t>)` (no 65536-byte quota in Bun)                                                                                                                                                                                                                                                                                             |
| `crypto.getRandomValues(new Uint32Array(n))`                                                                  | `rt::crypto::getRandomValues(std::span<std::uint32_t>)` (little-endian words)                                                                                                                                                                                                                                                                                                   |
| `await crypto.subtle.digest("SHA-256", data)`                                                                 | `subtle::digest("SHA-256", data)` (SHA-1/224/256/384/512, name case-insensitive)                                                                                                                                                                                                                                                                                                |
| `importKey(fmt, data, { name: "AES-GCM" }, ext, usages)`                                                      | `subtle::importKey(fmt, data, Algorithm{.name = "AES-GCM"}, ext, {"encrypt", "decrypt"})`                                                                                                                                                                                                                                                                                       |
| `importKey("raw", bytes, "HKDF", false, ["deriveBits"])`                                                      | `subtle::importKey("raw", bytes, "HKDF", false, {"deriveBits"})`                                                                                                                                                                                                                                                                                                                |
| `importKey("jwk", { kty, n, e, alg, ext }, alg, ext, usages)`                                                 | `subtle::importKey("jwk", json::object{...}, Algorithm{...}, ext, usages)`                                                                                                                                                                                                                                                                                                      |
| `importKey("spki"/"pkcs8", der, { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" }, ...)`                         | same arguments; `Algorithm{.name = ..., .hash = "SHA-256"}`                                                                                                                                                                                                                                                                                                                     |
| `subtle.encrypt/decrypt({ name: "AES-GCM", iv, additionalData, tagLength }, key, data)`                       | `subtle::encrypt/decrypt(Algorithm{.name = "AES-GCM", .iv = iv, .additionalData = aad}, key, data)`; output is ciphertext followed by the tag, as Web Crypto                                                                                                                                                                                                                    |
| `subtle.sign/verify("HMAC" or "RSASSA-PKCS1-v1_5", key, ...)`                                                 | `subtle::sign(name, key, data)`, `subtle::verify(name, key, signature, data)`                                                                                                                                                                                                                                                                                                   |
| `subtle.deriveBits({ name: "HKDF", hash, salt, info }, key, 256)`                                             | `subtle::deriveBits(Algorithm{.name = "HKDF", .hash = ..., .salt = ..., .info = ...}, key, 256)` (PBKDF2: `.iterations`)                                                                                                                                                                                                                                                        |
| `subtle.deriveKey(...)`, `exportKey(fmt, key)`, `exportKey("jwk", key)`, `generateKey(rsa...)`                | `subtle::deriveKey`, `subtle::exportKey`, `subtle::exportJwk`, `subtle::generateKey`                                                                                                                                                                                                                                                                                            |
| `key.type/extractable/algorithm/usages`                                                                       | `key.type()`, `key.extractable()`, `key.algorithm()` (`KeyAlgorithm`), `key.usages()`                                                                                                                                                                                                                                                                                           |
| `createHash("sha256").update(x).digest("hex")`                                                                | `rt::crypto::createHash("sha256").update(x).digest("hex")` (`digest()` gives bytes)                                                                                                                                                                                                                                                                                             |
| `createHmac(alg, key)`, `timingSafeEqual(a, b)`, `randomBytes(n)`                                             | `rt::crypto::createHmac`, `timingSafeEqual`, `randomBytes`                                                                                                                                                                                                                                                                                                                      |
| `process.env[k]` / `Bun.env[k]` / `process.env.K`                                                             | `rt::env::get(k)` (`std::nullopt` is undefined)                                                                                                                                                                                                                                                                                                                                 |
| `process.env[k] = v` / `delete process.env[k]` / `k in process.env`                                           | `rt::env::set(k, v)` / `rt::env::remove(k)` / `rt::env::has(k)`                                                                                                                                                                                                                                                                                                                 |
| `Object.keys/entries(process.env)`, `{ ...process.env }`                                                      | `rt::env::keys()`, `rt::env::entries()` (index names first, then insertion order)                                                                                                                                                                                                                                                                                               |
| `process.loadEnvFile(path)` (node:process `loadEnvFile`)                                                      | `rt::env::loadEnvFile(path)` (default `".env"`)                                                                                                                                                                                                                                                                                                                                 |
| `process.platform`, `process.arch`                                                                            | `rt::process::platform` (`"linux"`), `rt::process::arch` (`"arm64"`)                                                                                                                                                                                                                                                                                                            |
| `process.argv`, `Bun.argv`, `.slice(2)`                                                                       | `rt::process::argv()` after `rt::process::setArgv(argc, argv)` in main                                                                                                                                                                                                                                                                                                          |
| `process.cwd()`, `process.execPath`, `process.pid`, `process.getuid?.()`                                      | `rt::process::cwd()`, `execPath()`, `pid()`, `getuid()`                                                                                                                                                                                                                                                                                                                         |
| `process.exitCode = n`, `process.exitCode`                                                                    | `rt::process::setExitCode(n)`, `rt::process::exitCode()`                                                                                                                                                                                                                                                                                                                        |
| `process.exit(code)`                                                                                          | `rt::process::exit(code)` (status modulo 256; no further JS-equivalent work runs)                                                                                                                                                                                                                                                                                               |
| event loop drained (natural exit)                                                                             | `return rt::process::exitStatus();` from main (1 after an uncaught error)                                                                                                                                                                                                                                                                                                       |
| `process.once/on("SIGINT", f)`, `process.off/removeListener("SIGINT", f)`                                     | `rt::process::once/on("SIGINT", listener)`, `off/removeListener`; keep one `rt::process::Listener` object per JS function                                                                                                                                                                                                                                                       |
| `Bun.version`                                                                                                 | `rt::bun::version` (`"1.4.0"`, written to KV metadata)                                                                                                                                                                                                                                                                                                                          |
| `Bun.sleep(ms)`                                                                                               | `co_await rt::sleep(ms)` (uos/rt/clock.hpp)                                                                                                                                                                                                                                                                                                                                     |
| `Bun.spawn(cmd, { cwd, env, stdin, stdout, stderr })`                                                         | `rt::process::spawn(cmd, {.cwd = ..., .env = ..., .stdin = Stdio::pipe, ...})`                                                                                                                                                                                                                                                                                                  |
| `child.exited`, `child.exitCode`, `child.signalCode`, `child.killed`, `child.kill(sig)`, `child.pid`          | `co_await child.exited().wait()`, `exitCode()`, `signalCode()`, `killed()`, `kill(sig)`, `pid()`                                                                                                                                                                                                                                                                                |
| `child.stdin.write(s)`, `await child.stdin.flush()`, `await child.stdin.end()`                                | `child.stdin().write(s)` (returns bytes buffered), `co_await child.stdin().flush().wait()`, `...end().wait()`                                                                                                                                                                                                                                                                   |
| `child.stdout` / `child.stderr` (ReadableStream)                                                              | `child.stdout()` / `child.stderr()` -> `rt::ReadableStream<std::vector<std::uint8_t>>`                                                                                                                                                                                                                                                                                          |
| `os.hostname()`, `os.release()`, `os.tmpdir()`                                                                | `rt::os::hostname()`, `rt::os::release()`, `rt::os::tmpdir()`                                                                                                                                                                                                                                                                                                                   |
| `readFileSync(p)`, `readFileSync(p, "utf8")`, `readFileSync(fd, ...)`                                         | `rt::fs::readFileSync(p)` (bytes), `readFileSync(p, "utf8")` (WHATWG decode, BOM kept)                                                                                                                                                                                                                                                                                          |
| `writeFileSync(p, data, { flag: "wx", mode: 0o600 })`                                                         | `rt::fs::writeFileSync(p, data, {.flag = "wx", .mode = 0600})`                                                                                                                                                                                                                                                                                                                  |
| `openSync(p, "wx", 0o600)`, `openSync(p, constants.O_RDONLY \| constants.O_NOFOLLOW)`                         | `rt::fs::openSync(p, "wx", 0600)`, `openSync(p, constants::O_RDONLY_ \| constants::O_NOFOLLOW_)`                                                                                                                                                                                                                                                                                |
| `closeSync`, `fstatSync`, `fsyncSync`, `statSync`, `lstatSync(p, { bigint })`                                 | same names; `Stats` holds exact integers plus `...Ms` doubles and `...Ns` integers                                                                                                                                                                                                                                                                                              |
| `realpathSync`, `existsSync`, `chmodSync`, `linkSync`, `mkdtempSync`                                          | same names                                                                                                                                                                                                                                                                                                                                                                      |
| `symlinkSync(target, path)`, `renameSync(old, new)`                                                           | same names (errors "symlink '<target>' -> '<path>'", "rename '<old>' -> '<new>'"; an empty destination is left out, as Bun does; no `type` argument)                                                                                                                                                                                                                            |
| `utimesSync(p, atime, mtime)`                                                                                 | `rt::fs::utimesSync(p, atime, mtime)`: a number is seconds (nanoseconds truncated, seconds saturated to int64, a negative number is now), a `rt::Date` its time value; TypeError "atime must be a number or a Date" for NaN/infinite/Invalid Date before the path is touched; ErrnoError `utime`; follows symlinks. The promise forms are not provided (src/ does not use them) |
| `mkdirSync(p, { recursive, mode })`                                                                           | `rt::fs::mkdirSync(p, {.recursive = true, .mode = 0700})` (returns the first directory created)                                                                                                                                                                                                                                                                                 |
| `rmSync(p, { recursive, force })`, `copyFileSync(a, b, constants.COPYFILE_EXCL)`                              | `rt::fs::rmSync(p, {...})`, `rt::fs::copyFileSync(a, b, constants::COPYFILE_EXCL)`                                                                                                                                                                                                                                                                                              |
| `await readdir(p, { withFileTypes: true })`                                                                   | `rt::fs::promises::readdir(p, rt::fs::withFileTypes)` -> `Dirent` (`name`, `parentPath`, `isFile()`, ...)                                                                                                                                                                                                                                                                       |
| `await readFile(new URL("../x", import.meta.url), "utf8")`                                                    | `rt::fs::promises::readFile(rt::fileURLToPath(url), "utf8")` (uos/rt/url.hpp)                                                                                                                                                                                                                                                                                                   |
| `await readFile/realpath/stat/mkdir/mkdtemp/rm/writeFile(...)`                                                | `rt::fs::promises::readFile/realpath/stat/mkdir/mkdtemp/rm/writeFile(...)`                                                                                                                                                                                                                                                                                                      |
| `const h = await open(p, "r"); await h.stat(); await h.read(buf, off, len, pos); await h.close()`             | `auto h = rt::fs::promises::open(p, "r"); h.stat(); h.read(buf, off, len, pos).bytesRead; h.close();`                                                                                                                                                                                                                                                                           |
| `for await (const chunk of Bun.file(p).stream())`                                                             | `rt::bun::file(p).stream()` (uos/rt/process.hpp; 256 KiB chunks, read with a reader)                                                                                                                                                                                                                                                                                            |
| `path.join/resolve(...parts)`, `dirname`, `basename`, `extname`, `relative`, `isAbsolute`, `normalize`, `sep` | `rt::path::join(a, b, ...)`, `rt::path::resolve(...)`, ... (Node's posix algorithms)                                                                                                                                                                                                                                                                                            |
| `fileURLToPath(url)`, `pathToFileURL(p)`                                                                      | `rt::fileURLToPath`, `rt::pathToFileURL` (uos/rt/url.hpp, web component)                                                                                                                                                                                                                                                                                                        |
| `console.log/info/debug(...)` (stdout), `console.warn/error(...)` (stderr)                                    | `rt::console::log/info/debug/warn/error(...)`                                                                                                                                                                                                                                                                                                                                   |
| `console.error("x", { error })` (object holding an Error)                                                     | `rt::console::error("x", Value::object({{"error", Value(error)}}))`                                                                                                                                                                                                                                                                                                             |
| custom Error class with own fields (`code`, `status`)                                                         | `Value::error(error, {{"status", Value(503)}, {"code", Value("...")}})`                                                                                                                                                                                                                                                                                                         |

## Test hooks

| TS test pattern                                                                                       | C++                                                                                                                                      |
| ----------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| stub randomness (TS injects `randomBytes`/`randomUUID` through module dependencies)                   | `rt::crypto::ScopedRandomSource source(fn)` replaces every random byte (randomUUID, getRandomValues, randomBytes) on this thread         |
| `const prev = process.env.X; process.env.X = v; try {} finally { restore }`                           | `rt::env::ScopedOverride scope({{"X", "v"}, {"Y", std::nullopt}})` (nullopt deletes; restores or deletes on exit)                        |
| a clean environment                                                                                   | `rt::env::ScopedEnvironment scope({{"PATH", "/usr/bin:/bin"}})`                                                                          |
| `console.info = (...args) => logs.push(args)`, `spyOn(console, "error").mockImplementation(() => {})` | `rt::console::Capture capture({Method::info})`; `capture.calls()[i].args[0] == "label"`, `.args[1].string()`, `.text` (the printed line) |
| a replacement that also calls the original                                                            | `rt::console::Capture capture({Method::info}, true)` (passthrough)                                                                       |
| `process.argv` for a CLI entry                                                                        | `rt::process::ScopedArgv scope({"/bun", "/script", "--flag", "v"})`                                                                      |

## Behaviour notes (pinned by tests)

- **Web Crypto**: HMAC JWK import checks in Bun's order: missing `kty` -> "Invalid keyData",
  `kty` != "oct" -> `Invalid JWK "kty" Parameter`, missing `k` -> "Invalid keyData", `use`,
  `key_ops` ("Key operations and usage mismatch"), `ext`, `alg`; a `k` that is not
  base64url imports as empty -> "Zero-length key is not supported". AES JWK errors are the
  generic DataError. RSA JWK: a private key needs all of p, q, dp, dq, qi; empty n/e/d ->
  "Invalid keyData"; one leading zero octet of `n` is dropped; `modulusLength` is the
  modulus byte length times 8. SPKI/PKCS#8 with trailing bytes import (Bun parses the
  leading structure). JWK members convert like WebIDL: `String(value)`, `Boolean(ext)`,
  `key_ops` must be an array of usage names (`TypeError: Value is not a sequence` /
  `value must be enumeration (string)`). AES-GCM output = ciphertext || tag (tagLength
  32..128 bits); any non-empty IV.
- **node:crypto**: `createHash` takes OpenSSL names case-insensitively; a second
  `digest()` throws "Digest already called" (Hmac returns empty); `timingSafeEqual`
  length mismatch: `RangeError: Input buffers must have the same byte length`.
- **process.env**: a copy of the OS environment at first use; writes never reach
  libc (a child spawned without `env` sees the original variables). Integer-like names
  ("0".."4294967294") enumerate first.
- **loadEnvFile**: Bun's parser (`KEY=v`, `KEY: v`, `export KEY=v`, keys `[A-Za-z0-9_.-]`,
  quotes `"` `'` `` ` `` may span lines, only `\n`/`\r` are escapes in double quotes, the
  rest of a line after a closing quote is ignored, unterminated quotes keep the raw line,
  no `$` expansion, BOM skipped). Existing variables win; later duplicates in the file win.
- **fs**: error message `"<CODE>: <description>, <syscall>[ '<path>'[ -> '<dest>']]"`
  with libuv descriptions; an empty path names no path. Quirks kept: `realpathSync`
  failures say `lstat`; `rmSync` under a non-directory is EFAULT `rm` (even with
  `force`); `rmSync` of a directory without `recursive` is SystemError `ERR_FS_EISDIR`;
  `copyFileSync` from a directory is ENOTSUP; a closed FileHandle throws SystemError
  "Bad file descriptor". Modes apply only at creation, under the umask; `copyFileSync`
  copies the permission bits. readdir is in directory order.
- **process**: exit statuses are modulo 256; signal listeners are deduplicated, called
  with (name, number), `once` listeners removed before they run, and removing the last
  listener restores the default action. Spawn: PATH lookup uses `env.PATH`
  (env given), else the OS `PATH`, else `/usr/bin:/bin`; `exited` is 128 + signal for a
  signalled child; `killed` is true once the exit has been observed.
- **console**: see the header of `console.hpp` for the formatting rules.

## Limits (cannot or need not match Bun)

- Errors printed by console have no source excerpt or stack frames (Bun prints both for
  Errors with frames, and dumps DOMException as an object with `line`, `column`,
  `sourceURL`, `stack` and its constants); the port prints the frame-less header form.
  Functions, symbols, Maps, class instances are not console values.
- Web Crypto algorithms not used by the gateway (AES-CBC/CTR/KW, ECDSA, ECDH, Ed25519,
  X25519, RSA-PSS, RSA-OAEP) throw `std::logic_error` (a porting gap, not a JS error).
  `generateKey` is implemented for RSASSA-PKCS1-v1_5 only.
- `Stats` has no `birthtime*`; `readFileSync`/`readFile` decode only `"utf8"`.
- A registered signal listener keeps the event loop running (Bun's do not); every TS
  site removes its listeners before it finishes.
- Spawn: `timeout`, `killSignal`, `onExit`, `stdio` arrays and `ipc` are not provided
  (unused). Requires Linux >= 5.3 (pidfd). `spawn` sets SIGPIPE to ignored, as Bun runs.
- `process.memoryUsage`, `node:v8` heap statistics and `process.hrtime` are unused by the
  TS. `node:v8` `serialize`/`deserialize` is the KV value codec (kv component).

## How the service loads `.env` (contract for the C++ entry point)

`ops/launch-vps.sh` / `launch-mac.sh` run the pinned Bun with `--no-env-file` (Bun's
automatic `.env` loading is off). `ops/launch-service.ts` verifies the release layout,
payload and runtime, then calls `loadEnvFile(join(root, ".env"))` before reading
`CODEX_HOME`/`HOME`; variables already in the environment (systemd `Environment=`,
e.g. `DENO_TIMELINE=production`, `DENO_DIR`) win over the file. VPS mode then applies
Linux permissions and imports `scripts/serve-vps.ts` in-process (which requires
`DENO_TIMELINE === "production"`); mac mode spawns `scripts/serve-mac.ts` with
`env: { ...process.env, SQLITE_TMPDIR }` (serve-mac deletes `DENO_DEPLOY`,
`DENO_DEPLOYMENT_ID`, `DENO_DEPLOY_BUILD_ID`, `DENO_REGION`, `DENO_TIMELINE`). The C++
entry point calls `rt::env::loadEnvFile(root + "/.env")` at the same point; a missing
file throws ENOENT (the launcher does not catch it).

## Scope: replacing `@simplewebauthn/server` 13.2.2 (not implemented)

Used only in `src/auth/passkeys.ts`; registration options are built by hand
(`pubKeyCredParams` alg -7 ES256 and -257 RS256, `attestation: "none"`,
`residentKey: "required"`, `userVerification` "required" for admins else
"preferred", challenge = base64url of 32 random bytes).

- `generateAuthenticationOptions({ rpID, timeout, allowCredentials, userVerification })`:
  returns `{ rpId, challenge, allowCredentials, timeout, userVerification }`; the
  challenge is base64url of 32 random bytes (from rt::crypto randomness).
- `verifyRegistrationResponse({ response, expectedChallenge, expectedOrigin, expectedRPID,
requireUserVerification })`: `id === rawId`, `type === "public-key"`; clientDataJSON
  (base64url JSON) `type "webauthn.create"`, `challenge`, `origin` exact; CBOR-decode
  `attestationObject` `{ fmt, authData, attStmt }`; parse authData (rpIdHash 32,
  flags, signCount u32 BE, attested credential data: AAGUID 16, credential id length
  u16 BE, id, COSE_Key CBOR; extensions CBOR when ED); rpIdHash == SHA-256(rpID);
  UP set; UV set when required; COSE `alg` in the library's default supported list
  (-8, -7, -36, -37, -38, -39, -257, -258, -259, -65535); verify `fmt` (with
  `attestation: "none"` browsers send "none"; the library also verifies "packed",
  "fido-u2f", "tpm", "android-key", "android-safetynet", "apple" and a faithful port
  must accept the same). Used outputs: `verified`, `registrationInfo.credential.id`
  (base64url), `.publicKey` (COSE bytes, stored base64url), `.counter`,
  `.transports` (from `response.response.transports`).
- `verifyAuthenticationResponse({ ..., credential: { id, publicKey, counter } })`:
  clientData `type "webauthn.get"`; authData rpIdHash, UP, UV when required; signature
  over `authData || SHA-256(clientDataJSON)` with the stored COSE key: EC2 (P-256/384/521,
  DER ECDSA), RSA (PKCS#1 v1.5 SHA-256/384/512/SHA-1, PSS), OKP Ed25519; counter rule:
  when either counter is non-zero the new one must be greater, else the library throws.
  Used output: `verified`, `authenticationInfo.newCounter`.
- The TS catches every failure and logs `error.message` (`console.warn("[ai.ubq.fi]
passkey assertion verification failed", { message, ... })`), so the replacement must
  reproduce the library's error messages for log fidelity.
- Needs: a CBOR decoder (major types 0-5, 7; definite lengths), COSE_Key parsing, and
  OpenSSL EVP verification for ES256/384/512, RS256/384/512/RS1, PS256/384/512, EdDSA.

## Scope: native addons (`process.dlopen`, `require(".node")`)

- `src/runtime/permissions-linux.ts`: after checking `.uos-permissions/manifest.json`
  (`path: "linux-arm64.node"`, `architecture: "arm64"`, sha256 of the artifact) it calls
  `process.dlopen(module, path)` on the addon built from `src/runtime/permissions-native.c`
  (exports `support()` -> `{ landlockAbi, seccomp }`, `readonlyMount(path)`,
  `enforce(grants)`: Landlock ruleset + seccomp process filter with thread sync).
  The native gateway verifies the packaged addon and enforces its own C++ Landlock
  and seccomp profiles, including reduced-ABI and opt-in child-process policies.
- `src/storage/native-addon.ts`: verifies provenance/input hashes, then
  `createRequire(import.meta.url)(path)` loads `native-kv.node` (@deno/kv 0.14.0 NAPI v4
  backend at commit 9fe65cc138ff56ca5bfe7a1bf8c53e832246f9d5; exports `open`, `close`,
  `snapshotRead`, `atomicWrite`, `dequeueNextMessage`, `finishMessage`, `startWatch`,
  `dequeueNextWatchMessage`, `endWatch`). The C++ port links the KV backend natively
  (kv component, `docs/rt/kv*.md`); the provenance verification maps to rt::fs +
  rt::crypto (`createHash("sha256")`) if an artifact is still loaded.
- `src/runtime/websocket.ts` uses `createRequire` only to read `ws/package.json` (a JS
  dependency check, no native code).
