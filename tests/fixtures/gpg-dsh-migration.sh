#!/bin/sh
# Offline issue #1182 probe: upgrade a fixture-installed legacy helper through the enforced
# quiescence installer, never the owner's helper. Requires the pinned legacy commit in local Git.
set -eu
umask 077
case ${1:-} in
  /*) helper=$1 ;;
  *) echo 'usage: sh tests/fixtures/gpg-dsh-migration.sh /absolute/path/to/helper' >&2; exit 2 ;;
esac
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
root=$(mktemp -d "${TMPDIR:?set TMPDIR to a writable fixture directory}/gpg-dsh-migration.XXXXXX")
root=$(CDPATH='' cd -- "$root" && pwd -P)
legacy_pid=
installer_pid=
new_pid=
cleanup() {
  : >"$root/resume"
  : >"$root/gpg-resume"
  if [ -n "$installer_pid" ]; then
    kill "$installer_pid" 2>/dev/null || :
    wait "$installer_pid" 2>/dev/null || :
  fi
  if [ -n "$new_pid" ]; then wait "$new_pid" 2>/dev/null || :; fi
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
if [ "${PAUSE_GPG:-}" = 1 ]; then
  : >"$FIXTURE_ROOT/gpg-paused"
  attempts=0
  while [ ! -f "$FIXTURE_ROOT/gpg-resume" ]; do
    attempts=$((attempts + 1))
    [ "$attempts" -lt 400 ] || exit 3
    sleep 0.05
  done
fi
[ "$(cat "$GNUPGHOME/pubring.kbx")" = fixture-public ]
[ "$(cat "$GNUPGHOME/private-keys-v1.d/AAAA.key")" = fixture-secret ]
printf '%s\n' "$GNUPGHOME" >>"$FIXTURE_ROOT/gpg.log"
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
installed="$root/bin/gpg-dsh 'fixture"
installer=$repo/scripts/install-gpg-dsh.sh
start_legacy() {
  rm -f "$root/resume" "$root/paused"
  cp -p "$root/legacy.sh" "$installed"
  PAUSE_LEGACY=1 /bin/sh "$installed" >"$root/legacy.out" 2>"$root/legacy.err" &
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
}
start_installer() {
  GPG_DSH_INSTALL_WAIT_SECONDS=$1 /bin/sh "$installer" "$installed" >"$root/install.out" 2>"$root/install.err" &
  installer_pid=$!
  attempts=0
  until grep -q 'waiting for legacy invocations' "$root/install.err"; do
    attempts=$((attempts + 1))
    [ "$attempts" -lt 200 ] || fail 'installer did not observe the active legacy invocation'
    kill -0 "$installer_pid" 2>/dev/null || { cat "$root/install.err" >&2; fail 'installer exited without draining'; }
    sleep 0.05
  done
}
resume_legacy() {
  : >"$root/resume"
  if wait "$legacy_pid"; then legacy_code=0; else legacy_code=$?; fi
  legacy_pid=
  if [ "$legacy_code" -ne 0 ]; then cat "$root/legacy.err" >&2; fail 'migration interrupted the live legacy rebuild'; fi
  [ "$(cat "$root/legacy.out")" = "$legacy" ] || fail 'legacy signing did not read its complete seed'
}
run_new() {
  /bin/sh "$installed" >"$root/new.out" 2>"$root/new.err" || { cat "$root/new.err" >&2; fail 'new helper failed'; }
  [ "$(cat "$root/new.out")" = "$seed" ] || fail 'new signing did not use the per-user seed'
}

start_legacy
start_installer 30
# Invoke signing through the fenced installation while the legacy call is still paused. Observe
# the queue process itself, not a sleep: it must not enter the unsafe new cleanup until the drain.
/bin/sh "$installed" >"$root/new.out" 2>"$root/new.err" &
new_pid=$!
attempts=0
until ps -ww -p "$new_pid" -o args= | grep -q gpg-dsh-install-wait; do
  attempts=$((attempts + 1))
  [ "$attempts" -lt 200 ] || fail 'new signing did not reach the installation barrier'
  kill -0 "$new_pid" 2>/dev/null || { cat "$root/new.err" >&2; fail 'new signing exited instead of waiting'; }
  sleep 0.05
done
{ [ -d "$legacy.lock" ] && [ -d "$stage" ]; } || fail 'installation removed the live legacy lock or stage'
[ ! -s "$root/gpgconf.log" ] || fail 'installation stopped an active legacy agent'
cmp "$root/legacy.sh" "$installed.install-lock/original" || fail 'installer did not preserve the original helper'
resume_legacy
if ! wait "$installer_pid"; then cat "$root/install.err" >&2; fail 'installer failed after legacy completion'; fi
installer_pid=
if ! wait "$new_pid"; then cat "$root/new.err" >&2; fail 'queued signing failed after installation'; fi
new_pid=
cmp "$helper" "$installed" || fail 'installer did not publish the current repository helper'
[ ! -e "$installed.install-lock" ] || fail 'successful installation left a barrier'
[ "$(cat "$root/new.out")" = "$seed" ] || fail 'queued signing did not use the per-user seed'
[ "$(sed -n '1p' "$root/gpg.log")" = "$legacy" ] || fail 'new signing ran before legacy signing finished'
printf 'paused rebuild: install=0 new=0 legacy=0 lock-and-stage-preserved=true\n'

# Timeout must roll back without touching the paused call, including an unpublished lock owner.
start_legacy
mv "$legacy.lock/owner" "$root/saved-owner"
start_installer 1
if wait "$installer_pid"; then fail 'installer published while a legacy call was active'; else install_code=$?; fi
installer_pid=
[ "$install_code" -eq 2 ] || fail 'installer did not fail closed on timeout'
grep -q 'timed out draining' "$root/install.err" || fail 'installer failed for a reason other than quiescence'
cmp "$root/legacy.sh" "$installed" || fail 'failed installation did not restore the original helper'
{ [ -d "$legacy.lock" ] && [ -d "$stage" ] && [ ! -e "$installed.install-lock" ]; } || fail 'timeout damaged legacy state or left a barrier'
mv "$root/saved-owner" "$legacy.lock/owner"
resume_legacy
# A legacy fast-path signer has no rebuild lock. Its GnuPG process must drain as well.
PAUSE_GPG=1 /bin/sh "$installed" >"$root/legacy.out" 2>"$root/legacy.err" &
legacy_pid=$!
attempts=0
while [ ! -f "$root/gpg-paused" ]; do
  attempts=$((attempts + 1))
  [ "$attempts" -lt 200 ] || fail 'legacy signer never reached GnuPG'
  kill -0 "$legacy_pid" 2>/dev/null || { cat "$root/legacy.err" >&2; fail 'legacy signer exited before GnuPG'; }
  sleep 0.05
done
[ ! -e "$legacy.lock" ] || fail 'fast-path signing unexpectedly held a rebuild lock'
start_installer 30
[ -d "$legacy" ] || fail 'installer removed the seed of a legacy GnuPG client'
: >"$root/gpg-resume"
resume_legacy
if ! wait "$installer_pid"; then cat "$root/install.err" >&2; fail 'retry after quiescence failed'; fi
installer_pid=
run_new

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
if /bin/sh "$installer" "$installed" >"$root/install.out" 2>"$root/install.err"; then fail 'installer accepted a symlinked legacy lock'; fi
cmp "$helper" "$installed" || fail 'refused installation changed the helper'
{ [ -L "$legacy.lock" ] && [ "$(cat "$root/outside/sentinel")" = untouched ]; } || fail 'installer followed a legacy lock symlink'
printf 'PASS: installation quiescence, timeout rollback, abandoned cleanup, config and layout preserved\n'
