#!/usr/bin/env bash
# Sweep the cpp/gateway tests: build and run each tests/**/*_test.cpp through tools/unit.sh, one at a
# time, with the link list tools/links.py resolves. Reference: docs/TESTING.md.
#   run-tests.sh [options] [substring...]   only tests whose path contains one of the substrings
#     --list              print the selected tests, their binary and link origin; build nothing
#     --failed            only tests whose last status in results.tsv is FAIL, BUILD_FAIL or TIMEOUT
#     --missing           only tests without a row in results.tsv (resume an interrupted sweep)
#     --cluster           include tests/cluster_w04c1/* (reported as SKIP otherwise)
#     --timeout N         run-phase limit per test in seconds (default 280; unit.sh stops at 300)
#     --build-timeout N   build-phase limit per test in seconds (default 5400)
#     --keep-binaries     keep each test's binary (default: deleted right after its run)
#     --keep-scratch      keep each test's scratch directory (HOME, TMPDIR, working directory)
#     --no-rescan         do not pick up tests that appear while the sweep runs
#     --prune-binaries [H]  only delete test binaries of any name under .data/cpp-gateway/bin older
#                         than H hours (default 6) that no process is executing, then exit
# Output: $SWEEP_DIR/results.tsv (one row per test, merged across runs), history.tsv (every run),
# logs/<binary>.log; SWEEP_DIR defaults to /tmp/claude-1002/wave/sweep.
set -uo pipefail
GATEWAY="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$GATEWAY/tools/env.sh"
UNIT="$GATEWAY/tools/unit.sh"
LINKS="$GATEWAY/tools/links.py"
SWEEP_DIR="${SWEEP_DIR:-/tmp/claude-1002/wave/sweep}"
RESULTS="$SWEEP_DIR/results.tsv"
HISTORY="$SWEEP_DIR/history.tsv"
HEADER=$'test\tstatus\trun\tpassed\tfailed\tseconds\tbuild_seconds\tfinished\tnote'
CLUSTER_DIR="tests/cluster_w04c1/"

list_only=0; failed_only=0; missing_only=0; with_cluster=0; keep_binaries=0; keep_scratch=0; rescan=1; prune_hours=""
run_timeout=280; build_timeout=5400; filters=()
while [ $# -gt 0 ]; do
  case "$1" in
    --list) list_only=1 ;;
    --failed) failed_only=1 ;;
    --missing) missing_only=1 ;;
    --cluster) with_cluster=1 ;;
    --keep-binaries) keep_binaries=1 ;;
    --keep-scratch) keep_scratch=1 ;;
    --keep) keep_binaries=1; keep_scratch=1 ;;
    --prune-binaries)
      prune_hours=6
      if [[ "${2-}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then prune_hours="$2"; shift; fi ;;
    --no-rescan) rescan=0 ;;
    --timeout) run_timeout="$2"; shift ;;
    --build-timeout) build_timeout="$2"; shift ;;
    -h|--help) sed -n 2,19p "${BASH_SOURCE[0]}"; exit 0 ;;
    -*) echo "run-tests.sh: unknown option $1" >&2; exit 2 ;;
    *) filters+=("$1") ;;
  esac
  shift
done
for f in "${filters[@]+"${filters[@]}"}"; do case "$f" in *cluster_w04c1*) with_cluster=1 ;; esac; done

# Test binaries are disposable (objects stay cached; relinking is cheap). Prune old ones of any name,
# sparing a binary some process is executing (it appears on that process's command line).
if [ -n "$prune_hours" ]; then
  minutes=$(awk -v h="$prune_hours" 'BEGIN { printf "%d", h * 60 }')
  running=$(cat /proc/[0-9]*/cmdline 2>/dev/null | tr '\0' '\n' | grep -F "$UOS_OUT/bin/" | sort -u)
  removed=0; freed=0; spared=0
  while IFS= read -r -d '' binary; do
    if printf '%s\n' "$running" | grep -qxF "$binary"; then spared=$((spared + 1)); continue; fi
    size=$(stat -c %s "$binary" 2>/dev/null) || continue
    rm -f -- "$binary" && { removed=$((removed + 1)); freed=$((freed + size)); }
  done < <(find "$UOS_OUT/bin" -maxdepth 1 -type f -mmin "+$minutes" -print0 2>/dev/null)
  echo "run-tests.sh: pruned $removed binaries older than ${prune_hours}h under $UOS_OUT/bin ($((freed / 1048576)) MiB); spared $spared in use"
  exit 0
