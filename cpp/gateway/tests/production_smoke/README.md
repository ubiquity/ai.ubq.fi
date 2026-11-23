# Native package smoke

The portable package verifier runs the sealed gateway with a scripted loopback upstream on Debian 12 and 13. It checks health, both model schemas, progressive SSE, WebSocket delivery, shutdown and durable reopen. See [release verification](../../docs/RELEASES.md).

`tools/smoke.py` also supports a native gateway with an explicit build receipt, binary hash, fixture flag and runtime template. Use `--fixture-self-test` to check only the loopback client and upstream. The driver requires a fresh OS network and PID namespace and an empty home.
