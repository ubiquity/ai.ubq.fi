# Paired gateway conformance

The suite compares a sealed native ARM64 package with the TypeScript application at the pinned
`development` commit. It uses Bun 1.4.0 and loopback mocks. Prepare the package from the same clean
candidate commit and keep the seal receipt in your scratch directory.
[Production pin](oracle-revision.txt) · [Build and seal instructions](../docs/CONFORMANCE.md#run-on-linux-arm64)

From the repository root, with `scratch` set to the absolute directory used to seal the package, run:

```bash
bash cpp/gateway/conformance/run.sh "$scratch" "$scratch/seal-receipt.json"
```

The command prints the directory containing `results.json` and `table.md`. The baseline is 184
passing cases, zero wire differences and two shared expectation failures, `chat-invalid-role`
and `responses-invalid-tool-type`. Both gateways return HTTP 200 and dispatch to the DeepSeek mock.
The case assertions require HTTP 400 and zero dispatch, so the command exits 1.
The suite checks each runtime's exact identity in HTTP headers, health JSON and audit rows.
The native package reports its build SHA and the detached Bun oracle uses its source's unstamped identity.
The receipt keeps the raw values.

## Move the production pin

1. Fetch `origin/development`, verify its full SHA against the remote ref and record the commit
   and tree. Write the verified SHA to the pin file.

   ```bash
   (
     set -euo pipefail
     git fetch --no-tags origin refs/heads/development:refs/remotes/origin/development
     production=$(git rev-parse --verify 'origin/development^{commit}')
     test "$production" = "$(git ls-remote --exit-code origin refs/heads/development | cut -f1)"
     git show --no-patch --format='%H %T %s' "$production"
     printf '%s\n' "$production" > cpp/gateway/conformance/oracle-revision.txt
   )
   ```

2. Commit the pin and rebuild and seal the native package from that clean checkout. Run the command
   above. The runner uses `oracle.sh --production` to fetch the exact pinned SHA and installs the
   frozen application dependencies.
   The native checkout supplies the launcher, isolation and fixtures.
3. Check the receipt for the two known shared failures and zero other differences. If another case
   differs, retain the assertions and record the case, both outputs and which side changed using
   the previous receipt. Record the oracle and candidate commit and tree with the new receipt.

For historical fixtures such as standalone smoke and the regex benchmark, the default `oracle.sh`
invocation uses the legacy revision. [Historical pin](legacy-oracle-revision.txt)
