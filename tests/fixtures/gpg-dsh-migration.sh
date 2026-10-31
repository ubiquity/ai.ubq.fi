#!/bin/sh
# Offline issue #1182 probe. The helper argument is explicit so an owner can test a protected-path
# candidate without installing it or changing ops/. Requires the pinned legacy commit in local Git.
set -eu
case ${1:-} in
  /*) helper=$1 ;;
  *) echo 'usage: sh tests/fixtures/gpg-dsh-migration.sh /absolute/path/to/helper' >&2; exit 2 ;;
esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
root=$(mktemp -d "${TMPDIR:?set TMPDIR to a writable fixture directory}/gpg-dsh-migration.XXXXXX")
legacy_pid=
cleanup() {
  if [ -n "$legacy_pid" ]; then
    : >"$root/resume"
    wait "$legacy_pid" 2>/dev/null || :
  fi
  rm -rf "$root"
}
trap cleanup EXIT
trap 'exit 2' HUP INT TERM
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
export FIXTURE_ROOT="$root" REAL_MKTEMP REAL_CP REAL_DATE
REAL_MKTEMP=$(command -v mktemp)
REAL_CP=$(command -v cp)
REAL_DATE=$(command -v date)
export HOME="$root/home" GNUPGHOME="$root/home/.gnupg" TMPDIR="$root/tmp"
mkdir -p "$GNUPGHOME/private-keys-v1.d" "$TMPDIR" "$root/bin"
printf 'fixture-public\n' >"$GNUPGHOME/pubring.kbx"
printf 'fixture-secret\n' >"$GNUPGHOME/private-keys-v1.d/AAAA.key"
git -C "$repo" show ed0ecdda85ee261bc8388558bb26c2c86f5ac9a5:ops/gpg-dsh.sh >"$root/legacy.sh"
cat >"$root/bin/mktemp" <<'SH'
#!/bin/sh
case $1 in "$GNUPGHOME"/.gpg-dsh-probe.*) exit 1 ;; esac
exec "$REAL_MKTEMP" "$@"
SH
cat >"$root/bin/date" <<'SH'
#!/bin/sh
# The live lock looks ancient without a sleep or a change to the legacy helper's owner token.
if [ "${PAUSE_LEGACY:-}" = 1 ]; then printf '1\n'; else exec "$REAL_DATE" "$@"; fi
SH
cat >"$root/bin/cp" <<'SH'
#!/bin/sh
case ${3:-} in
  "$TMPDIR"/dsh-gnupg/*.new.*/*)
    if [ "${PAUSE_LEGACY:-}" = 1 ] && [ ! -f "$FIXTURE_ROOT/paused" ]; then
      dirname -- "$3" >"$FIXTURE_ROOT/paused.tmp"
      mv "$FIXTURE_ROOT/paused.tmp" "$FIXTURE_ROOT/paused"
      attempts=0
      while [ ! -f "$FIXTURE_ROOT/resume" ]; do
        attempts=$((attempts + 1))
        [ "$attempts" -lt 400 ] || exit 3
        sleep 0.05
      done
    fi
    ;;
esac
exec "$REAL_CP" "$@"
SH
cat >"$root/bin/gpg" <<'SH'
#!/bin/sh
set -eu
[ "$(cat "$GNUPGHOME/pubring.kbx")" = fixture-public ]
[ "$(cat "$GNUPGHOME/private-keys-v1.d/AAAA.key")" = fixture-secret ]
printf '%s\n' "$GNUPGHOME"
SH
cat >"$root/bin/gpgconf" <<'SH'
#!/bin/sh
printf '%s\n' "$*" >>"$FIXTURE_ROOT/gpgconf.log"
SH
chmod 700 "$root/bin/"*
export PATH="$root/bin:$PATH"
uid=$(id -u)
legacy=$TMPDIR/dsh-gnupg/$uid
seed=$TMPDIR/dsh-gnupg-$uid/seed
PAUSE_LEGACY=1 /bin/sh "$root/legacy.sh" >"$root/legacy.out" 2>"$root/legacy.err" &
legacy_pid=$!
attempts=0
while [ ! -f "$root/paused" ]; do
  attempts=$((attempts + 1))
  [ "$attempts" -lt 200 ] || fail 'legacy rebuild never reached the copy barrier'
  kill -0 "$legacy_pid" 2>/dev/null || { cat "$root/legacy.err" >&2; fail 'legacy exited before the barrier'; }
  sleep 0.05
