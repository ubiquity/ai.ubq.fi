# C++ terminal record and request ownership performance

## Work contract v1

- Start from `b54357a92c0f2669335d83f04047c3f7de32f925` (tree `d0aabad25d3dbe02f2f33ff519e5b77715d19d37`) on Linux ARM64. Reduce CPU per rejected request by at least 10%, with no regression for model listing or a small inference request.
- Source ownership starts with `cpp/gateway/src/handler/terminal_log.cpp` and `cpp/gateway/src/handler/serve_handler.cpp`. Use the baseline profile to confirm the callback change before editing. A change in source ownership requires a new contract version.
- Keep terminal field names, values, types and null/omission semantics, response statuses and bodies, authentication, delivery receipts, and settlement and durable-write ordering. Preserve owners across suspended or detached work. Deployment, other gateway modules, test edits, and provider traffic are outside this work.
- Run the rejected request, authorized model list, and small inference against loopback fixtures. Measure gateway CPU, request rate, allocation calls, and a reject call-stack profile on the same baseline and candidate configuration.
- After measurement, run the exact reverse `tools/links.py` closure under the portable release and ASan+UBSan profiles, and run the full release suite. Use isolated processes with empty home, fresh temp storage, and only mock credentials. Check the path boundary and diff before handing off the branch.
- The supplied checkout does not contain `bench/request_path/` or `docs/findings/cppgw-perf-request-path-20261117.md`. The baseline fixture source and the exact reverse link closure are still being established.