fi

mkdir -p "$SWEEP_DIR/logs" "$SWEEP_DIR/tmp"
if [ "$list_only" = 0 ]; then
  exec 9>"$SWEEP_DIR/run.lock"
  flock -n 9 || { echo "run-tests.sh: another sweep holds $SWEEP_DIR/run.lock" >&2; exit 3; }
fi

# Credential isolation: every test process runs with HOME and TMPDIR pointing at empty scratch
# directories and without CODEX_HOME, CODEX_AUTH_JSON_B64 or any variable whose name marks a
# credential (provider *_API_KEY, *_TOKEN, *_SECRET, ...; the names src/ and tests/ read are all of
# this form). Only names are ever printed. MAKEFLAGS and friends are dropped so make stays serial.
CREDENTIAL_NAME='(^|_)(API_?KEY|KEYS?|KEY_?ID|TOKENS?|SECRETS?|PASSWORD|PASSWD|PASS|CREDENTIALS?|AUTH|OAUTH|PAT|PRIVATE|SESSION_?KEY|ACCESS_?KEY)(_|$)'
isolate=(); isolated_names=()
while IFS= read -r name; do
  case "$name" in
    CODEX_HOME|CODEX_AUTH_JSON_B64|CLAUDE_CODE_OAUTH_TOKEN|CLAUDE_CODE_WORKSPACE_ID|SENTINEL_REPLAY_KEY|UOS_AI_TOKEN|DENO_DEPLOY_TOKEN|SSH_AUTH_SOCK|GNUPGHOME) ;;
    MAKEFLAGS|MFLAGS|GNUMAKEFLAGS|MAKELEVEL|MAKEFILES) isolate+=(-u "$name"); continue ;;
    *) [[ "$name" =~ $CREDENTIAL_NAME ]] || continue ;;
  esac
  isolate+=(-u "$name"); isolated_names+=("$name")
done < <(compgen -e)

now_us() { local t="${EPOCHREALTIME/[.,]/}"; printf '%s' "$t"; }
seconds_between() { local d=$(( $2 - $1 )); printf '%d.%d' $((d / 1000000)) $(( (d % 1000000) / 100000 )); }
age_minutes() { local t; t=$(stat -c %Y "$1" 2>/dev/null) || { echo 99999; return; }; echo $(( ($(date +%s) - t) / 60 )); }
clean() { local s="${1//$'\t'/ }"; s="${s//$'\n'/ | }"; printf '%s' "${s:0:400}"; }

binary_of() { local stem="${1#tests/}"; stem="${stem%.cpp}"; printf 'sweep_%s' "${stem//\//__}"; }

status_of() { [ -f "$RESULTS" ] && awk -F'\t' -v t="$1" 'NR > 1 && $1 == t { print $2 }' "$RESULTS"; }

record() { # test status run passed failed seconds build_seconds note
  local finished row note; finished=$(date -u +%Y-%m-%dT%H:%M:%SZ); note=$(clean "$8")
  row="$1"$'\t'"$2"$'\t'"$3"$'\t'"$4"$'\t'"$5"$'\t'"$6"$'\t'"$7"$'\t'"$finished"$'\t'"$note"
  {
    printf '%s\n' "$HEADER"
    { [ -f "$RESULTS" ] && tail -n +2 "$RESULTS" | awk -F'\t' -v t="$1" '$1 != t'; printf '%s\n' "$row"; } | LC_ALL=C sort -t $'\t' -k1,1
  } > "$RESULTS.tmp.$$" && mv -f "$RESULTS.tmp.$$" "$RESULTS"
  [ -f "$HISTORY" ] || printf '%s\n' "$HEADER" > "$HISTORY"
  printf '%s\n' "$row" >> "$HISTORY"
  printf '%-10s %s  run=%s passed=%s failed=%s %ss build=%ss %s\n' "$2" "$1" "$3" "$4" "$5" "$6" "$7" "$note"
}