done
stage=$(cat "$root/paused")
{ [ -d "$legacy.lock" ] && [ -d "$stage" ]; } || fail 'the legacy lock and staging directory must exist'
read -r owner_pid owner_time <"$legacy.lock/owner"
{ [ "$owner_pid" = "$legacy_pid" ] && [ "$owner_time" = 1 ]; } || fail 'the lock must have a live, old owner token'
run_new() {
  /bin/sh "$helper" >"$root/new.out" 2>"$root/new.err" || { cat "$root/new.err" >&2; fail 'new helper failed'; }
  [ "$(cat "$root/new.out")" = "$seed" ] || fail 'new signing did not use the per-user seed'
}
run_new
intact=true
if [ ! -d "$legacy.lock" ] || [ ! -d "$stage" ]; then intact=false; fi
if [ "$intact" = true ]; then
  # A lock holder may not have published its owner yet: this must also defer migration.
  mv "$legacy.lock/owner" "$root/saved-owner"
  run_new
  { [ -d "$legacy.lock" ] && [ -d "$stage" ]; } || fail 'migration removed a lock with an unpublished owner'
  mv "$root/saved-owner" "$legacy.lock/owner"
  [ ! -s "$root/gpgconf.log" ] || fail 'migration stopped an active legacy agent'
fi
: >"$root/resume"
if wait "$legacy_pid"; then legacy_code=0; else legacy_code=$?; fi
legacy_pid=
printf 'paused rebuild: new=0 legacy=%s lock-and-stage-preserved=%s\n' "$legacy_code" "$intact"
if [ "$legacy_code" -ne 0 ]; then cat "$root/legacy.err" >&2; fi
{ [ "$intact" = true ] && [ "$legacy_code" -eq 0 ]; } || fail 'migration interrupted the live legacy rebuild'
[ "$(cat "$root/legacy.out")" = "$legacy" ] || fail 'legacy signing did not read its complete seed'

# A known exited holder and abandoned private-key copies must still be collected.
/bin/sh -c 'exit 0' &
dead_pid=$!
wait "$dead_pid"
if kill -0 "$dead_pid" 2>/dev/null; then fail 'fixture dead-owner PID was reused'; fi
mkdir -p "$legacy.lock" "$legacy.new.abandoned/private-keys-v1.d" "$legacy.old.abandoned/seed/private-keys-v1.d"
printf '%s 1\n' "$dead_pid" >"$legacy.lock/owner"
printf 'fixture-deleted-secret\n' >"$legacy.new.abandoned/private-keys-v1.d/OLD.key"
printf 'fixture-deleted-secret\n' >"$legacy.old.abandoned/seed/private-keys-v1.d/OLD.key"
run_new
[ ! -e "$TMPDIR/dsh-gnupg" ] || fail 'abandoned legacy copies or lock remain after migration'
grep -Fx -- "--homedir $legacy --kill gpg-agent" "$root/gpgconf.log" >/dev/null || fail 'legacy agent was not stopped during cleanup'

# Preserve configuration refusal and symlink/ownership boundaries while migrating.
printf 'pinentry-program /fixture/never-execute\n' >"$seed/gpg-agent.conf"
run_new
[ ! -e "$seed/gpg-agent.conf" ] || fail 'a planted configuration file survived rebuilding'
mkdir -p "$TMPDIR/dsh-gnupg" "$root/outside"
printf 'untouched\n' >"$root/outside/sentinel"
ln -s "$root/outside" "$legacy.lock"
run_new
{ [ -L "$legacy.lock" ] && [ "$(cat "$root/outside/sentinel")" = untouched ]; } || fail 'migration followed a legacy lock symlink'
printf 'PASS: live and unpublished owners deferred; abandoned copies removed; config and layout preserved\n'
