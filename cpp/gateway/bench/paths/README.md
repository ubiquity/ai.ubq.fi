# Production-path measurements

`json.cpp` calls the gateway's actual `rt::jsonParse` and `rt::jsonStringify` with fixed input bytes and bounded iteration and sample counts. The build owner integrates `uos-json-bench` with the primary gateway target settings and bounded compiler/link wrappers. No direct compiler invocation or replacement JSON implementation is admissible.

`tools/smoke.py` owns real HTTP/SSE/request and child-RSS measurements against the same release gateway executable and scripted loopback upstream in a fresh network namespace. Runtime results are accepted only with the binary/input/runtime/profile hashes, raw receipts, natural shutdown and settled fixture jobs. The namespace preflight can run independently with `python3 -I cpp/gateway/tools/smoke.py --preflight --output /tmp/claude-1002/prod/smoke_perf/<new-directory> --timeout 10`.

`tools/smoke.py --json-benchmark <uos-json-bench> --json-sha256 <hash> --build-receipt <receipt> --output <new-directory>` runs the native trials.