select_tests() {
  local test f keep_it
  while IFS= read -r test; do
    keep_it=1
    if [ ${#filters[@]} -gt 0 ]; then
      keep_it=0
      for f in "${filters[@]}"; do case "$test" in *"$f"*) keep_it=1 ;; esac; done
    fi
    if [ "$keep_it" = 1 ] && [ "$failed_only" = 1 ]; then
      case "$(status_of "$test")" in FAIL|BUILD_FAIL|TIMEOUT) ;; *) keep_it=0 ;; esac
    fi
    if [ "$keep_it" = 1 ] && [ "$missing_only" = 1 ] && [ -n "$(status_of "$test")" ]; then keep_it=0; fi
    [ "$keep_it" = 1 ] && printf '%s\n' "$test"
  done < <(python3 "$LINKS" --list-tests)
}

current_pid=""
on_signal() {
  [ -n "$current_pid" ] && { kill -TERM -- "-$current_pid" 2>/dev/null; sleep 2; kill -KILL -- "-$current_pid" 2>/dev/null; }
  echo "run-tests.sh: interrupted" >&2; exit 130
}
trap on_signal INT TERM HUP

first_error() { # the first diagnostic of a failed build or run
  grep -m1 -E 'error:|undefined reference|multiple definition|unit\.sh: |No such file|cannot find|collect2|make: \*\*\*' "$1" | cut -c1-300
}

