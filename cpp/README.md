# C++ direct DeepSeek fixture pilot

This isolated C++20 candidate serves an authenticated streamed loopback `POST /v1/responses` for direct `deepseek-flash`, using the TypeScript Chat translation as its oracle. It is a fixture candidate, with a separate synthetic authority and storage codec. It grants no production standby, credential, database, provider-spend or activation authority.

PR1 is merged at `db1d78cf9037775ba29b8aa72e89f2b2dbd6ba80`, tree `26b895d911b50f3ffcc3c139aa2869aec05706d8`. Its underlying deployed handler is `d94fef52ccc76e0d5aa019154f11e47d5e10fba6`. The C++ implementation contract and acceptance matrix remain the gate authority; a successful build or health response does not establish acceptance.

The runtime has one Asio IO thread, one provider attempt per request, two active permits, no waiting queue, and at most eight header-reading sockets. It preserves source dispatch-quota reservation/defer/release order, full history, tool-call IDs, reasoning and usage missingness. The captured namespace replay uses the bare original name; the immediate peer-FIN vector is interruption, with no response, dispatch or charge. Those source quirks are retained deliberately.

The reduced profile caps headers at16KiB, ingress at4MiB, an upstream SSE frame at1MiB, and the full terminal at2MiB. JSON depth64 and named allocation/node/event limits apply. These are declared pilot capacities; they do not claim the production32MiB ingress or unbounded cumulative output contracts. The request-owned first-frame/inactivity deadline does not authorize provider retry or quota failover.

## Reproducible offline build

Use the controller-qualified Debian ARM64 SDK manifest `9dde9684e06dcbbdc5433fd26c8517cb0ff0b63c2a80e2501f05f6ce27eb52f5`. It contains GCC12.2, Boost1.81, OpenSSL3, CMake3.25 and Ninja1.11. No dependency downloads, installs or application compilation occur at runtime. The builder's installed GCC16 is used only for the native sanitizer preset.

```sh
python3 cpp/tools/build.py /absolute/qualified-sdk pi-release checkpoint
python3 cpp/tools/build.py /absolute/qualified-sdk pi-release test
python3 cpp/tools/build.py /absolute/qualified-sdk native-asan checkpoint
python3 cpp/tools/build.py /absolute/qualified-sdk native-asan test
python3 cpp/tools/build.py /absolute/qualified-sdk pi-release tidy
```

`pi-release complete` runs configure, build, all six CTests and the mandatory static-analysis gate. `tidy` runs that same gate separately; the always-run CMake target is `cpp-static-analysis`. The gate verifies the Pi compilation database, analyzes all twelve production translation units plus `protocol_tests.cpp`, and checks three analysis-only controls. See [static-analysis.md](docs/static-analysis.md) for the selected checks, receipts and limitations.

The driver creates one reusable directory per preset under `.data/cpp-build`, a task-local CMake executable view, disk-backed compiler scratch, and full command/resource logs under `.data/cpp-evidence`. Its view executes the same qualified SDK binaries through their pinned loader with `--argv0`, so generated archive, regeneration, test and install commands use that same loader. The SDK itself stays read-only; no global library path changes are required.

Build jobs remain1. The first512MiB compiler virtual-memory limit was measured insufficient for the transport translation unit. The owner authorized768MiB for the compiler and bounded build cgroup; this is distinct from the candidate's provisional96MiB service-memory acceptance target. The one-second build sampler reports a lower-bound disk peak, enforces the additional768MiB/aggregate2GB storage envelope and refuses filesystem free space below2GiB. Native ASan runtime requires its shadow address space and does not inherit the compiler VM cap.

The Pi release uses the SDK compiler/sysroot, static Boost.JSON/System and dynamic target OpenSSL/libstdc++/libc. CTest launches that binary through the same target loader on the VPS; actual Pi execution remains a separate acceptance surface. The native sanitizer preset uses host Linux headers and only the SDK's Boost header subtree.

## Private fixture configuration

Create an owned mode700 state directory and a private JSON configuration. The only runtime command is `uos_cpp_pilot --config /path/to/config.json`. A minimal synthetic configuration is:

