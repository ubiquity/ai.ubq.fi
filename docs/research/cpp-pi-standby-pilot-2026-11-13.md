# C++ Pi standby pilot — qualification report, 2026-11-13

**Gateway/namespace budget, native/raw prefix, one-hour soak and post-soak controls passed. Strict whole-transport zero-new-swap was NOT MET/UNPROVED. Production standby is not qualified or activated.** The C++ route is the best currently measured Pi runtime candidate for this bounded profile; the separate recommendation is one small C++ VPS utility pilot, not a default rewrite.

## Six requested deliverables

| Deliverable                                                                 | Result                                                                                                                                                            |
| --------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1. Pinned TS/Rust source, runtimes, binary identities and profiles          | Recorded below and in the existing PR1 baseline; historical Rust is not substituted with an unbuilt current lane.                                                 |
| 2. Repeatable two-turn Codex fixture, real disconnect and accounting        | Shared committed raw/native fixture, actual Codex first pair through Pi SSH ingress, local/final pressured cancellation and stopped journal accounting qualified. |
| 3. TS-versus-Rust results, mismatches and uncovered behavior                | Preserved: bounded VPS results plus the historical Rust upstream-release failure and accounting/delivery gaps.                                                    |
| 4. Actual Pi-userland C++ release, reproducible build and sanitizer results | Exact stripped artifact ran on Pi; ABI/library readback matched; four measured builds and release/native ASan+UBSan 6+6 CTests passed.                            |
| 5. Memory, latency and build/disk scorecard                                 | Exact scope/method tables below; missing or differently bounded measures remain explicit.                                                                         |
| 6. Two separate decisions                                                   | C++ Pi candidate conditional on unmet transport/production gaps; yes to one bounded new utility pilot, no default rewrite.                                        |

## Frozen identities and scope

Candidate source **`7069c31b3f50d0552ef77e99b984f59fde479b13`**, tree **`827f3281deaa0b36002d710c172a6b0e43d89a38`**. Installed stripped SHA-256 **`bbb18300ea225e2803bc1f95ce5576b42af862c135c828667fd163a191c5bfd3`**, **991,848 B**. Full source identity/`cpp-<SHA>` and clean-build identity were asserted. The physical controller workspace was7aafe3de5f73237dd428c4e8677eecd6961ea1f8; it is distinct from candidate source.

One loopback POST /v1/responses supports **stream:true only**, one canonical `deepseek-flash` via official Chat-to-Responses translation, two active requests, zero waiters and eight header sockets. Reduced limits are4MiB request,1MiB upstream event,2MiB complete terminal,16KiB headers and depth64. Missing/false stream is a declared refusal, not buffered API parity. Synthetic credential/quota/custom journal authority is not production credentials, Bun-JSC/Deno-V8 KV compatibility, subscription routing or distributed state.

Pi state used disk-backed /tmp per host preflight; the sealed summary does not restate its exact filesystem type, which remains an explicit metadata gap. VPS baseline scratch was tmpfs, so persistence timing is not interchangeable. Actual Pi controls were memory.max/high96MiB, swap0, cpu.max90000/100000 and pids.max256.

All inference work used synthetic providers; local TLS used private test CAs. No live-provider generation, credit, quota or production-network latency is established. The shared fixtures and opt-in CLI are committed source; use `cpp/README.md`, `cpp/tools/build.py` and the shared conformance helpers with the pinned SDK/native bindings. Exact executed argv, profiles and original-artifact hashes remain in the selected provenance; this report adds no source/replay framework.

| Earlier bounded VPS reference                          | Warm RSS MiB c1/c2 | Held RSS MiB c1/c2 | Raw p95 ms c1/c2 |
| ------------------------------------------------------ | ------------------ | ------------------ | ---------------- |
| TS d94 application / measured96cf foundation, Bun1.4.0 | 85.621 /89.586     | 86.055 /90.734     | 34.827 /62.067   |
| Historical Rust b588 WASM / workerd1.20261012.1        | 287.496 /275.133   | 291.613 /280.191   | 231.545 /373.316 |