run_one() {
  local test="$1" bin log scratch origin='' warnings='' error='' skip=''
  local -a sources=() env_assign=() extra=()
  bin=$(binary_of "$test"); log="$SWEEP_DIR/logs/$bin.log"
  if [ -f "$log" ]; then
    mkdir -p "$SWEEP_DIR/logs/archive"
    mv "$log" "$SWEEP_DIR/logs/archive/$bin.$(date -u +%Y%m%dT%H%M%S).$$.log"
  fi
  printf '# test: %s\n# started: %s\n' "$test" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$log"
  if [ ! -f "$GATEWAY/$test" ]; then
    printf '# skipped: file vanished during the sweep\n' >> "$log"
    record "$test" SKIP 0 0 0 0 0 "file vanished during the sweep"; return
  fi
  if [ "$with_cluster" = 0 ] && [[ "$test" == "$CLUSTER_DIR"* ]]; then
    printf '# skipped: tests/cluster_w04c1 is skipped unless --cluster is given\n' >> "$log"
    record "$test" SKIP 0 0 0 0 0 "tests/cluster_w04c1 is skipped unless --cluster is given"; return
  fi
  eval "$(python3 "$LINKS" --shell "$test")"
  if [ -n "$error" ]; then
    printf '# links.py: %s\n' "$error" >> "$log"
    record "$test" BUILD_FAIL 0 0 0 0 0 "links: $error"; return
  fi
  if [ -n "$skip" ]; then
    printf '# skipped: %s\n' "$skip" >> "$log"
    record "$test" SKIP 0 0 0 0 0 "$skip"; return
  fi

  scratch=$(mktemp -d "$SWEEP_DIR/tmp/XXXXXX")
  mkdir -p "$scratch/t" "$scratch/h" "$scratch/w"
  local a; for a in "${env_assign[@]+"${env_assign[@]}"}"; do extra+=("${a//\$\{SCRATCH\}/$scratch}"); done
  rm -f "$UOS_OUT/bin/$bin"
  {
    printf '# test:     %s\n# started:  %s\n# links:    %s (%s)\n' "$test" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${sources[*]-}" "$origin"
    [ -n "$warnings" ] && printf '# warnings: %s\n' "$warnings"
    printf '# scratch:  %s (TMPDIR=t HOME=h cwd=w); %d credential variables unset; extra env: %s\n' "$scratch" "${#isolated_names[@]}" "${extra[*]-}"
    printf '# command:  tools/unit.sh %s %s %s\n' "$bin" "$test" "${sources[*]-}"
  } > "$log"
  if ! python3 "$LINKS" --hashes "$test" > "$scratch/inputs.before" 2>> "$log"; then
    record "$test" BUILD_FAIL 0 0 0 0 0 "could not hash build inputs (see log)"
    [ "$keep_scratch" = 1 ] || rm -rf "$scratch"
    return
  fi
  { printf '# inputs SHA-256 before:\n'; cat "$scratch/inputs.before"; } >> "$log"
  local before_tmp; before_tmp=$(ls -A /tmp 2>/dev/null | LC_ALL=C sort)

  local start run_start='' killed='' t rc killer_pid=''
  start=$(now_us)
  ( cd "$scratch/w" && exec env "${isolate[@]}" HOME="$scratch/h" TMPDIR="$scratch/t" "${extra[@]+"${extra[@]}"}" \
      setsid "$UNIT" "$bin" "$test" "${sources[@]+"${sources[@]}"}" ) >> "$log" 2>&1 < /dev/null &
  current_pid=$!
  # The binary appears (renamed into place by the link rule) the moment the run phase starts.
  while kill -0 "$current_pid" 2>/dev/null; do
    t=$(now_us)
    if [ -z "$run_start" ] && [ -e "$UOS_OUT/bin/$bin" ]; then run_start=$t; fi
    if [ -z "$killed" ]; then
      if [ -n "$run_start" ] && (( t - run_start > run_timeout * 1000000 )); then killed=run
      elif [ -z "$run_start" ] && (( t - start > build_timeout * 1000000 )); then killed=build
      fi
      if [ -n "$killed" ]; then
        kill -TERM -- "-$current_pid" 2>/dev/null
        ( sleep 5; kill -KILL -- "-$current_pid" 2>/dev/null ) &
        killer_pid=$!
      fi
    fi
    sleep 0.2
  done
  wait "$current_pid"; rc=$?
  if [ -n "$killer_pid" ]; then
    wait "$killer_pid" 2>/dev/null || :
  fi
  current_pid=""
  local end; end=$(now_us)
  [ -z "$run_start" ] && [ -e "$UOS_OUT/bin/$bin" ] && run_start=$end
  local build_seconds run_seconds
  build_seconds=$(seconds_between "$start" "${run_start:-$end}")
  run_seconds=$([ -n "$run_start" ] && seconds_between "$run_start" "$end" || echo 0)

  local status note='' summary runs=0 passed=0 failed=0
  summary=$(grep -E '^[0-9]+ run, [0-9]+ passed, [0-9]+ failed$' "$log" | tail -1)
  if [ -n "$summary" ]; then read -r runs _ passed _ failed _ <<< "$summary"; fi
  if [ "$killed" = run ]; then status=TIMEOUT; note="killed after ${run_timeout}s of running"
  elif [ "$killed" = build ]; then status=TIMEOUT; note="build phase exceeded ${build_timeout}s"
  elif [ ! -e "$UOS_OUT/bin/$bin" ]; then status=BUILD_FAIL; note=$(first_error "$log")
  elif [ "$rc" = 124 ]; then status=TIMEOUT; note="unit.sh's 300 s limit"
  elif [ -n "$summary" ] && [ "$rc" = 0 ] && [ "$failed" = 0 ] && [ "$runs" -gt 0 ]; then status=PASS
  else
    status=FAIL
    if [ -z "$summary" ]; then
      if [ "$rc" -gt 128 ]; then note="no summary line; killed by signal $((rc - 128))"; else note="no summary line; exit $rc"; fi
      local detail; detail=$(grep -m1 -E 'uncaught|terminate|Assertion|what\(\)|Segmentation|Aborted' "$log" | cut -c1-200)
      [ -n "$detail" ] && note="$note; $detail"
    else
      note="exit $rc; first: $(grep -m1 -E '^FAIL ' "$log" | cut -c1-200)"
    fi
  fi
  if [ "$status" != PASS ]; then
    local age; age=$(age_minutes "$GATEWAY/$test")
    [ "$age" -lt 30 ] && note="$note [test file modified ${age}m ago]"
    local newest='' newest_age=99999 s sa
    for s in "${sources[@]+"${sources[@]}"}"; do sa=$(age_minutes "$GATEWAY/$s"); [ "$sa" -lt "$newest_age" ] && { newest_age=$sa; newest=$s; }; done
    [ "$newest_age" -lt 30 ] && note="$note [$newest modified ${newest_age}m ago]"
  fi
  if ! python3 "$LINKS" --hashes "$test" > "$scratch/inputs.after" 2>> "$log" || ! cmp -s "$scratch/inputs.before" "$scratch/inputs.after"; then
    note="inputs changed during build/run (mid-edit); observed $status: $note"
    status=SKIP
    { printf '# inputs SHA-256 after (changed):\n'; cat "$scratch/inputs.after"; } >> "$log"
  else
    printf '# input hashes unchanged after build/run\n' >> "$log"
  fi
  # Files a test leaves in its scratch directories, or new entries directly under /tmp.
  local leftovers='' d n
  for d in t h w; do
    n=$(find "$scratch/$d" -mindepth 1 2>/dev/null | wc -l)
    [ "$n" -gt 0 ] && leftovers="$leftovers $d:$n($(find "$scratch/$d" -mindepth 1 -maxdepth 1 -printf '%f ' 2>/dev/null | cut -c1-80))"
  done
  # Other processes of this user create /tmp entries too: compiler temporaries and systemd units are
  # ignored, and an entry is reported only if it still exists and is owned by this user.
  local new_tmp='' entry
  while IFS= read -r entry; do
    case "$entry" in cc??????.*|systemd-private-*|claude-*|tmp.*) continue ;; esac
    [ -O "/tmp/$entry" ] && new_tmp="$new_tmp$entry "
  done < <(comm -13 <(printf '%s\n' "$before_tmp") <(ls -A /tmp 2>/dev/null | LC_ALL=C sort))
  [ -n "$new_tmp" ] && leftovers="$leftovers /tmp:+($new_tmp)"
  [ -n "$leftovers" ] && note="$note [left:$leftovers]"
  {
    printf '\n# finished: %s exit=%s status=%s build=%ss run=%ss\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$rc" "$status" "$build_seconds" "$run_seconds"
    [ -n "$leftovers" ] && printf '# leftovers:%s\n' "$leftovers"
  } >> "$log"
  [ "$keep_scratch" = 1 ] || rm -rf "$scratch"
  [ "$keep_binaries" = 1 ] || rm -f "$UOS_OUT/bin/$bin"
  record "$test" "$status" "$runs" "$passed" "$failed" "$run_seconds" "$build_seconds" "$note"
}

