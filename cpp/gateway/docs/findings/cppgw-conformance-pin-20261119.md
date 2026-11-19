# Paired conformance production pin

The starting revision is `541a897b3a5637da0d5b1442c84e80eb41eb9c37` on
`agent-jobs/cppgw-drift-port-20261119-44456061234-1`. The prior drift run reached 186 cases
against development only after restoring the historical `cpp/gateway/tests/production_smoke/`
fixture in its detached oracle. Without it the managed driver stopped at `ModuleNotFoundError:
No module named 'cases'`. [Production drift findings](cppgw-drift-port-20261119.md#paired-conformance)

The fixture now loads the smoke helpers, restored Bun launcher and preload-free Bun config from
the candidate checkout. The launcher takes the oracle app root explicitly and resolves `#http`
from `serve.ts` there. The frozen install checks `ws@8.18.3`. The production application's native
KV inputs still have to match the sealed package inputs. The existing nine comparator controls
passed, and Bash syntax, ShellCheck and Prettier passed on the changed launcher paths.

The production pin and clean package comparison are pending.
