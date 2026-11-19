# Paired conformance production pin

The starting revision is `541a897b3a5637da0d5b1442c84e80eb41eb9c37` on
`agent-jobs/cppgw-drift-port-20261119-44456061234-1`. The prior drift run reached 186 cases
against development only after restoring the historical `cpp/gateway/tests/production_smoke/`
fixture in its detached oracle. Without it the managed driver stopped at `ModuleNotFoundError:
No module named 'cases'`. [Production drift findings](cppgw-drift-port-20261119.md#paired-conformance)

The fixture move, production pin and clean package comparison are pending. This job has yet to
record any measured case counts.
