# Signing-helper migration candidate (#1182)

The runtime implementer cannot edit protected `ops/` paths. The repository helper is therefore **not fixed by
these files alone**. `patches/gpg-dsh-legacy-migration.patch` is an owner-applied candidate against
`670d2b3a494713a4631f57a2d84369c175963f2b`; applying it requires the repository owner's protected-path authority.
Merging this candidate does not install anything on the owner's Mac.

## Candidate behavior

- Acquire the legacy rebuild lock before discarding the legacy seed, staging copies, or set-aside copies.
- Defer migration when the legacy lock belongs to a live process, even if its timestamp looks stale. The new call
  still signs using `$TMPDIR/dsh-gnupg-<uid>/seed`; a later call performs cleanup after the legacy call finishes.
- Also defer when the owner has not been published, or the lock is not a real directory owned by this user.
  An unidentifiable owner is not proof of abandonment; any manual recovery requires quiescing legacy invocations first.
- Continue reclaiming locks with known dead owners and removing abandoned legacy key copies under the lock.
- Retain ownership checks, configuration-file refusal, and the per-user layout. Current-layout stale-lock recovery
  and publication behavior remain separate work under #1168.

## Owner application and deterministic fixture

Only an authorized owner should apply the protected-path patch:

```sh
git apply --check patches/gpg-dsh-legacy-migration.patch
git apply patches/gpg-dsh-legacy-migration.patch
sh tests/fixtures/gpg-dsh-migration.sh "$(pwd)/ops/gpg-dsh.sh"
```

The fixture needs a writable absolute `TMPDIR` and the local Git object for legacy helper
`ed0ecdda85ee261bc8388558bb26c2c86f5ac9a5`. It performs no fetch, install, real signing-key access, or real GnuPG invocation.
All keys are fixture strings; `gpg`, `gpgconf`, and the real-homedir write probe are replaced. A copy barrier pauses
that exact legacy helper after its lock and staging directory exist. A fake clock makes its live owner token old
without a timing-dependent sleep. The fixture invokes the candidate, checks preservation, repeats with the owner
file temporarily unpublished, and then releases and awaits the old call. Both calls must succeed. A subsequent call
must collect a dead-owner lock and abandoned key copies; configuration refusal and lock-symlink preservation are
also checked. All fixture state is removed on exit, including failure.

Against the unpatched base helper, the same command intentionally fails: the new call exits 0, the lock and stage
are deleted, and the resumed legacy call exits 2. Against a temporary patched copy, it must report
`new=0 legacy=0 lock-and-stage-preserved=true` and `PASS`. The helper argument can point to that temporary copy;
there is no need to change the protected checkout or install a helper to validate the candidate.

The patch also adds the probe to `tests/gpg-dsh.test.ts`, so the existing Linux and macOS test commands exercise
the regression after owner application, without changing any existing assertions. The legacy commit is an ancestor
of the base revision and is available with CI's full-history checkout.

After owner application, run the normal focused `tests/gpg-dsh.test.ts` suite and full
`bun --no-env-file run verify` with the repository's frozen dependencies and trusted native fixture binding.
Installation, if separately authorized, still requires comparing the candidate with `~/bin/gpg-dsh` first.
