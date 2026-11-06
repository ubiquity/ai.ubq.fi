#!/bin/sh
set -eu
uos_root=$(pwd -P)
uos_selector="$uos_root/.data/native-owner-current"
test -L "$uos_selector" || exit 1
uos_payload=$(cd -P "$uos_selector" && pwd)
uos_relative=${uos_payload#"$uos_root/.data/releases/"}
case "$uos_relative" in */native-owner) ;; *) exit 1 ;; esac
uos_sha=${uos_relative%/native-owner}
test "${#uos_sha}" = 40 || exit 1
case "$uos_sha" in *[!0-9a-f]*) exit 1 ;; esac
uos_config="$uos_root/.data/native-owner/config.json"
test -f "$uos_config" && ! test -L "$uos_config" || exit 1
uos_expected=$(/usr/bin/jq -er '.release.manifest_sha256' "$uos_config")
uos_manifest="$uos_payload/.uos-native-owner-release.json"
test -f "$uos_manifest" && ! test -L "$uos_manifest" || exit 1
uos_actual=$(/usr/bin/sha256sum "$uos_manifest")
test "${uos_actual%% *}" = "$uos_expected" || exit 1
/usr/bin/jq -e --arg sha "$uos_sha" '.v == 1 and .role == "native_owner_service_release" and .git_sha == $sha and (.files|length)==4' "$uos_manifest" >/dev/null
for uos_file in bin/uos-native ops/launch-native-owner.ts ops/native-owner-config.ts ops/native-owner-release.ts; do
    test -f "$uos_payload/$uos_file" && ! test -L "$uos_payload/$uos_file" || exit 1
    uos_expected=$(/usr/bin/jq -er --arg path "$uos_file" '.files[] | select(.path==$path) | .sha256' "$uos_manifest")
    uos_actual=$(/usr/bin/sha256sum "$uos_payload/$uos_file")
    test "${uos_actual%% *}" = "$uos_expected" || exit 1
done
uos_runtime="$uos_root/.data/runtimes/bun/linux-arm64/1.4.0-086c4121c8738a8e0f5ed730e8a461bc3973b4444e372ddb77aef9a747fa2ae9/bun"
test -f "$uos_runtime" && ! test -L "$uos_runtime" || exit 1
uos_actual=$(/usr/bin/sha256sum "$uos_runtime")
test "${uos_actual%% *}" = 086c4121c8738a8e0f5ed730e8a461bc3973b4444e372ddb77aef9a747fa2ae9 || exit 1
uos_bunfig="$uos_root/ops/runtime-bunfig.toml"
test -f "$uos_bunfig" && ! test -L "$uos_bunfig" || exit 1
uos_actual=$(/usr/bin/sha256sum "$uos_bunfig")
test "${uos_actual%% *}" = ffdc178d11d982efb34166963791f34b630aad6341f9fab8b252353189b50baa || exit 1
exec /usr/bin/env -i PATH=/usr/bin:/bin TZ=UTC "$uos_runtime" --config="$uos_bunfig" --no-env-file --no-install "$uos_payload/ops/launch-native-owner.ts"