TS source d94fef52ccc76e0d5aa019154f11e47d5e10fba6, measured fixture96cfca54d033aedff8e237a4cbcebb051f62f066/tree7ae301184cfe261d45b1add6e0587e77d1f26688; Bun SHA086c4121c8738a8e0f5ed730e8a461bc3973b4444e372ddb77aef9a747fa2ae9. Rust sourceb588420dc1275ac1f7548a6932a5e63330751cea/treec9bf439743b2f03d4761639fedd17ef2731430d3; WASM SHA99a3e146e0193c3c0d77847167374ecf1cef5834e8e123420b7a395e6df57f53; workerd SHAfabfd6c40a3bb74a98aac54999b456bdb3b7aa7ede4cbbb33898e1f8836a2564. Rust profile LTO=true/codegen-units1/opt-level=s/strip=debuginfo. Its disconnect released upstream only at the5s mock deadline (~4878ms after disconnect); delivery stayed unobserved and API/kernel/paid reservations were absent. These N20 warmup-excluded VPS numbers, older builds/features and unlike SSH/Pi hardware are not a language-only comparison or the owner's production700MB/350MB estimate.

## Physical Pi scorecard

Full-pipeline child0 ran3666.630s. Native Codex0.167.2 SHA b581df3989da98e36fccd7d40cc980727a9ee87bcf0e6ef52ffb2544d6a5302f was the first two-request inference pair through SSH; exact tool/result/final readback passed. The following RSS/PSS/current samples are phase-end observations. **Kernel peak is cumulative across all phases, not a per-phase or soak-only peak.**

| Phase                         | CPP RSS/PSS KiB | Group current / cumulative peak MiB | N / full-N p95 ms | POSTs / settlements, cumulative |
| ----------------------------- | --------------- | ----------------------------------- | ----------------- | ------------------------------- |
| cold_idle                     | 7960 /3949      | 19.473 /19.891                      | —                 | 0 /0                            |
| native_two_turn               | 8880 /4745      | 20.230 /20.848                      | —                 | 2 /2                            |
| warm_idle                     | 8880 /4853      | 20.230 /20.848                      | —                 | 2 /2                            |
| one_stream                    | 9136 /5109      | 20.727 /21.402                      | 20 /145.732       | 22 /22                          |
| two_streams                   | 9140 /5113      | 21.152 /21.742                      | 20 /155.528       | 42 /42                          |
| accepted_large_1536k          | 15824 /11797    | 27.559 /32.828                      | —                 | 43 /43                          |
| accepted_summary_720k         | 9240 /5213      | 20.887 /32.828                      | —                 | 44 /44                          |
| slow_reader                   | 11664 /7637     | 23.434 /32.828                      | —                 | 46 /46                          |
| repeated_cancel               | 11664 /7637     | 23.652 /32.828                      | 10 /157.404       | 56 /56                          |
| four_stream_overload          | 11664 /7637     | 23.746 /32.828                      | —                 | 58 /58                          |
| four_stream_overload_postsoak | 10360 /6418     | 23.391 /32.828                      | —                 | 778 /778                        |
| post_idle                     | 10360 /6312     | 23.090 /32.828                      | —                 | 778 /778                        |

The large cases preserve exact1.5MiB transcript/720KiB summary projection. Ten repeated cancellations and actual paused-receiver pressure passed; Pi pressured cancellation released upstream in32.430687ms (a single cancellation observation, not p95). Kernel send buffer was32768 B; same-epoch reads stayed111/111 or139/139 while a downstream write was pending. Both four-request overload rounds held exactly2 active/0queued and dispatched2POSTs; the other2 returned503/local_inference_overload/Retry-After1. Both accepted streams then completed and drained.

The soak ran08:58:57.927–09:58:57.927 UTC, **3600.000226s**,359 two-stream batches at10s intervals, **N718**. Nearest-rank p50/p95/p99/max were **141.509305 /292.401992 /762.215845 /1459.107242ms**. Timing is off-Pi client start to the first parsed meaningful delta through private SSH ingress before producer ACK; warmups/native excluded. Although some helper fields are named TTFB, this boundary is explicitly first-meaningful-delta, not a native-Codex percentile or unconstrained throughput. Prefix p95 uses independently recomputed full N20, not the helper's last sub-batch percentile.

