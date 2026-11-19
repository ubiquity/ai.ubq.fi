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

The fetched tip of `origin/development` and the independently queried remote ref both resolved to
`9353aa208a336da2c1516d604e0be16dc8af0ade`, tree `e349a209c455dad00d5c82579f8f69934c94d6ec`.
The production tree has no `cpp/gateway/tests/production_smoke/` entries, and its native KV inputs
match the candidate's packaged source inputs byte for byte.

```bash
git fetch --no-tags origin refs/heads/development:refs/remotes/origin/development
git rev-parse 'origin/development^{commit}' 'origin/development^{tree}'
git ls-remote --exit-code origin refs/heads/development
git ls-tree origin/development cpp/gateway/tests/production_smoke
git show origin/development:tools/native-kv/inputs.json | cmp - cpp/gateway/packaging/native-kv/inputs.json
```

The pin now points to that production revision. The clean package comparison is pending.
