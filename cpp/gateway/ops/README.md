# Native Linux operations

`launch-native.sh` takes one absolute path to a private launch manifest. It clears inherited loader overrides before executing the sealed gateway. Startup verifies the manifest, packaged files and host libraries, then applies Linux confinement before opening state or binding its loopback listener. [Runtime and launch schema](../docs/RUNTIME.md)

The draft systemd unit uses an explicit operator-owned `EnvironmentFile=`. Render its user, state, package and environment paths for the approved host. Ports 7999 and 8000 are reserved. Use the separate observation template for synthetic state and public-read replay. [Side-by-side runbook](../docs/SIDE-BY-SIDE.md) · [Owner cutover checklist](../docs/CUTOVER.md)

## Open-file budget

All three native Linux service templates set `LimitNOFILE=4096`. A single value
sets **both the soft and hard `RLIMIT_NOFILE`**, rather than inheriting the host's
possibly 1,024-descriptor soft limit. The launcher and gateway inherit it unchanged.
No admission or upstream-pool policy is changed.

The current single-loop, default-transport planning budget is:

| Use | Descriptors |
| --- | ---: |
| 512 active requests: one client and one upstream socket each | 1,024 |
| 128 waiting requests: client sockets only | 128 |
| Idle upstream pool: 64 HTTP plus 64 HTTPS connections | 128 |
| Listener, event-loop/DNS handles, SQLite/WAL, logs and other runtime work | 256 |
| Planned total | 1,536 |
| Additional headroom below the 4,096 limit | 2,560 |

The idle bounds apply per transport and per event loop. Revisit this budget and
`tools/check-open-files.py` if the admission defaults, pool size or loop/transport
count change. This is not a bound on arbitrary non-inference/idle inbound sockets.
Do not lower admission limits to compensate for an under-provisioned service.

### Fresh-unit verification (disposable Linux systemd host only)

This procedure requires separate operator authorization; it is **not** a production
activation instruction. Run it for each rendered template. First run the offline
source check from the checkout:

```sh
python3 -B cpp/gateway/tools/check-open-files.py --check
```

Render and freshly install the chosen template under the test-only name
`uos-open-files-fixture.service`, with a disposable user, state and package paths
as in the linked runbooks. Use an unused loopback port, e.g. 18080 (also render the
side-by-side template's `SocketBindAllow` for that port). Copy
`tools/check-open-files.py` to the read-only, service-visible path
`/opt/uos-open-files-fixture/check-open-files.py`. Do not use a production unit,
environment file, state, credentials or provider. Keep the template's
`LimitNOFILE` untouched: a transient `systemd-run -p LimitNOFILE=...` would not test
that the installed template provisions it.

Record `readlink /proc/self/ns/net` on that host. Add this test-only drop-in,
substituting its output for `<host-netns>` (a literal such as `net:[4026531840]`):

```ini
[Service]
Type=oneshot
RemainAfterExit=yes
EnvironmentFile=
RuntimeDirectory=uos-open-files-fixture
RuntimeDirectoryMode=0700
PrivateNetwork=yes
PrivateTmp=yes
TimeoutStartSec=60s
ExecStart=
ExecStart=/usr/bin/env -i PATH=/usr/bin:/bin HOME=/run/uos-open-files-fixture TMPDIR=/tmp /usr/bin/python3 -I /opt/uos-open-files-fixture/check-open-files.py --fixture --parent-netns <host-netns> --port 18080
```

The fresh runtime directory is an empty home; private temp and network namespaces
keep the credential-free fixture isolated, with only loopback sockets. On the
authorized disposable host:

```sh
sudo systemctl daemon-reload
sudo systemctl start uos-open-files-fixture.service
sudo systemctl show uos-open-files-fixture.service -p LimitNOFILESoft -p LimitNOFILE -p Result
sudo journalctl -u uos-open-files-fixture.service --no-pager -n 20
```

Require `LimitNOFILESoft=4096`, `LimitNOFILE=4096`, `Result=success`, and the
fixture's kernel-observed `effective_soft`/`effective_hard` values of 4096 plus
`result: PASS`. The fixture never changes its own limit and refuses the host
network namespace. It holds 500 client/upstream stream pairs simultaneously,
plus 128 waiting-client links, 128 idle-upstream links and 256 reserve files.
Both ends of each TCP link live in the fixture, so it exercises over 2,700 open
descriptors, conservatively more than the gateway's 1,536 planning allowance.
Any socket/open failure (including `EMFILE`) fails the unit. Archive the rendered
unit/drop-in, `systemctl show` output and journal; stop and remove the test unit
and its test-only files afterward. No production-VPS verification is claimed by
these source changes.

### Bounded overload regression

Run the native handler regression through the supported isolated test driver:

```sh
CC=clang CXX=clang++ bash cpp/gateway/tools/ci-build.sh --config release handler_admission_differential
```

The default-limit regression holds 512 permits and all 128 waiting slots, then
checks that the next request gets the ordinary HTTP 503 response with
`error.code=local_inference_overload` and `Retry-After: 5`, without increasing
either bound. Request 513 may wait; immediate queue-full rejection begins at
request 641, not 513. Existing tests separately cover the bounded 5,000 ms queue
wait and both overload response bodies. This tests the real native admission
controller and response builder, separately from the service socket fixture;
it does not claim a full gateway/provider load test.