if [ "$list_only" = 1 ]; then
  while IFS= read -r test; do
    origin='' error='' skip=''; sources=()
    eval "$(python3 "$LINKS" --shell "$test")"
    if [ -n "$error" ]; then info="ERROR $error"
    elif [ "$with_cluster" = 0 ] && [[ "$test" == "$CLUSTER_DIR"* ]]; then info="SKIP (cluster_w04c1; --cluster)"
    elif [ -n "$skip" ]; then info="SKIP $skip"
    else info="${#sources[@]} sources, $origin"; fi
    printf '%s\t%s\t%s\n' "$test" "$(binary_of "$test")" "$info"
  done < <(select_tests)
  exit 0
fi

echo "run-tests.sh: $(date -u +%Y-%m-%dT%H:%M:%SZ) unsetting ${#isolated_names[@]} credential variables: ${isolated_names[*]-}"
declare -A done_tests=()
declare -A counts=()
pass=0
while :; do
  mapfile -t queue < <(select_tests)
  pending=()
  for test in "${queue[@]+"${queue[@]}"}"; do [ -n "${done_tests[$test]-}" ] || pending+=("$test"); done
  [ ${#pending[@]} -gt 0 ] || break
  [ "$pass" -gt 0 ] && echo "run-tests.sh: rescan found ${#pending[@]} new test(s)"
  index=0
  for test in "${pending[@]}"; do
    index=$((index + 1)); done_tests[$test]=1
    printf '[%d/%d] %s\n' "$index" "${#pending[@]}" "$test"
    run_one "$test"
    st=$(status_of "$test"); counts[$st]=$(( ${counts[$st]-0} + 1 ))
  done
  pass=$((pass + 1))
  [ "$rescan" = 1 ] && [ "$failed_only" = 0 ] && [ "$pass" -lt 4 ] || break
done
summary_line=""
for st in PASS FAIL BUILD_FAIL TIMEOUT SKIP; do summary_line="$summary_line $st=${counts[$st]-0}"; done
echo "run-tests.sh: this run:$summary_line"
awk -F'\t' 'NR > 1 { n[$2]++; r += $3; p += $4 } END { printf "run-tests.sh: results.tsv:"; for (s in n) printf " %s=%d", s, n[s]; printf "; cases run=%d passed=%d\n", r, p }' "$RESULTS"
if (( ${counts[FAIL]-0} + ${counts[BUILD_FAIL]-0} + ${counts[TIMEOUT]-0} > 0 )); then
  exit 1
fi
exit 0
