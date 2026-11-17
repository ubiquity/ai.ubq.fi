# Uninstalled native operations draft

These files are packaging inputs, not an installed service. No live unit, service, proxy, production state, credentials, listener or release selector was changed. The draft uses placeholder paths and has no install target or automatic restart.

`launch-native.sh` accepts exactly one absolute path to a private `uos-gateway-launch-v1` manifest. It resolves its package once, clears inherited native-loader and JavaScript preload options, and executes the same packaged gateway binary. The binary verifies every packaged file, host-library hash, native-KV provenance and Linux permissions payload before opening state or listening. It does not discover dotenv files, install dependencies, compile helpers, convert stores or fall back to source execution.

The draft unit supplies an explicit operator-owned environment file. Its parsing and credential custody must be reconciled with the existing Bun launcher's explicit repository-root `.env` behavior before any cutover; this draft grants no new credential access. See `../README.md` in a package, or `../docs/RUNTIME.md` in the source tree, for prerequisites and the exact manifest/state contracts. Ports7999/8000 are deliberately refused by the candidate startup path.
