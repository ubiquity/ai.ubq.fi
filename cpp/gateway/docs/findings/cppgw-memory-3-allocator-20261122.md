# Returning freed gateway heap on Linux

- [ ] Reproduce retained free heap with mixed concurrent Codex traffic.
- [ ] Attribute retained pages by thread, arena and mapping before changing the gateway.
- [ ] Compare allocator options and record memory, throughput, latency and CPU.
- [ ] Guard retained free heap in Release CI and document the runtime policy.
- [ ] Verify the affected Release and ASan suites, registration and committed diff.

## Starting point

The integration base is `834c97e5`. The runner is Ubuntu 26.04.1 on Linux ARM64 with glibc 2.43, Clang 21 and four CPUs. All gateway runs use fresh state and a loopback upstream under the existing namespace and sealed-package checks.

The deployed process grew from 3.8 to 42.5 MiB anonymous over two hours. Live allocator bytes leveled off around 8 MiB as free arena bytes reached 37.1 MiB. The startup executable already fixes glibc's mmap threshold at 128 KiB. The earlier serial Codex benchmark observed arena pages that `malloc_trim(0)` could return. The new workload exercises overlapping requests and varied payload sizes. [Earlier measurements](cppgw-memory-2-20261121.md)
