#!/usr/bin/env bash
set -euo pipefail

usage() {
  printf '%s\n' \
    'Usage: replay.sh [--run] SCRATCH_DIR BUN_PORT NATIVE_PORT "METHOD /path" ...' \
    'Default is dry run. Select up to 12 GET or HEAD requests to /health or /openapi.json.'
}

die() {
  printf '%s\n' "$1" >&2
  exit 2
}

valid_port() {
  [[ $1 =~ ^[1-9][0-9]{0,4}$ ]] && (( $1 <= 65535 ))
}

request() {
  local port=$1 sample=$2 body=$3
  local -a options=(--output "$body")
  if [[ $sample == HEAD\ * ]]; then options+=(--head); fi
  # Disable curlrc and proxies. Every URL is a validated literal loopback target.
  curl -q --silent --noproxy '*' --proto '=http' --connect-timeout 1 --max-time 3 \
    --max-filesize 262144 --write-out '%{http_code} %{time_total}' "${options[@]}" \
    "http://127.0.0.1:$port${sample#* }"
}

normalize() {
  local sample=$1 body=$2
  jq -c -e -s -S --arg path "${sample#* }" '
    if length != 1 or (.[0] | type) != "object" then error("invalid JSON body") else .[0] end
    | if $path == "/health" then
        if .status == "available" and (.release.git_sha | type == "string" and length > 0)
           and (.release.deployment_id | type == "string" and length > 0)
        then .release.git_sha = null | .release.deployment_id = null
        else error("invalid health body") end
      elif (.openapi | type) == "string" and (.paths | type) == "object" then .
      else error("invalid OpenAPI body") end' "$body" 2>/dev/null
}

if [[ ${1-} == --help ]]; then usage; exit 0; fi
run=false
if [[ ${1-} == --run ]]; then run=true; shift; fi
if (( $# < 4 || $# > 15 )); then usage >&2; exit 2; fi
scratch_dir=$1 bun_port=$2 native_port=$3
shift 3
[[ $scratch_dir == /* && -d $scratch_dir && -O $scratch_dir && -w $scratch_dir ]] || die 'Select a writable absolute scratch directory you own.'
if ! valid_port "$bun_port" || ! valid_port "$native_port"; then
  die 'Expected decimal ports in the range 1–65535.'
fi
[[ $bun_port != "$native_port" && $native_port != 7999 && $native_port != 8000 ]] || die 'Select a separate, non-production native port.'

# Validate all input before even the first request. Accept no headers, bodies, or queries.
for sample do
  case $sample in
    'GET /health' | 'HEAD /health' | 'GET /openapi.json' | 'HEAD /openapi.json') ;;
    *) die 'Sample contains a method or route outside the read-only allowlist.' ;;
  esac
done

if [[ $run == false ]]; then
  for sample do printf 'Dry run %s on Bun port %s and native port %s\n' "$sample" "$bun_port" "$native_port"; done
  exit 0
fi
command -v jq >/dev/null || die 'Replay requires jq.'
if ! curl_version=$(curl -q --version 2>/dev/null) || [[ ! $curl_version =~ ^curl\ ([0-9]+)\.([0-9]+)\. ]]; then
  die 'Replay requires curl 8.4 or newer.'
fi
# Older curl versions cap only responses with a known size.
if (( BASH_REMATCH[1] < 8 || (BASH_REMATCH[1] == 8 && BASH_REMATCH[2] < 4) )); then
  die 'Replay requires curl 8.4 or newer.'
fi

umask 077
work=$(mktemp -d "$scratch_dir/uos-replay.XXXXXX")
trap 'rm -f -- "$work/bun.body" "$work/native.body"; rmdir -- "$work"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

failures=0
for sample do
  if ! bun_response=$(request "$bun_port" "$sample" "$work/bun.body"); then
    printf 'FAIL %s (Bun transport)\n' "$sample"
  elif ! native_response=$(request "$native_port" "$sample" "$work/native.body"); then
    printf 'FAIL %s (native transport)\n' "$sample"
  else
    read -r bun_status bun_time <<<"$bun_response"
    read -r native_status native_time <<<"$native_response"
    outcome=match
    if [[ $bun_status != "$native_status" ]]; then
      outcome='status mismatch'
    elif [[ $bun_status != 200 ]]; then
      outcome='unexpected status'
    elif [[ $sample == GET\ * ]]; then
      if ! bun_body=$(normalize "$sample" "$work/bun.body") ||
        ! native_body=$(normalize "$sample" "$work/native.body"); then
        outcome='invalid body'
      elif [[ $bun_body != "$native_body" ]]; then
        outcome='body mismatch'
      fi
    fi
    printf '%s %s (Bun %s %ss, native %s %ss)\n' "$outcome" "$sample" "$bun_status" "$bun_time" "$native_status" "$native_time"
    if [[ $outcome == match ]]; then continue; fi
  fi
  failures=$((failures + 1))
done
printf '%s of %s pairs passed\n' "$(( $# - failures ))" "$#"
(( failures == 0 ))