## Resource verdict and explicit swap gap

Gateway+namespace ALL-PHASE peak was **34,422,784 B =32.828125MiB**, under actual96MiB/zero-swap limits; all high/max/OOM counters were0. The group includes namespace/relay/control helpers. Host internal1Hz observed MemAvailable low was **114,840KiB =112.1484375MiB**, above the64MiB reserve; phase-only samples had a higher low119,532KiB. Sampling does not prove an absolute continuous minimum. CPP's pre-exit kernel VmHWM was21,220KiB (20.722656MiB), VmRSS10,360KiB, VmSwap0 at09:57:29; later shutdown is included only in the final cgroup peak.

**Strict whole-transport zero-new-swap: NOT MET in the observed interval; initial attribution/inheritance UNPROVED.** Outside SSH/guard/systemd-run/cat helpers had swap. Four tracked SSH processes had smaps swap1724→1728KiB between09:13:59 and09:28:04, including one+4KiB delta. Those are mid-run snapshots, not process-start baselines; do not label earlier pages inherited or conclude whole-transport zero-new-swap. Gateway/namespace swap remained0. The existing shared SSH master is excluded and was preserved.

| Outside helper at09:57:29 | VmHWM /VmRSS /VmSwap KiB |
| ------------------------- | ------------------------ |
| sshd                      | 9808 /9808 /100          |
| sshd                      | 6724 /6180 /484          |
| systemd-run               | 6204 /3680 /924          |
| python3                   | 10316 /2596 /5236        |
| sshd                      | 9956 /9820 /92           |
| sshd                      | 9032 /6396 /1048         |
| cat                       | 1172 /852 /88            |

Outside helper HWM sum53,212KiB plus gateway-group charge peak gives **84.79296875MiB**. This is a conservative non-concurrent RSS/charged-memory sum with shared-page double counting, **not physical incremental RAM**. Outside HWM coverage ends09:57:29 and is not an independently complete outside-helper lifetime peak. This bound cannot erase the swap criterion gap.

Final actual POST/logical dispatch/settlement was **778/778/778**:767 delivered and11 interrupted; reserved/unsettled/active/queued all0. Terminals were767completed,1failed and10cancelled. Independent stopped-state validation combined snapshot high-water3074 with816 valid remaining journal frames through sequence3890,778unique dispatched/settled records,1556quota transactions/3112mutations. No generation replay/refund was introduced. Snapshot/journal hashes and sizes are in the selected receipt; private state files are not published.

Independent readback confirmed both units not-found/inactive/dead, no cgroup/task PID/Unix listener/local18799listener, all handles settled0, shared master alive and final binary/all7OS library hashes unchanged. The two earlier failed Pi attempts and their separate58-dispatch/56-settlement ledger remain failed, outside the successful cohort.

## Build, package and static/local qualification

| Build case           | Wall s  | Child RSS /group peak MiB | Extra sampled disk MiB | TU /link s |
| -------------------- | ------- | ------------------------- | ---------------------- | ---------- |
| pi-release-clean     | 121.282 | 582.27 /761.19            | 24.40                  | 13 /0.539  |
| pi-release-one-file  | 5.539   | 269.64 /275.82            | 0.16                   | 1 /0.461   |
| native-asan-clean    | 154.145 | 899.71 /1024.00           | 122.65                 | 13 /2.099  |
| native-asan-one-file | 7.790   | 353.19 /353.71            | 0.66                   | 1 /1.428   |

Clean builds compiled13 TUs; one-file builds compiled only payload_policy.cpp plus archive/gateway/protocol links. Both profiles passed6/6 post-incremental CTests; native ASan+UBSan/leak/warnings/focused tidy passed. Native clean hit its1GiB cap with211memory.max events but0OOM/swap; do not call that uncapped cost. Configured caches remained warm;1Hz disk peaks are lower bounds, wall includes observer/exit quantization, child RSS and group/cache peak differ. Compiler/test RAM is not gateway RAM.