```json
{
  "fixture_mode": true,
  "token": "synthetic-secret",
  "upstream_key": "synthetic-provider-secret",
  "upstream_origin": "http://127.0.0.1:12345",
  "state_directory": "/private/task/fixture-state",
  "usage_limit": 50,
  "port": 0
}
```

Read the `ready` stdout event for the actual loopback endpoint. Plain HTTP upstreams require the explicit fixture mode and loopback address. Ordinary TLS transport fixes the provider origin, verifies certificate chain and hostname, sends SNI, and uses the configured CA file. No request body can choose an upstream URL, credential, filesystem path or provider.

The journal owns an exclusive lock and records source quota transitions separately from semantic terminal, physical delivery and internal settlement. Each committed journal frame is checksummed and fsynced before publication. Snapshots use file fsync, atomic rename and directory fsync with a high-water sequence. Restart never dispatches a generation. A reserved lease is not reclaimed merely because a process disappeared; an ambiguous dispatched request remains charged. The codec is `uos-cpp-fixture-state-v1`, not Bun or Deno KV.

## Async owners

`Runtime` owns the IO loop dependencies, acceptor, authority, control descriptors and graceful-drain deadline. Each `Session` owns its HTTP parser/buffer, socket, request arenas, upstream, cancellation state, permit and delivery facts. A downstream wait callback owns the session until it completes; teardown cancels and joins that wait before the permit and buffers are released. Each write retains its complete owned wire buffer until completion, and upstream reads pause until downstream events drain.

`Transport` owns resolver/TLS/socket/timer state across each await. Timer callbacks use weak ownership and cannot dereference a destroyed transport. `Upstream` separately owns HTTP response framing and the current bounded body buffer. JSON arenas outlive every value using them. Synchronous fixture durability occurs on this same IO thread; its fsync latency is measured and cannot be preempted by an Asio timer.

The optional inherited fixture-control channel has no HTTP route. It requires actual owned anonymous pipe descriptors with exact read/write direction; an arbitrary child-process `pipe` abstraction must not be assumed to provide them. Fixed framed commands control only synthetic revocation, bounded readback and finite crash fences. Ordinary runtime has no armed fence or channel.

## Qualification status

The first Pi-release checkpoint built and passed the pure protocol smoke plus the shared real-socket two-turn tool loop and schema-refusal accounting. It was a dirty working-tree checkpoint, not a final release identity. Real disconnect, pinned native client, extended boundaries, control/crash recovery, sanitizer/tidy and independent Pi resource/soak evidence remain to be bound to the final source and binary. Buffered/nonstream requests are UNSUPPORTED pilot scope: false or missing `stream` is rejected with HTTP400 `invalid_request_error`/`param: stream` before provider dispatch, preserving eager reserve/release accounting when quota exists. TypeScript buffered behavior is unchanged; this is not buffered parity or a general OpenAI replacement.

The shared TypeScript producer and client remain in `tests/helpers/responses-conformance-*.ts`. The C++ adapter adds no translation or expected-output repair: it starts the actual candidate, records its real socket output and checks the private checksummed quota journal independently. Official event field reference: <https://developers.openai.com/api/reference/resources/responses/streaming-events#response.function_call_arguments.delta>.

The24MiB request budget includes named JSON allocations plus conservative external capacity reservations: four times observed ingress bytes plus256KiB during projection and8MiB plus tool-name metadata during relay. These reservations allocate no backing arrays and are reported as capacity budget, separately from actual arena/cgroup peaks. Namespace labels share one immutable owner rather than multiplying a large label per nested tool.

The accepted downstream socket requests a fixed16KiB `SO_SNDBUF` (Linux commonly reports32KiB); the actual value and same-loop upstream-read/downstream-write pending/start/completion counters are available only through inherited `READ_STATE`. They are process-epoch observations, not durable quota counters. This bounded profile seam supports observed slow-reader pressure without adding a public API or environment setting. The ASan Session translation unit exceeded the measured768MiB build VM cap; the controller authorized a1GiB compiler/task retry after fresh2GiB host headroom, with the service96MiB target unchanged.
