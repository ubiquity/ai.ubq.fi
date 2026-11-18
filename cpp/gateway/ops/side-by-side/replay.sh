#!/usr/bin/env bash
set -euo pipefail

usage() {
  printf '%s\n' \
    'Usage: replay.sh [--run] BUN_PORT NATIVE_PORT "METHOD /path" ...' \
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
  local port=$1 sample=$2
  local -a options=()
  if [[ $sample == HEAD\ * ]]; then options=(--head --output /dev/null); fi
  # Disable curlrc and proxies. Every URL is a validated literal loopback target.
  curl -q --silent --noproxy '*' --proto '=http' --connect-timeout 1 --max-time 3 \
    --max-filesize 262144 --write-out '\n%{http_code} %{time_total}' "${options[@]}" \
    "http://127.0.0.1:$port${sample#* }"
}

normalize() {
  local sample=$1
  jq -c -e -s -S --arg path "${sample#* }" '
    if length != 1 or (.[0] | type) != "object" then error("invalid JSON body") else .[0] end
    | if $path == "/health" then
        if .status == "available" and (.release.git_sha | type) == "string"
           and (.release.deployment_id | type) == "string"
        then .release.git_sha = null | .release.deployment_id = null
        else error("invalid health body") end
      elif (.openapi | type) == "string" and (.paths | type) == "object" then .
      else error("invalid OpenAPI body") end' 2>/dev/null
}

if [[ ${1-} == --help ]]; then usage; exit 0; fi
run=false
if [[ ${1-} == --run ]]; then run=true; shift; fi
if (( $# < 3 || $# > 14 )); then usage >&2; exit 2; fi
bun_port=$1 native_port=$2
shift 2
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

failures=0
for sample do
  if ! bun_response=$(request "$bun_port" "$sample"); then
    printf 'FAIL %s (Bun transport)\n' "$sample"
  elif ! native_response=$(request "$native_port" "$sample"); then
    printf 'FAIL %s (native transport)\n' "$sample"
  else
    read -r bun_status bun_time <<<"${bun_response##*$'\n'}"
    read -r native_status native_time <<<"${native_response##*$'\n'}"
    outcome=match
    if [[ $bun_status != "$native_status" ]]; then
      outcome='status mismatch'
    elif [[ $bun_status != 200 ]]; then
      outcome='unexpected status'
    elif [[ $sample == GET\ * ]]; then
      if ! bun_body=$(normalize "$sample" <<<"${bun_response%$'\n'*}") ||
        ! native_body=$(normalize "$sample" <<<"${native_response%$'\n'*}"); then
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