Added stripped binary payload991,848 B; unstripped6,706,448 B and builder debug5,717,544 B remain separate. Already-existing OS library/loader closure10,023,608 B is not bundled; binary+required libraries11,015,456 B excludes separately generated runtime config/state. SDK1,230,209,024 B +owned outputs412,377,088 B =1,642,586,112 B; free snapshot3,083,436,032 B. CPP tracked source177,888 B; measured scope including Decisions478,631 B. ELF64/AArch64 interpreter/lib closure, GLIBC2.34/GLIBCXX3.4.29/CXXABI1.3.13 maxima, no RPATH/builder paths and basename debuglink were audited; final Pi library hashes matched.

Full no-emit TypeScript6.0.3 passed unchanged compiler/config/types/source on supported pinned Node24.20.0, exit0/72.051s, ~1254MiB child RSS/~1251MiB group peak, no max/OOM/swap under actual2GiB cap. First-party TS/application stays Bun. Both diagnostic-free240s/360s Bun timeouts remain incomplete. Frozen local staged core/native/header, TLS3 and pressure2 passed exactBBB: zero selected header/body/event/terminal differences; correct SNI and fail-closed wrong SAN/untrusted CA; no retry; raw SO_SNDBUF32768 and6.048879ms local pressured cancel, one charge/settlement and drained state. Local128MiB/outer768MiB controls are distinct from physical Pi96MiB. Dirty prototypes were never promoted.

## Two recommendations and remaining scope

**Pi redundancy:** C++ is the best currently measured runtime candidate for this bounded Pi profile, with completed physical-Pi evidence. No verified small Rust fix meeting the Pi requirements was supplied; user preference for such a fix remains. Production redundancy is **conditional, not activated**: strict whole-transport zero-new-swap is unmet/unproved, production credential/state/codec import, revocation/quota ownership, single-writer failover and independent public ingress are unimplemented/unqualified. Mock/client currently require VPS, so this is not a full VPS-host-outage proof. Process crash/reopen and fsync tests are not power-loss or host-reboot durability proof.

**Next small VPS utility:** recommend one narrowly owned C++ utility pilot using the demonstrated build/sanitizer/compatibility discipline and measured build/disk tradeoffs. Do not infer a language-wide RAM win, choose a default rewrite, expand the provider/feature surface or install persistent automation. Development-host C++ primary memory/p95 curves and live-provider inference remain UNMEASURED; the unlike TS/Rust/physical-Pi profiles cannot supply them.

PR1366 shipped the real opt-in CLI/scope repair and permanent ignore protection for9existing env/backup paths; root verified no tracked secrets and removed its temporary guard. Historical hosted outcomes remain separate: PR1365 CI43355368629 failed misplaced Darwin cases plus a new Knip finding; PR1366 CI43368578978 passed portable/Knip/docs/mac-containment but failed pinned Darwin utility and real macOS signing/private-copy gates. Earlier local reviews were unavailable without terminal verdicts or ingested findings; they are not clean reviews and remain non-gating. PR1368 publication and its delivery receipt are root-owned; no new hosted run or deployment was performed by this report task.

Receipt namespace is c18ba89e18e8c4c810e8dfab831978ea7ed5aa33d6f343bf3282dea4e12db13c. Pi:ae9e0f0b-6924-4833-829c-0c446d160c4b; build:eac26a0e-15ca-426f-8f8b-b0ea792b5262; package:6764590e-a563-4974-aba7-f34a021e5344; static:f87ecea8-bcb2-4ada-94e0-7e1dc5d67c92; core:8b0902cc-1c71-4f4c-8425-e937f586b498; TLS:d7858bb1-7fc4-4043-90fb-a36697ad89b7; pressure:8452eda1-6019-4596-a64a-f5fc70e28ce1. The compact scorecard and selected sanitized receipts retain exact values and source hashes. No keys, raw native prompts, private configs/databases or new source bundle are published. Publication gates belong to the publishing commit/tree's delivery receipt; runtime receipts retain their exactF/G/BBB identities.
